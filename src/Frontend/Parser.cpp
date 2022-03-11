#include "Parser.h"

using namespace lesma;

template<TokenType type, TokenType... remained_types>
bool Parser::AdvanceIfMatchAny() {
    if (CheckAny<type, remained_types...>()) {
        Advance();
        return true;
    }

    return false;
}

template<TokenType type, TokenType... remained_types>
bool Parser::CheckAny() {
    return CheckAny<type, remained_types...>(0);
}

template<TokenType type, TokenType... remained_types>
bool Parser::CheckAny(unsigned long pos) {
    if (!Check(type, pos)) {
        if constexpr (sizeof...(remained_types) > 0) {
            return CheckAny<remained_types...>(pos);
        } else {
            return false;
        }
    }
    return true;
}

Token Parser::Consume(TokenType type) {
    return Consume(type, std::string{"Expected: "} + std::string{NAMEOF_ENUM(type)} +
                                 ", found: " + std::string{NAMEOF_ENUM(Peek()->type)});
}

Token Parser::Consume(TokenType type, const std::string &error_message) {
    if (Check(type)) return Advance();
    Error(Peek(), error_message);
    return Token{};
}

Token Parser::ConsumeNewline() {
    if (Check(TokenType::NEWLINE) || Peek()->type == TokenType::EOF_TOKEN)
        return Advance();
    Error(Peek(), fmt::format("Expected: NEWLINE or EOF, found: {}", NAMEOF_ENUM(Peek()->type)));
    return Token{};
}

void Parser::Error(const Token &token, const std::string &error_message) {
    throw ParserError(token->span, "{}", error_message);
}

// TODO: Parse Type
Type *Parser::ParseType() {
    auto type = Peek();
    if (CheckAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE, TokenType::BOOL_TYPE,
                 TokenType::VOID_TYPE>()) {
        Advance();
        return new Type(type->span, type->lexeme, type->type);
    }

    Error(type, fmt::format("Unknown type: {}", type->lexeme));

    return nullptr;
}

// Expression
Expression *Parser::ParseFunctionCall() {
    auto token = Peek();
    Consume(TokenType::IDENTIFIER);
    Consume(TokenType::LEFT_PAREN);

    std::vector<Expression *> params;

    while (!Check(TokenType::RIGHT_PAREN)) {
        auto param = ParseExpression();

        if (!Check(TokenType::RIGHT_PAREN))
            Consume(TokenType::COMMA);

        params.push_back(param);
    }

    auto paren = Consume(TokenType::RIGHT_PAREN);

    return new FuncCall({token.getStart(), paren->span.End}, token->lexeme, params);
}

Expression *Parser::ParseTerm() {
    switch (Peek()->type) {
        case TokenType::STRING:
        case TokenType::INTEGER:
        case TokenType::DOUBLE:
        case TokenType::NIL:
        case TokenType::IDENTIFIER: {
            if (CheckAny<TokenType::LEFT_PAREN>(1))
                return ParseFunctionCall();

            auto token = Peek();
            Consume(token->type);
            return new Literal(token->span, token->lexeme, token->type);
        }
        case TokenType::LEFT_PAREN: {
            Consume(TokenType::LEFT_PAREN);
            auto expr = ParseExpression();
            Consume(TokenType::RIGHT_PAREN);
            return expr;
        }
        case TokenType::TRUE_:
        case TokenType::FALSE_: {
            auto token = Peek();
            Consume(token->type);
            return new Literal(token->span, token->lexeme, TokenType::BOOL);
        }
        default:
            Error(Peek(), fmt::format("Unknown literal: {}", Peek()->lexeme));
    }

    return nullptr;
}

Expression *Parser::ParseUnary() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::MINUS>()) {
        auto op = Previous();
        auto expr = ParseTerm();
        left = new UnaryOp({op.getStart(), expr->getEnd()}, op->type, expr);
    }
    return left == nullptr ? ParseTerm() : left;
}

Expression *Parser::ParseCast() {
    auto left = ParseUnary();
    while (AdvanceIfMatchAny<TokenType::AS>()) {
        auto type = ParseType();
        left = new CastOp({left->getStart(), type->getEnd()}, left, type);
    }
    return left;
}

