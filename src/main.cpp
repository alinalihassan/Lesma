#include <vector>

#include "lib/plf_nanotimer.h"

#include "Backend/Codegen.h"
#include "Common/Utils.h"
#include "Frontend/Lexer.h"
#include "Frontend/Parser.h"

using namespace lesma;

#define TIMEIT(debug_operation, statements)   \
    timer.start();                            \
    statements                                \
            results = timer.get_elapsed_ms(); \
    total += results;                         \
    if (options->debug)                       \
        print(DEBUG, "{} -> {:.2f} ms\n", debug_operation, results);

int main(int argc, char **argv) {
    try {
        // Configure Timer
        plf::nanotimer timer;
        double results, total = 0;

        // CLI Parsing
        TIMEIT("CLI", auto options = parseCLI(argc, argv);)

        // Read Source
        TIMEIT("File read", auto source = readFile(options->file);)

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>(source, options->file.substr(options->file.find_last_of("/\\") + 1));
               lexer->ScanAll();)

        //        print(DEBUG, "TOKENS: \n");
        //        for (const auto& tok: lexer->getTokens())
        //            print("Token: {}\n", tok->Dump());

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        //        print(DEBUG, "AST:\n{}", parser->getAST()->toString(0));

        // Codegen
        TIMEIT("Compiling",
               auto codegen = std::make_unique<Codegen>(std::move(parser), options->file);
               codegen->Run();)

        // Optimization
        TIMEIT("Optimizing", codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);)

        //        print(DEBUG, "LLVM IR: \n");
        //        codegen->Dump();

        // Executing
        TIMEIT("Execution",
               int exit_code = 0;
               if (options->jit)
                       exit_code = codegen->JIT(std::move(codegen->Modules));
               else codegen->Compile(options->output);)

        if (options->debug)
            print(DEBUG, "Total -> {:.2f} ms\n", total);

        return exit_code;
    } catch (const LesmaError &err) {
        print(ERROR, "{}\n", err.what());
        return err.exit_code;
    }
}