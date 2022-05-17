#include "Token.h"

using namespace lesma;

std::string Token::Dump(const std::shared_ptr<llvm::SourceMgr> &srcMgr) const {
    return std::string(
                   "[Type: ") +
           std::string{NAMEOF_ENUM(type)} +
           ", Lexeme: " + lexeme +
           ", Line: " + std::to_string(srcMgr->getLineAndColumn(span.Start, srcMgr->getNumBuffers() - 1).first) + " - " + std::to_string(srcMgr->getLineAndColumn(span.End, srcMgr->getNumBuffers() - 1).first) +
           ", Col: " + std::to_string(srcMgr->getLineAndColumn(span.Start, srcMgr->getNumBuffers() - 1).second) + " - " + std::to_string(srcMgr->getLineAndColumn(span.End, srcMgr->getNumBuffers() - 1).second) + "]";
}

TokenType Token::GetIdentifierType(const std::string &identifier, Token *lastTok) {
    if (identifier == "and")
        return TokenType::AND;
    else if (identifier == "class")
        return TokenType::CLASS;
    else if (identifier == "enum")
        return TokenType::ENUM;
    else if (identifier == "else")
        return TokenType::ELSE;
    else if (identifier == "if" and lastTok->type == TokenType::ELSE)
        return TokenType::ELSE_IF;
    else if (identifier == "false")
        return TokenType::FALSE_;
    else if (identifier == "for")
        return TokenType::FOR;
    else if (identifier == "def")
        return TokenType::DEF;
    else if (identifier == "defer")
        return TokenType::DEFER;
    else if (identifier == "func")
        return TokenType::FUNC;
    else if (identifier == "if")
        return TokenType::IF;
    else if (identifier == "not")
        return TokenType::NOT;
    else if (identifier == "null")
        return TokenType::NIL;
    else if (identifier == "or")
        return TokenType::OR;
    else if (identifier == "return")
        return TokenType::RETURN;
    else if (identifier == "this")
        return TokenType::THIS;
    else if (identifier == "true")
        return TokenType::TRUE_;
    else if (identifier == "var")
        return TokenType::VAR;
    else if (identifier == "let")
        return TokenType::LET;
    else if (identifier == "while")
        return TokenType::WHILE;
    else if (identifier == "break")
        return TokenType::BREAK;
    else if (identifier == "continue")
        return TokenType::CONTINUE;
    else if (identifier == "super")
        return TokenType::SUPER;
    else if (identifier == "extern")
        return TokenType::EXTERN_FUNC;
    else if (identifier == "as")
        return TokenType::AS;
    else if (identifier == "is")
        return TokenType::IS;
    else if (identifier == "in")
        return TokenType::IN;
    else if (identifier == "int")
        return TokenType::INT_TYPE;
    else if (identifier == "float")
        return TokenType::FLOAT_TYPE;
    else if (identifier == "str")
        return TokenType::STRING_TYPE;
    else if (identifier == "bool")
        return TokenType::BOOL_TYPE;
    else if (identifier == "void")
        return TokenType::VOID_TYPE;
    else if (identifier == "import")
        return TokenType::IMPORT;
    else
        return TokenType::IDENTIFIER;
}