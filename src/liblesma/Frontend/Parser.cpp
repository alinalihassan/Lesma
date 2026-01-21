#include "Parser.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/SMLoc.h>

#include "fmt/core.h"
#include "nameof.hpp"
#include <fmt/format.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

template<TokenType type, TokenType... remained_types>
bool Parser::advanceIfMatchAny() {
    if (checkAny<type, remained_types...>()) {
        advance();
        return true;
    }

    return false;
}

template<TokenType type, TokenType... remained_types>
bool Parser::checkAny() {
    return checkAny<type, remained_types...>(0);
}

template<TokenType type, TokenType... remained_types>
bool Parser::checkAnyInLine() {
    int i = 0;
    while (!checkAny<TokenType::NEWLINE, TokenType::EOF_TOKEN>(i)) {
        if (checkAny<type, remained_types...>(i)) {
            return true;
        }
        i++;
    }

    return false;
}

template<TokenType type, TokenType... remained_types>
bool Parser::checkAny(unsigned long pos) {
    if (!check(type, pos)) {
        if constexpr (sizeof...(remained_types) > 0) {
            return checkAny<remained_types...>(pos);
        } else {
            return false;
        }
    }
    return true;
}

Token *Parser::consume(TokenType type) {
    return consume(type, std::string{"Expected: "} + std::string{NAMEOF_ENUM(type)} +
                                 ", found: " + std::string{NAMEOF_ENUM(peek()->type)});
}

Token *Parser::consume(TokenType type, const std::string &error_message) {
    if (check(type)) {
        return advance();
    }
    error(peek(), error_message);
    return new Token{};
}

Token *Parser::consumeNewline() {
    if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN) {
        return advance();
    }
    error(peek(), fmt::format("Expected: NEWLINE or EOF, found: {}", NAMEOF_ENUM(peek()->type)));
    return new Token{};
}

void Parser::error(Token *token, const std::string &error_message) {
    throw ParserError(token->span, "{}", error_message);
}

TypeExpr *Parser::parseType() {
    auto *type = peek();
    if (check(TokenType::STAR)) {
        advance();
        auto *elementType = parseType();
        return new TypeExpr({type->getStart(), elementType->getEnd()}, "*" + elementType->getName(), TokenType::PTR_TYPE, elementType);
    }
    if (checkAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE, TokenType::BOOL_TYPE,
                 TokenType::INT8_TYPE, TokenType::INT16_TYPE, TokenType::INT32_TYPE, TokenType::FLOAT32_TYPE, TokenType::VOID_TYPE>()) {
        advance();
        return new TypeExpr(type->span, type->lexeme, type->type);
    }
    if (check(TokenType::FUNC)) {
        std::vector<TypeExpr *> params;
        TypeExpr *ret = nullptr;
        std::string lexeme = type->lexeme + " (";

        advance();
        consume(TokenType::LEFT_PAREN);
        do {
            if (!params.empty()) {
                lexeme += ", ";
            }
            params.push_back(parseType());
            lexeme += params.back()->getName();
        } while (advanceIfMatchAny<TokenType::COMMA>());

        consume(TokenType::RIGHT_PAREN);
        lexeme += ")";

        if (advanceIfMatchAny<TokenType::ARROW>()) {
            ret = parseType();
            lexeme += " -> " + ret->getName();
        } else {
            ret = new TypeExpr({params.back()->getEnd(), params.back()->getEnd()}, type->lexeme, type->type);
        }

        // TODO: This should really be a pointer to a function type
        return new TypeExpr({type->getStart(), ret->getEnd()}, lexeme, TokenType::FUNC_TYPE, params, ret);
    }

    if (check(TokenType::IDENTIFIER)) {
        advance();
        return new TypeExpr(type->span, type->lexeme, TokenType::CUSTOM_TYPE);
    }

    error(type, fmt::format("Unknown type: {}", type->lexeme));

    return nullptr;
}

// Expression
Expression *Parser::parseFunctionCall() {
    auto *token = peek();
    consume(TokenType::IDENTIFIER);
    consume(TokenType::LEFT_PAREN);

    std::vector<Expression *> params;

    while (!check(TokenType::RIGHT_PAREN)) {
        auto *param = parseExpression();

        if (!check(TokenType::RIGHT_PAREN)) {
            consume(TokenType::COMMA);
        }

        params.push_back(param);
    }

    auto *paren = consume(TokenType::RIGHT_PAREN);

    return new FuncCall({token->getStart(), paren->span.End}, token->lexeme, params);
}

