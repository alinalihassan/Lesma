#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/SourceMgr.h>

#include <sysexits.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Driver/AnalysisDiagnostic.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
[[nodiscard]] inline auto isCommentToken(TokenType type) -> bool {
  return type == TokenType::LINE_COMMENT || type == TokenType::BLOCK_COMMENT;
}

class ParserError : public LesmaErrorWithExitCode<EX_DATAERR> {
public:
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};

class Parser {
public:
  explicit Parser(std::vector<Token*> tokens,
                  std::vector<AnalysisDiagnostic>* diagnosticSink = nullptr,
                  std::shared_ptr<llvm::SourceMgr> diagnosticSpanSrcMgr = nullptr,
                  unsigned diagnosticSpanBufferId = 0,
                  std::string diagnosticSpanDisplayPath = {})
      : tokens(std::move(tokens)), diagnosticsOut(diagnosticSink),
        diagnosticSpanSrcMgr(std::move(diagnosticSpanSrcMgr)),
        diagnosticSpanBufferId(diagnosticSpanBufferId),
        diagnosticSpanDisplayPath(std::move(diagnosticSpanDisplayPath)) {}
  ~Parser() = default;

  Parser(const Parser&) = delete;
  auto operator=(const Parser&) -> Parser& = delete;
  Parser(Parser&&) = delete;
  auto operator=(Parser&&) -> Parser& = delete;

  auto parse() -> void;

  [[nodiscard]] auto getAst() -> Compound* { return tree.get(); }

private:
  auto peek() -> Token* { return peek(0); }
  auto peek(unsigned long i) -> Token* { return tokens.at(visibleRawIndex(i)); }
  [[nodiscard]] auto canPeek(unsigned long visibleOffset) const -> bool {
    if (tokens.empty()) {
      return false;
    }
    size_t rawIndex = index;
    unsigned long remaining = visibleOffset;
    while (rawIndex < tokens.size()) {
      if (!isCommentToken(tokens[rawIndex]->type)) {
        if (remaining == 0U) {
          return true;
        }
        remaining--;
      }
      rawIndex++;
    }
    return false;
  }
  [[nodiscard]] auto visibleRawIndex(unsigned long visibleOffset) const -> size_t {
    size_t rawIndex = index;
    unsigned long remaining = visibleOffset;
    while (rawIndex < tokens.size()) {
      if (!isCommentToken(tokens[rawIndex]->type)) {
        if (remaining == 0U) {
          return rawIndex;
        }
        remaining--;
      }
      rawIndex++;
    }
    return tokens.empty() ? 0U : tokens.size() - 1U;
  }
  [[nodiscard]] auto previous() -> Token* {
    size_t rawIndex = index;
    while (rawIndex > 0U) {
      rawIndex--;
      if (!isCommentToken(tokens[rawIndex]->type)) {
        return tokens[rawIndex];
      }
    }
    return nullptr;
  }

  auto consume(TokenType type) -> Token*;
  auto consume(TokenType type, const std::string& errorMessage) -> Token*;
  auto consumeNewline() -> Token*;
  /** Like `consumeNewline` but allows closing `}` without a newline (last stmt in `{` … `}`). */
  auto consumeNewlineOrBlockEnd() -> void;
  [[nodiscard]] auto columnOf(llvm::SMLoc loc) const -> unsigned;
  [[nodiscard]] auto columnOf(const Token* token) const -> unsigned;
  auto consumeIndentedContinuationNewlines(unsigned anchorColumn) -> void;
  auto consumeIndentedContinuationNewlines(llvm::SMLoc anchorLoc) -> void;
  auto consumeOperandContinuationNewlines() -> void;

  auto isAtEnd() -> bool { return peek()->type == TokenType::EOF_TOKEN; }

  auto advance() -> Token* {
    Token* current = peek();
    if (!isAtEnd()) {
      index = visibleRawIndex(1);
    }
    return current;
  }

  auto check(TokenType type) -> bool { return check(type, 0); }

  auto check(TokenType type, unsigned long pos) -> bool { return peek(pos)->type == type; }

  auto isTypeArgClose() -> bool;
  auto consumeTypeArgClose(const std::string& errorMessage = "Expected '>' after type arguments")
      -> void;
  auto isTypeArgClose(unsigned long off, unsigned short pendingTypeArgClosers) -> bool;
  auto consumeTypeArgClose(unsigned long& off, unsigned short& pendingTypeArgClosers) -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto advanceIfMatchAny() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAny() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAnyInLine() -> bool;

  template <TokenType type, TokenType... remaining_types>
  auto checkAny(unsigned long pos) -> bool;

  std::vector<Token*> tokens;
  size_t index = 0;
  unsigned short pendingTypeArgClosers = 0;
  bool inClass = false;
  bool isExported = false;
  std::unique_ptr<Compound> tree;
  /** When non-null, parse errors are recorded here and parsing continues where possible. */
  std::vector<AnalysisDiagnostic>* diagnosticsOut = nullptr;
  std::shared_ptr<llvm::SourceMgr> diagnosticSpanSrcMgr;
  unsigned diagnosticSpanBufferId = 0;
  std::string diagnosticSpanDisplayPath;

  auto pushParserDiagnostic(llvm::SMRange span, std::string message) -> void;

