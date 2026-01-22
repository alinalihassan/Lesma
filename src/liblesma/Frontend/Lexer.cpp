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

auto Lexer::ScanAll() -> void {
  while (tokens.empty() || tokens.back()->type != TokenType::EOF_TOKEN) {
    tokens.push_back(ScanOne(false));
  }
}

auto Lexer::GetTokens() -> std::vector<Token*> {
  std::vector<Token*> result;
  result.reserve(tokens.size());
  for (const auto& tok : tokens) {
    result.push_back(tok.get());
  }
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::ScanOne(bool continuation) -> std::unique_ptr<Token> {
  if (IsAtEnd()) {
    return std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF",
                                   llvm::SMRange{beginLoc, loc});
  }
  ResetTokenBeg();
  char c = Advance();

  switch (c) {
  case '(':
    level++;
    return MakeToken(TokenType::LEFT_PAREN);
  case ')':
    level--;
    return MakeToken(TokenType::RIGHT_PAREN);
  case '[':
    level++;
    return MakeToken(TokenType::LEFT_SQUARE);
  case ']':
    level--;
    return MakeToken(TokenType::RIGHT_SQUARE);
  case '{':
    level++;
    return MakeToken(TokenType::LEFT_BRACE);
  case '}':
    level--;
    return MakeToken(TokenType::RIGHT_BRACE);
  case ':':
    return MakeToken(TokenType::COLON);
  case ',':
    return MakeToken(TokenType::COMMA);
  case '.': {
    if (MatchAndAdvance(('.'))) {
      if (MatchAndAdvance('.')) {
        return MakeToken(TokenType::ELLIPSIS);
      }

      return MakeToken(TokenType::RANGE);
    }

    return MakeToken(TokenType::DOT);
  }
  case '-': {
    if (MatchAndAdvance('>')) {
      return MakeToken(TokenType::ARROW);
    }
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::MINUS_EQUAL);
    }

    return MakeToken(TokenType::MINUS);
  }
  case '+': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::PLUS_EQUAL);
    }

    return MakeToken(TokenType::PLUS);
  }
  case ';':
    return MakeToken(TokenType::SEMICOLON);
  case '*': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::STAR_EQUAL);
    }
    return MakeToken(TokenType::STAR);
  }
  case '&':
    return MakeToken(TokenType::AMPERSAND);
  case '!':
    return MakeToken(MatchAndAdvance('=') ? TokenType::BANG_EQUAL
                                          : TokenType::BANG);
  case '=': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::EQUAL_EQUAL);
    }
    if (MatchAndAdvance('>')) {
      return MakeToken(TokenType::FAT_ARROW);
    }

    return MakeToken(TokenType::EQUAL);
  }
  case '<':
    return MakeToken(MatchAndAdvance('=') ? TokenType::LESS_EQUAL
                                          : TokenType::LESS);
  case '>':
    return MakeToken(MatchAndAdvance('=') ? TokenType::GREATER_EQUAL
                                          : TokenType::GREATER);
  case '/': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::SLASH_EQUAL);
    }
    return MakeToken(TokenType::SLASH);
  }
  case '%': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::MOD_EQUAL);
    }
    return MakeToken(TokenType::MOD);
  }
  case '^': {
    if (MatchAndAdvance('=')) {
      return MakeToken(TokenType::POWER_EQUAL);
    }
    return MakeToken(TokenType::POWER);
  }
  case '#': {
    // A comment goes until the end of the line.
    while (Peek() != '\n' && !IsAtEnd()) {
      Advance();
    }
    return ScanOne(continuation);
  }
  case '\\':
    c = Advance();
    continuation = true;

    while (true) {
      if (c == ' ' || c == '\r' || c == '\t') {
        c = Advance();
      } else if (c == '#') {
        while (Peek() != '\n' && !IsAtEnd()) {
          Advance();
        }
        c = Advance();
        break;
      } else {
        break;
      }
    }

    if (c != '\n') {
      Error(
          fmt::format("Newline expected after line continuation, found {}", c));
    }

    line++;
    col = 1;

    return ScanOne(continuation);
  case ' ':
  case '\r':
  case '\t':
    HandleWhitespace(c);
    if (col == 2) {
      HandleIndentation(false);
    }
    return ScanOne(continuation);
  case '\n':
    line++;
    col = 1;
    if (!continuation && level == 0) {
      tokens.push_back(std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE",
                                               llvm::SMRange{beginLoc, loc}));
    }
    HandleIndentation(continuation);
    return ScanOne(false);
  case '"':
    return AddStringToken();
  default:
    if (IsDigit(c)) {
      return AddNumToken();
    } else if (IsAlpha(c)) {
      return AddIdentifierToken();
    } else {
      Error(fmt::format("Unexpected character: {}", c));
    }
  }
  Error("Unknown error");
  return nullptr;
}

