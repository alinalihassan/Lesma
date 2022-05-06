#pragma once

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "Lexer.h"
#include <memory>
#include <utility>

namespace lesma {
    class ParserError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Parser {
    public:
        explicit Parser(std::vector<Token> tokens) : tokens(std::move(tokens)), index(0), tree(nullptr) {}

        void Parse();

        Compound *getAST() { return tree; }

    protected:
        const Token &Peek() { return Peek(0); }
        const Token &Peek(unsigned long i) { return tokens.at(index + i); }

        Token Consume(TokenType type);
        Token Consume(TokenType type, const std::string &error_message);
        Token ConsumeNewline();

        Token Previous() { return Peek(-1); }

        bool IsAtEnd() { return Peek()->type == TokenType::EOF_TOKEN; }

        const Token &Advance() {
            if (!IsAtEnd())
                index++;

            return Peek(-1);
        }

        bool Check(TokenType type) {
            return Check(type, 0);
        }

        bool Check(TokenType type, unsigned long pos) {
            return Peek(pos)->type == type;
        }

        template<TokenType type, TokenType... remained_types>
        bool AdvanceIfMatchAny();

        template<TokenType type, TokenType... remained_types>
        bool CheckAny();

        template<TokenType type, TokenType... remained_types>
        bool CheckAny(unsigned long pos);

        const std::vector<Token> tokens;
        unsigned long index;
        Compound *tree;

        static void Error(const Token &token, const std::string &basicString);

        Compound *ParseCompound();
        Compound *ParseBlock();
        Statement *ParseFunctionDeclaration();
        Statement *ParseImport();
        Statement *ParseStatement();
        Statement *ParseIf();
        Statement *ParseWhile();
        Statement *ParseFor();
        Statement *ParseVarDecl();
        Statement *ParseAssignment();
        Statement *ParseBreak();
        Statement *ParseContinue();
        Statement *ParseReturn();
        Statement *ParseDefer();
        Type *ParseType();
        Expression *ParseExpression();
        Expression *ParseOr();
        Expression *ParseAnd();
        Expression *ParseNot();
        Expression *ParseCompare();
        Expression *ParseAdd();
        Expression *ParseMult();
        Expression *ParseCast();
        Expression *ParseUnary();
        Expression *ParseTerm();
        Expression *ParseFunctionCall();
    };
}// namespace lesma