  auto error(Token* token, const std::string& errorMessage) -> void;
  auto error(llvm::SMRange span, const std::string& errorMessage) -> void;
  /** Skip tokens until the next newline (or EOF) after a recovered parse error. */
  auto synchronizeToNextLine() -> void;
  auto recoverFromParserError(const ParserError& err) -> void;
  auto attachTrivia() -> void;

  auto parseCompound() -> std::unique_ptr<Compound>;
  auto parseBlock() -> std::unique_ptr<Compound>;

  struct ParameterListParseResult {
    std::vector<std::unique_ptr<Parameter>> parameters;
    bool varargs = false;
  };
  /// Parses `(` … `)` contents (caller consumes `(` before and `)` after). When
  /// `allowVarargsEllipsis` is true, `...` is accepted as a trailing varargs marker.
  auto parseParameterList(bool allowVarargsEllipsis) -> ParameterListParseResult;

  auto parseFunctionDeclaration(bool methodIsPrivate = false,
                                bool declaresInheritanceOverload = false, bool methodIsStatic = false)
      -> std::unique_ptr<Statement>;
  auto parseExport() -> std::unique_ptr<Statement>;
  auto parseImport() -> std::unique_ptr<Statement>;
  auto parseClass() -> std::unique_ptr<Statement>;
  auto parseTrait() -> std::unique_ptr<Statement>;
  auto parseGenericParamList() -> std::vector<GenericParamDecl>;
  auto parseIgnoredTypeArgList() -> void;
  auto parseTraitMethodDeclaration() -> std::unique_ptr<FuncDecl>;
  auto parseEnum() -> std::unique_ptr<Statement>;
  auto parseStatement(bool isTopLevel) -> std::unique_ptr<Statement>;
  auto parseIf() -> std::unique_ptr<Statement>;
  auto parseWhile() -> std::unique_ptr<Statement>;
  auto parseFor() -> std::unique_ptr<Statement>;
  auto parseVarDecl(bool fieldIsPrivate = false, bool fieldIsStatic = false)
      -> std::unique_ptr<Statement>;
  auto parseAssignment() -> std::unique_ptr<Statement>;
  auto parseBreak() -> std::unique_ptr<Statement>;
  auto parseContinue() -> std::unique_ptr<Statement>;
  auto parsePass() -> std::unique_ptr<Statement>;
  auto parseReturn() -> std::unique_ptr<Statement>;
  auto parseDefer() -> std::unique_ptr<Statement>;
  auto parseType() -> std::unique_ptr<TypeExpr>;
  /** Parses `<` … `>` as a comma-separated list of types (caller ensures current token is `<`). */
  auto parseAngleBracketTypeArgList() -> std::vector<std::unique_ptr<TypeExpr>>;
  /** One union arm: no top-level `|` (inner `parseType` still allows unions in parens / ptr). */
  auto parseTypePrimary() -> std::unique_ptr<TypeExpr>;
  auto parseExpression() -> std::unique_ptr<Expression>;
  auto parseOr() -> std::unique_ptr<Expression>;
  auto parseAnd() -> std::unique_ptr<Expression>;
  auto parseNot() -> std::unique_ptr<Expression>;
  auto parseCoalesce() -> std::unique_ptr<Expression>;
  auto parseBitwiseOr() -> std::unique_ptr<Expression>;
  auto parseBitwiseXor() -> std::unique_ptr<Expression>;
  auto parseBitwiseAnd() -> std::unique_ptr<Expression>;
  auto parseShift() -> std::unique_ptr<Expression>;
  auto parseDot() -> std::unique_ptr<Expression>;
  auto parsePostfix() -> std::unique_ptr<Expression>;
  auto parseCompare() -> std::unique_ptr<Expression>;
  auto parseAdd() -> std::unique_ptr<Expression>;
  auto parseMult() -> std::unique_ptr<Expression>;
  auto parsePower() -> std::unique_ptr<Expression>;
  auto parseCast() -> std::unique_ptr<Expression>;
  auto parseUnary() -> std::unique_ptr<Expression>;
  auto parseTerm() -> std::unique_ptr<Expression>;
  auto parseStringInterpolation() -> std::unique_ptr<Expression>;
  auto parseLambda() -> std::unique_ptr<Expression>;
  auto parseFunctionCall() -> std::unique_ptr<Expression>;
  auto parseListLiteral() -> std::unique_ptr<Expression>;
  auto parseDictLiteral() -> std::unique_ptr<Expression>;

  // Lookahead: true if from current position we have IDENTIFIER LESS type-list
  // GREATER LEFT_PAREN (so parsing as call with explicit type args is valid).
  auto hasExplicitTypeArgsAndParen() -> bool;
  auto parseTypeAt(unsigned long& off) -> bool;
  auto parseTypeAt(unsigned long& off, unsigned short& pendingTypeArgClosers) -> bool;
  auto parseTypePrimaryAt(unsigned long& off) -> bool;
  auto parseTypePrimaryAt(unsigned long& off, unsigned short& pendingTypeArgClosers) -> bool;
  auto skipOneTypeAt(unsigned long& off) -> bool;
  auto skipOneTypeAt(unsigned long& off, unsigned short& pendingTypeArgClosers) -> bool;
};
} // namespace lesma
