#include "Parser.h"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

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

auto Parser::error(Token* token, const std::string& errorMessage) -> void {
  throw ParserError(token->span, "{}", errorMessage);
}

auto Parser::parseGenericParamList() -> std::vector<GenericParamDecl> {
  std::vector<GenericParamDecl> genericParams;
  if (!check(TokenType::LESS)) {
    return genericParams;
  }
  consume(TokenType::LESS);
  while (!check(TokenType::GREATER)) {
    auto* genericParam = consume(TokenType::IDENTIFIER);
    std::vector<std::string> bounds;
    std::vector<llvm::SMRange> boundSpans;
    if (advanceIfMatchAny<TokenType::COLON>()) {
      auto* boundId = consume(TokenType::IDENTIFIER);
      bounds.push_back(boundId->lexeme);
      boundSpans.push_back(boundId->span);
      while (advanceIfMatchAny<TokenType::AMPERSAND>()) {
        boundId = consume(TokenType::IDENTIFIER);
        bounds.push_back(boundId->lexeme);
        boundSpans.push_back(boundId->span);
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

auto Parser::parseType() -> std::unique_ptr<TypeExpr> {
  auto* type = peek();
  if (check(TokenType::LEFT_PAREN)) {
    auto* left = consume(TokenType::LEFT_PAREN);
    std::unique_ptr<TypeExpr> innerFirst = parseType();
    if (advanceIfMatchAny<TokenType::COMMA>()) {
      std::vector<std::unique_ptr<TypeExpr>> elems;
      elems.push_back(std::move(innerFirst));
      while (!check(TokenType::RIGHT_PAREN)) {
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

    // Function types are represented as pointer-to-function in the type system.
    auto inner =
        std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), ret->getEnd()}, lexeme,
                                   TokenType::FUNC_TYPE, std::move(params), std::move(ret));
    return std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), inner->getEnd()},
                                      "*" + lexeme, TokenType::PTR_TYPE, std::move(inner));
  }

  if (check(TokenType::IDENTIFIER)) {
    advance();
    if (check(TokenType::LESS)) {
      consume(TokenType::LESS);
      std::vector<std::unique_ptr<TypeExpr>> typeArgs;
      while (true) {
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
    elements.push_back(parseExpression());
    if (!check(TokenType::RIGHT_SQUARE)) {
      consume(TokenType::COMMA);
    }
  }
  auto* end = consume(TokenType::RIGHT_SQUARE);
  return std::make_unique<ListLiteral>(llvm::SMRange{start->getStart(), end->getEnd()},
                                       std::move(elements));
}

auto Parser::parseTerm() -> std::unique_ptr<Expression> {
  switch (peek()->type) {
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
    auto first = parseExpression();
    if (advanceIfMatchAny<TokenType::COMMA>()) {
      std::vector<std::unique_ptr<Expression>> elems;
      elems.push_back(std::move(first));
      while (!check(TokenType::RIGHT_PAREN)) {
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
  case TokenType::TRUE_:
  case TokenType::FALSE_: {
    auto* token = peek();
    consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, TokenType::BOOL);
  }
  default:
    error(peek(), fmt::format("Unknown literal: {}", peek()->lexeme));
  }

  return nullptr;
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
auto Parser::parseVarDecl() -> std::unique_ptr<Statement> {
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
    throw ParserError(
        llvm::SMRange{startTok->getStart(), type != nullptr ? type->getEnd() : nameEnd->getEnd()},
        "Cannot declare an immutable variable without an initial expression");
  }

  consumeNewline();
  llvm::SMLoc endLoc = nameEnd->getEnd();
  if (expr != nullptr) {
    endLoc = expr->getEnd();
  } else if (type != nullptr) {
    endLoc = type->getEnd();
  }
  return std::make_unique<VarDecl>(llvm::SMRange{startTok->getStart(), endLoc}, std::move(vars),
                                   std::move(type), std::move(expr), isMutable);
}

auto Parser::parseIf() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::IF);

  std::vector<std::unique_ptr<Expression>> conds;
  std::vector<std::unique_ptr<Compound>> blocks;

  // Read first If cond and block
  conds.push_back(parseExpression());
  blocks.push_back(parseBlock());

  while (advanceIfMatchAny<TokenType::ELSE_IF>()) {
    conds.push_back(parseExpression());
    blocks.push_back(parseBlock());
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

    consumeNewline();
    return std::make_unique<Assignment>(llvm::SMRange{identifier->getStart(), expr->getEnd()},
                                        std::move(identifier), op, std::move(expr));
  }

  error(peek(), fmt::format("Unsupported assignment operator: {}", peek()->lexeme));

  return nullptr;
}

auto Parser::parseBreak() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::BREAK)->span;
  consumeNewline();
  return std::make_unique<Break>(span);
}

