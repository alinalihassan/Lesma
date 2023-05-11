#include "Driver.h"

using namespace lesma;

#define TIMEIT(debug_operation, statements)   \
    timer.start();                            \
    statements                                \
            results = timer.get_elapsed_ms(); \
    total += results;                         \
    if (options->timer)                       \
        print(DEBUG, "{} -> {:.2f} ms\n", debug_operation, results);

int Driver::BaseCompile(std::unique_ptr<lesma::Driver::Options> options, bool jit) {
    // Configure Timer
    plf::nanotimer timer;
    double results, total = 0;

    try {
        // Read Source
        TIMEIT(
                "File read",
                if (options->sourceType == SourceType::FILE) {
                    auto buffer = MemoryBuffer::getFileAsStream(options->source);
                    if (!buffer) {
                        ServiceLocator::getDiagnosticManager().emitError({}, fmt::format("Could not read file: {}", options->source));
                        throw LesmaError();
                    }

                    ServiceLocator::getSourceManager().AddNewSourceBuffer(std::move(*buffer), options->source);
                } else {
                    auto buffer = MemoryBuffer::getMemBuffer(options->source);
                    ServiceLocator::getSourceManager().AddNewSourceBuffer(std::move(buffer), "");
                })

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>();
               lexer->ScanAll();)

        if (options->debug & LEXER) {
            print(DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens())
                print("Token: {}\n", tok->Dump());
        }

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        if (options->debug & AST)
            print(DEBUG, "AST:\n{}", parser->getAST()->toString("", true));

        // Codegen
        TIMEIT("Compiling",
               std::vector<std::string> modules;
               auto codegen = std::make_unique<Codegen>(parser->getAST(), modules, jit, true);
               codegen->Run();)

        if (options->debug & IR) {
            print(DEBUG, "LLVM IR: \n");
            codegen->Dump();
        }

        // If we have errors, stop the execution. We don't want to go further
        if (ServiceLocator::getDiagnosticManager().hasErrors()) {
            throw LesmaError();
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
        for (const auto &diag: ServiceLocator::getDiagnosticManager().getDiagnostics()) {
            if (!diag.getLocation().isValid()) {
                print(ERROR, diag.getMessage());
            } else {
                lesma::DiagnosticPrinter::handleDiagnostic(ServiceLocator::getSourceManager(), diag);
            }
        }

        return 1;
    }
}

int Driver::Run(std::unique_ptr<lesma::Driver::Options> options) {
    return BaseCompile(std::move(options), true);
}

int Driver::Compile(std::unique_ptr<lesma::Driver::Options> options) {
    return BaseCompile(std::move(options), false);
}
