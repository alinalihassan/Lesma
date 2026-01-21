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
              loc_(llvm::SMLoc::getFromPointer(curBuffer_->getBufferStart())),
              srcMgr_(srcMgr) {
        }
        ~Lexer() = default;

        Lexer(const Lexer &) = delete;
        Lexer &operator=(const Lexer &) = delete;
        Lexer(Lexer &&) = default;
        Lexer &operator=(Lexer &&) = default;

        void scanAll();
        std::vector<Token *> getTokens();
        std::vector<std::unique_ptr<Token>> &getOwnedTokens() { return tokens_; };

    private:
        std::unique_ptr<Token> scanOne(bool continuation = false);

        bool matchAndAdvance(char expected);

        char peek(int offset = 0);

        std::unique_ptr<Token> addStringToken();

        static bool isDigit(char c) { return c >= '0' && c <= '9'; }

        static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        static bool isAlphaNumeric(char c) { return isAlpha(c) || isDigit(c); }

        std::unique_ptr<Token> addNumToken();

        std::unique_ptr<Token> makeToken(TokenType type);
        std::unique_ptr<Token> makeToken(TokenType type, const std::string &value);

        void error(const std::string &msg) const;

        bool isAtEnd() { return curPos_ >= curBuffer_->getBufferSize(); }

        // Helper to get pointer at current position for SMLoc (isolates pointer arithmetic)
        const char *getLocPointer() const {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            return curBuffer_->getBufferStart() + curPos_;
        }

        // Helper to get pointer at specific offset for SMLoc
        const char *getLocPointer(size_t offset) const {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            return curBuffer_->getBufferStart() + offset;
        }

        // Helper to get character at specific position (isolates array subscript)
        char getCharAt(size_t pos) const {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            return curBuffer_->getBufferStart()[pos];
        }

        char lastChar();

        char advance();

        Token *getLastToken();
        std::unique_ptr<Token> addIdentifierToken();

        void handleWhitespace(char c);
        bool handleIndentation(bool continuation);
        void fallback();

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

        void resetTokenBeg();
    };
}// namespace lesma
