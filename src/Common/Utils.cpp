#include "Utils.h"

namespace lesma {
    std::string readFile(const std::string &path) {
        std::ifstream input_file(path);
        if (!input_file.is_open())
            throw LesmaError("Could not open file: {}", path);

        std::stringstream buffer;
        buffer << input_file.rdbuf();
        return buffer.str();
    }

    std::string getBasename(const std::string &file_path) {
        auto filename = file_path.substr(file_path.find_last_of("/\\") + 1);
        auto filename_wo_ext = filename.substr(0, filename.find_last_of('.'));

        return filename_wo_ext;
    }

    CLIOptions *parseCLI(int argc, char **argv) {
        bool debug;
        std::string output = "output";
        std::string file;

        CLI::App app{"Lesma"};
        app.set_help_all_flag("--help-all", "Expand all help");
        app.add_flag("-d,--debug", debug, "Enable debug logging");

        CLI::App *run = app.add_subcommand("run", "Run source code");
        CLI::App *compile = app.add_subcommand("compile", "Compile source code");
        app.require_subcommand();

        compile->add_option("-o,--output", output, "Output filename");

        run->allow_extras(true);
        compile->allow_extras(true);

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError &e) {
            print(ERROR, e.what());
            exit(app.exit(e));
        }

        if (app.remaining(true).size() != 1) {
            print(ERROR, "Expected 1 input file");
            exit(EXIT_FAILURE);
        }

        file = app.remaining(true).at(0);
        return new CLIOptions{file, output, debug, run->parsed()};
    }
}// namespace lesma