#include "Driver.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>

#include <fmt/format.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/ExportDiscovery.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/AnalysisDiagnostic.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

namespace {

template <typename F>
void maybeTimed(Timer* timerPtr, const std::string& label, F&& fn) {
  if (timerPtr != nullptr) {
    timerPtr->measure(label, std::forward<F>(fn));
  } else {
    std::forward<F>(fn)();
  }
}

template <typename F>
void maybeTimedFile(Timer* timerPtr, const std::string& phase, const std::string& fileLabel,
                    F&& fn) {
  if (timerPtr != nullptr) {
    timerPtr->measureFile(phase, fileLabel, std::forward<F>(fn));
  } else {
    std::forward<F>(fn)();
  }
}

void displayWarnings(const lesma::AnalysisResult& result) {
  if (result.suppressWarnings) {
    return;
  }
  for (const auto& d : result.diagnostics) {
    if (d.severity != lesma::AnalysisDiagnosticSeverity::Warning) {
      continue;
    }
    lesma::showAnalysisDiagnostic(result, d, false);
  }
}

} // namespace

auto lesma::analyze(std::unique_ptr<Options> options, Timer* phaseTimer) -> AnalysisResult {
  AnalysisResult result;
  result.suppressWarnings = options->suppressWarnings;
  if (options->sourceType == SourceType::FILE) {
    result.mainFilePath = options->source;
  } else {
    result.mainFilePath = options->implicitFilePath;
  }

  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  unsigned mainBufferId = 0;

  try {
    bool readFailed = false;
    maybeTimedFile(phaseTimer, "Reading", result.mainFilePath, [&]() -> void {
      if (options->sourceType == SourceType::FILE) {
        auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
        if (!buffer) {
          result.diagnostics.push_back(AnalysisDiagnostic{
              .message = "Could not read file: " + options->source,
              .span = llvm::SMRange(),
              .severity = AnalysisDiagnosticSeverity::Error,
          });
          readFailed = true;
          return;
        }
        mainBufferId = srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
      } else {
        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(options->source);
        mainBufferId = srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
      }
    });
    if (readFailed) {
      result.sourceMgr = std::move(srcMgr);
      result.mainBufferId = mainBufferId;
      return result;
    }
  } catch (const LesmaError& err) {
    result.diagnostics.push_back(AnalysisDiagnostic{
        .message = err.what(),
        .span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange(),
        .severity = AnalysisDiagnosticSeverity::Error,
        .spanSourceMgr = mainBufferId != 0U ? srcMgr : nullptr,
        .spanBufferId = mainBufferId,
        .spanDisplayPath = result.mainFilePath,
    });
    result.sourceMgr = std::move(srcMgr);
    result.mainBufferId = mainBufferId;
    return result;
  }

  std::unique_ptr<Lexer> lexer;
  try {
    maybeTimedFile(phaseTimer, "Lexing", result.mainFilePath, [&]() -> void {
      lexer = std::make_unique<Lexer>(srcMgr, &result.diagnostics, result.mainFilePath);
      lexer->scanAll();
      if ((options->debug & Debug::LEXER) != Debug::NONE) {
        lesma::print(LogType::DEBUG, "Lexer tokens:\n");
        for (Token* tok : lexer->getTokens()) {
          if (tok != nullptr) {
            lesma::print(LogType::CLEAR, "  {}\n", tok->dump(srcMgr));
          }
        }
        // stdout is often fully buffered when not a TTY; LLVM's IR print can flush earlier.
        std::fflush(stdout);
      }
    });
  } catch (const LesmaError& err) {
    result.diagnostics.push_back(AnalysisDiagnostic{
        .message = err.what(),
        .span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange(),
        .severity = AnalysisDiagnosticSeverity::Error,
        .spanSourceMgr = srcMgr,
        .spanBufferId = mainBufferId,
        .spanDisplayPath = result.mainFilePath,
    });
    result.sourceMgr = std::move(srcMgr);
    result.mainBufferId = mainBufferId;
    return result;
  }

  std::unique_ptr<Parser> parser;
  try {
    maybeTimedFile(phaseTimer, "Parsing", result.mainFilePath, [&]() -> void {
      parser = std::make_unique<Parser>(lexer->getTokens(), &result.diagnostics, srcMgr,
                                        mainBufferId, result.mainFilePath);
      parser->parse();
      if ((options->debug & Debug::AST) != Debug::NONE) {
        Compound* ast = parser->getAst();
        if (ast != nullptr) {
          lesma::print(LogType::DEBUG, "AST:\n{}\n", ast->toString(srcMgr.get(), "", true));
        }
        std::fflush(stdout);
      }
    });
  } catch (const LesmaError& err) {
    result.diagnostics.push_back(AnalysisDiagnostic{
        .message = err.what(),
        .span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange(),
        .severity = AnalysisDiagnosticSeverity::Error,
        .spanSourceMgr = srcMgr,
        .spanBufferId = mainBufferId,
        .spanDisplayPath = result.mainFilePath,
    });
    result.sourceMgr = std::move(srcMgr);
    result.mainBufferId = mainBufferId;
    return result;
  }

  Typechecker typechecker(
      result.mainFilePath,
      [&](const std::string& path, bool isStd, const std::string& main) {
        return discoverExportedTopLevelNames(path, isStd, main);
      },
      &result.diagnostics, srcMgr, mainBufferId, phaseTimer);
  try {
    maybeTimedFile(phaseTimer, "Typecheck", result.mainFilePath,
                   [&]() -> void { typechecker.run(parser->getAst()); });
    result.sourceMgr = std::move(srcMgr);
    result.mainBufferId = mainBufferId;
    result.parser = std::move(parser);
    result.typeCache = typechecker.takeTypeCache();
    result.rootScope = typechecker.takeRootScope();
    result.specializedTypeEnv = typechecker.takeSpecializedTypeEnv();
    result.specializedTypeToTemplate = typechecker.takeSpecializedTypeToTemplate();
    result.specializedClassTypes = typechecker.takeSpecializedClassTypes();
    result.importAliasToPath = typechecker.takeImportAliasToPath();
    result.importedNameToSource = typechecker.takeImportedNameToSource();
    result.importedModules = typechecker.takeImportedModules();
    result.index =
        buildAnalysisIndex(result.parser != nullptr ? result.parser->getAst() : nullptr,
                           result.sourceMgr.get(), result.mainBufferId, result.mainFilePath);
    return result;
  } catch (const LesmaError& err) {
    result.diagnostics.push_back(AnalysisDiagnostic{
        .message = err.what(),
        .span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange(),
        .severity = AnalysisDiagnosticSeverity::Error,
        .spanSourceMgr = srcMgr,
        .spanBufferId = mainBufferId,
        .spanDisplayPath = result.mainFilePath,
    });
    result.sourceMgr = std::move(srcMgr);
    result.mainBufferId = mainBufferId;
    result.parser = std::move(parser);
    // Capture partial rootScope even if typecheck failed partway through
    result.typeCache = typechecker.takeTypeCache();
    result.rootScope = typechecker.takeRootScope();
    result.specializedTypeEnv = typechecker.takeSpecializedTypeEnv();
    result.specializedTypeToTemplate = typechecker.takeSpecializedTypeToTemplate();
    result.specializedClassTypes = typechecker.takeSpecializedClassTypes();
    result.importAliasToPath = typechecker.takeImportAliasToPath();
    result.importedNameToSource = typechecker.takeImportedNameToSource();
    result.importedModules = typechecker.takeImportedModules();
    result.index =
        buildAnalysisIndex(result.parser != nullptr ? result.parser->getAst() : nullptr,
                           result.sourceMgr.get(), result.mainBufferId, result.mainFilePath);
    return result;
  }
}

