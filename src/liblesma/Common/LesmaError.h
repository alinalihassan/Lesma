#pragma once

#include <exception>
#include <string>
#include <utility>

#include "Utils.h"
#include "fmt/format.h"

namespace lesma {
    class LesmaError : public std::exception {
    public:
        LesmaError() = delete;

        template<typename S, typename... Args>
        explicit LesmaError(llvm::SMRange span, const S &format_str, const Args &...args) : what_(fmt::format(format_str, args...)), span(span){};
        explicit LesmaError(llvm::SMRange span, std::string what) : what_(std::move(what)), span(span){};

        const char *what() const noexcept override {
            return what_.c_str();
        }

        llvm::SMRange getSpan() const {
            return span;
        }

        uint8_t exit_code = -1;// if error should cause exit, this should be used.
    protected:
        mutable std::string what_;
        llvm::SMRange span;
    };

    template<uint8_t DEFAULT_EXIT_CODE>
    class LesmaErrorWithExitCode : public LesmaError {
    public:
        LesmaErrorWithExitCode() = default;

        template<typename S, typename... Args>
        explicit LesmaErrorWithExitCode(llvm::SMRange span, const S &format_str, const Args &...args) : LesmaError(span, fmt::format(format_str, args...)) { exit_code = DEFAULT_EXIT_CODE; };
        explicit LesmaErrorWithExitCode(llvm::SMRange span, const std::string &what) : LesmaError(span, what) { exit_code = DEFAULT_EXIT_CODE; };

        explicit LesmaErrorWithExitCode(llvm::SMRange span, std::string &&what) : LesmaError(span, std::move(what)) { exit_code = DEFAULT_EXIT_CODE; };
    };
}// namespace lesma