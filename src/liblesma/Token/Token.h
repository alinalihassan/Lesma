#pragma once

#include <map>
#include <string>
#include <utility>

#include "nameof.hpp"

#include "TokenType.h"
#include "liblesma/Common/Utils.h"

namespace lesma {

    class TokenState {
    public:
        [[nodiscard]] std::string Dump(std::shared_ptr<llvm::SourceMgr> srcMgr) const;

        std::string lexeme;
        TokenType type;
        llvm::SMRange span;

        bool operator==(const TokenState &rhs) const {
            return (lexeme == rhs.lexeme) && (type == rhs.type) && (span.Start.getPointer() == rhs.span.Start.getPointer()) && (span.End.getPointer() == rhs.span.End.getPointer());
        }
        bool operator!=(const TokenState &rhs) const {
            return !operator==(rhs);
        }

        friend std::ostream &operator<<(std::ostream &os, const TokenState &tok) {
            os << *tok.span.Start.getPointer() << " - " << *tok.span.End.getPointer();
            return os;
        }

    protected:
        friend struct Token;
        explicit TokenState(const TokenType &type, std::string lexeme, llvm::SMRange span)
            : lexeme(std::move(lexeme)), type(type), span(span) {}
    };

    struct Token {
        Token() = default;
        Token(const TokenType &type, const std::string &lexeme, llvm::SMRange loc) {
            state_ = std::shared_ptr<TokenState>(new TokenState(type, lexeme, loc));
        }

        llvm::SMLoc getStart() { return state_->span.Start; }
        llvm::SMLoc getEnd() { return state_->span.End; };

        static TokenType GetIdentifierType(const std::string &identifier, Token lastTok);

        TokenState *operator->() const { return state_.get(); }
        explicit operator bool() const { return static_cast<bool>(state_); }

        bool operator==(const Token &rhs) const {
            return *state_ == *rhs.state_;
        }
        bool operator!=(const Token &rhs) const {
            return !operator==(rhs);
        }
        friend std::ostream &operator<<(std::ostream &os, const Token &tok) {
            os << *tok.state_->span.Start.getPointer() << " - " << *tok.state_->span.End.getPointer();
            return os;
        }

    private:
        explicit Token(std::shared_ptr<TokenState> state) : state_(std::move(state)) {}
        std::shared_ptr<TokenState> state_;
    };

    // Token is designed to be shared ptr like, and only contains one data member
    static_assert(sizeof(Token) == sizeof(std::shared_ptr<TokenState>));
}// namespace lesma