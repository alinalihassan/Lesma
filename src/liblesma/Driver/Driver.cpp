#include "Driver.h"

#include "plf_nanotimer.h"

using namespace lesma;

#define TIMEIT(debug_operation, statements)   \
    timer.start();                            \
    statements                                \
            results = timer.get_elapsed_ms(); \
    total += results;                         \
    if (options->timer)                       \
        print(DEBUG, "{} -> {:.2f} ms\n", debug_operation, results);


int Driver::BaseCompile(std::unique_ptr<lesma::Options> options, bool jit) {
    // Configure Timer
    plf::nanotimer timer;
    double results, total = 0;

    // Configure Source Manager
    std::shared_ptr<llvm::SourceMgr> srcMgr = std::make_shared<llvm::SourceMgr>(llvm::SourceMgr());

    try {
        // Read Source
        TIMEIT(
                "File read",
                if (options->sourceType == SourceType::FILE) {
                    auto buffer = llvm::MemoryBuffer::getFileAsStream(options->source);
                    if (!buffer) {
                        throw LesmaError(llvm::SMRange(), "Could not read file: {}", options->source);
                    }

                    srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
                } else {
                    auto buffer = llvm::MemoryBuffer::getMemBuffer(options->source);
                    srcMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
                })

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>(srcMgr);
               lexer->ScanAll();)

        if (options->debug & LEXER) {
            print(DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens())
                print("Token: {}\n", tok->Dump(srcMgr));
        }

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        if (options->debug & AST)
            print(DEBUG, "AST:\n{}", parser->getAST()->toString(srcMgr.get(), "", true));

        // Codegen
        TIMEIT("Compiling",
               std::vector<std::string> modules;
               auto codegen = std::make_unique<Codegen>(std::move(parser), srcMgr,
                                                        options->sourceType == FILE ? options->source : "",
                                                        modules, jit, true);
               codegen->Run();)

        if (options->debug & IR) {
            print(DEBUG, "LLVM IR: \n");
            codegen->Dump();
        }

        // Optimization
        TIMEIT("Optimizing", codegen->Optimize(OptimizationLevel::O3);)

        int exit_code = 0;
        if (!jit) {
            // Compile to Object File
            TIMEIT("Writing Object File", codegen->WriteToObjectFile(options->output_filename);)

            // Link Object File
            TIMEIT("Linking Object File", codegen->LinkObjectFile(fmt::format("{}.o", options->output_filename));)
        } else {
            // Executing
            TIMEIT("JIT", codegen->PrepareJIT();)
            TIMEIT("Execution", exit_code = codegen->ExecuteJIT();)
        }

        if (options->timer)
            print(DEBUG, "Total -> {:.2f} ms\n", total);

        return exit_code;
    } catch (const LesmaError &err) {
        if (!err.getSpan().isValid())
            print(ERROR, err.what());
        else
            showInline(srcMgr.get(), 1, err.getSpan(), err.what(), options->sourceType == FILE ? options->source : "", true);
        return err.exit_code;
    }
}

int Driver::Run(std::unique_ptr<lesma::Options> options) {
    return BaseCompile(std::move(options), true);
}

int Driver::Compile(std::unique_ptr<lesma::Options> options) {
    return BaseCompile(std::move(options), false);
}
