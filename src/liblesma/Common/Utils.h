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
// CLEAR = no log prefix (plain output); use for version, help, etc.
enum class LogType : std::uint8_t { ERROR, WARNING, DEBUG, SUCCESS, CLEAR };

struct CLIOptions {
  std::string file;
  std::string output;
  std::vector<std::string> debug;
  bool timer;
  bool jit;
  /** Optimization level 0–3 for compile and run. */
  int optimizationLevel = 3;
  /** Emit DWARF when compiling (compile subcommand -g). */
  bool emitDebugInfo = false;
};

template <typename S, typename... Args>
void print(LogType typ, const S& formatStr, const Args&... args) {
  switch (typ) {
  case LogType::ERROR:
    fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
    break;
  case LogType::WARNING:
    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[!] Warning: ");
    break;
  case LogType::DEBUG:
    fmt::print(fg(fmt::color::medium_purple) | fmt::emphasis::bold, "[?] Debug: ");
    break;
  case LogType::SUCCESS:
    fmt::print(fg(fmt::color::forest_green) | fmt::emphasis::bold, "[+] Success: ");
    break;
  case LogType::CLEAR:
    /* no prefix */
    break;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  fmt::print(fmt::runtime(formatStr), args...);
}

template <typename S, typename... Args>
void print(const S& formatStr, const Args&... args) {
  print(LogType::CLEAR, formatStr, args...);
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
  auto measure(const std::string& operation, F&& func) -> decltype(auto) {
    auto recordElapsed = [this, &operation](double elapsed) {
      total += elapsed;
      if (enabled) {
        print(LogType::DEBUG, "{} -> {:.2f} ms\n", operation, elapsed);
      }
    };
    timer.start();
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
      std::forward<F>(func)();
      recordElapsed(timer.get_elapsed_ms());
    } else {
      decltype(auto) result = std::forward<F>(func)();
      recordElapsed(timer.get_elapsed_ms());
      return result;
    }
  }

  [[nodiscard]] auto getTotal() const -> double { return total; }

  auto printTotal() const -> void {
    if (enabled) {
      print(LogType::DEBUG, "Total -> {:.2f} ms\n", total);
    }
  }
};

auto showInline(llvm::SourceMgr* srcMgr, unsigned int bufferId, llvm::SMRange span,
                const std::string& file, bool isError, const std::string& reason) -> void;
auto getBasename(const std::string& filePath) -> std::string;
auto getStdDir() -> std::string;
/** True if \p path resolves under the stdlib root directory ([getStdDir]()). */
[[nodiscard]] auto isStdlibSourcePath(const std::string& path) -> bool;
/** Canonical absolute path for identity (matches import cache / weakly_canonical). */
[[nodiscard]] auto normalizeResolvedFilesystemPath(const std::string& path) -> std::string;
/** Resolved absolute path for an import relative to \p mainModulePath's directory. */
[[nodiscard]] auto normalizeModuleImportPath(const std::string& mainModulePath,
                                             const std::string& importPath) -> std::string;
} // namespace lesma