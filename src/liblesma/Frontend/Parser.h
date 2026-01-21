#pragma once

#include <string>
#include <utility>
#include <vector>

#include <sysexits.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
    class ParserError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Parser {
    public:
        explicit Parser(std::vector<Token *> tokens) : tokens_(std::move(tokens)) {}
        ~Parser() {
            delete tree_;
        }

        Parser(const Parser &) = delete;
        Parser &operator=(const Parser &) = delete;
        Parser(Parser &&) = delete;
        Parser &operator=(Parser &&) = delete;

        void parse();

        Compound *getAST() { return tree_; }

    private:
        Token *peek() { return peek(0); }
        Token *peek(unsigned long i) { return tokens_.at(index_ + i); }

        Token *consume(TokenType type);
        Token *consume(TokenType type, const std::string &error_message);
        Token *consumeNewline();

        Token *previous() { return peek(-1); }

        bool isAtEnd() { return peek()->type == TokenType::EOF_TOKEN; }

        Token *advance() {
            if (!isAtEnd()) {
                index_++;
            }

            return peek(-1);
        }

        bool check(TokenType type) {
            return check(type, 0);
        }

        bool check(TokenType type, unsigned long pos) {
            return peek(pos)->type == type;
        }

        template<TokenType type, TokenType... remained_types>
        bool advanceIfMatchAny();

        template<TokenType type, TokenType... remained_types>
        bool checkAny();

        template<TokenType type, TokenType... remained_types>
        bool checkAnyInLine();

        template<TokenType type, TokenType... remained_types>
        bool checkAny(unsigned long pos);

        std::vector<Token *> tokens_;
        unsigned long index_ = 0;
        bool in_class_ = false;
        bool is_exported_ = false;
        Compound *tree_ = nullptr;

        static void error(Token *token, const std::string &basicString);

        Compound *parseCompound();
        Compound *parseBlock();
        Statement *parseFunctionDeclaration();
        Statement *parseExport();
        Statement *parseImport();
        Statement *parseClass();
        Statement *parseEnum();
        Statement *parseStatement(bool isTopLevel);
        Statement *parseIf();
        Statement *parseWhile();
        Statement *parseFor();
        Statement *parseVarDecl();
        Statement *parseAssignment();
        Statement *parseBreak();
        Statement *parseContinue();
        Statement *parseReturn();
        Statement *parseDefer();
        TypeExpr *parseType();
        Expression *parseExpression();
        Expression *parseOr();
        Expression *parseAnd();
        Expression *parseNot();
        Expression *parseDot();
        Expression *parseCompare();
        Expression *parseAdd();
        Expression *parseMult();
        Expression *parsePower();
        Expression *parseCast();
        Expression *parseUnary();
        Expression *parseTerm();
        Expression *parseFunctionCall();
    };
}// namespace lesma