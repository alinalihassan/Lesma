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

template <TokenType type, TokenType... remained_types>
auto Parser::AdvanceIfMatchAny() -> bool {
  if (CheckAny<type, remained_types...>()) {
    Advance();
    return true;
  }

  return false;
}

template <TokenType type, TokenType... remained_types>
auto Parser::CheckAny() -> bool {
  return CheckAny<type, remained_types...>(0);
}

template <TokenType type, TokenType... remained_types>
auto Parser::CheckAnyInLine() -> bool {
  int i = 0;
  while (!CheckAny<TokenType::NEWLINE, TokenType::EOF_TOKEN>(i)) {
    if (CheckAny<type, remained_types...>(i)) {
      return true;
    }
    i++;
  }

  return false;
}

template <TokenType type, TokenType... remained_types>
auto Parser::CheckAny(unsigned long pos) -> bool {
  if (!Check(type, pos)) {
    if constexpr (sizeof...(remained_types) > 0) {
      return CheckAny<remained_types...>(pos);
    } else {
      return false;
    }
  }
  return true;
}

auto Parser::Consume(TokenType type) -> Token* {
  return Consume(type,
                 std::string{"Expected: "} + std::string{NAMEOF_ENUM(type)} +
                     ", found: " + std::string{NAMEOF_ENUM(Peek()->type)});
}

auto Parser::Consume(TokenType type, const std::string& errorMessage)
    -> Token* {
  if (Check(type)) {
    return Advance();
  }
  Error(Peek(), errorMessage);
  return nullptr;
}

auto Parser::ConsumeNewline() -> Token* {
  if (Check(TokenType::NEWLINE) || Peek()->type == TokenType::EOF_TOKEN) {
    return Advance();
  }
  Error(Peek(), fmt::format("Expected: NEWLINE or EOF, found: {}",
                            NAMEOF_ENUM(Peek()->type)));
  return nullptr;
}

auto Parser::Error(Token* token, const std::string& errorMessage) -> void {
  throw ParserError(token->span, "{}", errorMessage);
}

auto Parser::ParseType() -> std::unique_ptr<TypeExpr> {
  auto* type = Peek();
  if (Check(TokenType::STAR)) {
    Advance();
    auto elementType = ParseType();
    return std::make_unique<TypeExpr>(
        llvm::SMRange{type->GetStart(), elementType->GetEnd()},
        "*" + elementType->GetName(), TokenType::PTR_TYPE,
        std::move(elementType));
  }
  if (CheckAny<TokenType::INT_TYPE, TokenType::FLOAT_TYPE,
               TokenType::STRING_TYPE, TokenType::BOOL_TYPE,
               TokenType::INT8_TYPE, TokenType::INT16_TYPE,
               TokenType::INT32_TYPE, TokenType::FLOAT32_TYPE,
               TokenType::VOID_TYPE>()) {
    Advance();
    return std::make_unique<TypeExpr>(type->span, type->lexeme, type->type);
  }
  if (Check(TokenType::FUNC)) {
    std::vector<std::unique_ptr<TypeExpr>> params;
    std::unique_ptr<TypeExpr> ret;
    std::string lexeme = type->lexeme + " (";

    Advance();
    Consume(TokenType::LEFT_PAREN);
    do {
      if (!params.empty()) {
        lexeme += ", ";
      }
      params.push_back(ParseType());
      lexeme += params.back()->GetName();
    } while (AdvanceIfMatchAny<TokenType::COMMA>());

    Consume(TokenType::RIGHT_PAREN);
    lexeme += ")";

    if (AdvanceIfMatchAny<TokenType::ARROW>()) {
      ret = ParseType();
      lexeme += " -> " + ret->GetName();
    } else {
      ret = std::make_unique<TypeExpr>(
          llvm::SMRange{params.back()->GetEnd(), params.back()->GetEnd()},
          type->lexeme, type->type);
    }

    // TODO: This should really be a pointer to a function type
    return std::make_unique<TypeExpr>(
        llvm::SMRange{type->GetStart(), ret->GetEnd()}, lexeme,
        TokenType::FUNC_TYPE, std::move(params), std::move(ret));
  }

  if (Check(TokenType::IDENTIFIER)) {
    Advance();
    return std::make_unique<TypeExpr>(type->span, type->lexeme,
                                      TokenType::CUSTOM_TYPE);
  }

  Error(type, fmt::format("Unknown type: {}", type->lexeme));

  return nullptr;
}

