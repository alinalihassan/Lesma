#include <vector>

#include "CLI/CLI.hpp"

#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/Driver.h"

using namespace lesma;

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
    // CLI Parsing
    auto options = parseCLI(argc, argv);
    auto *driver_options = new Options{SourceType::FILE, options->file,
            static_cast<Debug>(options->debug ? (LEXER | AST | IR) : NONE), options->output, options->timer};
    return options->jit ? Driver::Run(driver_options) : Driver::Compile(driver_options);
}