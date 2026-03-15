#include "Driver.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>

#include <fmt/format.h>

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

using namespace lesma;

auto Driver::baseCompile(std::unique_ptr<lesma::Options> options,
                         bool jit) -> int {
  Timer timer(options->timer);

  // Configure Source Manager
  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  unsigned mainBufferId = 0;

  try {
    // Read Source
    timer.measure("File read", [&]() -> void {
      if (options->sourceType == SourceType::FILE) {
        auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
        if (!buffer) {
          throw LesmaError(llvm::SMRange(), "Could not read file: {}",
                           options->source);
        }
        mainBufferId =
            srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
      } else {
        auto buffer = llvm::MemoryBuffer::getMemBuffer(options->source);
        mainBufferId =
            srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
      }
    });

    // Lexer
    auto lexer =
        timer.measure("Lexer scan", [&]() -> std::unique_ptr<lesma::Lexer> {
          auto lex = std::make_unique<Lexer>(srcMgr);
          lex->scanAll();
          return lex;
        });

    if ((options->debug & Debug::LEXER) != Debug::NONE) {
      lesma::print(LogType::DEBUG, "TOKENS: \n");
      for (const auto& tok : lexer->getTokens()) {
        lesma::print("Token: {}\n", tok->dump(srcMgr));
      }
    }

    // Parser
    auto parser =
        timer.measure("Parsing", [&]() -> std::unique_ptr<lesma::Parser> {
          auto pars = std::make_unique<Parser>(lexer->getTokens());
          pars->parse();
          return pars;
        });

    if ((options->debug & Debug::AST) != Debug::NONE) {
      lesma::print(LogType::DEBUG, "AST:\n{}",
                   parser->getAst()->toString(srcMgr.get(), "", true));
    }

    // Codegen
    auto codegen =
        timer.measure("Compiling", [&]() -> std::unique_ptr<lesma::Codegen> {
          // No preloaded modules; Codegen populates importedModules as it
          // compiles each import.
          std::vector<std::string> const modules;
          auto cg = std::make_unique<Codegen>(
              std::move(parser), srcMgr,
              options->sourceType == SourceType::FILE ? options->source : "",
              modules, jit, true);
          cg->run();
          return cg;
        });

    if ((options->debug & Debug::IR) != Debug::NONE) {
      lesma::print(LogType::DEBUG, "LLVM IR: \n");
      codegen->dump();
    }

    // Optimization
    timer.measure("Optimizing",
                  [&]() -> void { codegen->optimize(OptimizationLevel::O3); });

    int exitCode = 0;
    if (!jit) {
      // Compile to Object File
      timer.measure("Writing Object File", [&]() -> void {
        codegen->writeToObjectFile(options->outputFilename);
      });

      // Link Object File
      timer.measure("Linking Object File", [&]() -> void {
        codegen->linkObjectFile(fmt::format("{}.o", options->outputFilename));
      });
    } else {
      // Executing
      timer.measure("JIT", [&]() -> void { codegen->prepareJit(); });

      exitCode = timer.measure("Execution",
                               [&]() -> int { return codegen->executeJit(); });
    }

    timer.printTotal();

    return exitCode;
  } catch (const LesmaError& err) {
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, err.what());
    } else {
      showInline(srcMgr.get(), mainBufferId, err.getSpan(),
                 options->sourceType == SourceType::FILE ? options->source : "",
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