// Expression
auto Parser::ParseFunctionCall() -> std::unique_ptr<Expression> {
  auto* token = Peek();
  Consume(TokenType::IDENTIFIER);
  Consume(TokenType::LEFT_PAREN);

  std::vector<std::unique_ptr<Expression>> params;

  while (!Check(TokenType::RIGHT_PAREN)) {
    auto param = ParseExpression();

    if (!Check(TokenType::RIGHT_PAREN)) {
      Consume(TokenType::COMMA);
    }

    params.push_back(std::move(param));
  }

  auto* paren = Consume(TokenType::RIGHT_PAREN);

  return std::make_unique<FuncCall>(
      llvm::SMRange{token->GetStart(), paren->span.End}, token->lexeme,
      std::move(params));
}

auto Parser::ParseTerm() -> std::unique_ptr<Expression> {
  switch (Peek()->type) {
  case TokenType::STRING:
  case TokenType::INTEGER:
  case TokenType::DOUBLE:
  case TokenType::NIL: {
    auto* token = Peek();
    Consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, token->type);
  }
  case TokenType::IDENTIFIER: {
    if (CheckAny<TokenType::LEFT_PAREN>(1)) {
      return ParseFunctionCall();
    }

    auto* token = Peek();
    Consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme, token->type);
  }
  case TokenType::LEFT_PAREN: {
    Consume(TokenType::LEFT_PAREN);
    auto expr = ParseExpression();
    Consume(TokenType::RIGHT_PAREN);
    return expr;
  }
  case TokenType::TRUE_:
  case TokenType::FALSE_: {
    auto* token = Peek();
    Consume(token->type);
    return std::make_unique<Literal>(token->span, token->lexeme,
                                     TokenType::BOOL);
  }
  default:
    Error(Peek(), fmt::format("Unknown literal: {}", Peek()->lexeme));
  }

  return nullptr;
}

auto Parser::ParseDot() -> std::unique_ptr<Expression> {
  auto left = ParseTerm();

  while (AdvanceIfMatchAny<TokenType::DOT>()) {
    auto* op = Previous();
    auto expr = ParseTerm();
    left =
        std::make_unique<DotOp>(llvm::SMRange{left->GetStart(), expr->GetEnd()},
                                std::move(left), op->type, std::move(expr));
  }

  return left;
}

auto Parser::ParseUnary() -> std::unique_ptr<Expression> {
  // Handle unary operators recursively to allow chaining: - - x, * * ptr, etc.
  if (AdvanceIfMatchAny<TokenType::MINUS, TokenType::STAR,
                        TokenType::AMPERSAND>()) {
    auto* op = Previous();
    auto expr = ParseUnary(); // Recursive call for chained unary operators
    return std::make_unique<UnaryOp>(
        llvm::SMRange{op->GetStart(), expr->GetEnd()}, op->type,
        std::move(expr));
  }

  return ParseDot();
}

auto Parser::ParseCast() -> std::unique_ptr<Expression> {
  auto left = ParseUnary();
  while (AdvanceIfMatchAny<TokenType::AS>()) {
    auto type = ParseType();
    left = std::make_unique<CastOp>(
        llvm::SMRange{left->GetStart(), type->GetEnd()}, std::move(left),
        std::move(type));
  }
  return left;
}