auto Driver::baseCompile(std::unique_ptr<lesma::Options> options, bool jit) -> int {
  Timer timer(options->timer);
  std::string outputFilename = options->outputFilename;
  Debug debugFlags = options->debug;
  llvm::OptimizationLevel const optLevel = options->optimizationLevel;
  bool const emitDebugInfo = options->emitDebugInfo;
  bool const emitArcDebug = options->arcDebug || options->arcTrace;
  bool const emitArcTrace = options->arcTrace;
  bool const timerEnabled = options->timer;

  auto result = analyze(std::move(options), timerEnabled ? &timer : nullptr);

  if (result.hasErrors()) {
    for (const auto& d : result.diagnostics) {
      if (d.severity != AnalysisDiagnosticSeverity::Error) {
        continue;
      }
      showAnalysisDiagnostic(result, d, true);
    }
    displayWarnings(result);
    return 1;
  }

  displayWarnings(result);

  try {
    int exitCode = 0;
    {
      auto codegen = timer.measureFile(
          "Compiling", result.mainFilePath, [&]() -> std::unique_ptr<lesma::Codegen> {
            std::vector<std::string> const modules;
            auto cg = std::make_unique<Codegen>(
                std::move(result.parser), result.sourceMgr,
                result.mainFilePath.empty() ? "" : result.mainFilePath, modules, jit, true, "",
                nullptr, nullptr, nullptr, nullptr, nullptr, std::move(result.rootScope),
                std::move(result.typeCache), std::move(result.specializedTypeEnv),
                std::move(result.specializedTypeToTemplate),
                std::move(result.specializedClassTypes), std::move(result.importedModules), &timer,
                emitDebugInfo, emitArcDebug, emitArcTrace, optLevel);
            cg->run();
            return cg;
          });

      timer.measure("Optimizing", [&]() -> void { codegen->optimize(optLevel); });

      if ((debugFlags & Debug::IR) != Debug::NONE) {
        lesma::print(LogType::DEBUG, "LLVM IR (after optimization):\n");
        std::fflush(stdout);
        codegen->dump();
      }

      if (!jit) {
        timer.measure("Writing Object File",
                      [&]() -> void { codegen->writeToObjectFile(outputFilename); });
        timer.measure("Linking Object File", [&]() -> void {
          codegen->linkObjectFile(fmt::format("{}.o", outputFilename));
        });
      } else {
        timer.measure("JIT", [&]() -> void { codegen->prepareJit(); });
        exitCode = timer.measure("Execution", [&]() -> int { return codegen->executeJit(); });
      }
    }
    llvm::llvm_shutdown();
    timer.printReport();
    return exitCode;
  } catch (const LesmaError& err) {
    llvm::llvm_shutdown();
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, "{}", std::string_view(err.what()));
    } else {
      showInline(result.sourceMgr.get(), result.mainBufferId, err.getSpan(), result.mainFilePath,
                 true, err.what());
    }
    return err.getExitCode();
  }
}

auto Driver::run(std::unique_ptr<lesma::Options> options) -> int {
  return baseCompile(std::move(options), true);
}

auto Driver::compile(std::unique_ptr<lesma::Options> options) -> int {
  return baseCompile(std::move(options), false);
}
