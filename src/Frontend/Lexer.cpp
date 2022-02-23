#include "Lexer.h"

using namespace lesma;

std::vector<Token> Lexer::ScanAll() {
    std::vector<Token> output;
    while (output.empty() || output.back()->type != TokenType::EOF_TOKEN)
        output.push_back(ScanOne());
    return output;
}

Token Lexer::ScanOne() {
    if (IsAtEnd())
        return Token{TokenType::EOF_TOKEN, "EOF", loc};
    ResetTokenBeg();
    char c = Advance();
    switch (c) {
        case '(': return AddToken(TokenType::LEFT_PAREN);
        case ')': return AddToken(TokenType::RIGHT_PAREN);
        case '[': return AddToken(TokenType::LEFT_SQUARE);
        case ']': return AddToken(TokenType::RIGHT_SQUARE);
        case '{': return AddToken(TokenType::LEFT_BRACE);
        case '}': return AddToken(TokenType::RIGHT_BRACE);
        case ':': return AddToken(TokenType::COLON);
        case ',': return AddToken(TokenType::COMMA);
        case '.': {
            if (MatchAndAdvance(('.'))) {
                if (MatchAndAdvance('.'))
                    return AddToken(TokenType::ELLIPSIS);
                else
                    return AddToken(TokenType::RANGE);
            } else
                return AddToken(TokenType::DOT);
        }
        case '-': {
            if (MatchAndAdvance('>'))
                return AddToken(TokenType::ARROW);
            else if (MatchAndAdvance('='))
                return AddToken(TokenType::MINUS_EQUAL);

            return AddToken( TokenType::MINUS);
        }
        case '+': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::PLUS_EQUAL);

            return AddToken(TokenType::PLUS);
        }
        case ';': return AddToken(TokenType::SEMICOLON);
        case '*': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::STAR_EQUAL);
            return AddToken(TokenType::STAR);
        }
        case '!': return AddToken(MatchAndAdvance('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
        case '=': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::EQUAL_EQUAL);
            else if (MatchAndAdvance('>'))
                return AddToken(TokenType::FAT_ARROW);

            return AddToken(TokenType::EQUAL);
        }
        case '<': return AddToken(MatchAndAdvance('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
        case '>': return AddToken(MatchAndAdvance('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
        case '/': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::SLASH_EQUAL);
            return AddToken(TokenType::SLASH);
        }
        case '%': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::MOD_EQUAL);
            return AddToken(TokenType::MOD);
        }
        case '^': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::POWER_EQUAL);
            return AddToken(TokenType::POWER);
        }
        case '#': {
            // A comment goes until the end of the line.
            while (Peek() != '\n' && !IsAtEnd()) Advance();
            return ScanOne();
        }
        case ' ':
        case '\r':
        case '\t':
            return ScanOne();
        case '\n':
            loc.Line++;
            loc.Col = 0;
            return ScanOne();
        case '"':
            return AddStringToken();
        default:
            if (IsDigit(c))
                return AddNumToken();
            else if (IsAlpha(c))
                return AddIdentifierToken();
            else
                Error(fmt::format("Unexpected character: {}", c));
    }
    Error("Unknown error");
    return {};
}

// TODO: Could possibly make it more efficient
Token Lexer::AddToken(TokenType type) {
    auto ret = Token(type, std::string(srcs_->cbegin() + start_lex_pos_, srcs_->cbegin() + current_lex_pos_), loc);
    ResetTokenBeg();
    return ret;
}

void Lexer::ResetTokenBeg() { start_lex_pos_ = current_lex_pos_; }

char Lexer::Advance() {
    auto ret = LastChar();
    ++current_lex_pos_;
    ++loc.Col;
    return ret;
}

bool Lexer::MatchAndAdvance(char expected) {
    if (IsAtEnd()) return false;
    if (LastChar() != expected) return false;
    Advance();
    return true;
}

char Lexer::Peek(int offset) {
    if ((current_lex_pos_ + offset) >= srcs_->size()) return '\0';
    return srcs_->at(current_lex_pos_ + offset);
}

Token Lexer::AddStringToken() {
    while (Peek() != '"' && !IsAtEnd()) {
        if (Peek() == '\n') {
            loc.Line++;
            loc.Col = 0;
        }
        Advance();
    }

    if (IsAtEnd())
        Error("Unterminated string.");

    // The closing ".
    Advance();

    return AddToken(TokenType::STRING);
}
Token Lexer::AddNumToken() {
    while (IsDigit(Peek())) Advance();

    // Look for a fractional part.
    if ((Peek() == '.') && IsDigit(Peek(1))) {
        // Consume the "."
        Advance();

        while (IsDigit(Peek())) Advance();

        return AddToken(TokenType::DOUBLE);
    } else {
        return AddToken(TokenType::INTEGER);
    }
}

Token Lexer::AddIdentifierToken() {
    while (IsAlphaNumeric(Peek())) Advance();

    return AddToken(Token::GetIdentifierType(std::string(srcs_->cbegin() + start_lex_pos_, srcs_->cbegin() + current_lex_pos_)));
}

char Lexer::LastChar() { return srcs_->at(current_lex_pos_); }

void Lexer::Error(const std::string &msg) const {
    throw LexerError("[line {}, col {}] {}\n", loc.Line + 1, loc.Col + 1, msg);
}