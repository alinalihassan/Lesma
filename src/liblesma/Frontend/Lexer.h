#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include <sysexits.h>

#include "liblesma/Common/LesmaError.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
class LexerError : public LesmaErrorWithExitCode<EX_DATAERR> {
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};

class Lexer {
public:
  explicit Lexer(const std::shared_ptr<llvm::SourceMgr>& srcMgr)
      : curBuffer(srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())),
        beginLoc(llvm::SMLoc::getFromPointer(curBuffer->getBufferStart())),
        loc(llvm::SMLoc::getFromPointer(curBuffer->getBufferStart())),
        srcMgr(srcMgr) {}
  ~Lexer() = default;

  Lexer(const Lexer&) = delete;
  auto operator=(const Lexer&) -> Lexer& = delete;
  Lexer(Lexer&&) = default;
  auto operator=(Lexer&&) -> Lexer& = default;

  auto ScanAll() -> void;
  auto GetTokens() -> std::vector<Token*>;
  auto GetOwnedTokens() -> std::vector<std::unique_ptr<Token>>& {
    return tokens;
  };

private:
  auto ScanOne(bool continuation = false) -> std::unique_ptr<Token>;

  auto MatchAndAdvance(char expected) -> bool;

  auto Peek(int offset = 0) -> char;

  auto AddStringToken() -> std::unique_ptr<Token>;

  static auto IsDigit(char c) -> bool { return c >= '0' && c <= '9'; }

  static auto IsAlpha(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  static auto IsAlphaNumeric(char c) -> bool {
    return IsAlpha(c) || IsDigit(c);
  }

  auto AddNumToken() -> std::unique_ptr<Token>;

  auto MakeToken(TokenType type) -> std::unique_ptr<Token>;
  auto MakeToken(TokenType type, const std::string& value)
      -> std::unique_ptr<Token>;

  auto Error(const std::string& msg) const -> void;

  auto IsAtEnd() -> bool { return curPos >= curBuffer->getBufferSize(); }

  // Helper to get pointer at current position for SMLoc (isolates pointer
  // arithmetic)
  [[nodiscard]] auto GetLocPointer() const -> const char* {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart() + curPos;
  }

  // Helper to get pointer at specific offset for SMLoc
  [[nodiscard]] auto GetLocPointer(size_t offset) const -> const char* {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart() + offset;
  }

  // Helper to get character at specific position (isolates array subscript)
  [[nodiscard]] auto GetCharAt(size_t pos) const -> char {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart()[pos];
  }

  auto LastChar() -> char;

  auto Advance() -> char;

  auto GetLastToken() -> Token*;
  auto AddIdentifierToken() -> std::unique_ptr<Token>;

  auto HandleWhitespace(char c) -> void;
  auto HandleIndentation(bool continuation) -> bool;
  auto Fallback() -> void;

  const llvm::MemoryBuffer* curBuffer;
  size_t curPos = 0;
  unsigned int line = 1;
  unsigned int col = 1;
  llvm::SMLoc beginLoc;
  llvm::SMLoc loc;
  std::vector<std::unique_ptr<Token>> tokens;
  std::shared_ptr<llvm::SourceMgr> srcMgr;

  std::optional<char> firstIndentChar;
  int level = 0;
  int indent = 0;
  std::vector<int> indentStack = {0};
  std::vector<int> altIndentStack = {0};

  auto ResetTokenBeg() -> void;
};
} // namespace lesma
