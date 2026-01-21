#include "Driver.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>

#include "plf_nanotimer.h"

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

using namespace lesma;

#define TIMEIT(debug_operation, statements)   \
    timer.start();                            \
    statements                                \
            results = timer.get_elapsed_ms(); \
    total += results;                         \
    if (options->timer)                       \
        print(LogType::DEBUG, "{} -> {:.2f} ms\n", debug_operation, results);


int Driver::baseCompile(std::unique_ptr<lesma::Options> options, bool jit) {
    // Configure Timer
    plf::nanotimer timer;
    double results = 0;
    double total = 0;

    // Configure Source Manager
    std::shared_ptr<llvm::SourceMgr> srcMgr = std::make_shared<llvm::SourceMgr>(llvm::SourceMgr());

    try {
        // Read Source
        TIMEIT("File read", if (options->sourceType == SourceType::FILE) {
                auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
                if (!buffer) {
                    throw LesmaError(llvm::SMRange(), "Could not read file: {}", options->source);
                }
                srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc()); } else {
                auto buffer = llvm::MemoryBuffer::getMemBuffer(options->source);
                srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc()); })

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>(srcMgr);
               lexer->ScanAll();)

        if (options->debug & Debug::LEXER) {
            print(LogType::DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens())
                print("Token: {}\n", tok->Dump(srcMgr));
        }

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        if (options->debug & Debug::AST)
            print(LogType::DEBUG, "AST:\n{}", parser->getAST()->toString(srcMgr.get(), "", true));

        // Codegen
        TIMEIT("Compiling",
               std::vector<std::string> modules;
               auto codegen = std::make_unique<Codegen>(std::move(parser), srcMgr,
                                                        options->sourceType == SourceType::FILE ? options->source : "",
                                                        modules, jit, true);
               codegen->Run();)

        if (options->debug & Debug::IR) {
            print(LogType::DEBUG, "LLVM IR: \n");
            codegen->Dump();
        }

        // Optimization
        TIMEIT("Optimizing", codegen->Optimize(OptimizationLevel::O3);)

        int exitCode = 0;
        if (!jit) {
            // Compile to Object File
            TIMEIT("Writing Object File", codegen->WriteToObjectFile(options->output_filename);)

            // Link Object File
            TIMEIT("Linking Object File", codegen->LinkObjectFile(fmt::format("{}.o", options->output_filename));)
        } else {
            // Executing
            TIMEIT("JIT", codegen->PrepareJIT();)
            TIMEIT("Execution", exitCode = codegen->ExecuteJIT();)
        }

        if (options->timer)
            print(LogType::DEBUG, "Total -> {:.2f} ms\n", total);

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

int Driver::run(std::unique_ptr<lesma::Options> options) {
    return baseCompile(std::move(options), true);
}

int Driver::compile(std::unique_ptr<lesma::Options> options) {
    return baseCompile(std::move(options), false);
}
