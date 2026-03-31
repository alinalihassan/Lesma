#include "Parser.h"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/SMLoc.h>

#include "fmt/core.h"
#include "nameof.hpp"
#include <fmt/format.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

template <TokenType type, TokenType... remaining_types>
auto Parser::advanceIfMatchAny() -> bool {
  if (checkAny<type, remaining_types...>()) {
    advance();
    return true;
  }

  return false;
}

template <TokenType type, TokenType... remaining_types>
auto Parser::checkAny() -> bool {
  return checkAny<type, remaining_types...>(0);
}

template <TokenType type, TokenType... remaining_types>
auto Parser::checkAnyInLine() -> bool {
  int i = 0;
  while (!checkAny<TokenType::NEWLINE, TokenType::EOF_TOKEN>(i)) {
    if (checkAny<type, remaining_types...>(i)) {
      return true;
    }
    i++;
  }

  return false;
}

template <TokenType type, TokenType... remaining_types>
auto Parser::checkAny(unsigned long pos) -> bool {
  if (!check(type, pos)) {
    if constexpr (sizeof...(remaining_types) > 0) {
      return checkAny<remaining_types...>(pos);
    } else {
      return false;
    }
  }
  return true;
}

auto Parser::consume(TokenType type) -> Token* {
  return consume(type, std::string{"Expected: "} + std::string{NAMEOF_ENUM(type)} +
                           ", found: " + std::string{NAMEOF_ENUM(peek()->type)});
}

auto Parser::consume(TokenType type, const std::string& errorMessage) -> Token* {
  if (check(type)) {
    return advance();
  }
  error(peek(), errorMessage);
  return nullptr;
}

auto Parser::consumeNewline() -> Token* {
  if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN) {
    return advance();
  }
  error(peek(), fmt::format("Expected: NEWLINE or EOF, found: {}", NAMEOF_ENUM(peek()->type)));
  return nullptr;
}

auto Parser::consumeNewlineOrBlockEnd() -> void {
  if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN) {
    advance();
    return;
  }
  if (check(TokenType::RIGHT_BRACE)) {
    return;
  }
  error(peek(), fmt::format("Expected: NEWLINE, EOF, or RIGHT_BRACE, found: {}",
                             NAMEOF_ENUM(peek()->type)));
}

auto Parser::error(Token* token, const std::string& errorMessage) -> void {
  throw ParserError(token->span, "{}", errorMessage);
}

auto Parser::error(llvm::SMRange span, const std::string& errorMessage) -> void {
  throw ParserError(span, "{}", errorMessage);
}

auto Parser::synchronizeToNextLine() -> void {
  while (!isAtEnd()) {
    TokenType const t = peek()->type;
    if (t == TokenType::EOF_TOKEN) {
      return;
    }
    if (t == TokenType::NEWLINE) {
      advance();
      while (!isAtEnd()) {
        TokenType const t2 = peek()->type;
        if (t2 == TokenType::NEWLINE) {
          advance();
          continue;
        }
        if (t2 == TokenType::SEMICOLON) {
          advance();
          continue;
        }
        break;
      }
      return;
    }
    if (t == TokenType::RIGHT_BRACE) {
      return;
    }
    advance();
  }
}

auto Parser::pushParserDiagnostic(llvm::SMRange span, std::string message) -> void {
  if (diagnosticsOut == nullptr) {
    return;
  }
  diagnosticsOut->push_back(AnalysisDiagnostic{.message = std::move(message),
                                                 .span = span,
                                                 .severity = AnalysisDiagnosticSeverity::Error,
                                                 .spanSourceMgr = diagnosticSpanSrcMgr,
                                                 .spanBufferId = diagnosticSpanBufferId,
                                                 .spanDisplayPath = diagnosticSpanDisplayPath});
}

auto Parser::recoverFromParserError(const ParserError& err) -> void {
  llvm::SMRange const span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange();
  pushParserDiagnostic(span, std::string(err.what()));
  synchronizeToNextLine();
}

auto Parser::parseGenericParamList() -> std::vector<GenericParamDecl> {
  std::vector<GenericParamDecl> genericParams;
  if (!check(TokenType::LESS)) {
    return genericParams;
  }
  consume(TokenType::LESS);
  while (!check(TokenType::GREATER)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    auto* genericParam = consume(TokenType::IDENTIFIER);
    std::vector<std::string> bounds;
    std::vector<llvm::SMRange> boundSpans;
    if (advanceIfMatchAny<TokenType::COLON>()) {
      auto* boundId = consume(TokenType::IDENTIFIER);
      bounds.push_back(boundId->lexeme);
      boundSpans.push_back(boundId->span);
      parseIgnoredTypeArgList();
      while (advanceIfMatchAny<TokenType::AMPERSAND>()) {
        boundId = consume(TokenType::IDENTIFIER);
        bounds.push_back(boundId->lexeme);
        boundSpans.push_back(boundId->span);
        parseIgnoredTypeArgList();
      }
    }
    genericParams.push_back(GenericParamDecl{
        .name = genericParam->lexeme,
        .span = genericParam->span,
        .traitBounds = std::move(bounds),
        .traitBoundSpans = std::move(boundSpans),
    });
    if (!check(TokenType::GREATER)) {
      consume(TokenType::COMMA);
    }
  }
  consume(TokenType::GREATER);
  return genericParams;
}

auto Parser::parseIgnoredTypeArgList() -> void {
  if (!check(TokenType::LESS)) {
    return;
  }

  consume(TokenType::LESS);
  while (!check(TokenType::GREATER)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }

    std::ignore = parseType();
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    if (!check(TokenType::GREATER)) {
      consume(TokenType::COMMA);
    }
  }
  consume(TokenType::GREATER);
}

auto Parser::parseType() -> std::unique_ptr<TypeExpr> {
  std::vector<std::unique_ptr<TypeExpr>> arms;
  arms.push_back(parseTypePrimary());
  if (arms[0] == nullptr) {
    return nullptr;
  }
  while (advanceIfMatchAny<TokenType::PIPE>()) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    std::unique_ptr<TypeExpr> next = parseTypePrimary();
    if (next == nullptr) {
      return nullptr;
    }
    arms.push_back(std::move(next));
  }
  if (arms.size() == 1U) {
    return std::move(arms[0]);
  }
  std::string lexeme = arms[0]->getName();
  for (size_t i = 1; i < arms.size(); ++i) {
    lexeme += " | ";
    lexeme += arms[i]->getName();
  }
  return TypeExpr::makeUnionType(
      llvm::SMRange{arms.front()->getStart(), arms.back()->getEnd()}, std::move(lexeme),
      std::move(arms));
}

