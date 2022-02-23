#include "Token.h"

using namespace lesma;

static std::map<std::string,TokenType> g_reserved_map{
        {"and",     TokenType::AND},
        {"class",   TokenType::CLASS},
        {"else",    TokenType::ELSE},
        {"elseif",  TokenType::ELSE_IF},
        {"false",   TokenType::FALSE_},
        {"for",     TokenType::FOR},
        {"def",     TokenType::DEF},
        {"if",      TokenType::IF},
        {"not",     TokenType::NOT},
        {"null",    TokenType::NIL},
        {"or",      TokenType::OR},
        {"print",   TokenType::PRINT},
        {"return",  TokenType::RETURN},
        {"this",    TokenType::THIS},
        {"true",    TokenType::TRUE_},
        {"var",     TokenType::VAR},
        {"let",     TokenType::LET},
        {"while",   TokenType::WHILE},
        {"break",   TokenType::BREAK},
        {"continue",TokenType::CONTINUE},
        {"super",   TokenType::SUPER},
        {"extern",  TokenType::EXTERN},
        {"as",      TokenType::CAST},
        {"is",      TokenType::IS},
        {"in",      TokenType::IN},
        {"int",     TokenType::INT_TYPE},
        {"float",   TokenType::FLOAT_TYPE},
        {"string",  TokenType::STRING_TYPE},
        {"bool",    TokenType::BOOL_TYPE},
        {"void",    TokenType::VOID_TYPE},
};

std::string TokenState::Dump() const {
    return std::string(
            "[Type: ") + std::string{NAMEOF_ENUM(type)} +
            ", Lexme: " + lexeme +
            ", Line: " + std::to_string(loc.Line) +
            ", Col: " + std::to_string(loc.Col) +"]";
}

TokenType Token::GetIdentifierType(const std::string& identifier) {
    if (g_reserved_map.count(identifier) == 0)
        return TokenType::IDENTIFIER;
    else
        return g_reserved_map[identifier];
}