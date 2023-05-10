#pragma once

#include <cmath>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "fmt/color.h"
#include "fmt/core.h"

#include "liblesma/Token/TokenType.h"

namespace lesma {
    enum LogType {
        ERROR,
        WARNING,
        DEBUG,
        SUCCESS,
        CLEAR
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
        print(CLEAR, format_str, args...);
    }

    void showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason, const std::string &file, bool is_error);
    void showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason, const std::string &file, bool is_error, const std::string &note);
    std::string getBasename(const std::string &file_path);
    std::string getStdDir();
}// namespace lesma