auto Parser::parseTypePrimary() -> std::unique_ptr<TypeExpr> {
  auto* type = peek();
  if (check(TokenType::LEFT_PAREN)) {
    auto* left = consume(TokenType::LEFT_PAREN);
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    std::unique_ptr<TypeExpr> innerFirst = parseType();
    if (advanceIfMatchAny<TokenType::COMMA>()) {
      std::vector<std::unique_ptr<TypeExpr>> elems;
      elems.push_back(std::move(innerFirst));
      while (!check(TokenType::RIGHT_PAREN)) {
        while (check(TokenType::NEWLINE)) {
          advance();
        }
        elems.push_back(parseType());
        if (!check(TokenType::RIGHT_PAREN)) {
          consume(TokenType::COMMA);
        }
      }
      auto* right = consume(TokenType::RIGHT_PAREN);
      std::string lexeme = "(";
      for (size_t i = 0; i < elems.size(); ++i) {
        if (i > 0) {
          lexeme += ", ";
        }
        lexeme += elems[i]->getName();
      }
      lexeme += ")";
      return TypeExpr::makeTupleType(llvm::SMRange{left->getStart(), right->getEnd()},
                                     std::move(lexeme), std::move(elems));
    }
    std::ignore = consume(TokenType::RIGHT_PAREN);
    return innerFirst;
  }
  if (check(TokenType::STAR)) {
    advance();
    auto elementType = parseType();
    return std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), elementType->getEnd()},
                                      "*" + elementType->getName(), TokenType::PTR_TYPE,
                                      std::move(elementType));
  }
  if (checkAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE,
               TokenType::BOOL_TYPE, TokenType::INT8_TYPE, TokenType::INT16_TYPE,
               TokenType::INT32_TYPE, TokenType::UINT_TYPE, TokenType::UINT8_TYPE,
               TokenType::UINT16_TYPE, TokenType::UINT32_TYPE, TokenType::FLOAT32_TYPE,
               TokenType::VOID_TYPE>()) {
    advance();
    return std::make_unique<TypeExpr>(type->span, type->lexeme, type->type);
  }
  if (check(TokenType::FUNC)) {
    std::vector<std::unique_ptr<TypeExpr>> params;
    std::unique_ptr<TypeExpr> ret;
    std::string lexeme = type->lexeme + " (";

    advance();
    consume(TokenType::LEFT_PAREN);
    while (true) {
      while (check(TokenType::NEWLINE)) {
        advance();
      }
      if (!params.empty()) {
        lexeme += ", ";
      }
      params.push_back(parseType());
      lexeme += params.back()->getName();

      if (!advanceIfMatchAny<TokenType::COMMA>()) {
        break;
      }
    }

    consume(TokenType::RIGHT_PAREN);
    lexeme += ")";

    if (advanceIfMatchAny<TokenType::ARROW>()) {
      ret = parseType();
      lexeme += " -> " + ret->getName();
    } else {
      ret = std::make_unique<TypeExpr>(
          llvm::SMRange{params.back()->getEnd(), params.back()->getEnd()}, "void",
          TokenType::VOID_TYPE);
    }

    // Function types are nominal (like classes): values are function pointers in LLVM, but the
    // type is written `func(...)` without a leading `*`. `*func(...)` is still accepted and lowers
    // to the same type.
    return std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), ret->getEnd()}, lexeme,
                                      TokenType::FUNC_TYPE, std::move(params), std::move(ret));
  }

  if (check(TokenType::IDENTIFIER)) {
    advance();
    if (check(TokenType::LESS)) {
      consume(TokenType::LESS);
      std::vector<std::unique_ptr<TypeExpr>> typeArgs;
      while (true) {
        while (check(TokenType::NEWLINE)) {
          advance();
        }
        typeArgs.push_back(parseType());
        if (!advanceIfMatchAny<TokenType::COMMA>()) {
          break;
        }
      }
      auto* greater = consume(TokenType::GREATER, "Expected '>' after generic type arguments");
      std::string lexeme = type->lexeme + "<";
      for (size_t i = 0; i < typeArgs.size(); ++i) {
        lexeme += typeArgs[i]->getName();
        if (i + 1U < typeArgs.size()) {
          lexeme += ", ";
        }
      }
      lexeme += ">";
      return std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), greater->getEnd()}, lexeme,
                                        TokenType::CUSTOM_TYPE, std::move(typeArgs));
    }
    return std::make_unique<TypeExpr>(type->span, type->lexeme, TokenType::CUSTOM_TYPE);
  }

  error(type, fmt::format("Unknown type: {}", type->lexeme));

  return nullptr;
}

auto Parser::parseTypeAt(unsigned long& off) -> bool {
  if (!parseTypePrimaryAt(off)) {
    return false;
  }
  while (index + off < tokens.size() && peek(off)->type == TokenType::PIPE) {
    off++;
    while (index + off < tokens.size() && peek(off)->type == TokenType::NEWLINE) {
      off++;
    }
    if (!parseTypePrimaryAt(off)) {
      return false;
    }
  }
  return true;
}

auto Parser::parseTypePrimaryAt(unsigned long& off) -> bool {
  if (index + off >= tokens.size()) {
    return false;
  }

  if (check(TokenType::STAR, off)) {
    off++;
    return parseTypeAt(off);
  }

  if (check(TokenType::LEFT_PAREN, off)) {
    off++;
    if (!parseTypeAt(off)) {
      return false;
    }
    if (index + off < tokens.size() && peek(off)->type == TokenType::COMMA) {
      off++;
      while (index + off < tokens.size() && peek(off)->type != TokenType::RIGHT_PAREN) {
        if (!parseTypeAt(off)) {
          return false;
        }
        if (index + off < tokens.size() && peek(off)->type == TokenType::COMMA) {
          off++;
        }
      }
    }
    if (index + off >= tokens.size() || peek(off)->type != TokenType::RIGHT_PAREN) {
      return false;
    }
    off++;
    return true;
  }

  if (checkAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE,
               TokenType::BOOL_TYPE, TokenType::INT8_TYPE, TokenType::INT16_TYPE,
               TokenType::INT32_TYPE, TokenType::UINT_TYPE, TokenType::UINT8_TYPE,
               TokenType::UINT16_TYPE, TokenType::UINT32_TYPE, TokenType::FLOAT32_TYPE,
               TokenType::VOID_TYPE>(off)) {
    off++;
    return true;
  }

  if (check(TokenType::FUNC, off)) {
    off++;

    if (index + off >= tokens.size() || peek(off)->type != TokenType::LEFT_PAREN) {
      return false;
    }
    off++;

    if (!parseTypeAt(off)) {
      return false;
    }

    while (index + off < tokens.size() && peek(off)->type == TokenType::COMMA) {
      off++;
      if (!parseTypeAt(off)) {
        return false;
      }
    }

    if (index + off >= tokens.size() || peek(off)->type != TokenType::RIGHT_PAREN) {
      return false;
    }
    off++;

    if (index + off < tokens.size() && peek(off)->type == TokenType::ARROW) {
      off++;
      return parseTypeAt(off);
    }

    return true;
  }

  if (check(TokenType::IDENTIFIER, off) || check(TokenType::STRING_TYPE, off)) {
    off++;
    if (index + off < tokens.size() && peek(off)->type == TokenType::LESS) {
      off++;
      if (!parseTypeAt(off)) {
        return false;
      }
      while (index + off < tokens.size() && peek(off)->type == TokenType::COMMA) {
        off++;
        if (!parseTypeAt(off)) {
          return false;
        }
      }
      if (index + off >= tokens.size() || peek(off)->type != TokenType::GREATER) {
        return false;
      }
      off++;
    }
    return true;
  }

  return false;
}

auto Parser::skipOneTypeAt(unsigned long& off) -> bool { return parseTypeAt(off); }

