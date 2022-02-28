#pragma once

#include <fstream>
#include <sstream>

#include "cxxopts.hpp"
#include "fmt/color.h"
#include "fmt/core.h"

#include "LesmaError.h"
#include "Token/TokenType.h"

namespace lesma {
    struct SourceLocation {
        unsigned int Line;
        unsigned int Col;
    };

    enum LogType {
        ERROR,
        WARNING,
        DEBUG,
        SUCCESS,
        NONE
    };

    struct CLIOptions {
        bool debug;
        std::string file;
    };

    template<typename S, typename... Args>
    void print(LogType typ, const S &format_str, const Args &...args) {
        if (typ == ERROR)
            fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
        else if (typ == WARNING)
            fmt::print(fg(fmt::color::medium_purple) | fmt::emphasis::bold, "[!] Warning: ");
        else if (typ == DEBUG)
            fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[?] Debug: ");
        else if (typ == SUCCESS)
            fmt::print(fg(fmt::color::forest_green) | fmt::emphasis::bold, "[+] Success: ");

        fmt::print(format_str, args...);
    }

    template<typename S, typename... Args>
    void print(const S &format_str, const Args &...args) {
        print(NONE, format_str, args...);
    }

    std::string readFile(const std::string &path);
    CLIOptions *parseCLI(int argc, char **argv);
}// namespace lesma