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

auto Driver::baseCompile(std::unique_ptr<lesma::Options> options, bool jit) -> int {
    Timer timer(options->timer);

    // Configure Source Manager
    auto srcMgr = std::make_shared<llvm::SourceMgr>();

    try {
        // Read Source
        timer.measure("File read", [&] {
            if (options->sourceType == SourceType::FILE) {
                auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
                if (!buffer) {
                    throw LesmaError(llvm::SMRange(), "Could not read file: {}", options->source);
                }
                srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
            } else {
                auto buffer = llvm::MemoryBuffer::getMemBuffer(options->source);
                srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
            }
        });

        // Lexer
        auto lexer = timer.measure("Lexer scan", [&] {
            auto lex = std::make_unique<Lexer>(srcMgr);
            lex->scanAll();
            return lex;
        });

        if ((options->debug & Debug::LEXER) != Debug::NONE) {
            print(LogType::DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens()) {
                print("Token: {}\n", tok->dump(srcMgr));
            }
        }

        // Parser
        auto parser = timer.measure("Parsing", [&] {
            auto pars = std::make_unique<Parser>(lexer->getTokens());
            pars->parse();
            return pars;
        });

        if ((options->debug & Debug::AST) != Debug::NONE) {
            print(LogType::DEBUG, "AST:\n{}", parser->getAST()->toString(srcMgr.get(), "", true));
        }

        // Codegen
        auto codegen = timer.measure("Compiling", [&] {
            std::vector<std::string> modules;
            auto cg = std::make_unique<Codegen>(std::move(parser), srcMgr,
                                                options->sourceType == SourceType::FILE ? options->source : "",
                                                modules, jit, true);
            cg->run();
            return cg;
        });

        if ((options->debug & Debug::IR) != Debug::NONE) {
            print(LogType::DEBUG, "LLVM IR: \n");
            codegen->dump();
        }

        // Optimization
        timer.measure("Optimizing", [&] {
            codegen->optimize(OptimizationLevel::O3);
        });

        int exitCode = 0;
        if (!jit) {
            // Compile to Object File
            timer.measure("Writing Object File", [&] {
                codegen->writeToObjectFile(options->output_filename);
            });

            // Link Object File
            timer.measure("Linking Object File", [&] {
                codegen->linkObjectFile(fmt::format("{}.o", options->output_filename));
            });
        } else {
            // Executing
            timer.measure("JIT", [&] {
                codegen->prepareJit();
            });

            exitCode = timer.measure("Execution", [&] {
                return codegen->executeJit();
            });
        }

        timer.printTotal();

        return exitCode;
    } catch (const LesmaError &err) {
        if (!err.getSpan().isValid()) {
            print(LogType::ERROR, err.what());
        } else {
            showInline(srcMgr.get(), 1, err.getSpan(), err.what(), options->sourceType == SourceType::FILE ? options->source : "", true);
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