auto Parser::parseContinue() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::CONTINUE)->span;
  consumeNewline();
  return std::make_unique<Continue>(span);
}

auto Parser::parsePass() -> std::unique_ptr<Statement> {
  auto span = consume(TokenType::PASS)->span;
  consumeNewline();
  return std::make_unique<Pass>(span);
}

auto Parser::parseReturn() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::RETURN);
  if (check(TokenType::NEWLINE) || peek()->type == TokenType::EOF_TOKEN) {
    consumeNewline();
    return std::make_unique<Return>(loc, nullptr);
  }
  auto val = parseExpression();
  consumeNewline();
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
  if (checkAny<TokenType::DEF, TokenType::IMPORT, TokenType::CLASS, TokenType::ENUM,
               TokenType::TRAIT, TokenType::EXPORT>() &&
      !isTopLevel) {
    error(peek(), "Statement not allowed inside a block");
  }

  if (check(TokenType::DEF)) {
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
    consumeNewline();
    return std::make_unique<ExpressionStatement>(expr->getSpan(), std::move(expr));
  }

  error(peek(), "Unknown statement");
  return nullptr;
}

auto Parser::parseBlock() -> std::unique_ptr<Compound> {
  std::vector<std::unique_ptr<Statement>> statements;

  consume(TokenType::NEWLINE);
  auto* indentTok = consume(TokenType::INDENT);

  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    if (checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
      break;
    }
    statements.push_back(parseStatement(false));
  }

  advanceIfMatchAny<TokenType::DEDENT>();

  if (statements.empty()) {
    return std::make_unique<Compound>(indentTok->span, std::move(statements));
  }
  return std::make_unique<Compound>(
      llvm::SMRange{statements.front()->getStart(), statements.back()->getEnd()},
      std::move(statements));
}

