#pragma once

#include <map>
#include <string>
#include <utility>

#include "nameof.hpp"

#include "TokenType.h"
#include "liblesma/Common/Utils.h"

namespace lesma {
    struct Token {
        std::string lexeme;
        TokenType type;
        llvm::SMRange span;

        Token() = default;
        Token(const TokenType &type, std::string lexeme, llvm::SMRange span): lexeme(std::move(lexeme)), type(type), span(span) {}

        [[nodiscard]] llvm::SMLoc getStart() const { return span.Start; }
        [[nodiscard]] llvm::SMLoc getEnd() const { return span.End; };

        static TokenType GetIdentifierType(const std::string &identifier, const Token& lastTok);
        [[nodiscard]] std::string Dump(const std::shared_ptr<llvm::SourceMgr>& srcMgr) const;

        bool operator==(const Token &rhs) const {
            return (lexeme == rhs.lexeme) && (type == rhs.type) && (span.Start.getPointer() == rhs.span.Start.getPointer()) && (span.End.getPointer() == rhs.span.End.getPointer());
        }
        bool operator!=(const Token &rhs) const {
            return !operator==(rhs);
        }

        friend std::ostream &operator<<(std::ostream &os, const Token &tok) {
            os << *tok.span.Start.getPointer() << " - " << *tok.span.End.getPointer();
            return os;
        }
    };
}// namespace lesma