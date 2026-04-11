#include "Lexer.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/SMLoc.h>

#include <fmt/core.h>
#include <fmt/format.h>

#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

auto Lexer::scanAll() -> void {
  while (tokens.empty() || tokens.back()->type != TokenType::EOF_TOKEN) {
    tokens.push_back(scanOne(false));
  }
}

auto Lexer::getTokens() -> std::vector<Token*> {
  std::vector<Token*> result;
  result.reserve(tokens.size());
  for (const auto& tok : tokens) {
    result.push_back(tok.get());
  }
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::scanOne(bool continuation) -> std::unique_ptr<Token> {
  if (!pendingTokens.empty()) {
    auto t = std::move(pendingTokens.front());
    pendingTokens.pop_front();
    return t;
  }
  if (resumeTemplateStringChunk) {
    resumeTemplateStringChunk = false;
    return continueTemplateStringChunk();
  }
  if (isAtEnd()) {
    return std::make_unique<Token>(TokenType::EOF_TOKEN, "EOF", llvm::SMRange{beginLoc, loc});
  }
  resetTokenBeg();
  char c = advance();

  switch (c) {
  case '(':
    level++;
    return makeToken(TokenType::LEFT_PAREN);
  case ')':
    level--;
    return makeToken(TokenType::RIGHT_PAREN);
  case '[':
    level++;
    return makeToken(TokenType::LEFT_SQUARE);
  case ']':
    level--;
    return makeToken(TokenType::RIGHT_SQUARE);
  case '{':
    level++;
    return makeToken(TokenType::LEFT_BRACE);
  case '}':
    if (templateInterpolationDepth > 0) {
      templateInterpolationDepth--;
      resumeTemplateStringChunk = true;
      return makeToken(TokenType::STRING_TEMPLATE_EXPR_END, "}");
    }
    level--;
    return makeToken(TokenType::RIGHT_BRACE);
  case ':':
    return makeToken(TokenType::COLON);
  case ',':
    return makeToken(TokenType::COMMA);
  case '|':
    if (matchAndAdvance('=')) {
      return makeToken(TokenType::PIPE_EQUAL);
    }
    return makeToken(TokenType::PIPE);
  case '?':
    if (matchAndAdvance('?')) {
      if (matchAndAdvance('=')) {
        return makeToken(TokenType::NULL_COALESCE_EQUAL);
      }
      return makeToken(TokenType::NULL_COALESCE);
    }
    return makeToken(TokenType::QUESTION);
  case '.': {
    if (matchAndAdvance('.')) {
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
    if (matchAndAdvance('*')) {
      if (matchAndAdvance('=')) {
        return makeToken(TokenType::POWER_EQUAL);
      }
      return makeToken(TokenType::POWER);
    }
    if (matchAndAdvance('=')) {
      return makeToken(TokenType::STAR_EQUAL);
    }
    return makeToken(TokenType::STAR);
  }
  case '&':
    if (matchAndAdvance('=')) {
      return makeToken(TokenType::AMPERSAND_EQUAL);
    }
    return makeToken(TokenType::AMPERSAND);
  case '~':
    return makeToken(TokenType::TILDE);
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
    if (matchAndAdvance('<')) {
      if (matchAndAdvance('=')) {
        return makeToken(TokenType::SHIFT_LEFT_EQUAL);
      }
      return makeToken(TokenType::SHIFT_LEFT);
    }
    return makeToken(matchAndAdvance('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
  case '>':
    if (matchAndAdvance('>')) {
      if (matchAndAdvance('=')) {
        return makeToken(TokenType::SHIFT_RIGHT_EQUAL);
      }
      return makeToken(TokenType::SHIFT_RIGHT);
    }
    return makeToken(matchAndAdvance('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
  case '/': {
    if (matchAndAdvance('=')) {
      return makeToken(TokenType::SLASH_EQUAL);
    }
    if (matchAndAdvance('/')) {
      return scanLineComment();
    }
    if (matchAndAdvance('*')) {
      return scanBlockComment();
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
      return makeToken(TokenType::XOR_EQUAL);
    }
    return makeToken(TokenType::XOR);
  }
  case '\\':
    c = advance();
    continuation = true;

    while (true) {
      if (c == ' ' || c == '\r' || c == '\t') {
        c = advance();
      } else if (c == '/' && peek() == '/') {
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
      lexError(currentSpan(),
               fmt::format("Newline expected after line continuation, found {}", c));
      skipRestOfPhysicalLine();
      return scanOne(false);
    }

    line++;
    col = 1;

    return scanOne(continuation);
  case ' ':
  case '\r':
  case '\t':
    if (!handleWhitespace(c)) {
      return scanOne(continuation);
    }
    if (col == 2) {
      handleIndentation(false);
    }
    return scanOne(continuation);
  case '\n':
    line++;
    col = 1;
    // Emit NEWLINE at every physical line break (including inside `{` … `}` blocks) so the
    // parser can separate statements. INDENT/DEDENT are no longer used at brace depth 0.
    if (!continuation) {
      tokens.push_back(
          std::make_unique<Token>(TokenType::NEWLINE, "NEWLINE", llvm::SMRange{beginLoc, loc}));
    }
    handleIndentation(continuation);
    return scanOne(false);
  case '"':
    return openStringLiteral();
  default:
    if (isDigit(c)) {
      return addNumToken();
    } else if (isAlpha(c)) {
      return addIdentifierToken();
    } else {
      lexError(currentSpan(), fmt::format("Unexpected character: {}", c));
      return scanOne(continuation);
    }
  }
  assert(false && "Lexer::scanOne should not fall through");
  lexError(currentSpan(), "Unknown lexer error");
  return scanOne(continuation);
}

auto Lexer::handleWhitespace(char c) -> bool {
  if (level != 0) {
    if (!firstIndentChar.has_value()) {
      firstIndentChar = c;
    }
    if (firstIndentChar != c) {
      lexError(currentSpan(), fmt::format("Mixed indentation, first indentation character is: {}",
                                          firstIndentChar.value()));
      skipRestOfPhysicalLine();
      firstIndentChar.reset();
      return false;
    }
  }
  if (c == '\t') {
    col += 7;
  }
  return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Lexer::handleIndentation(bool continuation) -> bool {
  char c = 0;
  bool advanced = false;
  for (;;) {
    if (isAtEnd()) {
      break;
    }
    c = advance();
    advanced = true;
    if (c != ' ' && c != '\t') {
      break;
    }
  }

  if (!isAtEnd() || advanced) {
    fallback();
  }

  if (continuation || level != 0 || c == '\n' || c == '\r') {
    // Collapse repeated blank lines at brace depth 0 only; inside `{` … `}` we keep every emitted
    // NEWLINE so statements stay separated.
    if (level == 0 && c == '\n') {
      if (!tokens.empty() && tokens.back()->type == TokenType::NEWLINE) {
        tokens.pop_back();
      }
    }
    return true;
  }

  // At brace depth 0, leading spaces/tabs are not significant; brace blocks use `{}`.
  firstIndentChar.reset();
  return true;
}

auto Lexer::makeToken(TokenType type) -> std::unique_ptr<Token> {
  auto token = std::make_unique<Token>(type, std::string(beginLoc.getPointer(), loc.getPointer()),
                                       llvm::SMRange{beginLoc, loc});
  resetTokenBeg();
  return token;
}

auto Lexer::makeToken(TokenType type, const std::string& value) -> std::unique_ptr<Token> {
  auto token = std::make_unique<Token>(type, value, llvm::SMRange{beginLoc, loc});
  resetTokenBeg();
  return token;
}

auto Lexer::scanLineComment() -> std::unique_ptr<Token> {
  while (peek() != '\n' && !isAtEnd()) {
    advance();
  }
  return makeToken(TokenType::LINE_COMMENT);
}

auto Lexer::scanBlockComment() -> std::unique_ptr<Token> {
  while (!isAtEnd()) {
    char const c = advanceWithNewlineTracking();
    if (c == '*' && peek() == '/') {
      advance();
      return makeToken(TokenType::BLOCK_COMMENT);
    }
  }
  lexError(currentSpan(), "Unterminated block comment.");
  return makeToken(TokenType::BLOCK_COMMENT);
}

auto Lexer::resetTokenBeg() -> void { beginLoc = loc; }

auto Lexer::fallback() -> void {
  --curPos;
  loc = llvm::SMLoc::getFromPointer(getLocPointer());
  --col;
}

auto Lexer::advance() -> char {
  auto ret = lastChar();
  ++curPos;
  loc = llvm::SMLoc::getFromPointer(getLocPointer());
  ++col;
  return ret;
}

auto Lexer::advanceWithNewlineTracking() -> char {
  char const c = advance();
  if (c == '\n') {
    line++;
    col = 1;
  }
  return c;
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
  size_t const targetPos = curPos + static_cast<size_t>(offset);
  if (targetPos >= curBuffer->getBufferSize()) {
    return '\0';
  }
  return getCharAt(targetPos);
}

auto Lexer::scanStringContentUnit(std::string& acc) -> StringScanStep {
  if (isAtEnd()) {
    lexError(currentSpan(), "Unterminated string.");
    return StringScanStep::ClosedQuote;
  }
  if (peek() == '\n') {
    acc.push_back(advance());
    ++line;
    col = 1;
    return StringScanStep::Continue;
  }
  if (peek() == '"') {
    advance();
    return StringScanStep::ClosedQuote;
  }
  if (peek() == '$' && peek(1) == '{') {
    return StringScanStep::StartInterpolation;
  }
  if (peek() != '\\') {
    acc.push_back(advance());
    return StringScanStep::Continue;
  }

  advance();
  if (isAtEnd()) {
    lexError(currentSpan(), "Unterminated string.");
    return StringScanStep::ClosedQuote;
  }

  switch (peek()) {
  case 'n':
    acc.push_back('\n');
    break;
  case 'r':
    acc.push_back('\r');
    break;
  case 't':
    acc.push_back('\t');
    break;
  case 'b':
    acc.push_back('\b');
    break;
  case '0':
    acc.push_back('\0');
    break;
  case '"':
    acc.push_back('"');
    break;
  case 'e':
    acc.push_back(static_cast<char>(0x1B));
    break;
  case '\'':
    acc.push_back('\'');
    break;
  case '\\':
    acc.push_back('\\');
    break;
  case '$':
    acc.push_back('$');
    break;
  default:
    lexError(currentSpan(), "Unknown escape sequence.");
    advance();
    return StringScanStep::Continue;
  }

  advance();
  return StringScanStep::Continue;
}

auto Lexer::openStringLiteral() -> std::unique_ptr<Token> {
  llvm::SMLoc const openQuoteLoc = beginLoc;
  resetTokenBeg();
  std::string acc;
  bool sawTemplate = false;
  for (;;) {
    StringScanStep const step = scanStringContentUnit(acc);
    if (step == StringScanStep::ClosedQuote) {
      llvm::SMRange const fullSpan{openQuoteLoc, loc};
      if (!sawTemplate) {
        auto tok = std::make_unique<Token>(TokenType::STRING, acc, fullSpan);
        resetTokenBeg();
        return tok;
      }
      auto tok = std::make_unique<Token>(TokenType::STRING_TEMPLATE_CHUNK, acc,
                                         llvm::SMRange{beginLoc, loc});
      resetTokenBeg();
      return tok;
    }
    if (step == StringScanStep::StartInterpolation) {
      sawTemplate = true;
      llvm::SMLoc const exprStartSml = loc;
      advance();
      advance();
      pendingTokens.push_back(std::make_unique<Token>(TokenType::STRING_TEMPLATE_EXPR_START, "${",
                                                      llvm::SMRange{exprStartSml, loc}));
      templateInterpolationDepth++;
      auto tok = std::make_unique<Token>(TokenType::STRING_TEMPLATE_CHUNK, acc,
                                         llvm::SMRange{beginLoc, loc});
      resetTokenBeg();
      return tok;
    }
  }
}

auto Lexer::continueTemplateStringChunk() -> std::unique_ptr<Token> {
  resetTokenBeg();
  std::string acc;
  for (;;) {
    StringScanStep const step = scanStringContentUnit(acc);
    if (step == StringScanStep::ClosedQuote) {
      auto tok = std::make_unique<Token>(TokenType::STRING_TEMPLATE_CHUNK, acc,
                                         llvm::SMRange{beginLoc, loc});
      resetTokenBeg();
      return tok;
    }
    if (step == StringScanStep::StartInterpolation) {
      llvm::SMLoc const exprStartSml = loc;
      advance();
      advance();
      pendingTokens.push_back(std::make_unique<Token>(TokenType::STRING_TEMPLATE_EXPR_START, "${",
                                                      llvm::SMRange{exprStartSml, loc}));
      templateInterpolationDepth++;
      auto tok = std::make_unique<Token>(TokenType::STRING_TEMPLATE_CHUNK, acc,
                                         llvm::SMRange{beginLoc, loc});
      resetTokenBeg();
      return tok;
    }
  }
}

auto Lexer::addNumToken() -> std::unique_ptr<Token> {
  char const* const start = beginLoc.getPointer();
  if (*start == '0') {
    char const n = peek();
    if (n == 'b' || n == 'B') {
      advance();
      bool any = false;
      while (peek() == '0' || peek() == '1') {
        advance();
        any = true;
      }
      if (!any) {
        lexError(currentSpan(),
                 "Binary integer literal requires at least one digit after 0b");
      } else if (isDigit(peek()) && peek() != '0' && peek() != '1') {
        lexError(currentSpan(), "Invalid digit in binary integer literal");
        while (isDigit(peek())) {
          advance();
        }
      }
      if ((peek() == '.') && isDigit(peek(1))) {
        lexError(currentSpan(), "Binary integer literal cannot have a fractional part");
        advance();
        while (isDigit(peek())) {
          advance();
        }
        return makeToken(TokenType::DOUBLE);
      }
      return makeToken(TokenType::INTEGER);
    }
    if (n == 'o' || n == 'O') {
      advance();
      bool any = false;
      while (isOctalDigit(peek())) {
        advance();
        any = true;
      }
      if (!any) {
        lexError(currentSpan(),
                 "Octal integer literal requires at least one digit after 0o");
      } else if (peek() == '8' || peek() == '9') {
        lexError(currentSpan(), "Invalid digit in octal integer literal");
        while (isDigit(peek())) {
          advance();
        }
      }
      if ((peek() == '.') && isDigit(peek(1))) {
        lexError(currentSpan(), "Octal integer literal cannot have a fractional part");
        advance();
        while (isDigit(peek())) {
          advance();
        }
        return makeToken(TokenType::DOUBLE);
      }
      return makeToken(TokenType::INTEGER);
    }
    if (n == 'x' || n == 'X') {
      advance();
      bool any = false;
      while (isHexDigit(peek())) {
        advance();
        any = true;
      }
      if (!any) {
        lexError(currentSpan(),
                 "Hexadecimal integer literal requires at least one digit after 0x");
      }
      if ((peek() == '.') && isDigit(peek(1))) {
        lexError(currentSpan(), "Hexadecimal integer literal cannot have a fractional part");
        advance();
        while (isDigit(peek())) {
          advance();
        }
        return makeToken(TokenType::DOUBLE);
      }
      return makeToken(TokenType::INTEGER);
    }
  }

  while (isDigit(peek())) {
    advance();
  }

  char const radixProbe = peek();
  if ((radixProbe == 'b' || radixProbe == 'B' || radixProbe == 'o' || radixProbe == 'O' ||
       radixProbe == 'x' || radixProbe == 'X') &&
      *start == '0' && loc.getPointer() > start + 1) {
    lexError(currentSpan(),
             "Invalid numeric literal: use 0b, 0o, or 0x with a single leading 0 only");
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

auto Lexer::getLastToken() -> Token* {
  if (!tokens.empty()) {
    return tokens.back().get();
  }
  return nullptr;
}

auto Lexer::addIdentifierToken() -> std::unique_ptr<Token> {
  while (isAlphaNumeric(peek())) {
    advance();
  }

  TokenType const type = Token::getIdentifierType(
      std::string(beginLoc.getPointer(), loc.getPointer()), getLastToken());
  auto tok = makeToken(type);

  // If it's a multi-word keyword, remove the last token
  if (tok->type == TokenType::ELSE_IF || tok->type == TokenType::IS_NOT) {
    tokens.pop_back(); // unique_ptr automatically deletes
  }

  return tok;
}

auto Lexer::lastChar() -> char { return getCharAt(curPos); }

auto Lexer::lexError(llvm::SMRange span, const std::string& msg) -> void {
  if (diagnosticSink == nullptr) {
    throw LexerError(span, msg);
  }
  llvm::SMRange const useSpan = span.isValid() ? span : llvm::SMRange{beginLoc, loc};
  diagnosticSink->push_back(AnalysisDiagnostic{.message = msg,
                                               .span = useSpan,
                                               .severity = AnalysisDiagnosticSeverity::Error,
                                               .spanSourceMgr = srcMgr,
                                               .spanBufferId = srcMgr->getNumBuffers(),
                                               .spanDisplayPath = diagnosticFilePath});
}

auto Lexer::skipRestOfPhysicalLine() -> void {
  while (!isAtEnd()) {
    if (peek() == '\n') {
      advance();
      line++;
      col = 1;
      return;
    }
    advance();
  }
}

