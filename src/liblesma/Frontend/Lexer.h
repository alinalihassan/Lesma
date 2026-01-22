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
    explicit Lexer(const std::shared_ptr<llvm::SourceMgr> &srcMgr)
        : curBuffer_(srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())),
          begin_loc_(llvm::SMLoc::getFromPointer(curBuffer_->getBufferStart())),
          loc_(llvm::SMLoc::getFromPointer(curBuffer_->getBufferStart())), srcMgr_(srcMgr) {}
    ~Lexer() = default;

    Lexer(const Lexer &) = delete;
    Lexer &operator=(const Lexer &) = delete;
    Lexer(Lexer &&) = default;
    Lexer &operator=(Lexer &&) = default;

    auto scanAll() -> void;
    auto getTokens() -> std::vector<Token *>;
    auto getOwnedTokens() -> std::vector<std::unique_ptr<Token>> & { return tokens_; };

private:
    auto scanOne(bool continuation = false) -> std::unique_ptr<Token>;

    auto matchAndAdvance(char expected) -> bool;

    auto peek(int offset = 0) -> char;

    auto addStringToken() -> std::unique_ptr<Token>;

    static auto isDigit(char c) -> bool { return c >= '0' && c <= '9'; }

    static auto isAlpha(char c) -> bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

    static auto isAlphaNumeric(char c) -> bool { return isAlpha(c) || isDigit(c); }

    auto addNumToken() -> std::unique_ptr<Token>;

    auto makeToken(TokenType type) -> std::unique_ptr<Token>;
    auto makeToken(TokenType type, const std::string &value) -> std::unique_ptr<Token>;

    auto error(const std::string &msg) const -> void;

    auto isAtEnd() -> bool { return curPos_ >= curBuffer_->getBufferSize(); }

    // Helper to get pointer at current position for SMLoc (isolates pointer arithmetic)
    auto getLocPointer() const -> const char * {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return curBuffer_->getBufferStart() + curPos_;
    }

    // Helper to get pointer at specific offset for SMLoc
    auto getLocPointer(size_t offset) const -> const char * {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return curBuffer_->getBufferStart() + offset;
    }

    // Helper to get character at specific position (isolates array subscript)
    auto getCharAt(size_t pos) const -> char {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return curBuffer_->getBufferStart()[pos];
    }

    auto lastChar() -> char;

    auto advance() -> char;

    auto getLastToken() -> Token *;
    auto addIdentifierToken() -> std::unique_ptr<Token>;

    auto handleWhitespace(char c) -> void;
    auto handleIndentation(bool continuation) -> bool;
    auto fallback() -> void;

    const llvm::MemoryBuffer *curBuffer_;
    size_t curPos_ = 0;
    unsigned int line_ = 1;
    unsigned int col_ = 1;
    llvm::SMLoc begin_loc_;
    llvm::SMLoc loc_;
    std::vector<std::unique_ptr<Token>> tokens_;
    std::shared_ptr<llvm::SourceMgr> srcMgr_;

    std::optional<char> first_indent_char_;
    int level_ = 0;
    int indent_ = 0;
    std::vector<int> indent_stack_ = {0};
    std::vector<int> alt_indent_stack_ = {0};

    auto resetTokenBeg() -> void;
};
}  // namespace lesma