auto Parser::hasExplicitTypeArgsAndParen() -> bool {
  if (!(check(TokenType::IDENTIFIER) || check(TokenType::STRING_TYPE)) ||
      !check(TokenType::LESS, 1)) {
    return false;
  }
  unsigned long off = 2;
  while (index + off < tokens.size()) {
    if (peek(off)->type == TokenType::GREATER) {
      return index + off + 1 < tokens.size() && peek(off + 1)->type == TokenType::LEFT_PAREN;
    }
    if (!skipOneTypeAt(off)) {
      return false;
    }
    if (index + off >= tokens.size()) {
      return false;
    }
    if (peek(off)->type == TokenType::COMMA) {
      off++;
    } else if (peek(off)->type != TokenType::GREATER) {
      return false;
    }
  }
  return false;
}

// Expression
auto Parser::parseFunctionCall() -> std::unique_ptr<Expression> {
  auto* token = peek();
  if (token->type != TokenType::IDENTIFIER && token->type != TokenType::STRING_TYPE) {
    error(token, "Expected function or type name");
  }
  advance();

  std::vector<std::unique_ptr<TypeExpr>> explicitTypeArgs;
  if (check(TokenType::LESS)) {
    consume(TokenType::LESS);
    while (!check(TokenType::GREATER)) {
      explicitTypeArgs.push_back(parseType());
      if (!check(TokenType::GREATER)) {
        consume(TokenType::COMMA);
      }
    }
    consume(TokenType::GREATER);
  }

  consume(TokenType::LEFT_PAREN);

  std::vector<std::unique_ptr<Expression>> params;

  while (!check(TokenType::RIGHT_PAREN)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    auto param = parseExpression();

    if (!check(TokenType::RIGHT_PAREN)) {
      consume(TokenType::COMMA);
    }

    params.push_back(std::move(param));
  }

  auto* paren = consume(TokenType::RIGHT_PAREN);

  return std::make_unique<FuncCall>(llvm::SMRange{token->getStart(), paren->span.End},
                                    token->lexeme, std::move(explicitTypeArgs), std::move(params));
}

auto Parser::parseListLiteral() -> std::unique_ptr<Expression> {
  auto* start = consume(TokenType::LEFT_SQUARE);
  std::vector<std::unique_ptr<Expression>> elements;
  while (!check(TokenType::RIGHT_SQUARE)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    elements.push_back(parseExpression());
    if (!check(TokenType::RIGHT_SQUARE)) {
      consume(TokenType::COMMA);
    }
  }
  auto* end = consume(TokenType::RIGHT_SQUARE);
  return std::make_unique<ListLiteral>(llvm::SMRange{start->getStart(), end->getEnd()},
                                       std::move(elements));
}

auto Parser::parseDictLiteral() -> std::unique_ptr<Expression> {
  auto* start = consume(TokenType::LEFT_BRACE);
  std::vector<std::unique_ptr<Expression>> keyExprs;
  std::vector<std::unique_ptr<Expression>> valueExprs;
  while (!check(TokenType::RIGHT_BRACE)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    keyExprs.push_back(parseExpression());
    consume(TokenType::COLON);
    valueExprs.push_back(parseExpression());
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    if (!check(TokenType::RIGHT_BRACE)) {
      consume(TokenType::COMMA);
    }
  }
  auto* end = consume(TokenType::RIGHT_BRACE);
  return std::make_unique<DictLiteral>(llvm::SMRange{start->getStart(), end->getEnd()},
                                       std::move(keyExprs), std::move(valueExprs));
}

auto Parser::parseTerm() -> std::unique_ptr<Expression> {
  switch (peek()->type) {
  case TokenType::FUNC: {
    size_t i = 1;
    while (check(TokenType::NEWLINE, i)) {
      ++i;
    }
    if (check(TokenType::LESS, i) || check(TokenType::LEFT_PAREN, i)) {
      return parseLambda();
    }
    error(peek(), "Expected 'func<' or 'func(' to start a lambda");
    return nullptr;
  }
  case TokenType::STRING_TEMPLATE_CHUNK:
    return parseStringInterpolation();
  case TokenType::STRING:
  case TokenType::INTEGER:
  case TokenType::DOUBLE:
  case TokenType::NIL: {
    auto* token = peek();
    consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, token->type);
  }
  case TokenType::STRING_TYPE:
  case TokenType::IDENTIFIER: {
    if (check(TokenType::LEFT_PAREN, 1) || hasExplicitTypeArgsAndParen()) {
      return parseFunctionCall();
    }

    auto* token = peek();
    consume(token->type);
    TokenType const litType =
        token->type == TokenType::STRING_TYPE ? TokenType::IDENTIFIER : token->type;
    return std::make_unique<Literal>(token->span, token->lexeme, litType);
  }
  case TokenType::LEFT_PAREN: {
    auto* left = consume(TokenType::LEFT_PAREN);
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    auto first = parseExpression();
    if (advanceIfMatchAny<TokenType::COMMA>()) {
      std::vector<std::unique_ptr<Expression>> elems;
      elems.push_back(std::move(first));
      while (!check(TokenType::RIGHT_PAREN)) {
        while (check(TokenType::NEWLINE)) {
          advance();
        }
        elems.push_back(parseExpression());
        if (!check(TokenType::RIGHT_PAREN)) {
          consume(TokenType::COMMA);
        }
      }
      auto* right = consume(TokenType::RIGHT_PAREN);
      return std::make_unique<TupleLiteral>(llvm::SMRange{left->getStart(), right->getEnd()},
                                            std::move(elems));
    }
    std::ignore = consume(TokenType::RIGHT_PAREN);
    return first;
  }
  case TokenType::LEFT_SQUARE:
    return parseListLiteral();
  case TokenType::LEFT_BRACE:
    return parseDictLiteral();
  case TokenType::TRUE_:
  case TokenType::FALSE_: {
    auto* token = peek();
    consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, TokenType::BOOL);
  }
  case TokenType::SUPER: {
    auto* token = consume(TokenType::SUPER);
    return std::make_unique<SuperExpr>(token->span);
  }
  default:
    error(peek(), fmt::format("Unknown literal: {}", peek()->lexeme));
  }

  return nullptr;
}

auto Parser::parseLambda() -> std::unique_ptr<Expression> {
  auto* start = consume(TokenType::FUNC);
  std::vector<GenericParamDecl> genericParams = parseGenericParamList();
  consume(TokenType::LEFT_PAREN);
  auto paramList = parseParameterList(false);
  std::vector<std::unique_ptr<Parameter>> parameters = std::move(paramList.parameters);
  consume(TokenType::RIGHT_PAREN);

  std::unique_ptr<TypeExpr> returnType;
  if (advanceIfMatchAny<TokenType::ARROW>()) {
    returnType = parseType();
  }

  if (advanceIfMatchAny<TokenType::FAT_ARROW>()) {
    auto bodyExpr = parseExpression();
    if (bodyExpr == nullptr) {
      error(peek(), "Expected expression after '=>'");
      return nullptr;
    }
    return std::make_unique<LambdaExpr>(llvm::SMRange{start->getStart(), bodyExpr->getEnd()},
                                        std::move(genericParams), std::move(parameters),
                                        std::move(returnType), std::move(bodyExpr), nullptr);
  }

  if (returnType == nullptr) {
    returnType = std::make_unique<TypeExpr>(start->span, "void", TokenType::VOID_TYPE);
  }
  auto block = parseBlock();
  auto lambdaEnd = block != nullptr ? block->getEnd() : returnType->getEnd();
  return std::make_unique<LambdaExpr>(llvm::SMRange{start->getStart(), lambdaEnd},
                                      std::move(genericParams), std::move(parameters),
                                      std::move(returnType), nullptr, std::move(block));
}

