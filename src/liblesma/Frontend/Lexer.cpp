#include "Lexer.h"

#include <cassert>
#include <cstddef>
#include <string>

#include <llvm/Support/SMLoc.h>

#include <fmt/core.h>
#include <fmt/format.h>

#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

void Lexer::scanAll() {
    while (tokens_.empty() || tokens_.back()->type != TokenType::EOF_TOKEN) {
        tokens_.push_back(scanOne(false));
    }
}

Token *Lexer::scanOne(bool continuation) {
    if (isAtEnd()) {
        return new Token{TokenType::EOF_TOKEN, "EOF", llvm::SMRange{begin_loc_, loc_}};
    }
    resetTokenBeg();
    char c = advance();

    switch (c) {
        case '(':
            level_++;
            return addToken(TokenType::LEFT_PAREN);
        case ')':
            level_--;
            return addToken(TokenType::RIGHT_PAREN);
        case '[':
            level_++;
            return addToken(TokenType::LEFT_SQUARE);
        case ']':
            level_--;
            return addToken(TokenType::RIGHT_SQUARE);
        case '{':
            level_++;
            return addToken(TokenType::LEFT_BRACE);
        case '}':
            level_--;
            return addToken(TokenType::RIGHT_BRACE);
        case ':':
            return addToken(TokenType::COLON);
        case ',':
            return addToken(TokenType::COMMA);
        case '.': {
            if (matchAndAdvance(('.'))) {
                if (matchAndAdvance('.')) {
                    return addToken(TokenType::ELLIPSIS);
                }

                return addToken(TokenType::RANGE);
            }

            return addToken(TokenType::DOT);
        }
        case '-': {
            if (matchAndAdvance('>')) {
                return addToken(TokenType::ARROW);
            }
            if (matchAndAdvance('=')) {
                return addToken(TokenType::MINUS_EQUAL);
            }

            return addToken(TokenType::MINUS);
        }
        case '+': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::PLUS_EQUAL);
            }

            return addToken(TokenType::PLUS);
        }
        case ';':
            return addToken(TokenType::SEMICOLON);
        case '*': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::STAR_EQUAL);
            }
            return addToken(TokenType::STAR);
        }
        case '&':
            return addToken(TokenType::AMPERSAND);
        case '!':
            return addToken(matchAndAdvance('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
        case '=': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::EQUAL_EQUAL);
            }
            if (matchAndAdvance('>')) {
                return addToken(TokenType::FAT_ARROW);
            }

            return addToken(TokenType::EQUAL);
        }
        case '<':
            return addToken(matchAndAdvance('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
        case '>':
            return addToken(matchAndAdvance('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
        case '/': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::SLASH_EQUAL);
            }
            return addToken(TokenType::SLASH);
        }
        case '%': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::MOD_EQUAL);
            }
            return addToken(TokenType::MOD);
        }
        case '^': {
            if (matchAndAdvance('=')) {
                return addToken(TokenType::POWER_EQUAL);
            }
            return addToken(TokenType::POWER);
        }
        case '#': {
            // A comment goes until the end of the line.
            while (peek() != '\n' && !isAtEnd()) {
                advance();
            }
            return scanOne(continuation);
        }
        case '\\':
            c = advance();
            continuation = true;

            while (true) {
                if (c == ' ' || c == '\r' || c == '\t') {
                    c = advance();
                } else if (c == '#') {
                    while (peek() != '\n' && !isAtEnd()) {
                        advance();
                    }
                    c = advance();
                    break;
                } else {
                    break;
                }
            }

            if (c != '\n') {
                error(fmt::format("Newline expected after line continuation, found {}", c));
            }

            line_++;
            col_ = 1;

            return scanOne(continuation);
        case ' ':
        case '\r':
        case '\t':
            handleWhitespace(c);
            if (col_ == 2) {
                handleIndentation(false);
            }
            return scanOne(continuation);
        case '\n':
            line_++;
            col_ = 1;
            if (!continuation && level_ == 0) {
                tokens_.push_back(addToken(new Token{TokenType::NEWLINE, "NEWLINE", llvm::SMRange{begin_loc_, loc_}}));
            }
            handleIndentation(continuation);
            return scanOne(false);
        case '"':
            return addStringToken();
        default:
            if (isDigit(c)) {
                return addNumToken();
            } else if (isAlpha(c)) {
                return addIdentifierToken();
            } else {
                error(fmt::format("Unexpected character: {}", c));
            }
    }
    error("Unknown error");
    return {};
}

void Lexer::handleWhitespace(char c) {
    if (!first_indent_char_.has_value()) {
        first_indent_char_ = c;
    }
    if (first_indent_char_ != c) {
        error(fmt::format("Mixed indentation, first indentation character is: {}", first_indent_char_.value()));
    }
    if (c == '\t') {
        col_ += 7;
    }
}

