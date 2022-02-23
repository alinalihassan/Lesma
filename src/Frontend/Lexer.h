#pragma once

#include <sysexits.h>
#include <string>
#include <vector>

#include "Token/Token.h"
#include "Common/LesmaError.h"
#include "Common/Utils.h"

namespace lesma {

    class LexerError : public LesmaErrorWithExitCode<EX_DATAERR> {
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Lexer {
    public:
        explicit Lexer(const std::string &srcs, std::string src_file_name)
                : srcs_(&srcs), src_file_name(std::move(src_file_name)) {}

        std::vector<Token> ScanAll();

        Token ScanOne();

        void Reset() { Reset(*srcs_); }

        void Reset(const std::string &srcs) {
            Lexer tmp(srcs, src_file_name);
            std::swap(tmp, *this);
        }

    private:
        bool MatchAndAdvance(char expected);

        char Peek(int offset = 0);

        Token AddStringToken();

        static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        static bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        static bool IsAlphaNumeric(char c) { return IsAlpha(c) || IsDigit(c); }

        Token AddNumToken();

        Token AddToken(TokenType type);

        void Error(const std::string &msg) const;

        bool IsAtEnd() { return current_lex_pos_ >= srcs_->size(); }

        char LastChar();

        char Advance();

        Token AddIdentifierToken();

        const std::string *srcs_;
        unsigned long start_lex_pos_ = 0;
        unsigned long current_lex_pos_ = 0;
        SourceLocation loc{0, 0};
        std::string src_file_name;

        void ResetTokenBeg();
    };
}