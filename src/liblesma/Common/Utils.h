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
enum class LogType : std::uint8_t { ERROR, WARNING, DEBUG, SUCCESS, CLEAR };

struct CLIOptions {
  std::string file;
  std::string output;
  std::vector<std::string> debug;
  bool timer;
  bool jit;
};

template <typename S, typename... Args>
void Print(LogType typ, const S& formatStr, const Args&... args) {
  if (typ == LogType::ERROR) {
    fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
  } else if (typ == LogType::WARNING) {
    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[!] Warning: ");
  } else if (typ == LogType::DEBUG) {
    fmt::print(fg(fmt::color::medium_purple) | fmt::emphasis::bold,
               "[?] Debug: ");
  } else if (typ == LogType::SUCCESS) {
    fmt::print(fg(fmt::color::forest_green) | fmt::emphasis::bold,
               "[+] Success: ");
  }

  fmt::print(fmt::runtime(formatStr), args...);
}

template <typename S, typename... Args>
void Print(const S& formatStr, const Args&... args) {
  Print(LogType::CLEAR, formatStr, args...);
}

// Timer class for measuring execution time of code blocks
class Timer {
private:
  plf::nanotimer timer;
  double total = 0;
  bool enabled;

public:
  explicit Timer(bool enabled) : enabled(enabled) {}

  // Measure execution time of a callable, works with both void and non-void
  // return types
  template <typename F>
  auto Measure(const std::string& operation, F&& func) -> decltype(auto) {
    timer.start();

    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
      std::forward<F>(func)();
      double const elapsed = timer.get_elapsed_ms();
      total += elapsed;
      if (enabled) {
        Print(LogType::DEBUG, "{} -> {:.2f} ms\n", operation, elapsed);
      }
    } else {
      auto result = std::forward<F>(func)();
      double const elapsed = timer.get_elapsed_ms();
      total += elapsed;
      if (enabled) {
        Print(LogType::DEBUG, "{} -> {:.2f} ms\n", operation, elapsed);
      }
      return result;
    }
  }

  [[nodiscard]] auto Total() const -> double { return total; }

  auto PrintTotal() const -> void {
    if (enabled) {
      Print(LogType::DEBUG, "Total -> {:.2f} ms\n", total);
    }
  }
};

auto ShowInline(llvm::SourceMgr* srcMgr, unsigned int bufferId,
                llvm::SMRange span, const std::string& file, bool isError,
                const std::string& reason) -> void;
auto GetBasename(const std::string& filePath) -> std::string;
auto GetStdDir() -> std::string;
} // namespace lesma