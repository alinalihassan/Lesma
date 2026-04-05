#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include <sysexits.h>

#include "liblesma/Common/LesmaError.h"
#include "liblesma/Driver/AnalysisDiagnostic.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
class LexerError : public LesmaErrorWithExitCode<EX_DATAERR> {
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};

class Lexer {
public:
  explicit Lexer(const std::shared_ptr<llvm::SourceMgr>& srcMgr,
                 std::vector<AnalysisDiagnostic>* diagnosticSink = nullptr,
                 std::string diagnosticFilePath = {})
      : curBuffer(srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())),
        beginLoc(llvm::SMLoc::getFromPointer(curBuffer->getBufferStart())),
        loc(llvm::SMLoc::getFromPointer(curBuffer->getBufferStart())), srcMgr(srcMgr),
        diagnosticSink(diagnosticSink), diagnosticFilePath(std::move(diagnosticFilePath)) {}
  ~Lexer() = default;

  Lexer(const Lexer&) = delete;
  auto operator=(const Lexer&) -> Lexer& = delete;
  Lexer(Lexer&&) = default;
  auto operator=(Lexer&&) -> Lexer& = default;

  auto scanAll() -> void;
  [[nodiscard]] auto getTokens() -> std::vector<Token*>;
  auto getOwnedTokens() -> std::vector<std::unique_ptr<Token>>& { return tokens; };

private:
  auto scanOne(bool continuation = false) -> std::unique_ptr<Token>;

  auto openStringLiteral() -> std::unique_ptr<Token>;
  auto continueTemplateStringChunk() -> std::unique_ptr<Token>;
  enum class StringScanStep { Continue, ClosedQuote, StartInterpolation };
  auto scanStringContentUnit(std::string& acc) -> StringScanStep;
  auto scanLineComment() -> std::unique_ptr<Token>;
  auto scanBlockComment() -> std::unique_ptr<Token>;
  auto advanceWithNewlineTracking() -> char;

  auto matchAndAdvance(char expected) -> bool;

  auto peek(int offset = 0) -> char;

  static auto isDigit(char c) -> bool { return c >= '0' && c <= '9'; }

  static auto isAlpha(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  static auto isAlphaNumeric(char c) -> bool { return isAlpha(c) || isDigit(c); }

  auto addNumToken() -> std::unique_ptr<Token>;

  auto makeToken(TokenType type) -> std::unique_ptr<Token>;
  auto makeToken(TokenType type, const std::string& value) -> std::unique_ptr<Token>;

  [[nodiscard]] auto currentSpan() const -> llvm::SMRange { return llvm::SMRange{beginLoc, loc}; }

  auto lexError(llvm::SMRange span, const std::string& msg) -> void;

  /** Consume until after the next `\n`, or EOF. Updates line/col. */
  auto skipRestOfPhysicalLine() -> void;

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

  /** Returns false if the caller should immediately `return scanOne(continuation)`. */
  auto handleWhitespace(char c) -> bool;
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
  std::vector<AnalysisDiagnostic>* diagnosticSink = nullptr;
  std::string diagnosticFilePath;

  std::optional<char> firstIndentChar;
  int level = 0;

  std::deque<std::unique_ptr<Token>> pendingTokens;
  /** Nested `${ ... }`; incremented on `${`, decremented on closing `}`. */
  int templateInterpolationDepth = 0;
  bool resumeTemplateStringChunk = false;

  auto resetTokenBeg() -> void;
};
} // namespace lesma
