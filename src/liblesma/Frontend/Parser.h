#pragma once

#include <memory>
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
        ~Parser() = default;

        Parser(const Parser &) = delete;
        Parser &operator=(const Parser &) = delete;
        Parser(Parser &&) = delete;
        Parser &operator=(Parser &&) = delete;

        void parse();

        Compound *getAST() { return tree_.get(); }

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
        std::unique_ptr<Compound> tree_;

        static void error(Token *token, const std::string &basicString);

        std::unique_ptr<Compound> parseCompound();
        std::unique_ptr<Compound> parseBlock();
        std::unique_ptr<Statement> parseFunctionDeclaration();
        std::unique_ptr<Statement> parseExport();
        std::unique_ptr<Statement> parseImport();
        std::unique_ptr<Statement> parseClass();
        std::unique_ptr<Statement> parseEnum();
        std::unique_ptr<Statement> parseStatement(bool isTopLevel);
        std::unique_ptr<Statement> parseIf();
        std::unique_ptr<Statement> parseWhile();
        std::unique_ptr<Statement> parseFor();
        std::unique_ptr<Statement> parseVarDecl();
        std::unique_ptr<Statement> parseAssignment();
        std::unique_ptr<Statement> parseBreak();
        std::unique_ptr<Statement> parseContinue();
        std::unique_ptr<Statement> parseReturn();
        std::unique_ptr<Statement> parseDefer();
        std::unique_ptr<TypeExpr> parseType();
        std::unique_ptr<Expression> parseExpression();
        std::unique_ptr<Expression> parseOr();
        std::unique_ptr<Expression> parseAnd();
        std::unique_ptr<Expression> parseNot();
        std::unique_ptr<Expression> parseDot();
        std::unique_ptr<Expression> parseCompare();
        std::unique_ptr<Expression> parseAdd();
        std::unique_ptr<Expression> parseMult();
        std::unique_ptr<Expression> parsePower();
        std::unique_ptr<Expression> parseCast();
        std::unique_ptr<Expression> parseUnary();
        std::unique_ptr<Expression> parseTerm();
        std::unique_ptr<Expression> parseFunctionCall();
    };
}// namespace lesma