auto Parser::parseStringInterpolation() -> std::unique_ptr<Expression> {
  auto* chunk0 = consume(TokenType::STRING_TEMPLATE_CHUNK);
  std::vector<std::string> chunks;
  chunks.push_back(chunk0->lexeme);
  llvm::SMRange span = chunk0->span;
  std::vector<std::unique_ptr<Expression>> exprs;
  while (check(TokenType::STRING_TEMPLATE_EXPR_START)) {
    consume(TokenType::STRING_TEMPLATE_EXPR_START);
    std::unique_ptr<Expression> expr = parseExpression();
    consume(TokenType::STRING_TEMPLATE_EXPR_END);
    exprs.push_back(std::move(expr));
    auto* tail = consume(TokenType::STRING_TEMPLATE_CHUNK,
                         "Expected string text after } in interpolated string");
    chunks.push_back(tail->lexeme);
    span = llvm::SMRange{span.Start, tail->span.End};
  }
  return std::make_unique<StringInterpolation>(span, std::move(chunks), std::move(exprs));
}

auto Parser::parsePostfix() -> std::unique_ptr<Expression> {
  auto left = parseTerm();

  while (true) {
    if (advanceIfMatchAny<TokenType::DOT>()) {
      auto* op = previous();
      auto expr = parseTerm();
      left = std::make_unique<DotOp>(llvm::SMRange{left->getStart(), expr->getEnd()},
                                     std::move(left), op->type, std::move(expr));
      continue;
    }
    if (advanceIfMatchAny<TokenType::LEFT_SQUARE>()) {
      auto index = parseExpression();
      auto* end = consume(TokenType::RIGHT_SQUARE, "Expected ']' after subscript index");
      left = std::make_unique<SubscriptOp>(llvm::SMRange{left->getStart(), end->getEnd()},
                                           std::move(left), std::move(index));
      continue;
    }
    break;
  }

  return left;
}

auto Parser::parseDot() -> std::unique_ptr<Expression> { return parsePostfix(); }

auto Parser::parseUnary() -> std::unique_ptr<Expression> {
  // Handle unary operators recursively to allow chaining: - - x, * * ptr, etc.
  if (advanceIfMatchAny<TokenType::MINUS, TokenType::STAR, TokenType::AMPERSAND,
                        TokenType::BANG>()) {
    auto* op = previous();
    auto expr = parseUnary(); // Recursive call for chained unary operators
    return std::make_unique<UnaryOp>(llvm::SMRange{op->getStart(), expr->getEnd()}, op->type,
                                     std::move(expr));
  }

  return parseDot();
}

auto Parser::parseCast() -> std::unique_ptr<Expression> {
  auto left = parseUnary();
  while (advanceIfMatchAny<TokenType::AS>()) {
    auto type = parseType();
    left = std::make_unique<CastOp>(llvm::SMRange{left->getStart(), type->getEnd()},
                                    std::move(left), std::move(type));
  }
  return left;
}

auto Parser::parseMult() -> std::unique_ptr<Expression> {
  auto left = parsePower();
  while (advanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
    auto op = previous()->type;
    auto right = parsePower();
    left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                      std::move(left), op, std::move(right));
  }
  return left;
}

auto Parser::parsePower() -> std::unique_ptr<Expression> {
  auto left = parseCast();
  while (advanceIfMatchAny<TokenType::POWER>()) {
    auto op = previous()->type;
    auto right = parseCast();
    left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                      std::move(left), op, std::move(right));
  }
  return left;
}

auto Parser::parseAdd() -> std::unique_ptr<Expression> {
  auto left = parseMult();
  while (advanceIfMatchAny<TokenType::PLUS, TokenType::MINUS>()) {
    auto op = previous()->type;
    auto right = parseMult();
    left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                      std::move(left), op, std::move(right));
  }
  return left;
}

auto Parser::parseCompare() -> std::unique_ptr<Expression> {
  auto left = parseAdd();
  while (advanceIfMatchAny<TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL, TokenType::LESS,
                           TokenType::LESS_EQUAL, TokenType::GREATER, TokenType::GREATER_EQUAL,
                           TokenType::IS, TokenType::IS_NOT>()) {
    auto op = previous()->type;
    if (op == TokenType::IS || op == TokenType::IS_NOT) {
      auto right = parseType();
      left = std::make_unique<IsOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                    std::move(left), op, std::move(right));
    } else {
      auto right = parseAdd();
      left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                        std::move(left), op, std::move(right));
    }
  }
  return left;
}

auto Parser::parseNot() -> std::unique_ptr<Expression> {
  // Handle 'not' as a prefix unary operator, allowing chaining: not not x
  if (advanceIfMatchAny<TokenType::NOT>()) {
    auto* op = previous();
    // Recursively call ParseNot() to handle chained 'not' operators
    auto expr = parseNot();
    return std::make_unique<UnaryOp>(llvm::SMRange{op->getStart(), expr->getEnd()}, TokenType::NOT,
                                     std::move(expr));
  }

  return parseCompare();
}

auto Parser::parseAnd() -> std::unique_ptr<Expression> {
  auto left = parseNot();
  while (advanceIfMatchAny<TokenType::AND>()) {
    auto right = parseNot();
    left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                      std::move(left), TokenType::AND, std::move(right));
  }
  return left;
}

auto Parser::parseOr() -> std::unique_ptr<Expression> {
  auto left = parseAnd();
  while (advanceIfMatchAny<TokenType::OR>()) {
    auto right = parseAnd();
    left = std::make_unique<BinaryOp>(llvm::SMRange{left->getStart(), right->getEnd()},
                                      std::move(left), TokenType::OR, std::move(right));
  }
  return left;
}

auto Parser::parseExpression() -> std::unique_ptr<Expression> { return parseOr(); }

// Statements
auto Parser::parseVarDecl(bool fieldIsPrivate) -> std::unique_ptr<Statement> {
  bool isMutable = false;
  Token const* startTok = nullptr;
  if (advanceIfMatchAny<TokenType::LET>()) {
    startTok = previous();
    isMutable = false;
  } else {
    startTok = consume(TokenType::VAR);
    isMutable = true;
  }
  std::vector<std::unique_ptr<Literal>> vars;
  auto* firstId = consume(TokenType::IDENTIFIER);
  vars.push_back(std::make_unique<Literal>(firstId->span, firstId->lexeme, firstId->type));
  while (advanceIfMatchAny<TokenType::COMMA>()) {
    auto* nextId = consume(TokenType::IDENTIFIER);
    vars.push_back(std::make_unique<Literal>(nextId->span, nextId->lexeme, nextId->type));
  }
  if (inClass && vars.size() > 1U) {
    throw ParserError(llvm::SMRange{vars[1]->getStart(), vars.back()->getEnd()},
                      "Class fields must declare a single identifier per field (comma-separated "
                      "bindings are not supported)");
  }

  std::unique_ptr<TypeExpr> type;
  if (advanceIfMatchAny<TokenType::COLON>()) {
    type = parseType();
  }

  std::unique_ptr<Expression> expr;
  if (advanceIfMatchAny<TokenType::EQUAL>()) {
    expr = parseExpression();
  }

  Literal* nameEnd = vars.back().get();
  if (!type && !expr) {
    throw ParserError(llvm::SMRange{startTok->getStart(), nameEnd->getEnd()},
                      "Expected either a type or a value");
  }

  if (!expr && !isMutable) {
    // In a class, `let x: T` without `=` is allowed: initialized by `new` (explicit or
    // synthesized from required fields).
    if (!(inClass && type != nullptr)) {
      throw ParserError(
          llvm::SMRange{startTok->getStart(), type != nullptr ? type->getEnd() : nameEnd->getEnd()},
          "Cannot declare an immutable variable without an initial expression");
    }
  }

  consumeNewlineOrBlockEnd();
  llvm::SMLoc endLoc = nameEnd->getEnd();
  if (expr != nullptr) {
    endLoc = expr->getEnd();
  } else if (type != nullptr) {
    endLoc = type->getEnd();
  }
  return std::make_unique<VarDecl>(llvm::SMRange{startTok->getStart(), endLoc}, std::move(vars),
                                   std::move(type), std::move(expr), isMutable, isExported,
                                   fieldIsPrivate);
}

