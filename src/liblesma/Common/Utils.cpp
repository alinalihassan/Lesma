#include "Utils.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/format.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <pwd.h>
#include <unistd.h>
#endif

namespace lesma {
auto getBasename(const std::string& filePath) -> std::string {
  if (filePath.empty()) {
    return "";
  }
  auto const sep = filePath.find_last_of("/\\");
  auto const filename = (sep == std::string::npos) ? filePath : filePath.substr(sep + 1);
  auto const dot = filename.find_last_of('.');
  return (dot == std::string::npos) ? filename : filename.substr(0, dot);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto showInline(llvm::SourceMgr* srcMgr, unsigned int bufferId, llvm::SMRange span,
                const std::string& file, bool isError, const std::string& reason) -> void {
  std::istringstream ifs(srcMgr->getMemoryBuffer(bufferId)->getBuffer().str());
  unsigned int lineNum = 1;
  auto color = isError ? fg(fmt::color::red) : fg(fmt::color::yellow);
  auto accent = fg(static_cast<fmt::color>(0x008EEA)); // 008EEA

  print(isError ? LogType::ERROR : LogType::WARNING, "");
  fmt::print(fmt::emphasis::bold, "{}\n", reason);

  auto startLoc = srcMgr->getLineAndColumn(span.Start, bufferId);
  auto endLoc = srcMgr->getLineAndColumn(span.End, bufferId);
  fmt::print(accent, "{}--> ", std::string(int(log10(startLoc.first) + 1), ' '));
  fmt::print("{}:{}:{}\n", file, startLoc.first, startLoc.second);

  for (std::string line; std::getline(ifs, line);) {
    if (lineNum == startLoc.first) {
      // First line
      fmt::print(accent, "{} |\n", std::string(int(log10(startLoc.first) + 1), ' '));

      // Second line
      fmt::print(accent, "{} | ", lineNum);
      fmt::print("{}\n", line);

      // Third line: caret width guarded to avoid underflow and huge allocations
      // when end is on a different line or end column < start column
      unsigned int caretWidth = 1;
      if (startLoc.first == endLoc.first && endLoc.second > startLoc.second) {
        caretWidth = endLoc.second - startLoc.second;
      }
      fmt::print(accent, "{} |", std::string(int(log10(startLoc.first) + 1), ' '));
      fmt::print(color | fmt::emphasis::bold, "{}{}\n", std::string(startLoc.second, ' '),
                 std::string(caretWidth, '^'));

      // TODO: Support multiline
      break;
    }
    lineNum++;
  }
}

auto getStdDir() -> std::string {
  std::string homedir;
#if defined(_WIN32) || defined(_WIN64)
  const char* env = getenv("USERPROFILE");
  if (env != nullptr) {
    homedir = env;
  } else {
    env = getenv("HOMEDRIVE");
    const char* path = getenv("HOMEPATH");
    if (env != nullptr && path != nullptr) {
      homedir = std::string(env) + path;
    } else {
      fmt::print(fg(fmt::color::yellow),
                 "Warning: Could not determine home directory, using current "
                 "directory\n");
      homedir = ".";
    }
  }
  return fmt::format("{}\\.lesma\\stdlib\\", homedir);
#else
  if (getenv("HOME") != nullptr) {
    homedir = getenv("HOME");
  } else {
    struct passwd* pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_dir != nullptr) {
      homedir = pw->pw_dir;
    } else {
      fmt::print(fg(fmt::color::yellow),
                 "Warning: Could not determine home directory, using current "
                 "directory\n");
      homedir = ".";
    }
  }
  return fmt::format("{}/.lesma/stdlib/", homedir);
#endif
}
} // namespace lesma