Expression *Parser::parseTerm() {
    switch (peek()->type) {
        case TokenType::STRING:
        case TokenType::INTEGER:
        case TokenType::DOUBLE:
        case TokenType::NIL: {
            auto *token = peek();
            consume(token->type);
            return new Literal(token->span, token->lexeme, token->type);
        }
        case TokenType::IDENTIFIER: {
            if (checkAny<TokenType::LEFT_PAREN>(1)) {
                return parseFunctionCall();
            }

            auto *token = peek();
            consume(token->type);
            return new Literal(token->span, token->lexeme, token->type);
        }
        case TokenType::LEFT_PAREN: {
            consume(TokenType::LEFT_PAREN);
            auto *expr = parseExpression();
            consume(TokenType::RIGHT_PAREN);
            return expr;
        }
        case TokenType::TRUE_:
        case TokenType::FALSE_: {
            auto *token = peek();
            consume(token->type);
            return new Literal(token->span, token->lexeme, TokenType::BOOL);
        }
        default:
            error(peek(), fmt::format("Unknown literal: {}", peek()->lexeme));
    }

    return nullptr;
}

Expression *Parser::parseDot() {
    Expression *left = parseTerm();

    while (advanceIfMatchAny<TokenType::DOT>()) {
        auto *op = previous();
        auto *expr = parseTerm();
        left = new DotOp({left->getStart(), expr->getEnd()}, left, op->type, expr);
    }

    return left;
}

Expression *Parser::parseUnary() {
    Expression *left = nullptr;
    while (advanceIfMatchAny<TokenType::MINUS, TokenType::STAR, TokenType::AMPERSAND>()) {
        auto *op = previous();
        auto *expr = parseDot();
        left = new UnaryOp({op->getStart(), expr->getEnd()}, op->type, expr);
    }

    if (left == nullptr) {
        delete left;
        return parseDot();
    }

    return left;
}

Expression *Parser::parseCast() {
    auto *left = parseUnary();
    while (advanceIfMatchAny<TokenType::AS>()) {
        auto *type = parseType();
        left = new CastOp({left->getStart(), type->getEnd()}, left, type);
    }
    return left;
}

