#pragma once

#include <cmath>
#include <fstream>
#include <sstream>

#include "CLI/CLI.hpp"
#include "fmt/color.h"
#include "fmt/core.h"

#include "Token/TokenType.h"

namespace lesma {
    struct SourceLocation {
        unsigned int Line;
        unsigned int Col;

        bool operator==(const SourceLocation &rhs) const {
            return (Line == rhs.Line) && (Col == rhs.Col);
        }
        bool operator!=(const SourceLocation &rhs) const {
            return !operator==(rhs);
        }
    };

    struct Span {
        SourceLocation Start;
        SourceLocation End;

        bool operator==(const Span &rhs) const {
            return (Start == rhs.Start) && (End == rhs.End);
        }
        bool operator!=(const Span &rhs) const {
            return !operator==(rhs);
        }
    };

    enum LogType {
        ERROR,
        WARNING,
        DEBUG,
        SUCCESS,
        NONE
    };

    struct CLIOptions {
        std::string file;
        std::string output;
        bool debug;
        bool timer;
        bool jit;
    };

    template<typename S, typename... Args>
    void print(LogType typ, const S &format_str, const Args &...args) {
        if (typ == ERROR)
            fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
        else if (typ == WARNING)
            fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[!] Warning: ");
        else if (typ == DEBUG)
            fmt::print(fg(fmt::color::medium_purple) | fmt::emphasis::bold, "[?] Debug: ");
        else if (typ == SUCCESS)
            fmt::print(fg(fmt::color::forest_green) | fmt::emphasis::bold, "[+] Success: ");

        fmt::print(format_str, args...);
    }

    template<typename S, typename... Args>
    void print(const S &format_str, const Args &...args) {
        print(NONE, format_str, args...);
    }

    std::string readFile(const std::string &path);
    void showInline(Span span, const std::string &reason, const std::string &file, bool is_error);
    std::string getBasename(const std::string &file_path);
    std::unique_ptr<CLIOptions> parseCLI(int argc, char **argv);
}// namespace lesma