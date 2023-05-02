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
bool Parser::CheckAnyInLine() {
    int i = 0;
    while (!CheckAny<TokenType::NEWLINE, TokenType::EOF_TOKEN>(i)) {
        if (CheckAny<type, remained_types...>(i))
            return true;
        i++;
    }

    return false;
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

Token *Parser::Consume(TokenType type) {
    return Consume(type, std::string{"Expected: "} + std::string{NAMEOF_ENUM(type)} +
                                 ", found: " + std::string{NAMEOF_ENUM(Peek()->type)});
}

Token *Parser::Consume(TokenType type, const std::string &error_message) {
    if (Check(type)) return Advance();
    Error(Peek(), error_message);
    return new Token{};
}

Token *Parser::ConsumeNewline() {
    if (Check(TokenType::NEWLINE) || Peek()->type == TokenType::EOF_TOKEN)
        return Advance();
    Error(Peek(), fmt::format("Expected: NEWLINE or EOF, found: {}", NAMEOF_ENUM(Peek()->type)));
    return new Token{};
}

void Parser::Error(Token *token, const std::string &error_message) {
    throw ParserError(token->span, "{}", error_message);
}

TypeExpr *Parser::ParseType() {
    auto type = Peek();
    if (Check(TokenType::STAR)) {
        Advance();
        auto element_type = ParseType();
        return new TypeExpr({type->getStart(), element_type->getEnd()}, "*" + element_type->getName(), TokenType::PTR_TYPE, element_type);
    } else if (CheckAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE, TokenType::BOOL_TYPE,
                        TokenType::INT8_TYPE, TokenType::INT16_TYPE, TokenType::INT32_TYPE, TokenType::FLOAT32_TYPE, TokenType::VOID_TYPE>()) {
        Advance();
        return new TypeExpr(type->span, type->lexeme, type->type);
    } else if (Check(TokenType::FUNC)) {
        std::vector<TypeExpr *> params;
        TypeExpr *ret;
        std::string lexeme = type->lexeme + " (";

        Advance();
        Consume(TokenType::LEFT_PAREN);
        do {
            if (!params.empty())
                lexeme += ", ";
            params.push_back(ParseType());
            lexeme += params.back()->getName();
        } while (AdvanceIfMatchAny<TokenType::COMMA>());

        Consume(TokenType::RIGHT_PAREN);
        lexeme += ")";

        if (AdvanceIfMatchAny<TokenType::ARROW>()) {
            ret = ParseType();
            lexeme += " -> " + ret->getName();
        } else {
            ret = new TypeExpr({params.back()->getEnd(), params.back()->getEnd()}, type->lexeme, type->type);
        }

        // TODO: This should really be a pointer to a function type
        return new TypeExpr({type->getStart(), ret->getEnd()}, lexeme, TokenType::FUNC_TYPE, params, ret);
    } else if (Check(TokenType::IDENTIFIER)) {
        Advance();
        return new TypeExpr(type->span, type->lexeme, TokenType::CUSTOM_TYPE);
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

    return new FuncCall({token->getStart(), paren->span.End}, token->lexeme, params);
}

Expression *Parser::ParseTerm() {
    switch (Peek()->type) {
        case TokenType::STRING:
        case TokenType::INTEGER:
        case TokenType::DOUBLE:
        case TokenType::NIL: {
            auto token = Peek();
            Consume(token->type);
            return new Literal(token->span, token->lexeme, token->type);
        }
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

Expression *Parser::ParseDot() {
    Expression *left = ParseTerm();

    while (AdvanceIfMatchAny<TokenType::DOT>()) {
        auto op = Previous();
        auto expr = ParseTerm();
        left = new DotOp({left->getStart(), expr->getEnd()}, left, op->type, expr);
    }

    return left;
}

Expression *Parser::ParseUnary() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::MINUS, TokenType::STAR, TokenType::AMPERSAND>()) {
        auto op = Previous();
        auto expr = ParseDot();
        left = new UnaryOp({op->getStart(), expr->getEnd()}, op->type, expr);
    }

    if (left == nullptr) {
        delete left;
        return ParseDot();
    }

    return left;
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
    auto left = ParsePower();
    while (AdvanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
        auto op = Previous()->type;
        auto right = ParsePower();
        left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
    }
    return left;
}

Expression *Parser::ParsePower() {
    auto left = ParseCast();
    while (AdvanceIfMatchAny<TokenType::POWER>()) {
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
                             TokenType::GREATER, TokenType::GREATER_EQUAL, TokenType::IS, TokenType::IS_NOT>()) {
        auto op = Previous()->type;
        if (op == TokenType::IS || op == TokenType::IS_NOT) {
            auto right = ParseType();
            left = new IsOp({left->getStart(), right->getEnd()}, left, op, right);
        } else {
            auto right = ParseAdd();
            left = new BinaryOp({left->getStart(), right->getEnd()}, left, op, right);
        }
    }
    return left;
}

Expression *Parser::ParseNot() {
    Expression *left = nullptr;
    while (AdvanceIfMatchAny<TokenType::NOT>()) {
        auto op = Previous();
        auto expr = ParseCompare();
        left = new UnaryOp({expr->getStart(), op->getEnd()}, TokenType::NOT, expr);
    }

    if (left == nullptr) {
        delete left;
        return ParseCompare();
    }

    return left;
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
    Token *startTok;
    if (AdvanceIfMatchAny<TokenType::LET>()) {
        startTok = Previous();
        mutable_ = false;
    } else {
        startTok = Consume(TokenType::VAR);
        mutable_ = true;
    }
    auto identifier = Consume(TokenType::IDENTIFIER);
    auto var = new Literal(identifier->span, identifier->lexeme, identifier->type);

    std::optional<TypeExpr *> type = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::COLON>())
        type = ParseType();

    std::optional<Expression *> expr = std::nullopt;
    if (AdvanceIfMatchAny<TokenType::EQUAL>())
        expr = ParseExpression();

    if (type == std::nullopt && expr == std::nullopt)
        throw ParserError(llvm::SMRange{startTok->getStart(), var->getEnd()}, "Expected either a type or a value");
    else if (expr == std::nullopt && !mutable_)
        throw ParserError(llvm::SMRange{startTok->getStart(), type.value()->getEnd()}, "Cannot declare an immutable variable without an initial expression");

    ConsumeNewline();
    return new VarDecl({startTok->getStart(), expr != std::nullopt ? expr.value()->getEnd() : type.value()->getEnd()}, var, type, expr, mutable_);
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

    auto identifier = ParseDot();

    if (!(dynamic_cast<Literal *>(identifier) && dynamic_cast<Literal *>(identifier)->getType() == TokenType::IDENTIFIER) && !dynamic_cast<DotOp *>(identifier))
        throw ParserError(identifier->getSpan(), "Expected either identifier or class field for assignment");

    if (AdvanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                          TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>()) {
        auto op = Previous()->type;
        auto expr = ParseExpression();

        ConsumeNewline();
        return new Assignment({identifier->getStart(), expr->getEnd()}, identifier, op, expr);
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
    auto val = ParseStatement(false);
    //Don't consume newline, since statement will
    return reinterpret_cast<Statement *>(new Defer({loc.Start, val->getEnd()}, val));
}

Statement *Parser::ParseStatement(bool isTopLevel) {
    if (CheckAny<TokenType::DEF, TokenType::IMPORT, TokenType::CLASS, TokenType::ENUM, TokenType::EXPORT>() && !isTopLevel)
        Error(Peek(), "Statement not allowed inside a block");

    if (Check(TokenType::DEF))
        return ParseFunctionDeclaration();
    else if (CheckAny<TokenType::IMPORT, TokenType::FROM>())
        return ParseImport();
    else if (Check(TokenType::CLASS))
        return ParseClass();
    else if (Check(TokenType::ENUM))
        return ParseEnum();
    else if (Check(TokenType::EXPORT))
        return ParseExport();
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
    else if (CheckAnyInLine<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                            TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL, TokenType::POWER_EQUAL>())
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
        statements.push_back(ParseStatement(false));

    AdvanceIfMatchAny<TokenType::DEDENT>();

    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

Statement *Parser::ParseFunctionDeclaration() {
    auto loc = isExported ? Previous()->span : Peek()->span;
    Consume(TokenType::DEF);
    bool extern_func = false;

    if (AdvanceIfMatchAny<TokenType::EXTERN>())
        extern_func = true;

    if (extern_func && inClass) {
        Error(Previous(), "Extern functions are not allowed in class definition.");
        return nullptr;
    }

    auto identifier = Consume(TokenType::IDENTIFIER);

    // Parse parameters
    Consume(TokenType::LEFT_PAREN);
    std::vector<Parameter *> parameters;

    bool varargs = false;
    while (!Check(TokenType::RIGHT_PAREN)) {
        if (varargs)
            Error(Peek(), "Varargs should be the last parameter");

        if (Check(TokenType::ELLIPSIS) && extern_func) {
            Consume(TokenType::ELLIPSIS);
            varargs = true;
        } else {
            Expression *default_val = nullptr;
            TypeExpr *type = nullptr;
            auto param_ident = Consume(TokenType::IDENTIFIER);

            if (AdvanceIfMatchAny<TokenType::COLON>()) {
                type = ParseType();
            }

            if (AdvanceIfMatchAny<TokenType::EQUAL>()) {
                default_val = ParseExpression();
            }

            if (default_val == nullptr && type == nullptr) {
                throw ParserError(param_ident->span, "{} should have either a type, a value or both specified", param_ident->lexeme);
            }

            parameters.emplace_back(new Parameter(param_ident->lexeme, type, false, default_val));
        }

        if (!Check(TokenType::RIGHT_PAREN) && !Check(TokenType::RIGHT_PAREN, 1))
            Consume(TokenType::COMMA);
    }

    Consume(TokenType::RIGHT_PAREN);

    TypeExpr *return_type;
    if (AdvanceIfMatchAny<TokenType::ARROW>())
        return_type = ParseType();
    else
        return_type = new TypeExpr(Previous()->span, "void", TokenType::VOID_TYPE);

    if (extern_func) {
        ConsumeNewline();
        return new ExternFuncDecl({loc.Start, return_type->getEnd()}, identifier->lexeme, return_type, parameters, varargs, isExported);
    }

    auto body = ParseBlock();

    return new FuncDecl({loc.Start, return_type->getEnd()}, identifier->lexeme, return_type, parameters, body, false, isExported);
}

Statement *Parser::ParseExport() {
    Consume(TokenType::EXPORT);

    if (inClass)
        Error(Peek(), "Cannot export class members");

    if (!CheckAny<TokenType::DEF, TokenType::CLASS, TokenType::ENUM>())
        Error(Peek(), "Can only export functions, classes and enums");

    isExported = true;
    Statement *statement = nullptr;
    if (Check(TokenType::DEF))
        statement = ParseFunctionDeclaration();
    else if (Check(TokenType::IMPORT))
        statement = ParseImport();
    else if (Check(TokenType::CLASS))
        statement = ParseClass();
    else if (Check(TokenType::ENUM))
        statement = ParseEnum();
    else
        Error(Peek(), "Unexpected statement after export");

    isExported = false;
    return statement;
}

Statement *Parser::ParseImport() {
    auto loc = Peek()->span;
    bool selectiveImport = false;
    if (AdvanceIfMatchAny<TokenType::FROM>())
        selectiveImport = true;
    else
        Consume(TokenType::IMPORT);

    Token *token;
    std::string filepath;

    if (Peek()->type == TokenType::STRING) {
        token = Consume(TokenType::STRING);
        filepath = token->lexeme;
    } else if (Peek()->type == TokenType::IDENTIFIER) {
        token = Consume(TokenType::IDENTIFIER);
        filepath = getStdDir() + token->lexeme + ".les";
    } else {
        Error(Peek(), "Imports must be either strings for files or identifiers for standard library");
        return nullptr;
    }

    if (!selectiveImport) {
        std::string alias = getBasename(token->lexeme);
        if (AdvanceIfMatchAny<TokenType::AS>())
            alias = Consume(TokenType::IDENTIFIER)->lexeme;

        ConsumeNewline();
        return new Import({loc.Start, token->getEnd()}, filepath, alias, token->type == TokenType::IDENTIFIER, true, false, {});
    } else {
        Consume(TokenType::IMPORT);

        if (AdvanceIfMatchAny<TokenType::STAR>()) {
            ConsumeNewline();
            return new Import({loc.Start, token->getEnd()}, filepath, getBasename(token->lexeme), token->type == TokenType::IDENTIFIER, true, true, {});
        } else {
            std::vector<std::pair<std::string, std::string>> imported_names;

            do {
                auto ident = Consume(TokenType::IDENTIFIER)->lexeme;
                auto alias = ident;
                if (AdvanceIfMatchAny<TokenType::AS>())
                    alias = Consume(TokenType::IDENTIFIER)->lexeme;

                imported_names.emplace_back(ident, alias);
            } while (AdvanceIfMatchAny<TokenType::COMMA>());

            ConsumeNewline();
            return new Import({loc.Start, token->getEnd()}, filepath, getBasename(token->lexeme), token->type == TokenType::IDENTIFIER, false, true, imported_names);
        }
    }
}

Statement *Parser::ParseClass() {
    auto loc = Peek()->span;
    Consume(TokenType::CLASS);

    auto token = Consume(TokenType::IDENTIFIER);
    Consume(TokenType::NEWLINE);

    std::vector<VarDecl *> fields;
    std::vector<FuncDecl *> methods;
    Consume(TokenType::INDENT);

    inClass = true;
    while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
        if (CheckAny<TokenType::LET, TokenType::VAR>())
            fields.push_back(dynamic_cast<VarDecl *>(ParseVarDecl()));
        else if (CheckAny<TokenType::DEF>())
            methods.push_back(dynamic_cast<FuncDecl *>(ParseFunctionDeclaration()));
        else
            Consume(TokenType::NEWLINE);
    }
    inClass = false;

    AdvanceIfMatchAny<TokenType::DEDENT>();

    return new Class(loc, token->lexeme, fields, methods, isExported);
}

Statement *Parser::ParseEnum() {
    auto loc = Peek()->span;
    Consume(TokenType::ENUM);

    auto token = Consume(TokenType::IDENTIFIER);
    Consume(TokenType::NEWLINE);

    std::vector<std::string> values;
    Consume(TokenType::INDENT);

    while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
        values.push_back(Consume(TokenType::IDENTIFIER)->lexeme);
        Consume(TokenType::NEWLINE);
    }

    AdvanceIfMatchAny<TokenType::DEDENT>();

    return new Enum(loc, token->lexeme, values, isExported);
}

Compound *Parser::ParseCompound() {
    std::vector<Statement *> statements;
    while (!IsAtEnd()) {
        // Remove lingering newlines
        while (Peek()->type == TokenType::NEWLINE)
            Consume(TokenType::NEWLINE);
        statements.push_back(ParseStatement(true));
    }
    if (statements.empty()) {
        auto eof = Peek();
        return new Compound(eof->span, statements);
    }
    return new Compound({statements.front()->getStart(), statements.back()->getEnd()}, statements);
}

void Parser::Parse() {
    tree = ParseCompound();
}