auto Parser::ParseMult() -> std::unique_ptr<Expression> {
  auto left = ParsePower();
  while (
      AdvanceIfMatchAny<TokenType::STAR, TokenType::SLASH, TokenType::MOD>()) {
    auto op = Previous()->type;
    auto right = ParsePower();
    left = std::make_unique<BinaryOp>(
        llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left), op,
        std::move(right));
  }
  return left;
}

auto Parser::ParsePower() -> std::unique_ptr<Expression> {
  auto left = ParseCast();
  while (AdvanceIfMatchAny<TokenType::POWER>()) {
    auto op = Previous()->type;
    auto right = ParseCast();
    left = std::make_unique<BinaryOp>(
        llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left), op,
        std::move(right));
  }
  return left;
}

auto Parser::ParseAdd() -> std::unique_ptr<Expression> {
  auto left = ParseMult();
  while (AdvanceIfMatchAny<TokenType::PLUS, TokenType::MINUS>()) {
    auto op = Previous()->type;
    auto right = ParseMult();
    left = std::make_unique<BinaryOp>(
        llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left), op,
        std::move(right));
  }
  return left;
}

auto Parser::ParseCompare() -> std::unique_ptr<Expression> {
  auto left = ParseAdd();
  while (AdvanceIfMatchAny<TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL,
                           TokenType::LESS, TokenType::LESS_EQUAL,
                           TokenType::GREATER, TokenType::GREATER_EQUAL,
                           TokenType::IS, TokenType::IS_NOT>()) {
    auto op = Previous()->type;
    if (op == TokenType::IS || op == TokenType::IS_NOT) {
      auto right = ParseType();
      left = std::make_unique<IsOp>(
          llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left), op,
          std::move(right));
    } else {
      auto right = ParseAdd();
      left = std::make_unique<BinaryOp>(
          llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left), op,
          std::move(right));
    }
  }
  return left;
}

auto Parser::ParseNot() -> std::unique_ptr<Expression> {
  // Handle 'not' as a prefix unary operator, allowing chaining: not not x
  if (AdvanceIfMatchAny<TokenType::NOT>()) {
    auto* op = Previous();
    // Recursively call ParseNot() to handle chained 'not' operators
    auto expr = ParseNot();
    return std::make_unique<UnaryOp>(
        llvm::SMRange{op->GetStart(), expr->GetEnd()}, TokenType::NOT,
        std::move(expr));
  }

  return ParseCompare();
}

auto Parser::ParseAnd() -> std::unique_ptr<Expression> {
  auto left = ParseNot();
  while (AdvanceIfMatchAny<TokenType::AND>()) {
    auto right = ParseNot();
    left = std::make_unique<BinaryOp>(
        llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left),
        TokenType::AND, std::move(right));
  }
  return left;
}

auto Parser::ParseOr() -> std::unique_ptr<Expression> {
  auto left = ParseAnd();
  while (AdvanceIfMatchAny<TokenType::OR>()) {
    auto right = ParseAnd();
    left = std::make_unique<BinaryOp>(
        llvm::SMRange{left->GetStart(), right->GetEnd()}, std::move(left),
        TokenType::OR, std::move(right));
  }
  return left;
}

auto Parser::ParseExpression() -> std::unique_ptr<Expression> {
  return ParseOr();
}

