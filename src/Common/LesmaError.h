#pragma once

#include <exception>
#include <string>
#include <utility>

#include "fmt/format.h"

namespace lesma {
    class LesmaError : public std::exception {
    public:
        LesmaError() = default;

        template<typename S, typename... Args>
        explicit LesmaError(const S &format_str, const Args &... args) : what_(fmt::format(format_str, args...)) {};
        explicit LesmaError(std::string what) : what_(std::move(what)) {};

        const char *what() const noexcept override {
            return what_.c_str();
        }

        uint8_t exit_code = -1;  // if error should cause exit, this should be used.
    protected:
        mutable std::string what_;
    };

    template<uint8_t DEFAULT_EXIT_CODE>
    class LesmaErrorWithExitCode : public LesmaError {
    public:
        LesmaErrorWithExitCode() = default;

        template<typename S, typename... Args>
        explicit LesmaErrorWithExitCode(const S &format_str, const Args &... args) : LesmaError(fmt::format(format_str, args...)) { exit_code = DEFAULT_EXIT_CODE; };
        explicit LesmaErrorWithExitCode(const std::string &what) : LesmaError(what) { exit_code = DEFAULT_EXIT_CODE; };

        explicit LesmaErrorWithExitCode(std::string &&what) : LesmaError(
                std::move(what)) { exit_code = DEFAULT_EXIT_CODE; };
    };
}