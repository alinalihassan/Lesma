#pragma once

#include <exception>
#include <string>
#include <utility>

#include "Utils.h"

namespace lesma {
    class LesmaError : public std::exception {
    public:
        LesmaError() = delete;

        explicit LesmaError(llvm::SMRange span, std::string what) : what_(std::move(what)), span(span){};
        explicit LesmaError(llvm::SMRange span, std::string what, std::string note) : what_(std::move(what)), note(std::move(note)), span(span){};

        const char *what() const noexcept override {
            return what_.c_str();
        }

        const std::string getNote() const {
            return note;
        }

        llvm::SMRange getSpan() const {
            return span;
        }

        uint8_t exit_code = 1;// if error should cause exit, this should be used.
    protected:
        mutable std::string what_;
        mutable std::string note;
        llvm::SMRange span;
    };
}// namespace lesma