// Statements
auto Parser::ParseVarDecl() -> std::unique_ptr<Statement> {
  bool mutable_ = false;
  Token const* startTok = nullptr;
  if (AdvanceIfMatchAny<TokenType::LET>()) {
    startTok = Previous();
    mutable_ = false;
  } else {
    startTok = Consume(TokenType::VAR);
    mutable_ = true;
  }
  auto* identifier = Consume(TokenType::IDENTIFIER);
  auto var = std::make_unique<Literal>(identifier->span, identifier->lexeme,
                                       identifier->type);

  std::unique_ptr<TypeExpr> type;
  if (AdvanceIfMatchAny<TokenType::COLON>()) {
    type = ParseType();
  }

  std::unique_ptr<Expression> expr;
  if (AdvanceIfMatchAny<TokenType::EQUAL>()) {
    expr = ParseExpression();
  }

  if (!type && !expr) {
    throw ParserError(llvm::SMRange{startTok->GetStart(), var->GetEnd()},
                      "Expected either a type or a value");
  }

  if (!expr && !mutable_) {
    throw ParserError(
        llvm::SMRange{startTok->GetStart(), type->GetEnd()},
        "Cannot declare an immutable variable without an initial expression");
  }

  ConsumeNewline();
  llvm::SMLoc endLoc = expr ? expr->GetEnd() : type->GetEnd();
  return std::make_unique<VarDecl>(llvm::SMRange{startTok->GetStart(), endLoc},
                                   std::move(var), std::move(type),
                                   std::move(expr), mutable_);
}

auto Parser::ParseIf() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::IF);

  std::vector<std::unique_ptr<Expression>> conds;
  std::vector<std::unique_ptr<Compound>> blocks;

  // Read first If cond and block
  conds.push_back(ParseExpression());
  blocks.push_back(ParseBlock());

  while (AdvanceIfMatchAny<TokenType::ELSE_IF>()) {
    conds.push_back(ParseExpression());
    blocks.push_back(ParseBlock());
  }
  if (AdvanceIfMatchAny<TokenType::ELSE>()) {
    conds.push_back(std::make_unique<Else>(Peek()->span));
    blocks.push_back(ParseBlock());
  }

  return std::make_unique<If>(llvm::SMRange{loc.Start, blocks.back()->GetEnd()},
                              std::move(conds), std::move(blocks));
}

auto Parser::ParseWhile() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::WHILE);

  auto cond = ParseExpression();
  auto block = ParseBlock();

  return std::make_unique<While>(llvm::SMRange{loc.Start, block->GetEnd()},
                                 std::move(cond), std::move(block));
}

auto Parser::ParseFor() -> std::unique_ptr<Statement> {
  Error(Peek(), "Unimplemented");

  return nullptr;
}

auto Parser::ParseAssignment() -> std::unique_ptr<Statement> {
  auto identifier = ParseDot();

  auto* literalPtr = dynamic_cast<Literal*>(identifier.get());
  auto* dotOpPtr = dynamic_cast<DotOp*>(identifier.get());

  if ((literalPtr == nullptr ||
       literalPtr->GetType() != TokenType::IDENTIFIER) &&
      dotOpPtr == nullptr) {
    throw ParserError(
        identifier->GetSpan(),
        "Expected either identifier or class field for assignment");
  }

  if (AdvanceIfMatchAny<TokenType::EQUAL, TokenType::PLUS_EQUAL,
                        TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                        TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL,
                        TokenType::POWER_EQUAL>()) {
    auto op = Previous()->type;
    auto expr = ParseExpression();

    ConsumeNewline();
    return std::make_unique<Assignment>(
        llvm::SMRange{identifier->GetStart(), expr->GetEnd()},
        std::move(identifier), op, std::move(expr));
  }

  Error(Peek(),
        fmt::format("Unsupported assignment operator: {}", Peek()->lexeme));

  return nullptr;
}

auto Parser::ParseBreak() -> std::unique_ptr<Statement> {
  auto span = Consume(TokenType::BREAK)->span;
  ConsumeNewline();
  return std::make_unique<Break>(span);
}

auto Parser::ParseContinue() -> std::unique_ptr<Statement> {
  auto span = Consume(TokenType::CONTINUE)->span;
  ConsumeNewline();
  return std::make_unique<Continue>(span);
}

auto Parser::ParseReturn() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::RETURN);
  if (Check(TokenType::NEWLINE) || Peek()->type == TokenType::EOF_TOKEN) {
    ConsumeNewline();
    return std::make_unique<Return>(loc, nullptr);
  }
  auto val = ParseExpression();
  ConsumeNewline();
  return std::make_unique<Return>(llvm::SMRange{loc.Start, val->GetEnd()},
                                  std::move(val));
}

