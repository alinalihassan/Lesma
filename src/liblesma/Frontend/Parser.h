#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sysexits.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
class ParserError : public LesmaErrorWithExitCode<EX_DATAERR> {
public:
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};

class Parser {
public:
  explicit Parser(std::vector<Token*> tokens) : tokens(std::move(tokens)) {}
  ~Parser() = default;

  Parser(const Parser&) = delete;
  auto operator=(const Parser&) -> Parser& = delete;
  Parser(Parser&&) = delete;
  auto operator=(Parser&&) -> Parser& = delete;

  auto parse() -> void;

  [[nodiscard]] auto getAst() -> Compound* { return tree.get(); }

private:
  auto peek() -> Token* { return peek(0); }
  auto peek(unsigned long i) -> Token* { return tokens.at(index + i); }

  auto consume(TokenType type) -> Token*;
  auto consume(TokenType type, const std::string& errorMessage) -> Token*;
  auto consumeNewline() -> Token*;

  [[nodiscard]] auto previous() -> Token* { return (index > 0) ? tokens.at(index - 1) : nullptr; }

  auto isAtEnd() -> bool { return peek()->type == TokenType::EOF_TOKEN; }

  auto advance() -> Token* {
    if (!isAtEnd()) {
      index++;
    }

    return peek(-1);
  }

  auto check(TokenType type) -> bool { return check(type, 0); }

  auto check(TokenType type, unsigned long pos) -> bool { return peek(pos)->type == type; }

  template <TokenType type, TokenType... remaining_types>
  auto advanceIfMatchAny() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAny() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAnyInLine() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAny(unsigned long pos) -> bool;

  std::vector<Token*> tokens;
  unsigned long index = 0;
  bool inClass = false;
  bool isExported = false;
  std::unique_ptr<Compound> tree;

  static auto error(Token* token, const std::string& errorMessage) -> void;

  auto parseCompound() -> std::unique_ptr<Compound>;
  auto parseBlock() -> std::unique_ptr<Compound>;
  auto parseFunctionDeclaration() -> std::unique_ptr<Statement>;
  auto parseExport() -> std::unique_ptr<Statement>;
  auto parseImport() -> std::unique_ptr<Statement>;
  auto parseClass() -> std::unique_ptr<Statement>;
  auto parseEnum() -> std::unique_ptr<Statement>;
  auto parseStatement(bool isTopLevel) -> std::unique_ptr<Statement>;
  auto parseIf() -> std::unique_ptr<Statement>;
  auto parseWhile() -> std::unique_ptr<Statement>;
  auto parseFor() -> std::unique_ptr<Statement>;
  auto parseVarDecl() -> std::unique_ptr<Statement>;
  auto parseAssignment() -> std::unique_ptr<Statement>;
  auto parseBreak() -> std::unique_ptr<Statement>;
  auto parseContinue() -> std::unique_ptr<Statement>;
  auto parseReturn() -> std::unique_ptr<Statement>;
  auto parseDefer() -> std::unique_ptr<Statement>;
  auto parseType() -> std::unique_ptr<TypeExpr>;
  auto parseExpression() -> std::unique_ptr<Expression>;
  auto parseOr() -> std::unique_ptr<Expression>;
  auto parseAnd() -> std::unique_ptr<Expression>;
  auto parseNot() -> std::unique_ptr<Expression>;
  auto parseDot() -> std::unique_ptr<Expression>;
  auto parseCompare() -> std::unique_ptr<Expression>;
  auto parseAdd() -> std::unique_ptr<Expression>;
  auto parseMult() -> std::unique_ptr<Expression>;
  auto parsePower() -> std::unique_ptr<Expression>;
  auto parseCast() -> std::unique_ptr<Expression>;
  auto parseUnary() -> std::unique_ptr<Expression>;
  auto parseTerm() -> std::unique_ptr<Expression>;
  auto parseFunctionCall() -> std::unique_ptr<Expression>;

  // Lookahead: true if from current position we have IDENTIFIER LESS type-list
  // GREATER LEFT_PAREN (so parsing as call with explicit type args is valid).
  auto hasExplicitTypeArgsAndParen() -> bool;
  auto skipOneTypeAt(unsigned long& off) -> bool;
};
} // namespace lesma
