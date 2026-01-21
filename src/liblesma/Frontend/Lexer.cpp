#include "Lexer.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <llvm/Support/SMLoc.h>

#include <fmt/core.h>
#include <fmt/format.h>

#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

auto Lexer::scanAll() -> void {
    while (tokens_.empty() || tokens_.back()->type != TokenType::EOF_TOKEN) {
        tokens_.push_back(scanOne(false));
    }
}

auto Lexer::getTokens() -> std::vector<Token *> {
    std::vector<Token *> result;
    result.reserve(tokens_.size());
    for (const auto &tok: tokens_) {
        result.push_back(tok.get());
    }
    return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::scanOne(bool continuation) -> std::unique_ptr<Token> {
    if (isAtEnd()) {
        return std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF", llvm::SMRange{begin_loc_, loc_});
    }
    resetTokenBeg();
    char c = advance();

    switch (c) {
        case '(':
            level_++;
            return makeToken(TokenType::LEFT_PAREN);
        case ')':
            level_--;
            return makeToken(TokenType::RIGHT_PAREN);
        case '[':
            level_++;
            return makeToken(TokenType::LEFT_SQUARE);
        case ']':
            level_--;
            return makeToken(TokenType::RIGHT_SQUARE);
        case '{':
            level_++;
            return makeToken(TokenType::LEFT_BRACE);
        case '}':
            level_--;
            return makeToken(TokenType::RIGHT_BRACE);
        case ':':
            return makeToken(TokenType::COLON);
        case ',':
            return makeToken(TokenType::COMMA);
        case '.': {
            if (matchAndAdvance(('.'))) {
                if (matchAndAdvance('.')) {
                    return makeToken(TokenType::ELLIPSIS);
                }

                return makeToken(TokenType::RANGE);
            }

            return makeToken(TokenType::DOT);
        }
        case '-': {
            if (matchAndAdvance('>')) {
                return makeToken(TokenType::ARROW);
            }
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::MINUS_EQUAL);
            }

            return makeToken(TokenType::MINUS);
        }
        case '+': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::PLUS_EQUAL);
            }

            return makeToken(TokenType::PLUS);
        }
        case ';':
            return makeToken(TokenType::SEMICOLON);
        case '*': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::STAR_EQUAL);
            }
            return makeToken(TokenType::STAR);
        }
        case '&':
            return makeToken(TokenType::AMPERSAND);
        case '!':
            return makeToken(matchAndAdvance('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
        case '=': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::EQUAL_EQUAL);
            }
            if (matchAndAdvance('>')) {
                return makeToken(TokenType::FAT_ARROW);
            }

            return makeToken(TokenType::EQUAL);
        }
        case '<':
            return makeToken(matchAndAdvance('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
        case '>':
            return makeToken(matchAndAdvance('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
        case '/': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::SLASH_EQUAL);
            }
            return makeToken(TokenType::SLASH);
        }
        case '%': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::MOD_EQUAL);
            }
            return makeToken(TokenType::MOD);
        }
        case '^': {
            if (matchAndAdvance('=')) {
                return makeToken(TokenType::POWER_EQUAL);
            }
            return makeToken(TokenType::POWER);
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
                tokens_.push_back(std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", llvm::SMRange{begin_loc_, loc_}));
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
    return nullptr;
}

auto Lexer::handleWhitespace(char c) -> void {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::handleIndentation(bool continuation) -> bool {
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
        tokens_.push_back(std::make_unique<Token>(
                changes > 0 ? TokenType::INDENT : TokenType::DEDENT,
                changes > 0 ? "INDENT" : "DEDENT",
                llvm::SMRange{begin_loc_, loc_}));
        changes += changes > 0 ? -1 : 1;
    }
    return true;
}

auto Lexer::makeToken(TokenType type) -> std::unique_ptr<Token> {
    auto token = std::make_unique<Token>(type, std::string(begin_loc_.getPointer(), loc_.getPointer()), llvm::SMRange{begin_loc_, loc_});
    resetTokenBeg();
    return token;
}

auto Lexer::makeToken(TokenType type, const std::string &value) -> std::unique_ptr<Token> {
    auto token = std::make_unique<Token>(type, value, llvm::SMRange{begin_loc_, loc_});
    resetTokenBeg();
    return token;
}

auto Lexer::resetTokenBeg() -> void {
    begin_loc_ = loc_;
}

auto Lexer::fallback() -> void {
    --curPos_;
    loc_ = llvm::SMLoc::getFromPointer(getLocPointer());
    --col_;
}

auto Lexer::advance() -> char {
    auto ret = lastChar();
    ++curPos_;
    loc_ = llvm::SMLoc::getFromPointer(getLocPointer());
    ++col_;
    return ret;
}

auto Lexer::matchAndAdvance(char expected) -> bool {
    if (isAtEnd()) {
        return false;
    }
    if (lastChar() != expected) {
        return false;
    }

    advance();
    return true;
}

auto Lexer::peek(int offset) -> char {
    size_t targetPos = curPos_ + static_cast<size_t>(offset);
    if (targetPos >= curBuffer_->getBufferSize()) {
        return '\0';
    }
    return getCharAt(targetPos);
}

auto Lexer::addStringToken() -> std::unique_ptr<Token> {
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

    return makeToken(TokenType::STRING, string);
}

auto Lexer::addNumToken() -> std::unique_ptr<Token> {
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

        return makeToken(TokenType::DOUBLE);
    }

    return makeToken(TokenType::INTEGER);
}

auto Lexer::getLastToken() -> Token * {
    if (!tokens_.empty()) {
        return tokens_.back().get();
    }
    return nullptr;
}

auto Lexer::addIdentifierToken() -> std::unique_ptr<Token> {
    while (isAlphaNumeric(peek())) {
        advance();
    }

    TokenType type = Token::getIdentifierType(std::string(begin_loc_.getPointer(), loc_.getPointer()), getLastToken());
    auto tok = makeToken(type);

    // If it's a multi-word keyword, remove the last token
    if (tok->type == TokenType::ELSE_IF || tok->type == TokenType::IS_NOT) {
        tokens_.pop_back();// unique_ptr automatically deletes
    }

    return tok;
}

auto Lexer::lastChar() -> char { return getCharAt(curPos_); }

auto Lexer::error(const std::string &msg) const -> void {
    throw LexerError(llvm::SMRange{begin_loc_, loc_}, msg);
}