auto Parser::ParseDefer() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::DEFER);
  auto val = ParseStatement(false);

  // Don't consume newline, since statement will
  return std::make_unique<Defer>(llvm::SMRange{loc.Start, val->GetEnd()},
                                 std::move(val));
}

auto Parser::ParseStatement(bool isTopLevel) -> std::unique_ptr<Statement> {
  if (CheckAny<TokenType::DEF, TokenType::IMPORT, TokenType::CLASS,
               TokenType::ENUM, TokenType::EXPORT>() &&
      !isTopLevel) {
    Error(Peek(), "Statement not allowed inside a block");
  }

  if (Check(TokenType::DEF)) {
    return ParseFunctionDeclaration();
  }
  if (CheckAny<TokenType::IMPORT, TokenType::FROM>()) {
    return ParseImport();
  }
  if (Check(TokenType::CLASS)) {
    return ParseClass();
  }
  if (Check(TokenType::ENUM)) {
    return ParseEnum();
  }
  if (Check(TokenType::EXPORT)) {
    return ParseExport();
  }
  if (Check(TokenType::LET) || Check(TokenType::VAR)) {
    return ParseVarDecl();
  }
  if (Check(TokenType::IF)) {
    return ParseIf();
  }
  if (Check(TokenType::WHILE)) {
    return ParseWhile();
  }
  if (Check(TokenType::FOR)) {
    return ParseFor();
  }
  if (Check(TokenType::BREAK)) {
    return ParseBreak();
  }
  if (Check(TokenType::CONTINUE)) {
    return ParseContinue();
  }
  if (Check(TokenType::RETURN)) {
    return ParseReturn();
  }
  if (Check(TokenType::DEFER)) {
    return ParseDefer();
  }
  if (CheckAnyInLine<TokenType::EQUAL, TokenType::PLUS_EQUAL,
                     TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL,
                     TokenType::SLASH_EQUAL, TokenType::MOD_EQUAL,
                     TokenType::POWER_EQUAL>()) {
    return ParseAssignment();
  }

  // Check if it's just an expression statement
  auto expr = ParseExpression();
  if (expr) {
    ConsumeNewline();
    return std::make_unique<ExpressionStatement>(expr->GetSpan(),
                                                 std::move(expr));
  }

  Error(Peek(), "Unknown statement");
  return nullptr;
}

auto Parser::ParseBlock() -> std::unique_ptr<Compound> {
  std::vector<std::unique_ptr<Statement>> statements;

  Consume(TokenType::NEWLINE);
  Consume(TokenType::INDENT);

  while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    statements.push_back(ParseStatement(false));
  }

  AdvanceIfMatchAny<TokenType::DEDENT>();

  return std::make_unique<Compound>(
      llvm::SMRange{statements.front()->GetStart(),
                    statements.back()->GetEnd()},
      std::move(statements));
}

