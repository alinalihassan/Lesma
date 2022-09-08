#pragma once

#include <string>
#include <sysexits.h>
#include <vector>
#include <optional>

#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Token/Token.h"

namespace lesma {
    class LexerError : public LesmaErrorWithExitCode<EX_DATAERR> {
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Lexer {
    public:
        explicit Lexer(const std::shared_ptr<llvm::SourceMgr> &srcMgr)
            : curBuffer(srcMgr->getMemoryBuffer(srcMgr->getNumBuffers())),
              curPtr(curBuffer->getBufferStart()), begin_loc(llvm::SMLoc::getFromPointer(curPtr)), loc(llvm::SMLoc::getFromPointer(curPtr)), srcMgr(srcMgr) {
        }

        std::vector<Token *> ScanAll();
        Token *ScanOne(bool continuation = false);
        std::vector<Token *> getTokens() { return tokens; };

        void Reset() {
            Lexer tmp(srcMgr);
            std::swap(tmp, *this);
        }

    private:
        bool MatchAndAdvance(char expected);

        char Peek(int offset = 0);

        Token *AddStringToken();

        static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        static bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        static bool IsAlphaNumeric(char c) { return IsAlpha(c) || IsDigit(c); }

        Token *AddNumToken();

        Token *AddToken(TokenType type);
        Token *AddToken(Token *tok);

        void Error(const std::string &msg) const;

        bool IsAtEnd() { return curPtr == curBuffer->getBufferEnd(); }

        char LastChar();

        char Advance();

        Token *GetLastToken();
        Token *AddIdentifierToken();

        void HandleWhitespace(char c);
        bool HandleIndentation(bool continuation);
        void Fallback();

        const llvm::MemoryBuffer *curBuffer;
        const char *curPtr;
        unsigned int line = 1;
        unsigned int col = 1;
        llvm::SMLoc begin_loc;
        llvm::SMLoc loc;
        std::vector<Token *> tokens;
        std::shared_ptr<llvm::SourceMgr> srcMgr;

        std::optional<char> first_indent_char;
        int level_ = 0;
        int indent_ = 0;
        std::vector<int> indent_stack_ = {0};
        std::vector<int> alt_indent_stack_ = {0};

        void ResetTokenBeg();
    };
}// namespace lesma