auto Parser::parseParameterList(bool allowVarargsEllipsis) -> ParameterListParseResult {
  ParameterListParseResult result;
  while (!check(TokenType::RIGHT_PAREN)) {
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

auto Parser::parseFunctionDeclaration() -> std::unique_ptr<Statement> {
  auto loc = isExported ? previous()->span : peek()->span;
  consume(TokenType::DEF);
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
  std::optional<TokenType> pendingOverloadOperatorToken;
  if (advanceIfMatchAny<TokenType::OPERATOR>()) {
    if (!inClass) {
      error(previous(), "Operator declarations are only allowed in class definition.");
      return nullptr;
    }
    llvm::SMLoc operatorStart = previous()->getStart();
    llvm::SMLoc operatorEnd = previous()->getEnd();
    if (advanceIfMatchAny<TokenType::LEFT_SQUARE>()) {
      consume(TokenType::RIGHT_SQUARE, "Expected ']' after operator '['");
      operatorEnd = previous()->getEnd();
      functionName = std::string{OperatorUtils::SUBSCRIPT_GET_NAME};
      if (advanceIfMatchAny<TokenType::EQUAL>()) {
        operatorEnd = previous()->getEnd();
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
                                            std::move(parameters), varargs, isExported);
  }

  auto body = parseBlock();
  auto funcEnd = body ? body->getEnd() : returnType->getEnd();

  return std::make_unique<FuncDecl>(
      llvm::SMRange{loc.Start, funcEnd}, functionName, functionNameSpan, std::move(genericParams),
      std::move(returnType), std::move(parameters), std::move(body), false, isExported);
}

auto Parser::parseExport() -> std::unique_ptr<Statement> {
  consume(TokenType::EXPORT);

  if (inClass) {
    error(peek(), "Cannot export class members");
  }

  if (!checkAny<TokenType::DEF, TokenType::CLASS, TokenType::ENUM, TokenType::TRAIT>()) {
    error(peek(), "Can only export functions, classes, enums, and traits");
  }

  isExported = true;
  std::unique_ptr<Statement> statement;
  if (check(TokenType::DEF)) {
    statement = parseFunctionDeclaration();
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
  consume(TokenType::NEWLINE);

  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  auto endLoc = token->getEnd();
  consume(TokenType::INDENT);

  inClass = true;
  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    if (checkAny<TokenType::LET, TokenType::VAR>()) {
      auto stmt = parseVarDecl();
      auto* varDecl = dynamic_cast<VarDecl*>(stmt.get());
      if (varDecl != nullptr) {
        endLoc = varDecl->getEnd();
        std::ignore = stmt.release();
        fields.push_back(std::unique_ptr<VarDecl>(varDecl));
      }
    } else if (checkAny<TokenType::DEF>()) {
      auto stmt = parseFunctionDeclaration();
      auto* funcDecl = dynamic_cast<FuncDecl*>(stmt.get());
      if (funcDecl != nullptr) {
        endLoc = funcDecl->getEnd();
        std::ignore = stmt.release();
        methods.push_back(std::unique_ptr<FuncDecl>(funcDecl));
      }
    } else {
      consume(TokenType::NEWLINE);
    }
  }
  inClass = false;

  if (advanceIfMatchAny<TokenType::DEDENT>()) {
    endLoc = previous()->getEnd();
  }

  return std::make_unique<Class>(llvm::SMRange{loc.Start, endLoc}, token->lexeme, token->span,
                                 std::move(genericParams), std::move(implTraitNames),
                                 std::move(implTraitSpans), std::move(implTraitTypeArgs),
                                 std::move(fields), std::move(methods), isExported);
}

auto Parser::parseTraitMethodDeclaration() -> std::unique_ptr<FuncDecl> {
  auto loc = peek()->span;
  consume(TokenType::DEF);
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
  // Requirement: same as `def extern` — signature and newline only. Default implementation: an
  // indented block (same as a normal `def` body).
  std::unique_ptr<Compound> body;
  llvm::SMLoc funcEndLoc = returnType->getEnd();
  const bool hasIndentedBody = peek()->type == TokenType::NEWLINE && index + 1 < tokens.size() &&
                               peek(1)->type == TokenType::INDENT;
  if (hasIndentedBody) {
    body = parseBlock();
    funcEndLoc = body->getEnd();
  } else {
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    funcEndLoc = returnType->getEnd();
  }
  return std::make_unique<FuncDecl>(llvm::SMRange{loc.Start, funcEndLoc}, functionName,
                                    functionNameSpan, std::vector<GenericParamDecl>{},
                                    std::move(returnType), std::move(parameters), std::move(body),
                                    false, false);
}

auto Parser::parseTrait() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::TRAIT);
  auto* nameTok = consume(TokenType::IDENTIFIER);
  std::vector<GenericParamDecl> genericParams = parseGenericParamList();
  consume(TokenType::NEWLINE);
  consume(TokenType::INDENT);
  std::vector<std::unique_ptr<FuncDecl>> requirements;
  auto endLoc = nameTok->getEnd();
  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    while (peek()->type == TokenType::NEWLINE) {
      consume(TokenType::NEWLINE);
    }
    if (checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
      break;
    }
    if (check(TokenType::DEF)) {
      auto req = parseTraitMethodDeclaration();
      endLoc = req->getEnd();
      requirements.push_back(std::move(req));
    } else {
      error(peek(), "Expected 'def' in trait body");
    }
  }
  if (advanceIfMatchAny<TokenType::DEDENT>()) {
    endLoc = previous()->getEnd();
  }
  return std::make_unique<TraitDecl>(llvm::SMRange{loc.Start, endLoc}, nameTok->lexeme,
                                     nameTok->span, std::move(genericParams),
                                     std::move(requirements), isExported);
}

auto Parser::parseEnum() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::ENUM);

  auto* token = consume(TokenType::IDENTIFIER);
  consume(TokenType::NEWLINE);

  std::vector<std::string> values;
  std::vector<llvm::SMRange> valueSpans;
  auto endLoc = token->getEnd();
  consume(TokenType::INDENT);

  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    auto* valueToken = consume(TokenType::IDENTIFIER);
    values.push_back(valueToken->lexeme);
    valueSpans.push_back(valueToken->span);
    endLoc = valueToken->getEnd();
    consume(TokenType::NEWLINE);
  }

  if (advanceIfMatchAny<TokenType::DEDENT>()) {
    endLoc = previous()->getEnd();
  }

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
    statements.push_back(parseStatement(true));
  }
  if (statements.empty()) {
    return std::make_unique<Compound>(peek()->span, std::move(statements));
  }
  return std::make_unique<Compound>(
      llvm::SMRange{statements.front()->getStart(), statements.back()->getEnd()},
      std::move(statements));
}

auto Parser::parse() -> void { tree = parseCompound(); }