auto Parser::ParseFunctionDeclaration() -> std::unique_ptr<Statement> {
  auto loc = isExported ? Previous()->span : Peek()->span;
  Consume(TokenType::DEF);
  bool externFunc = false;

  if (AdvanceIfMatchAny<TokenType::EXTERN>()) {
    externFunc = true;
  }

  if (externFunc && inClass) {
    Error(Previous(), "Extern functions are not allowed in class definition.");
    return nullptr;
  }

  auto* identifier = Consume(TokenType::IDENTIFIER);

  // Parse parameters
  Consume(TokenType::LEFT_PAREN);
  std::vector<std::unique_ptr<Parameter>> parameters;

  bool varargs = false;
  while (!Check(TokenType::RIGHT_PAREN)) {
    if (varargs) {
      Error(Peek(), "Varargs should be the last parameter");
    }

    if (Check(TokenType::ELLIPSIS) && externFunc) {
      Consume(TokenType::ELLIPSIS);
      varargs = true;
    } else {
      std::unique_ptr<Expression> defaultVal;
      std::unique_ptr<TypeExpr> type;
      auto* paramIdent = Consume(TokenType::IDENTIFIER);

      if (AdvanceIfMatchAny<TokenType::COLON>()) {
        type = ParseType();
      }

      if (AdvanceIfMatchAny<TokenType::EQUAL>()) {
        defaultVal = ParseExpression();
      }

      if (!defaultVal && !type) {
        throw ParserError(
            paramIdent->span,
            "{} should have either a type, a value or both specified",
            paramIdent->lexeme);
      }

      parameters.push_back(std::make_unique<Parameter>(
          paramIdent->lexeme, std::move(type), false, std::move(defaultVal)));
    }

    if (!Check(TokenType::RIGHT_PAREN) && !Check(TokenType::RIGHT_PAREN, 1)) {
      Consume(TokenType::COMMA);
    }
  }

  Consume(TokenType::RIGHT_PAREN);

  std::unique_ptr<TypeExpr> returnType;
  if (AdvanceIfMatchAny<TokenType::ARROW>()) {
    returnType = ParseType();
  } else {
    returnType = std::make_unique<TypeExpr>(Previous()->span, "void",
                                            TokenType::VOID_TYPE);
  }

  if (externFunc) {
    ConsumeNewline();
    return std::make_unique<ExternFuncDecl>(
        llvm::SMRange{loc.Start, returnType->GetEnd()}, identifier->lexeme,
        std::move(returnType), std::move(parameters), varargs, isExported);
  }

  auto body = ParseBlock();

  return std::make_unique<FuncDecl>(
      llvm::SMRange{loc.Start, returnType->GetEnd()}, identifier->lexeme,
      std::move(returnType), std::move(parameters), std::move(body), false,
      isExported);
}

auto Parser::ParseExport() -> std::unique_ptr<Statement> {
  Consume(TokenType::EXPORT);

  if (inClass) {
    Error(Peek(), "Cannot export class members");
  }

  if (!CheckAny<TokenType::DEF, TokenType::CLASS, TokenType::ENUM>()) {
    Error(Peek(), "Can only export functions, classes and enums");
  }

  isExported = true;
  std::unique_ptr<Statement> statement;
  if (Check(TokenType::DEF)) {
    statement = ParseFunctionDeclaration();
  } else if (Check(TokenType::IMPORT)) {
    statement = ParseImport();
  } else if (Check(TokenType::CLASS)) {
    statement = ParseClass();
  } else if (Check(TokenType::ENUM)) {
    statement = ParseEnum();
  } else {
    Error(Peek(), "Unexpected statement after export");
  }

  isExported = false;
  return statement;
}

