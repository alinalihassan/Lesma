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
#include <pwd.h>
#include <unistd.h>

namespace lesma {
auto getBasename(const std::string &file_path) -> std::string {
    auto filename = file_path.substr(file_path.find_last_of("/\\") + 1);
    auto filenameWoExt = filename.substr(0, filename.find_last_of('.'));

    return filenameWoExt;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason,
                const std::string &file, bool is_error) -> void {
    std::istringstream ifs(srcMgr->getMemoryBuffer(bufferId)->getBuffer().str());
    unsigned int lineNum = 1;
    auto color = is_error ? fg(fmt::color::red) : fg(fmt::color::yellow);
    auto accent = fg(static_cast<fmt::color>(0x008EEA));  // 008EEA

    print(is_error ? LogType::ERROR : LogType::WARNING, "");
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

            // Third line
            fmt::print(accent, "{} |", std::string(int(log10(startLoc.first) + 1), ' '));
            fmt::print(color | fmt::emphasis::bold, "{}{}\n", std::string(startLoc.second, ' '),
                       std::string(endLoc.second - startLoc.second, '^'));

            // TODO: Support multiline
            break;
        }
        lineNum++;
    }
}

auto getStdDir() -> std::string {
    std::string homedir;

    if (getenv("HOME") != nullptr) {
        homedir = getenv("HOME");
    } else {
        homedir = getpwuid(getuid())->pw_dir;
    }

    return fmt::format("{}/.lesma/stdlib/", homedir);
}
}  // namespace lesma