Expression *Parser::ParseMult() {
    auto left = ParseCast();
    while (AdvanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
        auto op = Previous()->type;
        auto right = ParseCast();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::ParseAdd() {
    auto left = ParseMult();
    while (AdvanceIfMatchAny<TokenType::PLUS, TokenType::MINUS>()) {
        auto op = Previous()->type;
        auto right = ParseMult();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::ParseCompare() {
    auto left = ParseAdd();
    while (AdvanceIfMatchAny<TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL, TokenType::LESS, TokenType::LESS_EQUAL,
                             TokenType::GREATER, TokenType::GREATER_EQUAL>()) {
        auto op = Previous()->type;
        auto right = ParseAdd();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::ParseNot() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::NOT>()) {
        auto op = Previous();
        auto expr = ParseCompare();
        left = new UnaryOp({expr->getStart(), op.getEnd()}, TokenType::NOT, expr);
    }
    return left == nullptr ? ParseCompare() : left;
}

Expression *Parser::ParseAnd() {
    auto left = ParseNot();
    while (AdvanceIfMatchAny<TokenType::AND>()) {
        auto right = ParseNot();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, TokenType::AND, right);
    }
    return left;
}

Expression *Parser::ParseOr() {
    auto left = ParseAnd();
    while (AdvanceIfMatchAny<TokenType::OR>()) {
        auto right = ParseAnd();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, TokenType::OR, right);
    }
    return left;
}

Expression *Parser::ParseExpression() {
    return ParseOr();
}

// Statements
Statement *Parser::ParseVarDecl() {
    bool mutable_;
    Token startTok;
    if (AdvanceIfMatchAny<TokenType::LET>()) {
        startTok = Previous();
        mutable_ = false;
    } else {
        startTok = Consume(TokenType::VAR);
        mutable_ = true;
    }
    auto identifier = Consume(TokenType::IDENTIFIER);
    auto var = new Literal(identifier->span, identifier->lexeme, identifier->type);

    std::optional<Type *> type = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::COLON>())
        type = ParseType();

    std::optional<Expression *> expr = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::EQUAL>())
        expr = ParseExpression();

    if (type == std::nullopt && expr == std::nullopt)
        throw LexerError(Span{startTok.getStart(), var->getEnd()}, "Expected either a type or a value");
    else if (expr == std::nullopt && !mutable_)
        throw LexerError(Span{startTok.getStart(), type.value()->getEnd()}, "Cannot declare an immutable variable without an initial expression");

    ConsumeNewline();
    return new VarDecl({startTok.getStart(), expr != std::nullopt ? expr.value()->getEnd() : type.value()->getEnd()}, var, type, expr, mutable_);
}

Statement *Parser::ParseIf() {
    auto loc = Peek()->span;
    Consume(TokenType::IF);

    auto conds = std::vector<Expression *>();
    auto blocks = std::vector<Compound *>();

    // Read first If cond and block
    conds.push_back(ParseExpression());
    blocks.push_back(ParseBlock());

    while (AdvanceIfMatchAny<TokenType::ELSE_IF>()) {
        conds.push_back(ParseExpression());
        blocks.push_back(ParseBlock());
    }
    if (AdvanceIfMatchAny<TokenType::ELSE>()) {
        conds.push_back(new Else(Peek()->span));
        blocks.push_back(ParseBlock());
    }

    return new If({loc.Start, blocks.back()->getEnd()}, conds, blocks);
}

Statement *Parser::ParseWhile() {
    auto loc = Peek()->span;
    Consume(TokenType::WHILE);

    auto cond = ParseExpression();
    auto block = ParseBlock();

    return new While({loc.Start, block->getEnd()}, cond, block);
}

Statement *Parser::ParseFor() {
    Error(Peek(), "Unimplemented");

    return nullptr;
}

Statement *Parser::ParseAssignment() {
    auto identifier = Consume(TokenType::IDENTIFIER);
    auto var = new Literal(identifier->span, identifier->lexeme, identifier->type);

    if (AdvanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                          TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>()) {
        auto op = Previous()->type;
        auto expr = ParseExpression();

        ConsumeNewline();
        return new Assignment({identifier.getStart(), expr->getEnd()}, var, op, expr);
    }

    Error(Peek(), fmt::format("Unsupported assignment operator: {}", Peek()->lexeme));

    return nullptr;
}

Statement *Parser::ParseBreak() {
    auto tok = reinterpret_cast<Statement *>(new Break(Consume(TokenType::BREAK)->span));
    ConsumeNewline();
    return tok;
}

Statement *Parser::ParseContinue() {
    auto tok = reinterpret_cast<Statement *>(new Continue(Consume(TokenType::CONTINUE)->span));
    ConsumeNewline();
    return tok;
}