auto Parser::parseIf() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::IF);

  std::vector<std::unique_ptr<Expression>> conds;
  std::vector<std::unique_ptr<Compound>> blocks;

  // Read first If cond and block
  conds.push_back(parseExpression());
  blocks.push_back(parseBlock());

  while (true) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    if (!advanceIfMatchAny<TokenType::ELSE_IF>()) {
      break;
    }
    conds.push_back(parseExpression());
    blocks.push_back(parseBlock());
  }
  while (check(TokenType::NEWLINE)) {
    advance();
  }
  if (advanceIfMatchAny<TokenType::ELSE>()) {
    conds.push_back(std::make_unique<Else>(peek()->span));
    blocks.push_back(parseBlock());
  }

  return std::make_unique<If>(llvm::SMRange{loc.Start, blocks.back()->getEnd()}, std::move(conds),
                              std::move(blocks));
}

auto Parser::parseWhile() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::WHILE);

  auto cond = parseExpression();
  auto block = parseBlock();

  return std::make_unique<While>(llvm::SMRange{loc.Start, block->getEnd()}, std::move(cond),
                                 std::move(block));
}

auto Parser::parseFor() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::FOR);
  auto* identifier = consume(TokenType::IDENTIFIER);
  consume(TokenType::IN);
  auto iterable = parseExpression();
  auto block = parseBlock();
  auto var = std::make_unique<Literal>(identifier->span, identifier->lexeme, identifier->type);
  return std::make_unique<ForIn>(llvm::SMRange{loc.Start, block->getEnd()}, std::move(var),
                                 std::move(iterable), std::move(block));
}

auto Parser::parseAssignment() -> std::unique_ptr<Statement> {
  auto identifier = parsePostfix();

  auto* literalPtr = dynamic_cast<Literal*>(identifier.get());
  auto* dotOpPtr = dynamic_cast<DotOp*>(identifier.get());
  auto* subscriptPtr = dynamic_cast<SubscriptOp*>(identifier.get());

  if ((literalPtr == nullptr || literalPtr->getType() != TokenType::IDENTIFIER) &&
      dotOpPtr == nullptr && subscriptPtr == nullptr) {
    throw ParserError(identifier->getSpan(),
                      "Expected identifier, field access, or subscript for assignment");
  }

  if (advanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL,
                        TokenType::STAR_EQUAL, TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL,
                        TokenType::POWER_EQUAL>()) {
    auto op = previous()->type;
    auto expr = parseExpression();

    consumeNewlineOrBlockEnd();
    return std::make_unique<Assignment>(llvm::SMRange{identifier->getStart(), expr->getEnd()},
                                        std::move(identifier), op, std::move(expr));
  }

  error(peek(), fmt::format("Unsupported assignment operator: {}", peek()->lexeme));

  return nullptr;
}

auto Parser::parseBreak() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::BREAK)->span;
  consumeNewlineOrBlockEnd();
  return std::make_unique<Break>(span);
}

auto Parser::parseContinue() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::CONTINUE)->span;
  consumeNewlineOrBlockEnd();
  return std::make_unique<Continue>(span);
}

auto Parser::parsePass() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::PASS)->span;
  consumeNewlineOrBlockEnd();
  return std::make_unique<Pass>(span);
}

auto Parser::parseReturn() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::RETURN);
  if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN ||
      check(TokenType::RIGHT_BRACE)) {
    consumeNewlineOrBlockEnd();
    return std::make_unique<Return>(loc, nullptr);
  }
  auto val = parseExpression();
  consumeNewlineOrBlockEnd();
  return std::make_unique<Return>(llvm::SMRange{loc.Start, val->getEnd()}, std::move(val));
}

auto Parser::parseDefer() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::DEFER);
  auto val = parseStatement(false);

  // Don't consume newline, since statement will
  return std::make_unique<Defer>(llvm::SMRange{loc.Start, val->getEnd()}, std::move(val));
}

auto Parser::parseStatement(bool isTopLevel) -> std::unique_ptr<Statement> {
  while (check(TokenType::NEWLINE)) {
    advance();
  }

  if (checkAny<TokenType::FUNC, TokenType::IMPORT, TokenType::CLASS, TokenType::ENUM,
               TokenType::TRAIT, TokenType::EXPORT>() &&
      !isTopLevel) {
    error(peek(), "Statement not allowed inside a block");
  }

  if (check(TokenType::PRIVATE) || check(TokenType::OVERLOAD)) {
    error(peek(), "`private` and `overload` are only valid on class fields and methods");
  }
  if (check(TokenType::FUNC)) {
    return parseFunctionDeclaration();
  }
  if (check(TokenType::TRAIT)) {
    return parseTrait();
  }
  if (checkAny<TokenType::IMPORT, TokenType::FROM>()) {
    return parseImport();
  }
  if (check(TokenType::CLASS)) {
    return parseClass();
  }
  if (check(TokenType::ENUM)) {
    return parseEnum();
  }
  if (check(TokenType::EXPORT)) {
    return parseExport();
  }
  if (check(TokenType::LET) || check(TokenType::VAR)) {
    return parseVarDecl();
  }
  if (check(TokenType::IF)) {
    return parseIf();
  }
  if (check(TokenType::WHILE)) {
    return parseWhile();
  }
  if (check(TokenType::FOR)) {
    return parseFor();
  }
  if (check(TokenType::BREAK)) {
    return parseBreak();
  }
  if (check(TokenType::CONTINUE)) {
    return parseContinue();
  }
  if (check(TokenType::PASS)) {
    return parsePass();
  }
  if (check(TokenType::RETURN)) {
    return parseReturn();
  }
  if (check(TokenType::DEFER)) {
    return parseDefer();
  }
  if (checkAnyInLine<TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL,
                     TokenType::STAR_EQUAL, TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL,
                     TokenType::POWER_EQUAL>()) {
    return parseAssignment();
  }

  // Check if it's just an expression statement
  auto expr = parseExpression();
  if (expr) {
    consumeNewlineOrBlockEnd();
    return std::make_unique<ExpressionStatement>(expr->getSpan(), std::move(expr));
  }

  error(peek(), "Unknown statement");
  return nullptr;
}

