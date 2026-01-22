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

auto Driver::BaseCompile(std::unique_ptr<lesma::Options> options, bool jit)
    -> int {
  Timer timer(options->timer);

  // Configure Source Manager
  auto srcMgr = std::make_shared<llvm::SourceMgr>();

  try {
    // Read Source
    timer.Measure("File read", [&]() -> void {
      if (options->sourceType == SourceType::FILE) {
        auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
        if (!buffer) {
          throw LesmaError(llvm::SMRange(), "Could not read file: {}",
                           options->source);
        }
        srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
      } else {
        auto buffer = llvm::MemoryBuffer::getMemBuffer(options->source);
        srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
      }
    });

    // Lexer
    auto lexer =
        timer.Measure("Lexer scan", [&]() -> std::unique_ptr<lesma::Lexer> {
          auto lex = std::make_unique<Lexer>(srcMgr);
          lex->ScanAll();
          return lex;
        });

    if ((options->debug & Debug::LEXER) != Debug::NONE) {
      Print(LogType::DEBUG, "TOKENS: \n");
      for (const auto& tok : lexer->GetTokens()) {
        Print("Token: {}\n", tok->Dump(srcMgr));
      }
    }

    // Parser
    auto parser =
        timer.Measure("Parsing", [&]() -> std::unique_ptr<lesma::Parser> {
          auto pars = std::make_unique<Parser>(lexer->GetTokens());
          pars->Parse();
          return pars;
        });

    if ((options->debug & Debug::AST) != Debug::NONE) {
      Print(LogType::DEBUG, "AST:\n{}",
            parser->GetAst()->ToString(srcMgr.get(), "", true));
    }

    // Codegen
    auto codegen =
        timer.Measure("Compiling", [&]() -> std::unique_ptr<lesma::Codegen> {
          std::vector<std::string> const modules;
          auto cg = std::make_unique<Codegen>(
              std::move(parser), srcMgr,
              options->sourceType == SourceType::FILE ? options->source : "",
              modules, jit, true);
          cg->Run();
          return cg;
        });

    if ((options->debug & Debug::IR) != Debug::NONE) {
      Print(LogType::DEBUG, "LLVM IR: \n");
      codegen->Dump();
    }

    // Optimization
    timer.Measure("Optimizing",
                  [&]() -> void { codegen->Optimize(OptimizationLevel::O3); });

    int exitCode = 0;
    if (!jit) {
      // Compile to Object File
      timer.Measure("Writing Object File", [&]() -> void {
        codegen->WriteToObjectFile(options->outputFilename);
      });

      // Link Object File
      timer.Measure("Linking Object File", [&]() -> void {
        codegen->LinkObjectFile(fmt::format("{}.o", options->outputFilename));
      });
    } else {
      // Executing
      timer.Measure("JIT", [&]() -> void { codegen->PrepareJit(); });

      exitCode = timer.Measure("Execution",
                               [&]() -> int { return codegen->ExecuteJit(); });
    }

    timer.PrintTotal();

    return exitCode;
  } catch (const LesmaError& err) {
    if (!err.GetSpan().isValid()) {
      Print(LogType::ERROR, err.what());
    } else {
      ShowInline(srcMgr.get(), 1, err.GetSpan(), err.what(),
                 options->sourceType == SourceType::FILE ? options->source : "",
                 true);
    }

    return err.GetExitCode();
  }
}

auto Driver::Run(std::unique_ptr<lesma::Options> options) -> int {
  return BaseCompile(std::move(options), true);
}

auto Driver::Compile(std::unique_ptr<lesma::Options> options) -> int {
  return BaseCompile(std::move(options), false);
}
