#include "Parser.h"

using namespace lesma;

// Utility
template <TokenType type, TokenType... remained_types>
bool Parser::AdvanceIfMatchAny() {
    if (CheckAny<type, remained_types...>()) {
        Advance();
        return true;
    }

    return false;
}

template <TokenType type, TokenType... remained_types>
bool Parser::CheckAny() {
    return CheckAny<type, remained_types...>(0);
}

template <TokenType type, TokenType... remained_types>
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

Token Parser::Consume(TokenType type, const std::string& error_message) {
    if (Check(type)) return Advance();
    Error(Peek(), error_message);
    return Token{};
}

void Parser::Error(const Token &token, const std::string& error_message) {
    throw ParserError("{}: {}", token->Dump(), error_message);;
}

// TODO: Parse Type
Type* Parser::ParseType() {
    auto type = Peek();
    if (CheckAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE, TokenType::BOOL_TYPE,
            TokenType::VOID_TYPE>()) {
        Advance();
        return new Type(type->loc, type->lexeme, type->type);
    }

    Error(type, "Unknown type");
    exit(1);
}

// Expression
Expression* Parser::ParseFunctionCall() {
    auto token = Peek();
    Consume(TokenType::IDENTIFIER);
    Consume(TokenType::LEFT_PAREN);

    std::vector<Expression *> params;

    while(!Check(TokenType::RIGHT_PAREN)) {
        auto param = ParseExpression();

        if (!Check(TokenType::RIGHT_PAREN))
            Consume(TokenType::COMMA);

        params.push_back(param);
    }

    Consume(TokenType::RIGHT_PAREN);

    return new FuncCall(token->loc, token->lexeme, params);
}


Expression* Parser::ParseTerm() {
    switch(Peek()->type) {
        case TokenType::STRING:
        case TokenType::INTEGER:
        case TokenType::DOUBLE:
        case TokenType::NIL:
        case TokenType::IDENTIFIER: {
            if (CheckAny<TokenType::LEFT_PAREN>(1))
                return ParseFunctionCall();

            auto token = Peek();
            Consume(token->type);
            return new Literal(token->loc, token->lexeme, token->type);
        }
        case TokenType::TRUE_:
        case TokenType::FALSE_: {
            auto token = Peek();
            Consume(token->type);
            return new Literal(token->loc, token->lexeme, TokenType::BOOL);
        }
        default:
            Error(Peek(), std::string{NAMEOF_ENUM(Peek()->type)} + std::string{"Unknown literal"});
    }

    return nullptr;
}

Expression* Parser::ParseUnary() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::MINUS>()) {
        auto op = Previous()->type;
        auto expr = ParseTerm();
        left = new UnaryOp(Peek()->loc, op, expr);
    }
    return left == nullptr ? ParseTerm() : left;
}

Expression* Parser::ParseCast() {
    auto left = ParseUnary();
    while (AdvanceIfMatchAny<TokenType::CAST>()) {
        auto type = ParseType();
        left = new CastOp(Peek()->loc, left, type);
    }
    return left;
}

Expression* Parser::ParseMult() {
    auto left = ParseCast();
    while (AdvanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
        auto op = Previous()->type;
        auto right = ParseCast();
        left = new BinaryOp(Peek()->loc, left, op, right);
    }
    return left;
}

Expression* Parser::ParseAdd() {
    auto left = ParseMult();
    while (AdvanceIfMatchAny<TokenType::PLUS, TokenType::MINUS>()) {
        auto op = Previous()->type;
        auto right = ParseMult();
        left = new BinaryOp(Peek()->loc, left, op, right);
    }
    return left;
}

Expression* Parser::ParseCompare() {
    auto left = ParseAdd();
    while (AdvanceIfMatchAny<TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL, TokenType::LESS, TokenType::LESS_EQUAL,
                             TokenType::GREATER, TokenType::GREATER_EQUAL>()) {
        auto op = Previous()->type;
        auto right = ParseAdd();
        left = new BinaryOp(Peek()->loc, left, op, right);
    }
    return left;
}

Expression* Parser::ParseNot() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::NOT>()) {
        auto expr = ParseCompare();
        left = new UnaryOp(Peek()->loc, TokenType::NOT, expr);
    }
    return left == nullptr ? ParseCompare() : left;
}

Expression* Parser::ParseAnd() {
    auto left = ParseNot();
    while (AdvanceIfMatchAny<TokenType::AND>()) {
        auto right = ParseNot();
        left = new BinaryOp(Peek()->loc, left, TokenType::AND, right);
    }
    return left;
}

Expression* Parser::ParseOr() {
    auto left = ParseAnd();
    while (AdvanceIfMatchAny<TokenType::OR>()) {
        auto right = ParseAnd();
        left = new BinaryOp(Peek()->loc, left, TokenType::OR, right);
    }
    return left;
}

Expression* Parser::ParseExpression() {
    return ParseOr();
}

