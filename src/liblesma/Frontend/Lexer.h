#pragma once

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
              curPtr_(curBuffer_->getBufferStart()), begin_loc_(llvm::SMLoc::getFromPointer(curPtr_)), loc_(llvm::SMLoc::getFromPointer(curPtr_)), srcMgr_(srcMgr) {
        }
        ~Lexer() {
            for (auto *t: tokens_) {
                delete t;
            }
            tokens_.clear();
        }

        Lexer(const Lexer &) = delete;
        Lexer &operator=(const Lexer &) = delete;
        Lexer(Lexer &&) = default;
        Lexer &operator=(Lexer &&) = default;

        void scanAll();
        Token *scanOne(bool continuation = false);
        std::vector<Token *> getTokens() { return tokens_; };

    private:
        bool matchAndAdvance(char expected);

        char peek(int offset = 0);

        Token *addStringToken();

        static bool isDigit(char c) { return c >= '0' && c <= '9'; }

        static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        static bool isAlphaNumeric(char c) { return isAlpha(c) || isDigit(c); }

        Token *addNumToken();

        Token *addToken(TokenType type);
        Token *addToken(Token *tok);

        void error(const std::string &msg) const;

        bool isAtEnd() { return curPtr_ == curBuffer_->getBufferEnd(); }

        char lastChar();

        char advance();

        Token *getLastToken();
        Token *addIdentifierToken();

        void handleWhitespace(char c);
        bool handleIndentation(bool continuation);
        void fallback();

        const llvm::MemoryBuffer *curBuffer_;
        const char *curPtr_;
        unsigned int line_ = 1;
        unsigned int col_ = 1;
        llvm::SMLoc begin_loc_;
        llvm::SMLoc loc_;
        std::vector<Token *> tokens_;
        std::shared_ptr<llvm::SourceMgr> srcMgr_;

        std::optional<char> first_indent_char_;
        int level_ = 0;
        int indent_ = 0;
        std::vector<int> indent_stack_ = {0};
        std::vector<int> alt_indent_stack_ = {0};

        void resetTokenBeg();
    };
}// namespace lesma