auto Parser::parseBlock() -> std::unique_ptr<Compound> {
  while (check(TokenType::NEWLINE)) {
    consume(TokenType::NEWLINE);
  }
  auto* openBrace = consume(TokenType::LEFT_BRACE);
  std::vector<std::unique_ptr<Statement>> statements;

  while (!checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    if (checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
      break;
    }
    try {
      std::unique_ptr<Statement> stmt = parseStatement(false);
      if (stmt != nullptr) {
        statements.push_back(std::move(stmt));
      }
    } catch (const ParserError& err) {
      if (diagnosticsOut == nullptr) {
        throw;
      }
      recoverFromParserError(err);
    }
  }

  auto* closeBrace = consume(TokenType::RIGHT_BRACE);
  return std::make_unique<Compound>(
      llvm::SMRange{openBrace->getStart(), closeBrace->getEnd()}, std::move(statements));
}

auto Parser::parseParameterList(bool allowVarargsEllipsis) -> ParameterListParseResult {
  ParameterListParseResult result;
  while (!check(TokenType::RIGHT_PAREN)) {
    while (check(TokenType::NEWLINE)) {
      advance();
    }
    if (result.varargs) {
      error(peek(), "Varargs should be the last parameter");
    }

    if (allowVarargsEllipsis && check(TokenType::ELLIPSIS)) {
      consume(TokenType::ELLIPSIS);
      result.varargs = true;
    } else {
      std::unique_ptr<Expression> defaultVal;
      std::unique_ptr<TypeExpr> type;
      auto* paramIdent = consume(TokenType::IDENTIFIER);

      if (advanceIfMatchAny<TokenType::COLON>()) {
        type = parseType();
      }

      if (advanceIfMatchAny<TokenType::EQUAL>()) {
        defaultVal = parseExpression();
      }

      if (!defaultVal && !type) {
        throw ParserError(paramIdent->span,
                          "{} should have either a type, a value or both specified",
                          paramIdent->lexeme);
      }

      result.parameters.push_back(std::make_unique<Parameter>(
          paramIdent->lexeme, paramIdent->span, std::move(type), false, std::move(defaultVal)));
    }

    if (!check(TokenType::RIGHT_PAREN) && !check(TokenType::RIGHT_PAREN, 1)) {
      consume(TokenType::COMMA);
    }
  }
  return result;
}

auto Parser::parseFunctionDeclaration(bool methodIsPrivate, bool declaresInheritanceOverload)
    -> std::unique_ptr<Statement> {
  auto loc = isExported ? previous()->span : peek()->span;
  consume(TokenType::FUNC);
  // `export class` / `export func` leave isExported set; locals inside the body must not inherit it.
  bool const funcExported = isExported;
  bool externFunc = false;

  if (advanceIfMatchAny<TokenType::EXTERN>()) {
    externFunc = true;
  }

  if (externFunc && inClass) {
    error(previous(), "Extern functions are not allowed in class definition.");
    return nullptr;
  }

  std::string functionName;
  llvm::SMRange functionNameSpan;
  llvm::SMRange overloadGlyphSpan{};
  std::optional<TokenType> pendingOverloadOperatorToken;
  if (advanceIfMatchAny<TokenType::OPERATOR>()) {
    if (!inClass) {
      error(previous(), "Operator declarations are only allowed in class definition.");
      return nullptr;
    }
    llvm::SMLoc operatorStart = previous()->getStart();
    llvm::SMLoc operatorEnd = previous()->getEnd();
    if (advanceIfMatchAny<TokenType::LEFT_SQUARE>()) {
      llvm::SMLoc const bracketStart = previous()->getStart();
      consume(TokenType::RIGHT_SQUARE, "Expected ']' after operator '['");
      operatorEnd = previous()->getEnd();
      overloadGlyphSpan = llvm::SMRange{bracketStart, operatorEnd};
      functionName = std::string{OperatorUtils::SUBSCRIPT_GET_NAME};
      if (advanceIfMatchAny<TokenType::EQUAL>()) {
        operatorEnd = previous()->getEnd();
        overloadGlyphSpan = llvm::SMRange{bracketStart, operatorEnd};
        functionName = std::string{OperatorUtils::SUBSCRIPT_SET_NAME};
      }
    } else {
      Token* opToken = advance();
      if (!OperatorUtils::isOverloadableDeclarationToken(opToken->type)) {
        error(opToken,
              fmt::format("Unsupported operator declaration token {}", NAMEOF_ENUM(opToken->type)));
        return nullptr;
      }
      operatorEnd = opToken->getEnd();
      overloadGlyphSpan = opToken->span;
      pendingOverloadOperatorToken = opToken->type;
    }
    functionNameSpan = llvm::SMRange{operatorStart, operatorEnd};
  } else {
    Token* nameTok = nullptr;
    if (check(TokenType::IDENTIFIER)) {
      nameTok = consume(TokenType::IDENTIFIER);
    } else if (check(TokenType::STRING_TYPE)) {
      nameTok = consume(TokenType::STRING_TYPE);
    } else {
      error(peek(), "Expected function name");
      return nullptr;
    }
    functionName = nameTok->lexeme;
    functionNameSpan = nameTok->span;
  }
  std::vector<GenericParamDecl> genericParams = parseGenericParamList();

  consume(TokenType::LEFT_PAREN);
  auto paramList = parseParameterList(externFunc);
  std::vector<std::unique_ptr<Parameter>> parameters = std::move(paramList.parameters);
  bool varargs = paramList.varargs;
  consume(TokenType::RIGHT_PAREN);

  if (pendingOverloadOperatorToken.has_value()) {
    const TokenType opKind = *pendingOverloadOperatorToken;
    if (parameters.empty()) {
      auto unaryName = OperatorUtils::getUnaryOperatorName(opKind);
      if (!unaryName.has_value()) {
        error(previous(),
              fmt::format("Operator {} is not overloadable as a unary operator (no parameters)",
                          NAMEOF_ENUM(opKind)));
        return nullptr;
      }
      functionName = std::string{*unaryName};
    } else {
      auto binaryName = OperatorUtils::getBinaryOperatorName(opKind);
      if (!binaryName.has_value()) {
        error(previous(),
              fmt::format("Operator {} is not overloadable as a binary operator (with parameters)",
                          NAMEOF_ENUM(opKind)));
        return nullptr;
      }
      functionName = std::string{*binaryName};
    }
  }

  std::unique_ptr<TypeExpr> returnType;
  if (advanceIfMatchAny<TokenType::ARROW>()) {
    returnType = parseType();
  } else {
    returnType = std::make_unique<TypeExpr>(previous()->span, "void", TokenType::VOID_TYPE);
  }

  if (externFunc) {
    consumeNewline();
    return std::make_unique<ExternFuncDecl>(llvm::SMRange{loc.Start, returnType->getEnd()},
                                            functionName, functionNameSpan,
                                            std::move(genericParams), std::move(returnType),
                                            std::move(parameters), varargs, funcExported);
  }

  bool const savedExported = isExported;
  isExported = false;
  auto body = parseBlock();
  isExported = savedExported;
  auto funcEnd = body ? body->getEnd() : returnType->getEnd();

  return std::make_unique<FuncDecl>(
      llvm::SMRange{loc.Start, funcEnd}, functionName, functionNameSpan, overloadGlyphSpan,
      std::move(genericParams), std::move(returnType), std::move(parameters), std::move(body),
      false, funcExported, methodIsPrivate, declaresInheritanceOverload);
}