bool Lexer::handleIndentation(bool continuation) {
    const int tabSize = 8;
    int col = 0;
    int altCol = 0;
    char c = 0;
    int changes = 0;
    bool advanced = false;
    for (;;) {
        if (isAtEnd()) {
            break;
        }
        c = advance();
        advanced = true;
        if (c == ' ') {
            ++col;
            ++altCol;
        } else if (c == '\t') {
            col = (col / tabSize + 1) * tabSize;
            altCol += 1;
        } else {
            break;
        }
    }

    if (!isAtEnd() || advanced) {
        fallback();
    }

    if (continuation || level_ != 0 || c == '#' || c == '\n' || c == '\r') {
        if (c == '#' || c == '\n') {
            // If this line is a commented line or an empty line, don't emit NewLine
            if (!tokens_.empty() && tokens_.back()->type == TokenType::NEWLINE) {
                tokens_.pop_back();
            }
        }
        return true;
    }

    if (col == indent_stack_[indent_]) {
        if (altCol != alt_indent_stack_[indent_]) {
            error("Indentation error");
            return false;
        }
    } else if (col > indent_stack_[indent_]) {
        if (altCol <= alt_indent_stack_[indent_]) {
            error("Indentation error");
            return false;
        }
        ++indent_;
        ++changes;
        assert(indent_stack_.size() >= size_t(indent_));
        if (indent_stack_.size() == size_t(indent_)) {
            alt_indent_stack_.push_back(altCol);
            indent_stack_.push_back(col);
        } else {
            alt_indent_stack_[indent_] = altCol;
            indent_stack_[indent_] = col;
        }
    } else {
        while (indent_ > 0 && col < indent_stack_[indent_]) {
            --changes;
            --indent_;
        }
        if (col != indent_stack_[indent_]) {
            error("Dedentation error");
            return false;
        }
        if (altCol != alt_indent_stack_[indent_]) {
            error("Indentation error");
            return false;
        }
    }

    while (changes != 0) {
        tokens_.push_back(addToken(new Token{changes > 0 ? TokenType::INDENT : TokenType::DEDENT, changes > 0 ? "INDENT" : "DEDENT", llvm::SMRange{begin_loc_, loc_}}));
        changes += changes > 0 ? -1 : 1;
    }
    return true;
}

Token *Lexer::addToken(TokenType type) {
    auto *ret = new Token(type, std::string(begin_loc_.getPointer(), loc_.getPointer()), llvm::SMRange{begin_loc_, loc_});
    resetTokenBeg();
    return ret;
}

Token *Lexer::addToken(Token *tok) {
    resetTokenBeg();
    return tok;
}

void Lexer::resetTokenBeg() {
    begin_loc_ = loc_;
}

void Lexer::fallback() {
    loc_ = llvm::SMLoc::getFromPointer(loc_.getPointer() - 1);
    --curPtr_;
    --col_;
}

char Lexer::advance() {
    auto ret = lastChar();
    curPtr_++;
    loc_ = llvm::SMLoc::getFromPointer(loc_.getPointer() + 1);
    ++col_;
    return ret;
}

bool Lexer::matchAndAdvance(char expected) {
    if (isAtEnd()) {
        return false;
    }
    if (lastChar() != expected) {
        return false;
    }

    advance();
    return true;
}

char Lexer::peek(int offset) {
    if ((loc_.getPointer() + offset) >= curBuffer_->getBufferEnd()) {
        return '\0';
    }
    return *(loc_.getPointer() + offset);
}

Token *Lexer::addStringToken() {
    std::string string;

    while (peek() != '"' && !isAtEnd()) {
        // Should we allow newlines in strings? Probably not
        if (peek() == '\n') {
            line_++;
            col_ = 1;
        }
        // If it's not an escape sequence, proceed as usual
        if (peek() != '\\') {
            string.push_back(advance());
            continue;
        }

        switch (peek(1)) {
            case 'n':
                string.push_back('\n');
                break;
            case 'r':
                string.push_back('\r');
                break;
            case 't':
                string.push_back('\t');
                break;
            case 'b':
                string.push_back('\b');
                break;
            case '0':
                string.push_back('\0');
                break;
            case '"':
                string.push_back('"');
                break;
            case 'e':
                string.push_back(0x1B);
                break;
            case '\'':
                string.push_back('\'');
                break;
            case '\\':
                string.push_back('\\');
                break;
            default:
                error("Unknown escape sequence.");
        }

        // Skip the backslash and the escape sequence.
        advance();
        advance();
    }

    if (isAtEnd()) {
        error("Unterminated string.");
    }

    // Skip the closing ".
    advance();

    auto *ret = new Token(TokenType::STRING, string, llvm::SMRange{begin_loc_, loc_});
    resetTokenBeg();
    return ret;
}

Token *Lexer::addNumToken() {
    while (isDigit(peek())) {
        advance();
    }

    // Look for a fractional part.
    if ((peek() == '.') && isDigit(peek(1))) {
        // Consume the "."
        advance();

        while (isDigit(peek())) {
            advance();
        }

        return addToken(TokenType::DOUBLE);
    }

    return addToken(TokenType::INTEGER);
}

Token *Lexer::getLastToken() {
    if (!tokens_.empty()) {
        return tokens_.end()[-1];
    }

    return new Token{TokenType::EOF_TOKEN, "EOF", llvm::SMRange{begin_loc_, loc_}};
}

Token *Lexer::addIdentifierToken() {
    while (isAlphaNumeric(peek())) {
        advance();
    }

    auto *tok = addToken(Token::GetIdentifierType(std::string(begin_loc_.getPointer(), loc_.getPointer()), getLastToken()));

    // If it's a multi-word keyword, remove the last token
    if (tok->type == TokenType::ELSE_IF || tok->type == TokenType::IS_NOT) {
        auto *t = tokens_.back();
        tokens_.pop_back();
        delete t;
    }

    return tok;
}

char Lexer::lastChar() { return *curPtr_; }

void Lexer::error(const std::string &msg) const {
    throw LexerError(llvm::SMRange{begin_loc_, loc_}, msg);
}