#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Support/SMLoc.h>

#include "fmt/color.h"
#include "fmt/core.h"
#include "plf_nanotimer.h"

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

    // Timer class for measuring execution time of code blocks
    class Timer {
    private:
        plf::nanotimer timer_;
        double total_ = 0;
        bool enabled_;

    public:
        explicit Timer(bool enabled) : enabled_(enabled) {}

        // Measure execution time of a callable, works with both void and non-void return types
        template<typename F>
        decltype(auto) measure(const std::string &operation, F &&func) {
            timer_.start();

            if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                std::forward<F>(func)();
                double elapsed = timer_.get_elapsed_ms();
                total_ += elapsed;
                if (enabled_) {
                    print(LogType::DEBUG, "{} -> {:.2f} ms\n", operation, elapsed);
                }
            } else {
                auto result = std::forward<F>(func)();
                double elapsed = timer_.get_elapsed_ms();
                total_ += elapsed;
                if (enabled_) {
                    print(LogType::DEBUG, "{} -> {:.2f} ms\n", operation, elapsed);
                }
                return result;
            }
        }

        [[nodiscard]] double total() const { return total_; }

        void printTotal() const {
            if (enabled_) {
                print(LogType::DEBUG, "Total -> {:.2f} ms\n", total_);
            }
        }
    };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason, const std::string &file, bool is_error);
    std::string getBasename(const std::string &file_path);
    std::string getStdDir();
}// namespace lesma