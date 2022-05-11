#include "Token.h"

using namespace lesma;

static std::map<std::string, TokenType> g_reserved_map{
        {"and", TokenType::AND},
        {"class", TokenType::CLASS},
        {"enum", TokenType::ENUM},
        {"else", TokenType::ELSE},
        {"else if", TokenType::ELSE_IF},
        {"false", TokenType::FALSE_},
        {"for", TokenType::FOR},
        {"def", TokenType::DEF},
        {"if", TokenType::IF},
        {"not", TokenType::NOT},
        {"null", TokenType::NIL},
        {"or", TokenType::OR},
        {"return", TokenType::RETURN},
        {"this", TokenType::THIS},
        {"true", TokenType::TRUE_},
        {"var", TokenType::VAR},
        {"let", TokenType::LET},
        {"while", TokenType::WHILE},
        {"break", TokenType::BREAK},
        {"continue", TokenType::CONTINUE},
        {"super", TokenType::SUPER},
        {"extern", TokenType::EXTERN_FUNC},
        {"as", TokenType::AS},
        {"is", TokenType::IS},
        {"in", TokenType::IN},
        {"int", TokenType::INT_TYPE},
        {"float", TokenType::FLOAT_TYPE},
        {"str", TokenType::STRING_TYPE},
        {"bool", TokenType::BOOL_TYPE},
        {"void", TokenType::VOID_TYPE},
        {"import", TokenType::IMPORT},
        {"defer", TokenType::DEFER},
};

std::string TokenState::Dump(std::shared_ptr<llvm::SourceMgr> srcMgr) const {
    return std::string(
                   "[Type: ") +
           std::string{NAMEOF_ENUM(type)} +
           ", Lexeme: " + lexeme +
           ", Line: " + std::to_string(srcMgr->getLineAndColumn(span.Start, srcMgr->getNumBuffers() - 1).first) + " - " + std::to_string(srcMgr->getLineAndColumn(span.End, srcMgr->getNumBuffers() - 1).first) +
           ", Col: " + std::to_string(srcMgr->getLineAndColumn(span.Start, srcMgr->getNumBuffers() - 1).second) + " - " + std::to_string(srcMgr->getLineAndColumn(span.End, srcMgr->getNumBuffers() - 1).second) + "]";
}

TokenType Token::GetIdentifierType(const std::string &identifier, Token lastTok) {
    if (g_reserved_map.count(identifier) == 0)
        return TokenType::IDENTIFIER;
    else if (lastTok->type == TokenType::ELSE && identifier == "if")
        return g_reserved_map["else if"];
    else
        return g_reserved_map[identifier];
}