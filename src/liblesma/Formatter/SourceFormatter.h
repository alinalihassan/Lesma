#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "llvm/Support/SourceMgr.h"

#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"

namespace lesma {

struct FormattingError {
  std::string message;
  llvm::SMRange span;
  std::shared_ptr<llvm::SourceMgr> sourceMgr;
  unsigned bufferId = 0;
  std::string filePath;
};

struct FormattingParseResult {
  std::shared_ptr<llvm::SourceMgr> sourceMgr;
  unsigned mainBufferId = 0;
  std::string filePath;
  std::unique_ptr<Lexer> lexer;
  std::unique_ptr<Parser> parser;
};

[[nodiscard]] auto parseFileForFormatting(const std::filesystem::path& path)
    -> std::expected<FormattingParseResult, FormattingError>;
[[nodiscard]] auto formatParsedFile(const FormattingParseResult& parsed, int width) -> std::string;
[[nodiscard]] auto formatFile(const std::filesystem::path& path, int width)
    -> std::expected<std::string, FormattingError>;

} // namespace lesma
