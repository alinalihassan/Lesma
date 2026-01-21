#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Support/SMLoc.h>

#include "fmt/color.h"
#include "fmt/core.h"

namespace lesma {
    enum class LogType : std::uint8_t {
        ERROR,
        WARNING,
        DEBUG,
        SUCCESS,
        CLEAR
    };

    struct CLIOptions {
        std::string file;
        std::string output;
        std::vector<std::string> debug;
        bool timer;
        bool jit;
    };

    template<typename S, typename... Args>
    void print(LogType typ, const S &format_str, const Args &...args) {
        if (typ == LogType::ERROR) {
            fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
        } else if (typ == LogType::WARNING) {
            fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[!] Warning: ");
        } else if (typ == LogType::DEBUG) {
            fmt::print(fg(fmt::color::medium_purple) | fmt::emphasis::bold, "[?] Debug: ");
        } else if (typ == LogType::SUCCESS) {
            fmt::print(fg(fmt::color::forest_green) | fmt::emphasis::bold, "[+] Success: ");
        }

        fmt::print(format_str, args...);
    }

    template<typename S, typename... Args>
    void print(const S &format_str, const Args &...args) {
        print(LogType::CLEAR, format_str, args...);
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason, const std::string &file, bool is_error);
    std::string getBasename(const std::string &file_path);
    std::string getStdDir();
}// namespace lesma