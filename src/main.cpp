#include <vector>

#include "lib/plf_nanotimer.h"
#include "lib/CLI11.hpp"

#include "Frontend/Lexer.h"
#include "Frontend/Parser.h"
#include "Backend/Codegen.h"

using namespace lesma;

std::string readFile(const std::string& path) {
    std::ifstream input_file(path);
    if (!input_file.is_open())
        throw LesmaError("Could not open file: {}", path);

    std::stringstream buffer;
    buffer << input_file.rdbuf();
    return buffer.str();
}

int main(int argc, char** argv) {
    // Configure Timer
    plf::nanotimer timer;
    double results, total = 0;

    // CLI Parsing
    timer.start();

    bool debug;
    std::string output = "output";
    std::string file;

    CLI::App app{"Lesma"};
    app.set_help_all_flag("--help-all", "Expand all help");
    app.add_flag("-d,--debug", debug, "Enable debug logging");

    CLI::App *run = app.add_subcommand("run", "Run source code");
    CLI::App *compile = app.add_subcommand("compile", "Compile source code");
    app.require_subcommand();

    compile->add_option("-o,--output", output, "Enable debug logging");

    run->allow_extras(true);
    compile->allow_extras(true);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        print(ERROR, e.what());
        return app.exit(e);
    }

    if (app.remaining(true).size() != 1) {
        print(ERROR, "Expected 1 input file");
        exit(EXIT_FAILURE);
    }

    file = app.remaining(true).at(0);

    results = timer.get_elapsed_ms();
    total += results;

    if (debug)
        print(DEBUG, "CLI Parse -> {:.2f} ms\n", results);

    // Read Source
    timer.start();
    auto source = readFile(file);

    results = timer.get_elapsed_ms();
    total += results;

    if (debug)
        print(DEBUG, "File read -> {:.2f} ms\n", results);

    // Lexer
    timer.start();
    auto lexer = std::make_unique<Lexer>(source, "test.les");

    std::vector<Token> tokens;
    try {
        tokens = lexer->ScanAll();

        results = timer.get_elapsed_ms();
        total += results;
        if (debug)
            print(DEBUG, "Lexer scan -> {:.2f} ms\n", results);
    } catch (const LexerError &err) {
        print(ERROR, "{}\n", err.what());
        exit(err.exit_code);
    }

    // Parser
    timer.start();
    auto parser = std::make_unique<Parser>(tokens);

    try {
        parser->Parse();
    } catch (const ParserError &err) {
        print(ERROR, "{}\n", err.what());
        exit(err.exit_code);
    }

    results = timer.get_elapsed_ms();
    total += results;
    if (debug) {
        print(DEBUG, "Parsing -> {:.2f} ms\n", results);
        print(DEBUG, "AST:\n{}", parser->getAST()->toString(0));
    }

    // Codegen
    timer.start();
    auto codegen = std::make_unique<Codegen>(std::move(parser), file);

    try {
        codegen->Run();
    } catch (const CodegenError &err) {
        print(ERROR, "{}\n", err.what());
        exit(err.exit_code);
    }

    results = timer.get_elapsed_ms();
    total += results;
    if (debug) {
        print(DEBUG, "Codegen -> {:.2f} ms\n", results);
        print(DEBUG, "LLVM IR: \n");
        codegen->Dump();
    }

    // Executing
    timer.start();
    int exit_code;

    try {
        if (run->parsed())
            exit_code = codegen->JIT();
        else {
            exit_code = 0;
            codegen->Compile(output);
        }
    } catch (const CodegenError &err) {
        print(ERROR, "{}\n", err.what());
        exit(err.exit_code);
    }

    results = timer.get_elapsed_ms();
    total += results;

    if (debug) {
        print(DEBUG, "Execution -> {:.2f} ms\n", results);
        print(DEBUG, "Total -> {:.2f} ms\n", total);
    }

    return exit_code;
}