#include <vector>

#include "CLI/CLI.hpp"
#include "plf_nanotimer.h"

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

using namespace lesma;

#define TIMEIT(debug_operation, statements)   \
    timer.start();                            \
    statements                                \
            results = timer.get_elapsed_ms(); \
    total += results;                         \
    if (options->timer)                       \
        print(DEBUG, "{} -> {:.2f} ms\n", debug_operation, results);

std::unique_ptr<CLIOptions> parseCLI(int argc, char **argv) {
    bool debug = false;
    bool timer = false;
    std::string output = "output";
    std::string file;

    CLI::App app{"Lesma"};
    app.set_help_all_flag("--help-all", "Expand all help");
    app.add_flag("-d,--debug", debug, "Enable debug logging");
    app.add_flag("-t,--timer", timer, "Enable compiler timer");

    CLI::App *run = app.add_subcommand("run", "Run source code");
    CLI::App *compile = app.add_subcommand("compile", "Compile source code");
    app.require_subcommand();

    run->add_option("file", file, "Lesma source filename");
    compile->add_option("file", file, "Lesma source filename");
    compile->add_option("-o,--output", output, "Output filename");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        exit(app.exit(e));
    }

    return std::make_unique<CLIOptions>(CLIOptions{file, output, debug, timer, run->parsed()});
}

int main(int argc, char **argv) {
    // Configure Timer
    plf::nanotimer timer;
    double results, total = 0;

    // CLI Parsing
    TIMEIT("CLI", auto options = parseCLI(argc, argv);)

    try {
        // Read Source
        TIMEIT("File read", auto source = readFile(options->file);)

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>(source, options->file.substr(options->file.find_last_of("/\\") + 1));
               lexer->ScanAll();)

        if (options->debug) {
            print(DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens())
                print("Token: {}\n", tok->Dump());
        }

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        if (options->debug)
            print(DEBUG, "AST:\n{}", parser->getAST()->toString(0));

        // Codegen
        TIMEIT("Compiling",
               auto codegen = std::make_unique<Codegen>(std::move(parser), options->file, options->jit, true);
               codegen->Run();)

        // Optimization
        TIMEIT("Optimizing", codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);)

        if (options->debug) {
            print(DEBUG, "LLVM IR: \n");
            codegen->Dump();
        }

        int exit_code = 0;
        if (!options->jit) {
            // Compile to Object File
            TIMEIT("Writing Object File", codegen->WriteToObjectFile(options->output);)

            // Link Object File
            TIMEIT("Linking Object File", codegen->LinkObjectFile(fmt::format("{}.o", options->output));)
        } else {
            // Executing
            TIMEIT("Execution", exit_code = codegen->JIT();)
        }

        if (options->timer)
            print(DEBUG, "Total -> {:.2f} ms\n", total);

        return exit_code;
    } catch (const LesmaError &err) {
        if (err.getSpan() == Span{})
            print(ERROR, err.what());
        else
            showInline(err.getSpan(), err.what(), options->file, true);
        return err.exit_code;
    }
}