auto Parser::parseExport() -> std::unique_ptr<Statement> {
  consume(TokenType::EXPORT);

  if (inClass) {
    error(peek(), "Cannot export class members");
  }

  while (check(TokenType::NEWLINE)) {
    advance();
  }

  if (!checkAny<TokenType::FUNC, TokenType::CLASS, TokenType::ENUM, TokenType::TRAIT, TokenType::LET,
                TokenType::VAR>()) {
    error(peek(), "Can only export functions, classes, enums, traits, and variables");
  }

  isExported = true;
  std::unique_ptr<Statement> statement;
  if (check(TokenType::FUNC)) {
    statement = parseFunctionDeclaration();
  } else if (check(TokenType::LET) || check(TokenType::VAR)) {
    statement = parseVarDecl();
    if (auto* vd = dynamic_cast<VarDecl*>(statement.get());
        vd != nullptr && vd->getVarLiterals().size() > 1U) {
      error(vd->getSpan(), "Cannot export destructuring declarations");
    }
  } else if (check(TokenType::IMPORT)) {
    statement = parseImport();
  } else if (check(TokenType::CLASS)) {
    statement = parseClass();
  } else if (check(TokenType::TRAIT)) {
    statement = parseTrait();
  } else if (check(TokenType::ENUM)) {
    statement = parseEnum();
  } else {
    error(peek(), "Unexpected statement after export");
  }

  isExported = false;
  return statement;
}

auto Parser::parseImport() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  bool selectiveImport = false;
  if (advanceIfMatchAny<TokenType::FROM>()) {
    selectiveImport = true;
  } else {
    consume(TokenType::IMPORT);
  }

  Token const* token = nullptr;
  std::string filepath;

  if (peek()->type == TokenType::STRING) {
    token = consume(TokenType::STRING);
    filepath = token->lexeme;
  } else if (peek()->type == TokenType::IDENTIFIER) {
    token = consume(TokenType::IDENTIFIER);
    filepath = getStdDir() + token->lexeme + ".les";
  } else {
    error(peek(), "Imports must be either strings for files or identifiers for "
                  "standard library");
    return nullptr;
  }

  if (!selectiveImport) {
    std::string alias = getBasename(token->lexeme);
    llvm::SMRange aliasSpan = token->type == TokenType::IDENTIFIER ? token->span : llvm::SMRange();
    if (advanceIfMatchAny<TokenType::AS>()) {
      Token const* aliasToken = consume(TokenType::IDENTIFIER);
      alias = aliasToken->lexeme;
      aliasSpan = aliasToken->span;
    }

    auto* endToken = consumeNewline();
    auto endLoc = endToken->getEnd();
    return std::make_unique<Import>(llvm::SMRange{loc.Start, endLoc}, filepath, alias, aliasSpan,
                                    token->type == TokenType::IDENTIFIER, true, false,
                                    std::vector<ImportedNameBinding>{});
  }

  consume(TokenType::IMPORT);

  if (advanceIfMatchAny<TokenType::STAR>()) {
    auto* endToken = consumeNewline();
    auto endLoc = endToken->getEnd();
    return std::make_unique<Import>(llvm::SMRange{loc.Start, endLoc}, filepath, std::string{},
                                    llvm::SMRange(), token->type == TokenType::IDENTIFIER, true,
                                    true, std::vector<ImportedNameBinding>{});
  }

  std::vector<ImportedNameBinding> importedNames;

  while (true) {
    Token const* identToken = consume(TokenType::IDENTIFIER);
    auto ident = identToken->lexeme;
    auto alias = ident;
    llvm::SMRange aliasSpan = identToken->span;
    if (advanceIfMatchAny<TokenType::AS>()) {
      Token const* aliasToken = consume(TokenType::IDENTIFIER);
      alias = aliasToken->lexeme;
      aliasSpan = aliasToken->span;
    }

    importedNames.push_back(ImportedNameBinding{
        .name = ident,
        .alias = alias,
        .nameSpan = identToken->span,
        .aliasSpan = aliasSpan,
    });

    if (!advanceIfMatchAny<TokenType::COMMA>()) {
      break;
    }
  }

  auto* endToken = consumeNewline();
  auto endLoc = endToken->getEnd();
  return std::make_unique<Import>(llvm::SMRange{loc.Start, endLoc}, filepath, std::string{},
                                  llvm::SMRange(), token->type == TokenType::IDENTIFIER, false,
                                  true, importedNames);
}

auto Parser::parseClass() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::CLASS);

  auto* token = consume(TokenType::IDENTIFIER);
  std::vector<GenericParamDecl> genericParams = parseGenericParamList();
  std::unique_ptr<TypeExpr> baseType;
  llvm::SMRange baseTypeSpan;
  if (advanceIfMatchAny<TokenType::COLON>()) {
    baseType = parseType();
    baseTypeSpan = baseType->getSpan();
  }
  std::vector<std::string> implTraitNames;
  std::vector<llvm::SMRange> implTraitSpans;
  std::vector<std::vector<std::unique_ptr<TypeExpr>>> implTraitTypeArgs;
  if (advanceIfMatchAny<TokenType::IMPL>()) {
    while (true) {
      auto* traitName = consume(TokenType::IDENTIFIER);
      std::vector<std::unique_ptr<TypeExpr>> traitArgs;
      if (advanceIfMatchAny<TokenType::LESS>()) {
        while (true) {
          traitArgs.push_back(parseType());
          if (!advanceIfMatchAny<TokenType::COMMA>()) {
            break;
          }
        }
        consume(TokenType::GREATER, "Expected '>' after trait type arguments");
      }
      implTraitNames.push_back(traitName->lexeme);
      implTraitSpans.push_back(traitName->span);
      implTraitTypeArgs.push_back(std::move(traitArgs));
      if (!advanceIfMatchAny<TokenType::COMMA>()) {
        break;
      }
    }
  }
  while (check(TokenType::NEWLINE)) {
    consume(TokenType::NEWLINE);
  }
  consume(TokenType::LEFT_BRACE);

  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  auto endLoc = token->getEnd();

  inClass = true;
  while (!checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
    try {
      if (checkAny<TokenType::PRIVATE, TokenType::OVERLOAD, TokenType::LET, TokenType::VAR,
                   TokenType::FUNC>()) {
        bool memberPrivate = false;
        bool inheritanceOverload = false;
        while (check(TokenType::PRIVATE) || check(TokenType::OVERLOAD)) {
          if (advanceIfMatchAny<TokenType::PRIVATE>()) {
            if (memberPrivate) {
              error(previous(), "Duplicate `private`");
            }
            memberPrivate = true;
          } else {
            if (inheritanceOverload) {
              error(peek(), "Duplicate `overload`");
            }
            consume(TokenType::OVERLOAD);
            inheritanceOverload = true;
          }
        }
        if (checkAny<TokenType::LET, TokenType::VAR>()) {
          if (inheritanceOverload) {
            error(peek(), "`overload` is not valid on class fields");
          }
          // Class fields reuse parseVarDecl; do not inherit `export` from `export class …`.
          bool const savedExported = isExported;
          isExported = false;
          auto restoreExported =
              llvm::make_scope_exit([this, savedExported] { isExported = savedExported; });
          auto stmt = parseVarDecl(memberPrivate);
          auto* varDecl = dynamic_cast<VarDecl*>(stmt.get());
          if (varDecl != nullptr) {
            endLoc = varDecl->getEnd();
            std::ignore = stmt.release();
            fields.push_back(std::unique_ptr<VarDecl>(varDecl));
          }
        } else if (checkAny<TokenType::FUNC>()) {
          // Class methods reuse parseFunctionDeclaration; do not inherit `export` from `export
          // class …`.
          bool const savedExported = isExported;
          isExported = false;
          auto restoreExported =
              llvm::make_scope_exit([this, savedExported] { isExported = savedExported; });
          auto stmt = parseFunctionDeclaration(memberPrivate, inheritanceOverload);
          auto* funcDecl = dynamic_cast<FuncDecl*>(stmt.get());
          if (funcDecl != nullptr) {
            endLoc = funcDecl->getEnd();
            std::ignore = stmt.release();
            methods.push_back(std::unique_ptr<FuncDecl>(funcDecl));
          }
        } else {
          error(peek(), "Expected field or method after `private` / `overload`");
        }
      } else if (check(TokenType::NEWLINE)) {
        consume(TokenType::NEWLINE);
      } else if (diagnosticsOut != nullptr) {
        pushParserDiagnostic(peek()->span.isValid() ? peek()->span : llvm::SMRange(),
                             "Expected field, method, or newline in class body");
        synchronizeToNextLine();
      } else {
        error(peek(), "Expected field, method, or newline in class body");
      }
    } catch (const ParserError& err) {
      if (diagnosticsOut == nullptr) {
        throw;
      }
      recoverFromParserError(err);
    }
  }
  inClass = false;

  auto* classClose = consume(TokenType::RIGHT_BRACE);
  endLoc = classClose->getEnd();

  return std::make_unique<Class>(
      llvm::SMRange{loc.Start, endLoc}, token->lexeme, token->span, std::move(genericParams),
      std::move(implTraitNames), std::move(implTraitSpans), std::move(implTraitTypeArgs),
      std::move(baseType), baseTypeSpan, std::move(fields), std::move(methods), isExported);
}