Expression *Parser::parseMult() {
    auto *left = parsePower();
    while (advanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
        auto op = previous()->type;
        auto *right = parsePower();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::parsePower() {
    auto *left = parseCast();
    while (advanceIfMatchAny<TokenType::POWER>()) {
        auto op = previous()->type;
        auto *right = parseCast();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::parseAdd() {
    auto *left = parseMult();
    while (advanceIfMatchAny<TokenType::PLUS, TokenType::MINUS>()) {
        auto op = previous()->type;
        auto *right = parseMult();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::parseCompare() {
    auto *left = parseAdd();
    while (advanceIfMatchAny<TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL, TokenType::LESS, TokenType::LESS_EQUAL,
                             TokenType::GREATER, TokenType::GREATER_EQUAL, TokenType::IS, TokenType::IS_NOT>()) {
        auto op = previous()->type;
        if (op == TokenType::IS || op == TokenType::IS_NOT) {
            auto *right = parseType();
            left = new IsOp({left->getStart(), right->getEnd()}, left, op, right);
        } else {
            auto *right = parseAdd();
            left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
        }
    }
    return left;
}

Expression *Parser::parseNot() {
    Expression *left = nullptr;
    while (advanceIfMatchAny<TokenType::NOT>()) {
        auto *op = previous();
        auto *expr = parseCompare();
        left = new UnaryOp({expr->getStart(), op->getEnd()}, TokenType::NOT, expr);
    }

    if (left == nullptr) {
        delete left;
        return parseCompare();
    }

    return left;
}

Expression *Parser::parseAnd() {
    auto *left = parseNot();
    while (advanceIfMatchAny<TokenType::AND>()) {
        auto *right = parseNot();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, TokenType::AND, right);
    }
    return left;
}

Expression *Parser::parseOr() {
    auto *left = parseAnd();
    while (advanceIfMatchAny<TokenType::OR>()) {
        auto *right = parseAnd();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, TokenType::OR, right);
    }
    return left;
}

Expression *Parser::parseExpression() {
    return parseOr();
}

// Statements
Statement *Parser::parseVarDecl() {
    bool mutable_ = false;
    Token *startTok = nullptr;
    if (advanceIfMatchAny<TokenType::LET>()) {
        startTok = previous();
        mutable_ = false;
    } else {
        startTok = consume(TokenType::VAR);
        mutable_ = true;
    }
    auto *identifier = consume(TokenType::IDENTIFIER);
    auto *var = new Literal(identifier->span, identifier->lexeme, identifier->type);

    std::optional<TypeExpr *> type = std::nullopt;
    if (advanceIfMatchAny<TokenType::COLON>())
        type = parseType();

    std::optional<Expression *> expr = std::nullopt;
    if (advanceIfMatchAny<TokenType::EQUAL>()) {
        expr = parseExpression();
    }

    if (type == std::nullopt && expr == std::nullopt) {
        throw ParserError(llvm::SMRange{startTok->getStart(), var->getEnd()}, "Expected either a type or a value");
    }

    if (expr == std::nullopt && !mutable_) {
        throw ParserError(llvm::SMRange{startTok->getStart(), type.value()->getEnd()}, "Cannot declare an immutable variable without an initial expression");
    }

    consumeNewline();
    return new VarDecl({startTok->getStart(), expr != std::nullopt ? expr.value()->getEnd() : type.value()->getEnd()}, var, type, expr, mutable_);
}

Statement *Parser::parseIf() {
    auto loc = peek()->span;
    consume(TokenType::IF);

    auto conds = std::vector<Expression *>();
    auto blocks = std::vector<Compound *>();

    // Read first If cond and block
    conds.push_back(parseExpression());
    blocks.push_back(parseBlock());

    while (advanceIfMatchAny<TokenType::ELSE_IF>()) {
        conds.push_back(parseExpression());
        blocks.push_back(parseBlock());
    }
    if (advanceIfMatchAny<TokenType::ELSE>()) {
        conds.push_back(new Else(peek()->span));
        blocks.push_back(parseBlock());
    }

    return new If({loc.Start, blocks.back()->getEnd()}, conds, blocks);
}

Statement *Parser::parseWhile() {
    auto loc = peek()->span;
    consume(TokenType::WHILE);

    auto *cond = parseExpression();
    auto *block = parseBlock();

    return new While({loc.Start, block->getEnd()}, cond, block);
}

Statement *Parser::parseFor() {
    error(peek(), "Unimplemented");

    return nullptr;
}

Statement *Parser::parseAssignment() {

    auto *identifier = parseDot();

    if (((dynamic_cast<Literal *>(identifier) == nullptr) || dynamic_cast<Literal *>(identifier)->getType() != TokenType::IDENTIFIER) && (dynamic_cast<DotOp *>(identifier) == nullptr)) {
        throw ParserError(identifier->getSpan(), "Expected either identifier or class field for assignment");
    }

    if (advanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                          TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>()) {
        auto op = previous()->type;
        auto *expr = parseExpression();

        consumeNewline();
        return new Assignment({identifier->getStart(), expr->getEnd()}, identifier, op, expr);
    }

    error(peek(), fmt::format("Unsupported assignment operator: {}", peek()->lexeme));

    return nullptr;
}

Statement *Parser::parseBreak() {
    auto *tok = reinterpret_cast<Statement *>(new Break(consume(TokenType::BREAK)->span));
    consumeNewline();
    return tok;
}

Statement *Parser::parseContinue() {
    auto *tok = reinterpret_cast<Statement *>(new Continue(consume(TokenType::CONTINUE)->span));
    consumeNewline();
    return tok;
}

Statement *Parser::parseReturn() {
    auto loc = peek()->span;
    consume(TokenType::RETURN);
    if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN) {
        consumeNewline();
        return reinterpret_cast<Statement *>(new Return(loc, nullptr));
    }
    auto *val = parseExpression();
    consumeNewline();
    return reinterpret_cast<Statement *>(new Return({loc.Start, val->getEnd()}, val));
}

Statement *Parser::parseDefer() {
    auto loc = peek()->span;
    consume(TokenType::DEFER);
    auto *val = parseStatement(false);

    //Don't consume newline, since statement will
    return reinterpret_cast<Statement *>(new Defer({loc.Start, val->getEnd()}, val));
}

Statement *Parser::parseStatement(bool isTopLevel) {
    if (checkAny<TokenType::DEF, TokenType::IMPORT, TokenType::CLASS, TokenType::ENUM, TokenType::EXPORT>() && !isTopLevel) {
        error(peek(), "Statement not allowed inside a block");
    }

    if (check(TokenType::DEF)) {
        return parseFunctionDeclaration();
    }
    if (checkAny<TokenType::IMPORT, TokenType::FROM>()) {
        return parseImport();
    }
    if (check(TokenType::CLASS)) {
        return parseClass();
    }
    if (check(TokenType::ENUM)) {
        return parseEnum();
    }
    if (check(TokenType::EXPORT)) {
        return parseExport();
    }
    if (check(TokenType::LET) || check(TokenType::VAR)) {
        return parseVarDecl();
    }
    if (check(TokenType::IF)) {
        return parseIf();
    }
    if (check(TokenType::WHILE)) {
        return parseWhile();
    }
    if (check(TokenType::FOR)) {
        return parseFor();
    }
    if (check(TokenType::BREAK)) {
        return parseBreak();
    }
    if (check(TokenType::CONTINUE)) {
        return parseContinue();
    }
    if (check(TokenType::RETURN)) {
        return parseReturn();
    }
    if (check(TokenType::DEFER)) {
        return parseDefer();
    }
    if (checkAnyInLine<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                       TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>()) {
        return parseAssignment();
    }

    // Check if it's just an expression statement
    auto *expr = parseExpression();
    if (expr != nullptr) {
        consumeNewline();
        return new ExpressionStatement(expr->getSpan(), expr);
    }

    error(peek(), "Unknown statement");
    return nullptr;
}

Compound *Parser::parseBlock() {
    std::vector<Statement *> statements;

    consume(TokenType::NEWLINE);
    consume(TokenType::INDENT);

    while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
        statements.push_back(parseStatement(false));
    }

    advanceIfMatchAny<TokenType::DEDENT>();

    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

Statement *Parser::parseFunctionDeclaration() {
    auto loc = is_exported_ ? previous()->span : peek()->span;
    consume(TokenType::DEF);
    bool externFunc = false;

    if (advanceIfMatchAny<TokenType::EXTERN>()) {
        externFunc = true;
    }

    if (externFunc && in_class_) {
        error(previous(), "Extern functions are not allowed in class definition.");
        return nullptr;
    }

    auto *identifier = consume(TokenType::IDENTIFIER);

    // Parse parameters
    consume(TokenType::LEFT_PAREN);
    std::vector<Parameter *> parameters;

    bool varargs = false;
    while (!check(TokenType::RIGHT_PAREN)) {
        if (varargs) {
            error(peek(), "Varargs should be the last parameter");
        }

        if (check(TokenType::ELLIPSIS) && externFunc) {
            consume(TokenType::ELLIPSIS);
            varargs = true;
        } else {
            Expression *defaultVal = nullptr;
            TypeExpr *type = nullptr;
            auto paramIdent = consume(TokenType::IDENTIFIER);

            if (advanceIfMatchAny<TokenType::COLON>()) {
                type = parseType();
            }

            if (advanceIfMatchAny<TokenType::EQUAL>()) {
                defaultVal = parseExpression();
            }

            if (defaultVal == nullptr && type == nullptr) {
                throw ParserError(paramIdent->span, "{} should have either a type, a value or both specified", paramIdent->lexeme);
            }

            parameters.emplace_back(new Parameter(paramIdent->lexeme, type, false, defaultVal));
        }

        if (!check(TokenType::RIGHT_PAREN) && !check(TokenType::RIGHT_PAREN, 1)) {
            consume(TokenType::COMMA);
        }
    }

    consume(TokenType::RIGHT_PAREN);

    TypeExpr *returnType = nullptr;
    if (advanceIfMatchAny<TokenType::ARROW>()) {
        returnType = parseType();
    } else {
        returnType = new TypeExpr(previous()->span, "void", TokenType::VOID_TYPE);
    }

    if (externFunc) {
        consumeNewline();
        return new ExternFuncDecl({loc.Start, returnType->getEnd()}, identifier->lexeme, returnType, parameters, varargs, is_exported_);
    }

    auto *body = parseBlock();

    return new FuncDecl({loc.Start, returnType->getEnd()}, identifier->lexeme, returnType, parameters, body, false, is_exported_);
}

Statement *Parser::parseExport() {
    consume(TokenType::EXPORT);

    if (in_class_) {
        error(peek(), "Cannot export class members");
    }

    if (!checkAny<TokenType::DEF, TokenType::CLASS, TokenType::ENUM>()) {
        error(peek(), "Can only export functions, classes and enums");
    }

    is_exported_ = true;
    Statement *statement = nullptr;
    if (check(TokenType::DEF)) {
        statement = parseFunctionDeclaration();
    } else if (check(TokenType::IMPORT)) {
        statement = parseImport();
    } else if (check(TokenType::CLASS)) {
        statement = parseClass();
    } else if (check(TokenType::ENUM)) {
        statement = parseEnum();
    } else {
        error(peek(), "Unexpected statement after export");
    }

    is_exported_ = false;
    return statement;
}

Statement *Parser::parseImport() {
    auto loc = peek()->span;
    bool selectiveImport = false;
    if (advanceIfMatchAny<TokenType::FROM>()) {
        selectiveImport = true;
    } else {
        consume(TokenType::IMPORT);
    }

    Token *token = nullptr;
    std::string filepath;

    if (peek()->type == TokenType::STRING) {
        token = consume(TokenType::STRING);
        filepath = token->lexeme;
    } else if (peek()->type == TokenType::IDENTIFIER) {
        token = consume(TokenType::IDENTIFIER);
        filepath = getStdDir() + token->lexeme + ".les";
    } else {
        error(peek(), "Imports must be either strings for files or identifiers for standard library");
        return nullptr;
    }

    if (!selectiveImport) {
        std::string alias = getBasename(token->lexeme);
        if (advanceIfMatchAny<TokenType::AS>()) {
            alias = consume(TokenType::IDENTIFIER)->lexeme;
        }

        consumeNewline();
        auto *placeholder = new Import({loc.Start, token->getEnd()}, filepath, alias, token->type == TokenType::IDENTIFIER, true, false, {});
        return placeholder;
    }

    consume(TokenType::IMPORT);

    if (advanceIfMatchAny<TokenType::STAR>()) {
        consumeNewline();
        return new Import({loc.Start, token->getEnd()}, filepath, getBasename(token->lexeme), token->type == TokenType::IDENTIFIER, true, true, {});
    }

    std::vector<std::pair<std::string, std::string>> importedNames;

    do {
        auto ident = consume(TokenType::IDENTIFIER)->lexeme;
        auto alias = ident;
        if (advanceIfMatchAny<TokenType::AS>()) {
            alias = consume(TokenType::IDENTIFIER)->lexeme;
        }

        importedNames.emplace_back(ident, alias);
    } while (advanceIfMatchAny<TokenType::COMMA>());

    consumeNewline();
    return new Import({loc.Start, token->getEnd()}, filepath, getBasename(token->lexeme), token->type == TokenType::IDENTIFIER, false, true, importedNames);
}

Statement *Parser::parseClass() {
    auto loc = peek()->span;
    consume(TokenType::CLASS);

    auto *token = consume(TokenType::IDENTIFIER);
    consume(TokenType::NEWLINE);

    std::vector<VarDecl *> fields;
    std::vector<FuncDecl *> methods;
    consume(TokenType::INDENT);

    in_class_ = true;
    while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
        if (checkAny<TokenType::LET, TokenType::VAR>()) {
            fields.push_back(dynamic_cast<VarDecl *>(parseVarDecl()));
        } else if (checkAny<TokenType::DEF>()) {
            methods.push_back(dynamic_cast<FuncDecl *>(parseFunctionDeclaration()));
        } else {
            consume(TokenType::NEWLINE);
        }
    }
    in_class_ = false;

    advanceIfMatchAny<TokenType::DEDENT>();

    return new Class(loc, token->lexeme, fields, methods, is_exported_);
}

Statement *Parser::parseEnum() {
    auto loc = peek()->span;
    consume(TokenType::ENUM);

    auto *token = consume(TokenType::IDENTIFIER);
    consume(TokenType::NEWLINE);

    std::vector<std::string> values;
    consume(TokenType::INDENT);

    while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
        values.push_back(consume(TokenType::IDENTIFIER)->lexeme);
        consume(TokenType::NEWLINE);
    }

    advanceIfMatchAny<TokenType::DEDENT>();

    return new Enum(loc, token->lexeme, values, is_exported_);
}

Compound *Parser::parseCompound() {
    std::vector<Statement *> statements;
    while (!isAtEnd()) {
        // Remove lingering newlines
        while (peek()->type == TokenType::NEWLINE) {
            consume(TokenType::NEWLINE);
        }
        statements.push_back(parseStatement(true));
    }
    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

void Parser::parse() {
    tree_ = parseCompound();
}