auto Parser::ParseImport() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  bool selectiveImport = false;
  if (AdvanceIfMatchAny<TokenType::FROM>()) {
    selectiveImport = true;
  } else {
    Consume(TokenType::IMPORT);
  }

  Token const* token = nullptr;
  std::string filepath;

  if (Peek()->type == TokenType::STRING) {
    token = Consume(TokenType::STRING);
    filepath = token->lexeme;
  } else if (Peek()->type == TokenType::IDENTIFIER) {
    token = Consume(TokenType::IDENTIFIER);
    filepath = GetStdDir() + token->lexeme + ".les";
  } else {
    Error(Peek(), "Imports must be either strings for files or identifiers for "
                  "standard library");
    return nullptr;
  }

  if (!selectiveImport) {
    std::string alias = GetBasename(token->lexeme);
    if (AdvanceIfMatchAny<TokenType::AS>()) {
      alias = Consume(TokenType::IDENTIFIER)->lexeme;
    }

    ConsumeNewline();
    return std::make_unique<Import>(
        llvm::SMRange{loc.Start, token->GetEnd()}, filepath, alias,
        token->type == TokenType::IDENTIFIER, true, false,
        std::vector<std::pair<std::string, std::string>>{});
  }

  Consume(TokenType::IMPORT);

  if (AdvanceIfMatchAny<TokenType::STAR>()) {
    ConsumeNewline();
    return std::make_unique<Import>(
        llvm::SMRange{loc.Start, token->GetEnd()}, filepath,
        GetBasename(token->lexeme), token->type == TokenType::IDENTIFIER, true,
        true, std::vector<std::pair<std::string, std::string>>{});
  }

  std::vector<std::pair<std::string, std::string>> importedNames;

  do {
    auto ident = Consume(TokenType::IDENTIFIER)->lexeme;
    auto alias = ident;
    if (AdvanceIfMatchAny<TokenType::AS>()) {
      alias = Consume(TokenType::IDENTIFIER)->lexeme;
    }

    importedNames.emplace_back(ident, alias);
  } while (AdvanceIfMatchAny<TokenType::COMMA>());

  ConsumeNewline();
  return std::make_unique<Import>(llvm::SMRange{loc.Start, token->GetEnd()},
                                  filepath, GetBasename(token->lexeme),
                                  token->type == TokenType::IDENTIFIER, false,
                                  true, importedNames);
}

auto Parser::ParseClass() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::CLASS);

  auto* token = Consume(TokenType::IDENTIFIER);
  Consume(TokenType::NEWLINE);

  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  Consume(TokenType::INDENT);

  inClass = true;
  while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    if (CheckAny<TokenType::LET, TokenType::VAR>()) {
      auto stmt = ParseVarDecl();
      auto* varDecl = dynamic_cast<VarDecl*>(stmt.get());
      if (varDecl != nullptr) {
        std::ignore = stmt.release();
        fields.push_back(std::unique_ptr<VarDecl>(varDecl));
      }
    } else if (CheckAny<TokenType::DEF>()) {
      auto stmt = ParseFunctionDeclaration();
      auto* funcDecl = dynamic_cast<FuncDecl*>(stmt.get());
      if (funcDecl != nullptr) {
        std::ignore = stmt.release();
        methods.push_back(std::unique_ptr<FuncDecl>(funcDecl));
      }
    } else {
      Consume(TokenType::NEWLINE);
    }
  }
  inClass = false;

  AdvanceIfMatchAny<TokenType::DEDENT>();

  return std::make_unique<Class>(loc, token->lexeme, std::move(fields),
                                 std::move(methods), isExported);
}

auto Parser::ParseEnum() -> std::unique_ptr<Statement> {
  auto loc = Peek()->span;
  Consume(TokenType::ENUM);

  auto* token = Consume(TokenType::IDENTIFIER);
  Consume(TokenType::NEWLINE);

  std::vector<std::string> values;
  Consume(TokenType::INDENT);

  while (!CheckAny<TokenType::DEDENT, TokenType::EOF_TOKEN>()) {
    values.push_back(Consume(TokenType::IDENTIFIER)->lexeme);
    Consume(TokenType::NEWLINE);
  }

  AdvanceIfMatchAny<TokenType::DEDENT>();

  return std::make_unique<Enum>(loc, token->lexeme, values, isExported);
}

auto Parser::ParseCompound() -> std::unique_ptr<Compound> {
  std::vector<std::unique_ptr<Statement>> statements;
  while (!IsAtEnd()) {
    // Remove lingering newlines
    while (Peek()->type == TokenType::NEWLINE) {
      Consume(TokenType::NEWLINE);
    }
    statements.push_back(ParseStatement(true));
  }
  return std::make_unique<Compound>(
      llvm::SMRange{statements.front()->GetStart(),
                    statements.back()->GetEnd()},
      std::move(statements));
}

auto Parser::Parse() -> void { tree = ParseCompound(); }