Statement *Parser::ParseReturn() {
    auto loc = Peek()->span;
    Consume(TokenType::RETURN);
    if (Check(TokenType::NEWLINE) || Peek()->type == TokenType::EOF_TOKEN) {
        ConsumeNewline();
        return reinterpret_cast<Statement *>(new Return(loc, nullptr));
    }
    auto val = ParseExpression();
    ConsumeNewline();
    return reinterpret_cast<Statement *>(new Return({loc.Start, val->getEnd()}, val));
}

Statement *Parser::ParseDefer() {
    auto loc = Peek()->span;
    Consume(TokenType::DEFER);
    auto val = ParseStatement();
    //Don't consume newline, since statement will
    return reinterpret_cast<Statement *>(new Defer({loc.Start, val->getEnd()}, val));
}

Statement *Parser::ParseStatement() {
    if (Check(TokenType::DEF))
        return ParseFunctionDeclaration();
    else if (Check(TokenType::IMPORT))
        return ParseImport();
    else if (Check(TokenType::LET) || Check(TokenType::VAR))
        return ParseVarDecl();
    else if (Check(TokenType::IF))
        return ParseIf();
    else if (Check(TokenType::WHILE))
        return ParseWhile();
    else if (Check(TokenType::FOR))
        return ParseFor();
    else if (Check(TokenType::BREAK))
        return ParseBreak();
    else if (Check(TokenType::CONTINUE))
        return ParseContinue();
    else if (Check(TokenType::RETURN))
        return ParseReturn();
    else if (Check(TokenType::DEFER))
        return ParseDefer();
    else if (Check(TokenType::IDENTIFIER) &&
             CheckAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                      TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>(1))
        return ParseAssignment();

    // Check if it's just an expression statement
    auto expr = ParseExpression();
    if (expr != nullptr) {
        ConsumeNewline();
        return new ExpressionStatement(expr->getSpan(), expr);
    }

    Error(Peek(), "Unknown statement");
    return nullptr;
}

Compound *Parser::ParseBlock() {
    std::vector<Statement *> statements;

    Consume(TokenType::NEWLINE);
    Consume(TokenType::INDENT);

    while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>())
        statements.push_back(ParseStatement());

    AdvanceIfMatchAny<TokenType::DEDENT>();

    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

Statement *Parser::ParseFunctionDeclaration() {
    auto loc = Peek()->span;
    Consume(TokenType::DEF);
    bool extern_func = false;

    if (AdvanceIfMatchAny<TokenType::EXTERN_FUNC>())
        extern_func = true;

    auto identifier = Consume(TokenType::IDENTIFIER);

    // Parse parameters
    Consume(TokenType::LEFT_PAREN);
    std::vector<std::pair<std::string, Type *>> parameters;

    while (!Check(TokenType::RIGHT_PAREN)) {
        auto param_ident = Consume(TokenType::IDENTIFIER);
        Consume(TokenType::COLON);
        auto type = ParseType();

        if (!Check(TokenType::RIGHT_PAREN))
            Consume(TokenType::COMMA);

        parameters.emplace_back(param_ident->lexeme, type);
    }

    Consume(TokenType::RIGHT_PAREN);

    Type *return_type;
    if (AdvanceIfMatchAny<TokenType::ARROW>())
        return_type = ParseType();
    else
        return_type = new Type(Previous()->span, "void", TokenType::VOID_TYPE);

    if (extern_func) {
        ConsumeNewline();
        return new ExternFuncDecl({loc.Start, return_type->getEnd()}, identifier->lexeme, return_type, parameters);
    }

    auto body = ParseBlock();

    return new FuncDecl({loc.Start, body->getEnd()}, identifier->lexeme, return_type, parameters, body);
}

Statement *Parser::ParseImport() {
    auto loc = Peek()->span;
    Consume(TokenType::IMPORT);

    auto token = Consume(TokenType::STRING);
    auto filepath = token->lexeme.erase(0, 1).erase(token->lexeme.size() - 1);

    if (AdvanceIfMatchAny<TokenType::AS>()) {
        auto alias = Consume(TokenType::IDENTIFIER);
        return new Import({loc.Start, alias.getEnd()}, token->lexeme, alias->lexeme);
    }

    return new Import({loc.Start, token.getEnd()}, filepath, getBasename(token->lexeme));
}

Compound *Parser::ParseCompound() {
    std::vector<Statement *> statements;
    while (!IsAtEnd()) {
        // Remove lingering newlines
        while (Peek()->type == TokenType::NEWLINE)
            Consume(TokenType::NEWLINE);
        statements.push_back(ParseStatement());
    }
    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

void Parser::Parse() {
    tree = ParseCompound();
}
