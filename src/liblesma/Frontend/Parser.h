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

  auto Parse() -> void;

  auto GetAst() -> Compound* { return tree.get(); }

private:
  auto Peek() -> Token* { return Peek(0); }
  auto Peek(unsigned long i) -> Token* { return tokens.at(index + i); }

  auto Consume(TokenType type) -> Token*;
  auto Consume(TokenType type, const std::string& errorMessage) -> Token*;
  auto ConsumeNewline() -> Token*;

  auto Previous() -> Token* { return Peek(-1); }

  auto IsAtEnd() -> bool { return Peek()->type == TokenType::EOF_TOKEN; }

  auto Advance() -> Token* {
    if (!IsAtEnd()) {
      index++;
    }

    return Peek(-1);
  }

  auto Check(TokenType type) -> bool { return Check(type, 0); }

  auto Check(TokenType type, unsigned long pos) -> bool {
    return Peek(pos)->type == type;
  }

  template <TokenType type, TokenType... remained_types>
  auto AdvanceIfMatchAny() -> bool;

  template <TokenType type, TokenType... remained_types>
  auto CheckAny() -> bool;

  template <TokenType type, TokenType... remained_types>
  auto CheckAnyInLine() -> bool;

  template <TokenType type, TokenType... remained_types>
  auto CheckAny(unsigned long pos) -> bool;

  std::vector<Token*> tokens;
  unsigned long index = 0;
  bool inClass = false;
  bool isExported = false;
  std::unique_ptr<Compound> tree;

  static auto Error(Token* token, const std::string& errorMessage) -> void;

  auto ParseCompound() -> std::unique_ptr<Compound>;
  auto ParseBlock() -> std::unique_ptr<Compound>;
  auto ParseFunctionDeclaration() -> std::unique_ptr<Statement>;
  auto ParseExport() -> std::unique_ptr<Statement>;
  auto ParseImport() -> std::unique_ptr<Statement>;
  auto ParseClass() -> std::unique_ptr<Statement>;
  auto ParseEnum() -> std::unique_ptr<Statement>;
  auto ParseStatement(bool isTopLevel) -> std::unique_ptr<Statement>;
  auto ParseIf() -> std::unique_ptr<Statement>;
  auto ParseWhile() -> std::unique_ptr<Statement>;
  auto ParseFor() -> std::unique_ptr<Statement>;
  auto ParseVarDecl() -> std::unique_ptr<Statement>;
  auto ParseAssignment() -> std::unique_ptr<Statement>;
  auto ParseBreak() -> std::unique_ptr<Statement>;
  auto ParseContinue() -> std::unique_ptr<Statement>;
  auto ParseReturn() -> std::unique_ptr<Statement>;
  auto ParseDefer() -> std::unique_ptr<Statement>;
  auto ParseType() -> std::unique_ptr<TypeExpr>;
  auto ParseExpression() -> std::unique_ptr<Expression>;
  auto ParseOr() -> std::unique_ptr<Expression>;
  auto ParseAnd() -> std::unique_ptr<Expression>;
  auto ParseNot() -> std::unique_ptr<Expression>;
  auto ParseDot() -> std::unique_ptr<Expression>;
  auto ParseCompare() -> std::unique_ptr<Expression>;
  auto ParseAdd() -> std::unique_ptr<Expression>;
  auto ParseMult() -> std::unique_ptr<Expression>;
  auto ParsePower() -> std::unique_ptr<Expression>;
  auto ParseCast() -> std::unique_ptr<Expression>;
  auto ParseUnary() -> std::unique_ptr<Expression>;
  auto ParseTerm() -> std::unique_ptr<Expression>;
  auto ParseFunctionCall() -> std::unique_ptr<Expression>;
};
} // namespace lesma
