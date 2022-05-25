#include <vector>

#include "CLI/CLI.hpp"
#include "plf_nanotimer.h"

#include "liblesma/Backend/Codegen.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

#include "llvm/Support/SourceMgr.h"
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

    return std::make_unique<CLIOptions>(CLIOptions{std::filesystem::absolute(file), output, debug, timer, run->parsed()});
}

int main(int argc, char **argv) {
    // Configure Timer
    plf::nanotimer timer;
    double results, total = 0;

    // Configure Source Manager
    std::shared_ptr<SourceMgr> srcMgr = std::make_shared<SourceMgr>(SourceMgr());

    // CLI Parsing
    TIMEIT("CLI", auto options = parseCLI(argc, argv);)

    try {
        // Read Source
        TIMEIT("File read",
               auto buffer = MemoryBuffer::getFile(options->file);
               if (buffer.getError() != std::error_code()) throw LesmaError(llvm::SMRange(), "Could not read file: {}", options->file);

               srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
               auto source_str = srcMgr->getMemoryBuffer(1)->getBuffer().str();)

        // Lexer
        TIMEIT("Lexer scan",
               auto lexer = std::make_unique<Lexer>(srcMgr);
               lexer->ScanAll();)

        if (options->debug) {
            print(DEBUG, "TOKENS: \n");
            for (const auto &tok: lexer->getTokens())
                print("Token: {}\n", tok->Dump(srcMgr));
        }

        // Parser
        TIMEIT("Parsing",
               auto parser = std::make_unique<Parser>(lexer->getTokens());
               parser->Parse();)

        if (options->debug)
            print(DEBUG, "AST:\n{}", parser->getAST()->toString(srcMgr.get(), 0));

        // Codegen
        TIMEIT("Compiling",
               std::vector<std::string> modules;
               auto codegen = std::make_unique<Codegen>(std::move(parser), srcMgr, options->file, modules, options->jit, true);
               codegen->Run();)

        if (options->debug) {
            print(DEBUG, "LLVM IR: \n");
            codegen->Dump();
        }

        // Optimization
        TIMEIT("Optimizing", codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);)

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
        if (!err.getSpan().isValid())
            print(ERROR, err.what());
        else
            showInline(srcMgr.get(), err.getSpan(), err.what(), options->file, true);
        return err.exit_code;
    }
}