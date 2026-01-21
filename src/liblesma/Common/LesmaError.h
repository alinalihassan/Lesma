#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include <llvm/Support/SMLoc.h>

#include "fmt/format.h"

namespace lesma {
    class LesmaError : public std::exception {
    public:
        LesmaError() = delete;

        template<typename S, typename... Args>
        explicit LesmaError(llvm::SMRange span, const S &formatStr, const Args &...args) : what_(fmt::format(formatStr, args...)), span_(span){};
        explicit LesmaError(llvm::SMRange span, std::string what) : what_(std::move(what)), span_(span) {};

        [[nodiscard]] const char *what() const noexcept override {
            return what_.c_str();
        }

        [[nodiscard]] llvm::SMRange getSpan() const {
            return span_;
        }

        [[nodiscard]] uint8_t getExitCode() const {
            return exit_code_;
        }

    protected:
        template<typename S, typename... Args>
        explicit LesmaError(llvm::SMRange span, uint8_t exitCode, const S &formatStr, const Args &...args)
            : what_(fmt::format(formatStr, args...)), span_(span), exit_code_(exitCode){};
        explicit LesmaError(llvm::SMRange span, uint8_t exitCode, std::string what)
            : what_(std::move(what)), span_(span), exit_code_(exitCode) {};

    private:
        std::string what_;
        llvm::SMRange span_;
        uint8_t exit_code_ = static_cast<uint8_t>(-1);// if error should cause exit, this should be used.
    };

    template<uint8_t DEFAULT_EXIT_CODE>
    class LesmaErrorWithExitCode : public LesmaError {
    public:
        template<typename S, typename... Args>
        explicit LesmaErrorWithExitCode(llvm::SMRange span, const S &format_str, const Args &...args)
            : LesmaError(span, DEFAULT_EXIT_CODE, fmt::format(format_str, args...)){};
        explicit LesmaErrorWithExitCode(llvm::SMRange span, const std::string &what)
            : LesmaError(span, DEFAULT_EXIT_CODE, what) {};
        explicit LesmaErrorWithExitCode(llvm::SMRange span, std::string &&what)
            : LesmaError(span, DEFAULT_EXIT_CODE, std::move(what)) {};
    };
}// namespace lesma