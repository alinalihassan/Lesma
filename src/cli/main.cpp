#include <vector>

#include "CLI/CLI.hpp"

#include "liblesma/Common/LesmaVersion.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/Driver.h"

using namespace lesma;

std::unique_ptr<CLIOptions> parseCLI(int argc, char **argv) {
    bool debug = false;
    bool timer = false;
    std::string output = "output";
    std::string file;

    CLI::App app{"Lesma programming language", "lesma"};
    app.set_version_flag("-v,--version", LESMA_VERSION, "Print the Lesma version");
    app.set_help_all_flag("-s,--subcommands", "Expand help to show subcommand flags and options");
    app.add_flag("-d,--debug", debug, "Enable debug logging");
    app.add_flag("-t,--timer", timer, "Enable compiler timer");

    CLI::App *run = app.add_subcommand("run", "Run source code");
    CLI::App *compile = app.add_subcommand("compile", "Compile source code");
    app.require_subcommand();

    run->add_option("file", file, "Lesma source filename")->required();
    compile->add_option("file", file, "Lesma source filename")->required();
    compile->add_option("-o,--output", output, "Output filename");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        if ((app.get_subcommands().empty() && argc == 1) || (!app.get_subcommands().empty() && argc == 2)) {
            print(app.help());
            exit(0);
        } else {
            exit(app.exit(e));
        }
    }

    return std::make_unique<CLIOptions>(CLIOptions{std::filesystem::absolute(file), output, debug, timer, run->parsed()});
}

auto mapCLIOptionsToDriverOptions(std::unique_ptr<CLIOptions> cliOptions) -> std::unique_ptr<Driver::Options> {
    return std::make_unique<Driver::Options>(
            Driver::Options{Driver::SourceType::FILE,
                            cliOptions->file,
                            static_cast<Driver::Debug>(cliOptions->debug ? (Driver::Debug::LEXER | Driver::Debug::AST | Driver::Debug::IR) : Driver::Debug::NONE),
                            cliOptions->output,
                            cliOptions->timer});
}

int main(int argc, char **argv) {
    // CLI Parsing
    auto cliOptions = parseCLI(argc, argv);
    auto jit = cliOptions->jit;
    auto driverOptions = mapCLIOptionsToDriverOptions(std::move(cliOptions));
    
    return jit ? Driver::Run(std::move(driverOptions)) : Driver::Compile(std::move(driverOptions));
}