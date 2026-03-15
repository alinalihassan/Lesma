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

  auto scanAll() -> void;
  [[nodiscard]] auto getTokens() -> std::vector<Token*>;
  auto getOwnedTokens() -> std::vector<std::unique_ptr<Token>>& {
    return tokens;
  };

private:
  auto scanOne(bool continuation = false) -> std::unique_ptr<Token>;

  auto matchAndAdvance(char expected) -> bool;

  auto peek(int offset = 0) -> char;

  auto addStringToken() -> std::unique_ptr<Token>;

  static auto isDigit(char c) -> bool { return c >= '0' && c <= '9'; }

  static auto isAlpha(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  static auto isAlphaNumeric(char c) -> bool {
    return isAlpha(c) || isDigit(c);
  }

  auto addNumToken() -> std::unique_ptr<Token>;

  auto makeToken(TokenType type) -> std::unique_ptr<Token>;
  auto makeToken(TokenType type,
                 const std::string& value) -> std::unique_ptr<Token>;

  auto error(const std::string& msg) const -> void;

  auto isAtEnd() -> bool { return curPos >= curBuffer->getBufferSize(); }

  // Helper to get pointer at current position for SMLoc (isolates pointer
  // arithmetic)
  [[nodiscard]] auto getLocPointer() const -> const char* {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart() + curPos;
  }

  // Helper to get pointer at specific offset for SMLoc
  [[nodiscard]] auto getLocPointer(size_t offset) const -> const char* {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart() + offset;
  }

  // Helper to get character at specific position (isolates array subscript)
  [[nodiscard]] auto getCharAt(size_t pos) const -> char {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return curBuffer->getBufferStart()[pos];
  }

  auto lastChar() -> char;

  auto advance() -> char;

  auto getLastToken() -> Token*;
  auto addIdentifierToken() -> std::unique_ptr<Token>;

  auto handleWhitespace(char c) -> void;
  auto handleIndentation(bool continuation) -> bool;
  auto fallback() -> void;

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

  auto resetTokenBeg() -> void;
};
} // namespace lesma
