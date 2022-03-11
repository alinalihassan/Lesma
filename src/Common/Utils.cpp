#include "Utils.h"
#include "LesmaError.h"

namespace lesma {
    std::string readFile(const std::string &path) {
        std::ifstream input_file(path);
        if (!input_file.is_open())
            throw LesmaError({}, "Could not open file: {}", path);

        std::stringstream buffer;
        buffer << input_file.rdbuf();
        return buffer.str();
    }

    std::string getBasename(const std::string &file_path) {
        auto filename = file_path.substr(file_path.find_last_of("/\\") + 1);
        auto filename_wo_ext = filename.substr(0, filename.find_last_of('.'));

        return filename_wo_ext;
    }

    void showInline(Span span, const std::string &reason, const std::string &file, bool is_error) {
        std::ifstream ifs(file);
        std::string line;
        unsigned int lineNum = 1;
        auto color = is_error ? fg(fmt::color::red) : fg(fmt::color::yellow);
        auto accent = fg(static_cast<fmt::color>(0x008EEA));// 008EEA

        print(is_error ? ERROR : WARNING, "");
        fmt::print(fmt::emphasis::bold, "{}\n", reason);

        fmt::print(accent, "{}--> ", std::string(int(log10(span.Start.Line) + 1), ' '));
        fmt::print("{}:{}:{}\n", file, span.Start.Line, span.Start.Col);
        while (std::getline(ifs, line)) {
            if (lineNum == span.Start.Line) {
                // First line
                fmt::print(accent, "{} |\n", std::string(int(log10(span.Start.Line) + 1), ' '));

                // Second line
                fmt::print(accent, "{} | ", lineNum);
                fmt::print("{}\n", line);

                // Third line
                fmt::print(accent, "{} |", std::string(int(log10(span.Start.Line) + 1), ' '));
                fmt::print(color | fmt::emphasis::bold, "{}{}\n",
                           std::string(span.Start.Col, ' '), std::string(span.End.Col - span.Start.Col, '^'));

                // TODO: Support multiline
                break;
            }
            lineNum++;
        }
    }

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
}// namespace lesma