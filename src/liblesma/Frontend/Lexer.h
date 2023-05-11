#pragma once

#include <optional>
#include <string>
#include <vector>

#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Support/ServiceLocator.h"
#include "liblesma/Token/Token.h"

namespace lesma {
    class Lexer {
    public:
        explicit Lexer() {
            curBuffer = ServiceLocator::getSourceManager().getMemoryBuffer(ServiceLocator::getSourceManager().getNumBuffers());
            curPtr = curBuffer->getBufferStart();
            begin_loc = SMLoc::getFromPointer(curPtr);
            loc = SMLoc::getFromPointer(curPtr);
        }

        ~Lexer() {
            for (auto t: tokens)
                delete t;
            tokens.clear();
        }

        void ScanAll();
        Token *ScanOne(bool continuation = false);
        std::vector<Token *> getTokens() { return tokens; };

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

        const MemoryBuffer *curBuffer;
        const char *curPtr;
        unsigned int line = 1;
        unsigned int col = 1;
        SMLoc begin_loc;
        SMLoc loc;
        std::vector<Token *> tokens;
        std::shared_ptr<SourceManager> srcMgr;

        std::optional<char> first_indent_char;
        int level_ = 0;
        int indent_ = 0;
        std::vector<int> indent_stack_ = {0};
        std::vector<int> alt_indent_stack_ = {0};

        void ResetTokenBeg();
    };
}// namespace lesma