// Statements
Statement* Parser::ParseVarDecl() {
    bool mutable_;
    if (AdvanceIfMatchAny<TokenType::LET>()) {
        mutable_ = false;
    } else {
        Consume(TokenType::VAR);
        mutable_ = true;
    }
    auto identifier = Consume(TokenType::IDENTIFIER);
    auto var = new Literal(identifier->loc, identifier->lexeme, identifier->type);

    std::optional<Type*> type = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::COLON>())
        type = ParseType();

    std::optional<Expression*> expr = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::EQUAL>())
        expr = ParseExpression();

    if (type == std::nullopt && expr == std::nullopt)
        throw LexerError("Expected either a type or a value");
    else if (expr == std::nullopt && !mutable_)
        throw LexerError("Cannot declare an immutable variable without an initial expression");

    return new VarDecl(Peek()->loc, var, type, expr, mutable_);
}

Statement* Parser::ParseIf() {
    auto loc = Peek()->loc;
    Consume(TokenType::IF);

    auto conds = std::vector<Expression*>();
    auto blocks = std::vector<Compound*>();

    // Read first If cond and block
    conds.push_back(ParseExpression());
    blocks.push_back(ParseBlock());

    while (AdvanceIfMatchAny<TokenType::ELSE_IF>()) {
        conds.push_back(ParseExpression());
        blocks.push_back(ParseBlock());
    }
    if (AdvanceIfMatchAny<TokenType::ELSE>()) {
        conds.push_back(new Else(Peek()->loc));
        blocks.push_back(ParseBlock());
    }

    return new If(loc, conds, blocks);
}

Statement* Parser::ParseWhile() {
    auto loc = Peek()->loc;
    Consume(TokenType::WHILE);

    auto cond = ParseExpression();
    auto block = ParseBlock();

    return new While(loc, cond, block);
}

Statement* Parser::ParseFor() {
    Error(Peek(), "Unimplemented");

    return nullptr;
}

Statement* Parser::ParseAssignment() {
    auto identifier = Consume(TokenType::IDENTIFIER);
    auto var = new Literal(identifier->loc, identifier->lexeme, identifier->type);

    if (AdvanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
            TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>()) {
        auto op = Previous()->type;
        auto expr = ParseExpression();

        return new Assignment(identifier->loc, var, op, expr);
    }

    Error(Peek(), "Unsupported assignment operator");

    return nullptr;
}

Statement* Parser::ParseBreak() {
    return reinterpret_cast<Statement *>(new Break(Consume(TokenType::BREAK)->loc));
}

Statement* Parser::ParseContinue() {
    return reinterpret_cast<Statement *>(new Continue(Consume(TokenType::CONTINUE)->loc));
}

Statement* Parser::ParseReturn() {
    auto loc = Peek()->loc;
    Consume(TokenType::RETURN);
    auto val = ParseExpression();
    return reinterpret_cast<Statement *>(new Return(loc, val));
}

Statement* Parser::ParseStatement() {
    if (Check(TokenType::LET) || Check(TokenType::VAR))
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
    else if (Check(TokenType::IDENTIFIER) &&
             CheckAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
             TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>(1))
        return ParseAssignment();

    // Check if it's just an expression statement
    auto expr = ParseExpression();
    if (expr != nullptr)
        return new ExpressionStatement({expr->getLine(), expr->getCol()}, expr);

    Error(Peek(), "Unknown statement");
    return nullptr;
}

Compound* Parser::ParseBlock() {
    auto compound = new Compound(Peek()->loc);

    Consume(TokenType::LEFT_BRACE);

    while (!Check(TokenType::RIGHT_BRACE))
        compound->addChildren(ParseStatement());

    Consume(TokenType::RIGHT_BRACE);

    return compound;
}

Statement* Parser::ParseFunctionDeclaration() {
    Consume(TokenType::DEF);
    bool extern_func = false;

    if (AdvanceIfMatchAny<TokenType::EXTERN>())
        extern_func = true;

    auto identifier = Consume(TokenType::IDENTIFIER);

    // Parse parameters
    Consume(TokenType::LEFT_PAREN);
    std::vector<std::pair<std::string, Type*>> parameters;

    while(!Check(TokenType::RIGHT_PAREN)) {
        auto param_ident = Consume(TokenType::IDENTIFIER);
        Consume(TokenType::COLON);
        auto type = ParseType();

        if (!Check(TokenType::RIGHT_PAREN))
            Consume(TokenType::COMMA);

        parameters.emplace_back(param_ident->lexeme, type);
    }

    Consume(TokenType::RIGHT_PAREN);

    Type* return_type;
    if (AdvanceIfMatchAny<TokenType::ARROW>())
        return_type = ParseType();
    else
        return_type = new Type(Peek()->loc, "void", TokenType::VOID_TYPE);

    if (extern_func)
        return new ExternFuncDecl(Peek()->loc, identifier->lexeme, return_type, parameters);

    auto body = ParseBlock();

    return new FuncDecl(Peek()->loc, identifier->lexeme, return_type, parameters, body);
}

Statement* Parser::ParseTopLevelStatement() {
    Statement* Node;
    if (Check(TokenType::DEF)) {
        Node = ParseFunctionDeclaration();
    } else {
        Node = nullptr;
        Error(Peek(), "Unknown Top Level expression");
    }

    return Node;
}

Compound* Parser::ParseCompound() {
    auto compound = new Compound(Peek()->loc);
    while (!IsAtEnd()) {
        compound->addChildren(ParseTopLevelStatement());
    }
    return compound;
}

void Parser::Parse() {
    tree = new Program(Peek()->loc, ParseCompound());
}
