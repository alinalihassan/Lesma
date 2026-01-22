#include "Token.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <llvm/Support/SourceMgr.h>

#include "nameof.hpp"
#include <fmt/format.h>

#include "liblesma/Token/TokenType.h"

using namespace lesma;

auto Token::dump(const std::shared_ptr<llvm::SourceMgr> &srcMgr) const -> std::string {
    unsigned bufId = srcMgr->getNumBuffers() - 1;
    auto [startLine, startCol] = srcMgr->getLineAndColumn(span.Start, bufId);
    auto [endLine, endCol] = srcMgr->getLineAndColumn(span.End, bufId);

    return fmt::format("[Type: {}, Lexeme: {}, Line: {} - {}, Col: {} - {}]", NAMEOF_ENUM(type), lexeme, startLine,
                       endLine, startCol, endCol);
}

static const std::unordered_map<std::string_view, TokenType> keywords = {
    // Keywords
    {"and", TokenType::AND},
    {"class", TokenType::CLASS},
    {"enum", TokenType::ENUM},
    {"else", TokenType::ELSE},
    {"false", TokenType::FALSE_},
    {"for", TokenType::FOR},
    {"def", TokenType::DEF},
    {"defer", TokenType::DEFER},
    {"func", TokenType::FUNC},
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
    {"extern", TokenType::EXTERN},
    {"export", TokenType::EXPORT},
    {"as", TokenType::AS},
    {"is", TokenType::IS},
    {"in", TokenType::IN},
    {"import", TokenType::IMPORT},
    {"from", TokenType::FROM},
    // Type keywords
    {"int", TokenType::INT_TYPE},
    {"int64", TokenType::INT_TYPE},
    {"int8", TokenType::INT8_TYPE},
    {"int16", TokenType::INT16_TYPE},
    {"int32", TokenType::INT32_TYPE},
    {"float", TokenType::FLOAT_TYPE},
    {"float64", TokenType::FLOAT_TYPE},
    {"float32", TokenType::FLOAT32_TYPE},
    {"str", TokenType::STRING_TYPE},
    {"bool", TokenType::BOOL_TYPE},
    {"void", TokenType::VOID_TYPE},
};

auto Token::getIdentifierType(const std::string &identifier, Token *lastTok) -> TokenType {
    // Multi-word keywords first (check lastTok is not null)
    if (lastTok != nullptr) {
        if (identifier == "if" && lastTok->type == TokenType::ELSE) {
            return TokenType::ELSE_IF;
        }
        if (identifier == "not" && lastTok->type == TokenType::IS) {
            return TokenType::IS_NOT;
        }
    }

    auto it = keywords.find(identifier);
    if (it != keywords.end()) {
        return it->second;
    }

    return TokenType::IDENTIFIER;
}