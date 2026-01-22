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

  template <typename S, typename... Args>
  explicit LesmaError(llvm::SMRange span, const S& formatStr,
                      const Args&... args)
      : reason(fmt::format(fmt::runtime(formatStr), args...)), span(span){};
  explicit LesmaError(llvm::SMRange span, std::string what)
      : reason(std::move(what)), span(span) {};

  [[nodiscard]] auto what() const noexcept -> const char* override {
    return reason.c_str();
  }

  [[nodiscard]] auto GetSpan() const -> llvm::SMRange { return span; }

  [[nodiscard]] auto GetExitCode() const -> uint8_t { return exitCode; }

protected:
  template <typename S, typename... Args>
  explicit LesmaError(llvm::SMRange span, uint8_t exitCode, const S& formatStr,
                      const Args&... args)
      : reason(fmt::format(fmt::runtime(formatStr), args...)), span(span),
        exitCode(exitCode){};
  explicit LesmaError(llvm::SMRange span, uint8_t exitCode, std::string what)
      : reason(std::move(what)), span(span), exitCode(exitCode) {};

private:
  std::string reason;
  llvm::SMRange span;
  uint8_t exitCode = static_cast<uint8_t>(
      -1); // if error should cause exit, this should be used.
};

template <uint8_t DEFAULT_EXIT_CODE>
class LesmaErrorWithExitCode : public LesmaError {
public:
  template <typename S, typename... Args>
  explicit LesmaErrorWithExitCode(llvm::SMRange span, const S& formatStr,
                                  const Args&... args)
      : LesmaError(span, DEFAULT_EXIT_CODE,
                   fmt::format(fmt::runtime(formatStr), args...)){};
  explicit LesmaErrorWithExitCode(llvm::SMRange span, const std::string& what)
      : LesmaError(span, DEFAULT_EXIT_CODE, what) {};
  explicit LesmaErrorWithExitCode(llvm::SMRange span, std::string&& what)
      : LesmaError(span, DEFAULT_EXIT_CODE, std::move(what)) {};
};
} // namespace lesma