auto Lexer::HandleWhitespace(char c) -> void {
  if (!firstIndentChar.has_value()) {
    firstIndentChar = c;
  }
  if (firstIndentChar != c) {
    Error(fmt::format("Mixed indentation, first indentation character is: {}",
                      firstIndentChar.value()));
  }
  if (c == '\t') {
    col += 7;
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::HandleIndentation(bool continuation) -> bool {
  const int tabSize = 8;
  int col = 0;
  int altCol = 0;
  char c = 0;
  int changes = 0;
  bool advanced = false;
  for (;;) {
    if (IsAtEnd()) {
      break;
    }
    c = Advance();
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

  if (!IsAtEnd() || advanced) {
    Fallback();
  }

  if (continuation || level != 0 || c == '#' || c == '\n' || c == '\r') {
    if (c == '#' || c == '\n') {
      // If this line is a commented line or an empty line, don't emit NewLine
      if (!tokens.empty() && tokens.back()->type == TokenType::NEWLINE) {
        tokens.pop_back();
      }
    }
    return true;
  }

  if (col == indentStack[indent]) {
    if (altCol != altIndentStack[indent]) {
      Error("Indentation error");
      return false;
    }
  } else if (col > indentStack[indent]) {
    if (altCol <= altIndentStack[indent]) {
      Error("Indentation error");
      return false;
    }
    ++indent;
    ++changes;
    assert(indentStack.size() >= size_t(indent));
    if (indentStack.size() == size_t(indent)) {
      altIndentStack.push_back(altCol);
      indentStack.push_back(col);
    } else {
      altIndentStack[indent] = altCol;
      indentStack[indent] = col;
    }
  } else {
    while (indent > 0 && col < indentStack[indent]) {
      --changes;
      --indent;
    }
    if (col != indentStack[indent]) {
      Error("Dedentation error");
      return false;
    }
    if (altCol != altIndentStack[indent]) {
      Error("Indentation error");
      return false;
    }
  }

  while (changes != 0) {
    tokens.push_back(std::make_unique<Token>(
        changes > 0 ? TokenType::INDENT : TokenType::DEDENT,
        changes > 0 ? "INDENT" : "DEDENT", llvm::SMRange{beginLoc, loc}));
    changes += changes > 0 ? -1 : 1;
  }
  return true;
}

auto Lexer::MakeToken(TokenType type) -> std::unique_ptr<Token> {
  auto token = std::make_unique<Token>(
      type, std::string(beginLoc.getPointer(), loc.getPointer()),
      llvm::SMRange{beginLoc, loc});
  ResetTokenBeg();
  return token;
}

auto Lexer::MakeToken(TokenType type, const std::string& value)
    -> std::unique_ptr<Token> {
  auto token =
      std::make_unique<Token>(type, value, llvm::SMRange{beginLoc, loc});
  ResetTokenBeg();
  return token;
}

auto Lexer::ResetTokenBeg() -> void { beginLoc = loc; }

auto Lexer::Fallback() -> void {
  --curPos;
  loc = llvm::SMLoc::getFromPointer(GetLocPointer());
  --col;
}

auto Lexer::Advance() -> char {
  auto ret = LastChar();
  ++curPos;
  loc = llvm::SMLoc::getFromPointer(GetLocPointer());
  ++col;
  return ret;
}

auto Lexer::MatchAndAdvance(char expected) -> bool {
  if (IsAtEnd()) {
    return false;
  }
  if (LastChar() != expected) {
    return false;
  }

  Advance();
  return true;
}

auto Lexer::Peek(int offset) -> char {
  size_t const targetPos = curPos + static_cast<size_t>(offset);
  if (targetPos >= curBuffer->getBufferSize()) {
    return '\0';
  }
  return GetCharAt(targetPos);
}

auto Lexer::AddStringToken() -> std::unique_ptr<Token> {
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

  if (IsAtEnd()) {
    Error("Unterminated string.");
  }

  // Skip the closing ".
  Advance();

  return MakeToken(TokenType::STRING, string);
}

auto Lexer::AddNumToken() -> std::unique_ptr<Token> {
  while (IsDigit(Peek())) {
    Advance();
  }

  // Look for a fractional part.
  if ((Peek() == '.') && IsDigit(Peek(1))) {
    // Consume the "."
    Advance();

    while (IsDigit(Peek())) {
      Advance();
    }

    return MakeToken(TokenType::DOUBLE);
  }

  return MakeToken(TokenType::INTEGER);
}

auto Lexer::GetLastToken() -> Token* {
  if (!tokens.empty()) {
    return tokens.back().get();
  }
  return nullptr;
}

auto Lexer::AddIdentifierToken() -> std::unique_ptr<Token> {
  while (IsAlphaNumeric(Peek())) {
    Advance();
  }

  TokenType const type = Token::GetIdentifierType(
      std::string(beginLoc.getPointer(), loc.getPointer()), GetLastToken());
  auto tok = MakeToken(type);

  // If it's a multi-word keyword, remove the last token
  if (tok->type == TokenType::ELSE_IF || tok->type == TokenType::IS_NOT) {
    tokens.pop_back(); // unique_ptr automatically deletes
  }

  return tok;
}

auto Lexer::LastChar() -> char { return GetCharAt(curPos); }

auto Lexer::Error(const std::string& msg) const -> void {
  throw LexerError(llvm::SMRange{beginLoc, loc}, msg);
}
