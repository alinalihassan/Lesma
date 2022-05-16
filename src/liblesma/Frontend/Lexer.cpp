#include "Lexer.h"

using namespace lesma;

std::vector<Token> Lexer::ScanAll() {
    while (tokens.empty() || tokens.back().type != TokenType::EOF_TOKEN)
        tokens.push_back(ScanOne(false));
    return tokens;
}

Token Lexer::ScanOne(bool continuation) {
    if (IsAtEnd())
        return Token{TokenType::EOF_TOKEN, "EOF", llvm::SMRange{begin_loc, loc}};
    ResetTokenBeg();
    char c = Advance();

    switch (c) {
        case '(':
            level_++;
            return AddToken(TokenType::LEFT_PAREN);
        case ')':
            level_--;
            return AddToken(TokenType::RIGHT_PAREN);
        case '[':
            level_++;
            return AddToken(TokenType::LEFT_SQUARE);
        case ']':
            level_--;
            return AddToken(TokenType::RIGHT_SQUARE);
        case '{':
            level_++;
            return AddToken(TokenType::LEFT_BRACE);
        case '}':
            level_--;
            return AddToken(TokenType::RIGHT_BRACE);
        case ':':
            return AddToken(TokenType::COLON);
        case ',':
            return AddToken(TokenType::COMMA);
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

            return AddToken(TokenType::MINUS);
        }
        case '+': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::PLUS_EQUAL);

            return AddToken(TokenType::PLUS);
        }
        case ';':
            return AddToken(TokenType::SEMICOLON);
        case '*': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::STAR_EQUAL);
            return AddToken(TokenType::STAR);
        }
        case '!':
            return AddToken(MatchAndAdvance('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
        case '=': {
            if (MatchAndAdvance('='))
                return AddToken(TokenType::EQUAL_EQUAL);
            else if (MatchAndAdvance('>'))
                return AddToken(TokenType::FAT_ARROW);

            return AddToken(TokenType::EQUAL);
        }
        case '<':
            return AddToken(MatchAndAdvance('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
        case '>':
            return AddToken(MatchAndAdvance('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
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
            return ScanOne(continuation);
        }
        case '\\':
            c = Advance();
            continuation = true;

            // TODO: Keep it DRY
            while (true) {
                if (c == ' ' || c == '\r' || c == '\t')
                    c = Advance();
                else if (c == '#') {
                    while (Peek() != '\n' && !IsAtEnd()) c = Advance();
                    c = Advance();
                    break;
                } else
                    break;
            }

            if (c != '\n')
                Error(fmt::format("Newline expected after line continuation, found {}", c));

            line++;
            col = 1;

            return ScanOne(continuation);
        case ' ':
        case '\r':
        case '\t':
            HandleWhitespace(c);
            if (col == 2)
                HandleIndentation(false);
            return ScanOne(continuation);
        case '\n':
            line++;
            col = 1;
            if (!continuation && level_ == 0)
                tokens.push_back(AddToken({TokenType::NEWLINE, "NEWLINE", llvm::SMRange{begin_loc, loc}}));
            HandleIndentation(continuation);
            return ScanOne(false);
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

void Lexer::HandleWhitespace(char c) {
    if (!first_indent_char.has_value()) {
        first_indent_char = c;
    }
    if (first_indent_char != c)
        Error(fmt::format("Mixed indentation, first indentation character is: {}", first_indent_char.value()));
    if (c == '\t')
        col += 7;
}

bool Lexer::HandleIndentation(bool continuation) {
    const int tab_size = 8;
    int _col = 0, alt_col = 0;
    char c = 0;
    int changes = 0;
    bool advanced = false;
    for (;;) {
        if (IsAtEnd())
            break;
        c = Advance();
        advanced = true;
        if (c == ' ') {
            ++_col;
            ++alt_col;
        } else if (c == '\t') {
            _col = (_col / tab_size + 1) * tab_size;
            alt_col += 1;
        } else {
            break;
        }
    }
    if (!IsAtEnd() || advanced)
        Fallback();

    if (continuation || level_ != 0 || c == '#' || c == '\n' || c == '\r') {
        if (c == '#' || c == '\n') {
            // If this line is a commented line or an empty line, don't emit NewLine
            if (!tokens.empty() && tokens.back().type == TokenType::NEWLINE) {
                tokens.pop_back();
            }
        }
        return true;
    }

    if (_col == indent_stack_[indent_]) {
        if (alt_col != alt_indent_stack_[indent_]) {
            Error("Indentation error");
            return false;
        }
    } else if (_col > indent_stack_[indent_]) {
        if (alt_col <= alt_indent_stack_[indent_]) {
            Error("Indentation error");
            return false;
        }
        ++indent_;
        ++changes;
        assert(indent_stack_.size() >= size_t(indent_));
        if (indent_stack_.size() == size_t(indent_)) {
            alt_indent_stack_.push_back(alt_col);
            indent_stack_.push_back(_col);
        } else {
            alt_indent_stack_[indent_] = alt_col;
            indent_stack_[indent_] = _col;
        }
    } else {
        while (indent_ > 0 && _col < indent_stack_[indent_]) {
            --changes;
            --indent_;
        }
        if (_col != indent_stack_[indent_]) {
            Error("Dedentation error");
            return false;
        }
        if (alt_col != alt_indent_stack_[indent_]) {
            Error("Indentation error");
            return false;
        }
    }

    while (changes != 0) {
        tokens.push_back(AddToken({changes > 0 ? TokenType::INDENT : TokenType::DEDENT, changes > 0 ? "INDENT" : "DEDENT", llvm::SMRange{begin_loc, loc}}));
        changes += changes > 0 ? -1 : 1;
    }
    return true;
}

// TODO: Could possibly make it more efficient
Token Lexer::AddToken(TokenType type) {
    auto ret = Token(type, std::string(begin_loc.getPointer(), loc.getPointer()), llvm::SMRange{begin_loc, loc});
    ResetTokenBeg();
    return ret;
}

Token Lexer::AddToken(Token tok) {
    ResetTokenBeg();
    return tok;
}

void Lexer::ResetTokenBeg() {
    begin_loc = loc;
}

void Lexer::Fallback() {
    loc = llvm::SMLoc::getFromPointer(loc.getPointer() - 1);
    --curPtr;
    --col;
}

char Lexer::Advance() {
    auto ret = LastChar();
    curPtr++;
    loc = llvm::SMLoc::getFromPointer(loc.getPointer() + 1);
    ++col;
    return ret;
}

bool Lexer::MatchAndAdvance(char expected) {
    if (IsAtEnd()) return false;
    if (LastChar() != expected) return false;
    Advance();
    return true;
}

char Lexer::Peek(int offset) {
    if ((loc.getPointer() + offset) >= curBuffer->getBufferEnd()) return '\0';
    return *(loc.getPointer() + offset);
}

Token Lexer::AddStringToken() {
    std::string string;

    while (Peek() != '"' && !IsAtEnd()) {
        // Should we allow newlines in strings? Probably not
        if (Peek() == '\n') {
            line++;
            col = 1;
        }
        // If it's not an escape sequence, proceed as usual
        if (Peek() != '\\') {
            string.push_back(Advance());
            continue;
        }

        switch (Peek(1)) {
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
                Error("Unknown escape sequence.");
        }

        // Skip the backslash and the escape sequence.
        Advance();
        Advance();
    }

    if (IsAtEnd())
        Error("Unterminated string.");

    // Skip the closing ".
    Advance();

    auto ret = Token(TokenType::STRING, string, llvm::SMRange{begin_loc, loc});
    ResetTokenBeg();
    return ret;
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

Token Lexer::GetLastToken() {
    if (!tokens.empty())
        return tokens.end()[-1];
    else
        return Token{TokenType::EOF_TOKEN, "EOF", llvm::SMRange{begin_loc, loc}};
}

Token Lexer::AddIdentifierToken() {
    while (IsAlphaNumeric(Peek())) Advance();

    auto tok = AddToken(Token::GetIdentifierType(std::string(begin_loc.getPointer(), loc.getPointer()), GetLastToken()));

    // If it's an 'else if' multiword keyword, remove the last token (which is an 'else' in this case)
    if (tok.type == TokenType::ELSE_IF)
        tokens.pop_back();

    return tok;
}

char Lexer::LastChar() { return *curPtr; }

void Lexer::Error(const std::string &msg) const {
    throw LexerError(llvm::SMRange{begin_loc, loc}, msg);
}