auto Parser::parseTraitMethodDeclaration() -> std::unique_ptr<FuncDecl> {
  auto loc = peek()->span;
  consume(TokenType::FUNC);
  if (inClass) {
    error(previous(), "Trait requirements cannot be declared inside a class");
  }
  auto* identifier = consume(TokenType::IDENTIFIER);
  std::string functionName = identifier->lexeme;
  llvm::SMRange functionNameSpan = identifier->span;
  if (check(TokenType::LESS)) {
    error(peek(), "Generic parameters are not allowed on trait requirement methods");
  }
  consume(TokenType::LEFT_PAREN);
  auto traitParamList = parseParameterList(false);
  std::vector<std::unique_ptr<Parameter>> parameters = std::move(traitParamList.parameters);
  consume(TokenType::RIGHT_PAREN);
  std::unique_ptr<TypeExpr> returnType;
  if (advanceIfMatchAny<TokenType::ARROW>()) {
    returnType = parseType();
  } else {
    returnType = std::make_unique<TypeExpr>(previous()->span, "void", TokenType::VOID_TYPE);
  }
  // Requirement: signature only, or default body `{ ... }`.
  std::unique_ptr<Compound> body;
  llvm::SMLoc funcEndLoc = returnType->getEnd();
  while (check(TokenType::NEWLINE)) {
    consume(TokenType::NEWLINE);
  }
  if (check(TokenType::LEFT_BRACE)) {
    body = parseBlock();
    funcEndLoc = body->getEnd();
  } else {
    funcEndLoc = returnType->getEnd();
  }
  return std::make_unique<FuncDecl>(llvm::SMRange{loc.Start, funcEndLoc}, functionName,
                                    functionNameSpan, llvm::SMRange{},
                                    std::vector<GenericParamDecl>{}, std::move(returnType),
                                    std::move(parameters), std::move(body), false, false);
}

auto Parser::parseTrait() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::TRAIT);
  auto* nameTok = consume(TokenType::IDENTIFIER);
  std::vector<GenericParamDecl> genericParams = parseGenericParamList();
  while (check(TokenType::NEWLINE)) {
    consume(TokenType::NEWLINE);
  }
  consume(TokenType::LEFT_BRACE);
  std::vector<std::unique_ptr<FuncDecl>> requirements;
  auto endLoc = nameTok->getEnd();
  while (!checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    if (checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
      break;
    }
    try {
      if (check(TokenType::FUNC)) {
        auto req = parseTraitMethodDeclaration();
        endLoc = req->getEnd();
        requirements.push_back(std::move(req));
      } else {
        error(peek(), "Expected 'func' in trait body");
      }
    } catch (const ParserError& err) {
      if (diagnosticsOut == nullptr) {
        throw;
      }
      recoverFromParserError(err);
    }
  }
  auto* traitClose = consume(TokenType::RIGHT_BRACE);
  endLoc = traitClose->getEnd();
  return std::make_unique<TraitDecl>(llvm::SMRange{loc.Start, endLoc}, nameTok->lexeme,
                                     nameTok->span, std::move(genericParams),
                                     std::move(requirements), isExported);
}

auto Parser::parseEnum() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::ENUM);

  auto* token = consume(TokenType::IDENTIFIER);
  while (check(TokenType::NEWLINE)) {
    consume(TokenType::NEWLINE);
  }
  consume(TokenType::LEFT_BRACE);

  std::vector<std::string> values;
  std::vector<llvm::SMRange> valueSpans;
  auto endLoc = token->getEnd();

  while (!checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
    try {
      while (check(TokenType::NEWLINE)) {
        consume(TokenType::NEWLINE);
      }
      if (checkAny<TokenType::RIGHT_BRACE, TokenType::EOF_TOKEN>()) {
        break;
      }
      auto* valueToken = consume(TokenType::IDENTIFIER);
      values.push_back(valueToken->lexeme);
      valueSpans.push_back(valueToken->span);
      endLoc = valueToken->getEnd();
      consume(TokenType::NEWLINE);
    } catch (const ParserError& err) {
      if (diagnosticsOut == nullptr) {
        throw;
      }
      recoverFromParserError(err);
    }
  }

  auto* enumClose = consume(TokenType::RIGHT_BRACE);
  endLoc = enumClose->getEnd();

  return std::make_unique<Enum>(llvm::SMRange{loc.Start, endLoc}, token->lexeme, token->span,
                                values, std::move(valueSpans), isExported);
}

auto Parser::parseCompound() -> std::unique_ptr<Compound> {
  std::vector<std::unique_ptr<Statement>> statements;
  while (!isAtEnd()) {
    // Remove lingering newlines
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    if (isAtEnd()) {
      break;
    }
    try {
      std::unique_ptr<Statement> stmt = parseStatement(true);
      if (stmt != nullptr) {
        statements.push_back(std::move(stmt));
      }
    } catch (const ParserError& err) {
      if (diagnosticsOut == nullptr) {
        throw;
      }
      recoverFromParserError(err);
      if (peek()->type == TokenType::RIGHT_BRACE) {
        advance();
      }
    }
  }
  if (statements.empty()) {
    return std::make_unique<Compound>(peek()->span, std::move(statements));
  }
  return std::make_unique<Compound>(
      llvm::SMRange{statements.front()->getStart(), statements.back()->getEnd()},
      std::move(statements));
}

auto Parser::parse() -> void { tree = parseCompound(); }
