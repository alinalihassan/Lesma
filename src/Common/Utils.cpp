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

    CLIOptions *parseCLI(int argc, char **argv) {
        bool debug;
        std::string file;

        cxxopts::Options options("Lesma", "Lesma compiler");
        options.positional_help("[optional args]").show_positional_help();
        // clang-format off
        options.add_options()
                ("h,help", "Print usage")
                ("d,debug", "Enable debugging", cxxopts::value<bool>(debug))
                ;
        // clang-format on
        options.add_options("Hidden")("f,file", "Input Lesma file", cxxopts::value<std::string>(file));

        options.parse_positional({"file"});
        options.positional_help("<file>");

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            print("{}\n", options.help({""}));
            exit(0);
        }

        if (!result.count("file")) {
            print(ERROR, "Expected a file input, print usage with -h or --help for more information\n");
            exit(1);
        }

        return new CLIOptions{debug, file};
    }
}// namespace lesma