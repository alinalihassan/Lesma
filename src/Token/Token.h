#pragma once

#include <map>
#include <string>
#include <utility>

#include "nameof.hpp"

#include "Common/Utils.h"
#include "TokenType.h"

namespace lesma {

    class TokenState {
    public:
        [[nodiscard]] std::string Dump() const;

        std::string lexeme;
        TokenType type;
        SourceLocation loc;

        bool operator==(const TokenState& rhs) const {
            return (lexeme == rhs.lexeme) && (type == rhs.type) && (loc == rhs.loc);
        }
        bool operator!=(const TokenState& rhs) const {
            return !operator==(rhs);
        }
        friend std::ostream& operator<<(std::ostream& os, const TokenState& tok) {
            os << tok.Dump();
            return os;
        }

    protected:
        friend struct Token;
        explicit TokenState(const TokenType &type, std::string lexeme, SourceLocation loc)
            : lexeme(std::move(lexeme)), type(type), loc(loc) {}
    };

    struct Token {
        Token() = default;
        Token(const TokenType &type, const std::string &lexeme, SourceLocation loc) {
            state_ = std::shared_ptr<TokenState>(new TokenState(type, lexeme, loc));
        }

        static TokenType GetIdentifierType(const std::string &identifier);

        TokenState *operator->() const { return state_.get(); }
        explicit operator bool() const { return static_cast<bool>(state_); }

        bool operator==(const Token& rhs) const {
            return *state_ == *rhs.state_;
        }
        bool operator!=(const Token& rhs) const {
            return !operator==(rhs);
        }
        friend std::ostream& operator<<(std::ostream& os, const Token& tok) {
            os << tok.state_->Dump();
            return os;
        }

    private:
        explicit Token(std::shared_ptr<TokenState> state) : state_(std::move(state)) {}
        std::shared_ptr<TokenState> state_;
    };

    // Token is designed to be shared ptr like, and only contains one data member
    static_assert(sizeof(Token) == sizeof(std::shared_ptr<TokenState>));
}// namespace lesma