#include "Utils.h"

namespace lesma {
    std::string getBasename(const std::string &file_path) {
        auto filename = file_path.substr(file_path.find_last_of("/\\") + 1);
        auto filename_wo_ext = filename.substr(0, filename.find_last_of('.'));

        return filename_wo_ext;
    }

    void showInline(llvm::SourceMgr *srcMgr, unsigned int bufferId, llvm::SMRange span, const std::string &reason, const std::string &file, bool is_error) {
        std::istringstream ifs(srcMgr->getMemoryBuffer(bufferId)->getBuffer().str());
        unsigned int lineNum = 1;
        auto color = is_error ? fg(fmt::color::red) : fg(fmt::color::yellow);
        auto accent = fg(static_cast<fmt::color>(0x008EEA));// 008EEA

        print(is_error ? ERROR : WARNING, "");
        fmt::print(fmt::emphasis::bold, "{}\n", reason);

        auto start_loc = srcMgr->getLineAndColumn(span.Start, bufferId);
        auto end_loc = srcMgr->getLineAndColumn(span.End, bufferId);
        fmt::print(accent, "{}--> ", std::string(int(log10(start_loc.first) + 1), ' '));
        fmt::print("{}:{}:{}\n", file, start_loc.first, start_loc.second);

        for (std::string line; std::getline(ifs, line);) {
            if (lineNum == start_loc.first) {
                // First line
                fmt::print(accent, "{} |\n", std::string(int(log10(start_loc.first) + 1), ' '));

                // Second line
                fmt::print(accent, "{} | ", lineNum);
                fmt::print("{}\n", line);

                // Third line
                fmt::print(accent, "{} |", std::string(int(log10(start_loc.first) + 1), ' '));
                fmt::print(color | fmt::emphasis::bold, "{}{}\n",
                           std::string(start_loc.second, ' '), std::string(end_loc.second - start_loc.second, '^'));

                // TODO: Support multiline
                break;
            }
            lineNum++;
        }
    }

    std::string getStdDir() {
        std::string homedir;

        if (getenv("HOME"))
            homedir = getenv("HOME");
        else
            homedir = getpwuid(getuid())->pw_dir;

        return homedir + "/.lesma/stdlib/";
    }
}// namespace lesma