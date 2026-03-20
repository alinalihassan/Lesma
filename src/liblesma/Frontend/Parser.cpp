#include "Parser.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <llvm/Support/SMLoc.h>

#include "fmt/core.h"
#include "nameof.hpp"
#include <fmt/format.h>

#include "liblesma/AST/AST.h"
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

auto Parser::parseType() -> std::unique_ptr<TypeExpr> {
  auto* type = peek();
  if (check(TokenType::STAR)) {
    advance();
    auto elementType = parseType();
    return std::make_unique<TypeExpr>(llvm::SMRange{type->getStart(), elementType->getEnd()},
                                      "*" + elementType->getName(), TokenType::PTR_TYPE,
                                      std::move(elementType));
  }
  if (checkAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE,
               TokenType::BOOL_TYPE, TokenType::INT8_TYPE, TokenType::INT16_TYPE,
               TokenType::INT32_TYPE, TokenType::FLOAT32_TYPE, TokenType::VOID_TYPE>()) {
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

  if (checkAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE, TokenType::STRING_TYPE,
               TokenType::BOOL_TYPE, TokenType::INT8_TYPE, TokenType::INT16_TYPE,
               TokenType::INT32_TYPE, TokenType::FLOAT32_TYPE, TokenType::VOID_TYPE>(off)) {
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

  if (check(TokenType::IDENTIFIER, off)) {
    off++;
    return true;
  }

  return false;
}

auto Parser::skipOneTypeAt(unsigned long& off) -> bool { return parseTypeAt(off); }

auto Parser::hasExplicitTypeArgsAndParen() -> bool {
  if (!check(TokenType::IDENTIFIER) || !check(TokenType::LESS, 1)) {
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
  consume(TokenType::IDENTIFIER);

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
  case TokenType::IDENTIFIER: {
    if (check(TokenType::LEFT_PAREN, 1) || hasExplicitTypeArgsAndParen()) {
      return parseFunctionCall();
    }

    auto* token = peek();
    consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, token->type);
  }
  case TokenType::LEFT_PAREN: {
    consume(TokenType::LEFT_PAREN);
    auto expr = parseExpression();
    consume(TokenType::RIGHT_PAREN);
    return expr;
  }
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

auto Parser::parseDot() -> std::unique_ptr<Expression> {
  auto left = parseTerm();

  while (advanceIfMatchAny<TokenType::DOT>()) {
    auto* op = previous();
    auto expr = parseTerm();
    left = std::make_unique<DotOp>(llvm::SMRange{left->getStart(), expr->getEnd()}, std::move(left),
                                   op->type, std::move(expr));
  }

  return left;
}

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
  auto* identifier = consume(TokenType::IDENTIFIER);
  auto var = std::make_unique<Literal>(identifier->span, identifier->lexeme, identifier->type);

  std::unique_ptr<TypeExpr> type;
  if (advanceIfMatchAny<TokenType::COLON>()) {
    type = parseType();
  }

  std::unique_ptr<Expression> expr;
  if (advanceIfMatchAny<TokenType::EQUAL>()) {
    expr = parseExpression();
  }

  if (!type && !expr) {
    throw ParserError(llvm::SMRange{startTok->getStart(), var->getEnd()},
                      "Expected either a type or a value");
  }

  if (!expr && !isMutable) {
    throw ParserError(llvm::SMRange{startTok->getStart(), type->getEnd()},
                      "Cannot declare an immutable variable without an initial expression");
  }

  consumeNewline();
  llvm::SMLoc const endLoc = expr ? expr->getEnd() : type->getEnd();
  return std::make_unique<VarDecl>(llvm::SMRange{startTok->getStart(), endLoc}, std::move(var),
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
  consume(TokenType::IDENTIFIER);
  consume(TokenType::IN);
  parseExpression();
  auto block = parseBlock();
  return std::make_unique<UnimplementedStatement>(llvm::SMRange{loc.Start, block->getEnd()},
                                                  "for loop is not yet implemented");
}

auto Parser::parseAssignment() -> std::unique_ptr<Statement> {
  auto identifier = parseDot();

  auto* literalPtr = dynamic_cast<Literal*>(identifier.get());
  auto* dotOpPtr = dynamic_cast<DotOp*>(identifier.get());

  if ((literalPtr == nullptr || literalPtr->getType() != TokenType::IDENTIFIER) &&
      dotOpPtr == nullptr) {
    throw ParserError(identifier->getSpan(),
                      "Expected either identifier or class field for assignment");
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
               TokenType::EXPORT>() &&
      !isTopLevel) {
    error(peek(), "Statement not allowed inside a block");
  }

  if (check(TokenType::DEF)) {
    return parseFunctionDeclaration();
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

  auto* identifier = consume(TokenType::IDENTIFIER);
  std::vector<std::string> genericParams;
  if (check(TokenType::LESS)) {
    consume(TokenType::LESS);
    while (!check(TokenType::GREATER)) {
      genericParams.push_back(consume(TokenType::IDENTIFIER)->lexeme);
      if (!check(TokenType::GREATER)) {
        consume(TokenType::COMMA);
      }
    }
    consume(TokenType::GREATER);
  }

  // Parse parameters
  consume(TokenType::LEFT_PAREN);
  std::vector<std::unique_ptr<Parameter>> parameters;

  bool varargs = false;
  while (!check(TokenType::RIGHT_PAREN)) {
    if (varargs) {
      error(peek(), "Varargs should be the last parameter");
    }

    if (check(TokenType::ELLIPSIS) && externFunc) {
      consume(TokenType::ELLIPSIS);
      varargs = true;
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

      parameters.push_back(std::make_unique<Parameter>(paramIdent->lexeme, paramIdent->span,
                                                       std::move(type), false,
                                                       std::move(defaultVal)));
    }

    if (!check(TokenType::RIGHT_PAREN) && !check(TokenType::RIGHT_PAREN, 1)) {
      consume(TokenType::COMMA);
    }
  }

  consume(TokenType::RIGHT_PAREN);

  std::unique_ptr<TypeExpr> returnType;
  if (advanceIfMatchAny<TokenType::ARROW>()) {
    returnType = parseType();
  } else {
    returnType = std::make_unique<TypeExpr>(previous()->span, "void", TokenType::VOID_TYPE);
  }

  if (externFunc) {
    consumeNewline();
    return std::make_unique<ExternFuncDecl>(llvm::SMRange{loc.Start, returnType->getEnd()},
                                            identifier->lexeme, identifier->span,
                                            std::move(genericParams), std::move(returnType),
                                            std::move(parameters), varargs, isExported);
  }

  auto body = parseBlock();

  return std::make_unique<FuncDecl>(llvm::SMRange{loc.Start, returnType->getEnd()},
                                    identifier->lexeme, identifier->span, std::move(genericParams),
                                    std::move(returnType), std::move(parameters), std::move(body),
                                    false, isExported);
}

auto Parser::parseExport() -> std::unique_ptr<Statement> {
  consume(TokenType::EXPORT);

  if (inClass) {
    error(peek(), "Cannot export class members");
  }

  if (!checkAny<TokenType::DEF, TokenType::CLASS, TokenType::ENUM>()) {
    error(peek(), "Can only export functions, classes and enums");
  }

  isExported = true;
  std::unique_ptr<Statement> statement;
  if (check(TokenType::DEF)) {
    statement = parseFunctionDeclaration();
  } else if (check(TokenType::IMPORT)) {
    statement = parseImport();
  } else if (check(TokenType::CLASS)) {
    statement = parseClass();
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
    if (advanceIfMatchAny<TokenType::AS>()) {
      alias = consume(TokenType::IDENTIFIER)->lexeme;
    }

    consumeNewline();
    return std::make_unique<Import>(llvm::SMRange{loc.Start, token->getEnd()}, filepath, alias,
                                    token->type == TokenType::IDENTIFIER, true, false,
                                    std::vector<std::pair<std::string, std::string>>{});
  }

  consume(TokenType::IMPORT);

  if (advanceIfMatchAny<TokenType::STAR>()) {
    consumeNewline();
    return std::make_unique<Import>(llvm::SMRange{loc.Start, token->getEnd()}, filepath,
                                    getBasename(token->lexeme),
                                    token->type == TokenType::IDENTIFIER, true, true,
                                    std::vector<std::pair<std::string, std::string>>{});
  }

  std::vector<std::pair<std::string, std::string>> importedNames;

  while (true) {
    auto ident = consume(TokenType::IDENTIFIER)->lexeme;
    auto alias = ident;
    if (advanceIfMatchAny<TokenType::AS>()) {
      alias = consume(TokenType::IDENTIFIER)->lexeme;
    }

    importedNames.emplace_back(ident, alias);

    if (!advanceIfMatchAny<TokenType::COMMA>()) {
      break;
    }
  }

  consumeNewline();
  return std::make_unique<Import>(llvm::SMRange{loc.Start, token->getEnd()}, filepath,
                                  getBasename(token->lexeme), token->type == TokenType::IDENTIFIER,
                                  false, true, importedNames);
}

auto Parser::parseClass() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::CLASS);

  auto* token = consume(TokenType::IDENTIFIER);
  std::vector<std::string> genericParams;
  if (check(TokenType::LESS)) {
    consume(TokenType::LESS);
    while (!check(TokenType::GREATER)) {
      genericParams.push_back(consume(TokenType::IDENTIFIER)->lexeme);
      if (!check(TokenType::GREATER)) {
        consume(TokenType::COMMA);
      }
    }
    consume(TokenType::GREATER);
  }
  consume(TokenType::NEWLINE);

  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  consume(TokenType::INDENT);

  inClass = true;
  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    if (checkAny<TokenType::LET, TokenType::VAR>()) {
      auto stmt = parseVarDecl();
      auto* varDecl = dynamic_cast<VarDecl*>(stmt.get());
      if (varDecl != nullptr) {
        std::ignore = stmt.release();
        fields.push_back(std::unique_ptr<VarDecl>(varDecl));
      }
    } else if (checkAny<TokenType::DEF>()) {
      auto stmt = parseFunctionDeclaration();
      auto* funcDecl = dynamic_cast<FuncDecl*>(stmt.get());
      if (funcDecl != nullptr) {
        std::ignore = stmt.release();
        methods.push_back(std::unique_ptr<FuncDecl>(funcDecl));
      }
    } else {
      consume(TokenType::NEWLINE);
    }
  }
  inClass = false;

  advanceIfMatchAny<TokenType::DEDENT>();

  return std::make_unique<Class>(loc, token->lexeme, token->span, std::move(genericParams),
                                 std::move(fields), std::move(methods), isExported);
}

auto Parser::parseEnum() -> std::unique_ptr<Statement> {
  auto loc = peek()->span;
  consume(TokenType::ENUM);

  auto* token = consume(TokenType::IDENTIFIER);
  consume(TokenType::NEWLINE);

  std::vector<std::string> values;
  consume(TokenType::INDENT);

  while (!checkAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    values.push_back(consume(TokenType::IDENTIFIER)->lexeme);
    consume(TokenType::NEWLINE);
  }

  advanceIfMatchAny<TokenType::DEDENT>();

  return std::make_unique<Enum>(loc, token->lexeme, token->span, values, isExported);
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
