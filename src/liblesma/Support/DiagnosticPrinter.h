#pragma once

#include "fmt/color.h"
#include "fmt/core.h"
#include "liblesma/Support/Diagnostic.h"
#include "liblesma/Support/SourceManager.h"

namespace lesma {
    class DiagnosticPrinter {
    public:
        static void handleDiagnostic(SourceManager &srcMgr, const Diagnostic &diagnostic) {
            auto bufferID = srcMgr.FindBufferContainingLoc(diagnostic.getLocation().Start);
            auto buffer = srcMgr.getMemoryBuffer(bufferID);
            auto &bufferInfo = srcMgr.getBufferInfo(bufferID);

            auto color = diagnostic.getSeverity() == Diagnostic::Error ? fg(fmt::color::red) : fg(fmt::color::yellow);
            auto accent = fg(static_cast<fmt::color>(0x008EEA));// 008EEA
            unsigned int lineNum = 1;

            if (diagnostic.getSeverity() == Diagnostic::Error) {
                fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[-] Error: ");
            } else if (diagnostic.getSeverity() == Diagnostic::Warning) {
                fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "[!] Warning: ");
            }

            fmt::print(fmt::emphasis::bold, "{}\n", diagnostic.getMessage());

            auto start_loc = srcMgr.getLineAndColumn(diagnostic.getLocation().Start);
            auto end_loc = srcMgr.getLineAndColumn(diagnostic.getLocation().End);
            fmt::print(accent, "{}--> ", std::string(int(log10(start_loc.first) + 1), ' '));
            fmt::print("{}:{}:{}\n", bufferInfo.Filepath, start_loc.first, start_loc.second);

            const char *dataStart = buffer->getBufferStart();
            const char *dataEnd = buffer->getBufferEnd();
            const char *lineStart = dataStart;

            while (lineStart != dataEnd) {
                const char *lineEnd = std::find(lineStart, dataEnd, '\n');

                std::string line(lineStart, lineEnd);

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

                lineStart = lineEnd + 1;// Move to the start of the next line
                lineNum++;
            }
        }
    };
}// namespace lesma