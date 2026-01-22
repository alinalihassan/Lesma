#pragma once

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "fmt/core.h"
#include "fmt/format.h"
#include "nameof.hpp"

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
class AST {
  llvm::SMRange loc;

public:
  explicit AST(llvm::SMRange loc) : loc(loc) {}
  virtual ~AST() = default;
  AST(const AST&) = delete;
  auto operator=(const AST&) -> AST& = delete;
  AST(AST&&) = default;
  auto operator=(AST&&) -> AST& = default;
  virtual void Accept(ASTVisitor& visitor) const = 0;

  [[nodiscard]] [[maybe_unused]] auto GetSpan() const -> llvm::SMRange {
    return loc;
  }
  [[nodiscard]] [[maybe_unused]] auto GetStart() const -> llvm::SMLoc {
    return loc.Start;
  }
  [[nodiscard]] [[maybe_unused]] auto GetEnd() const -> llvm::SMLoc {
    return loc.End;
  }

  virtual auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                        bool isTail) const -> std::string {
    return fmt::format("{}{}AST[Line({}-{}):Col({}-{})]:\n", prefix,
                       (isTail ? "└──" : "├──"),
                       srcMgr->getLineAndColumn(loc.Start).first,
                       srcMgr->getLineAndColumn(loc.End).first,
                       srcMgr->getLineAndColumn(loc.Start).second,
                       srcMgr->getLineAndColumn(loc.End).second);
  }
};

class Expression : public AST {
public:
  explicit Expression(llvm::SMRange loc) : AST(loc) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }
};

class Statement : public AST {
public:
  explicit Statement(llvm::SMRange loc) : AST(loc) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }
};

class Literal : public Expression {
  std::string value;
  TokenType type;

public:
  Literal(llvm::SMRange loc, std::string value, TokenType type)
      : Expression(loc), value(std::move(value)), type(type) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetValue() const -> std::string {
    return value;
  }
  [[nodiscard]] [[maybe_unused]] auto GetType() const -> TokenType {
    return type;
  }

  auto ToString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/,
                bool /*isTail*/) const -> std::string override {
    if (type == TokenType::STRING) {
      return '"' + value + '"';
    }
    if (type == TokenType::NIL || type == TokenType::INTEGER ||
        type == TokenType::DOUBLE || type == TokenType::IDENTIFIER ||
        type == TokenType::BOOL) {
      return value;
    }
    return "Unknown literal";
  }
};

class Compound : public Statement {
  std::vector<std::unique_ptr<Statement>> children;

public:
  explicit Compound(llvm::SMRange loc) : Statement(loc) {}
  explicit Compound(llvm::SMRange loc,
                    std::vector<std::unique_ptr<Statement>> children)
      : Statement(loc), children(std::move(children)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetChildren() const
      -> std::vector<Statement*> {
    std::vector<Statement*> result;
    result.reserve(children.size());
    for (const auto& child : children) {
      result.push_back(child.get());
    }
    return result;
  }

  [[maybe_unused]] auto AddChildren(std::unique_ptr<Statement> ast) -> void {
    children.push_back(std::move(ast));
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    auto ret = fmt::format("{}{}Compound[Line({}-{}):Col({}-{})]:\n", prefix,
                           isTail ? "└──" : "├──",
                           srcMgr->getLineAndColumn(GetStart()).first,
                           srcMgr->getLineAndColumn(GetEnd()).first,
                           srcMgr->getLineAndColumn(GetStart()).second,
                           srcMgr->getLineAndColumn(GetEnd()).second);
    for (const auto& child : children) {
      ret += child->ToString(srcMgr, prefix + (isTail ? "    " : "│   "),
                             child.get() == children.back().get());
    }
    return ret;
  }
};

class TypeExpr : public Expression {
  std::string name;
  TokenType type;

  // Pointer fields
  std::unique_ptr<TypeExpr> elementType;

  // Function fields
  std::vector<std::unique_ptr<TypeExpr>> params;
  std::unique_ptr<TypeExpr> ret;

public:
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type)
      : Expression(loc), name(std::move(name)), type(type),
        elementType(nullptr), ret(nullptr) {}
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type,
           std::unique_ptr<TypeExpr> elementType)
      : Expression(loc), name(std::move(name)), type(type),
        elementType(std::move(elementType)), ret(nullptr) {}
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type,
           std::vector<std::unique_ptr<TypeExpr>> params,
           std::unique_ptr<TypeExpr> ret)
      : Expression(loc), name(std::move(name)), type(type),
        elementType(nullptr), params(std::move(params)), ret(std::move(ret)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetName() const -> std::string {
    return name;
  }
  [[nodiscard]] [[maybe_unused]] auto GetType() const -> TokenType {
    return type;
  }
  [[nodiscard]] [[maybe_unused]] auto GetElementType() const -> TypeExpr* {
    return elementType.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetParams() const
      -> std::vector<TypeExpr*> {
    std::vector<TypeExpr*> result;
    result.reserve(params.size());
    for (const auto& param : params) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto GetReturnType() const -> TypeExpr* {
    return ret.get();
  }

  auto ToString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/,
                bool /*isTail*/) const -> std::string override {
    return name;
  }
};

class Enum : public Statement {
  std::string identifier;
  std::vector<std::string> values;
  bool exported;

public:
  Enum(llvm::SMRange loc, std::string identifier,
       std::vector<std::string> values, bool exported)
      : Statement(loc), identifier(std::move(identifier)),
        values(std::move(values)), exported(exported) {};
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetIdentifier() const -> std::string {
    return identifier;
  }
  [[nodiscard]] [[maybe_unused]] auto GetValues() const
      -> std::vector<std::string> {
    return values;
  }
  [[nodiscard]] [[maybe_unused]] auto IsExported() const -> bool {
    return exported;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    std::ostringstream imploded;
    std::copy(values.begin(), values.end(),
              std::ostream_iterator<std::string>(imploded, ", "));
    return fmt::format(
        "{}{}Enum[Line({}-{}):Col({}-{})]: {} with: {}\n", prefix,
        isTail ? "└──" : "├──", srcMgr->getLineAndColumn(GetStart()).first,
        srcMgr->getLineAndColumn(GetEnd()).first,
        srcMgr->getLineAndColumn(GetStart()).second,
        srcMgr->getLineAndColumn(GetEnd()).second, identifier, imploded.str());
  }
};

class Import : public Statement {
  std::string filePath;
  std::string alias;
  std::vector<std::pair<std::string, std::string>> importedNames;
  bool std;
  bool importAll;
  bool importToScope;

public:
  Import(llvm::SMRange loc, std::string filePath, std::string alias, bool std,
         bool importAll, bool importToScope,
         std::vector<std::pair<std::string, std::string>> importedNames)
      : Statement(loc), filePath(std::move(filePath)), alias(std::move(alias)),
        importedNames(std::move(importedNames)), std(std), importAll(importAll),
        importToScope(importToScope) {};
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetFilePath() const -> std::string {
    return filePath;
  }
  [[nodiscard]] [[maybe_unused]] auto GetAlias() const -> std::string {
    return alias;
  }
  [[nodiscard]] [[maybe_unused]] auto GetImportAll() const -> bool {
    return importAll;
  }
  [[nodiscard]] [[maybe_unused]] auto GetImportScope() const -> bool {
    return importToScope;
  }
  [[nodiscard]] [[maybe_unused]] auto GetImportedNames() const
      -> std::vector<std::pair<std::string, std::string>> {
    return importedNames;
  }
  [[nodiscard]] [[maybe_unused]] auto IsStd() const -> bool { return std; }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format("{}{}Import[Line({}-{}):Col({}-{})]: {} as {} from {}\n",
                       prefix, isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second, filePath,
                       alias, std ? "std" : "file");
  }
};

class VarDecl : public Statement {
  std::unique_ptr<Literal> var;
  std::unique_ptr<TypeExpr> type;
  std::unique_ptr<Expression> expr;
  bool isMutable;

public:
  VarDecl(llvm::SMRange loc, std::unique_ptr<Literal> var,
          std::unique_ptr<TypeExpr> type, std::unique_ptr<Expression> expr,
          bool isMutable)
      : Statement(loc), var(std::move(var)), type(std::move(type)),
        expr(std::move(expr)), isMutable(isMutable) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetIdentifier() const -> Literal* {
    return var.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetType() const -> TypeExpr* {
    return type.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetValue() const -> Expression* {
    return expr.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetMutability() const -> bool {
    return isMutable;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format(
        "{}{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n", prefix,
        isTail ? "└──" : "├──", srcMgr->getLineAndColumn(GetStart()).first,
        srcMgr->getLineAndColumn(GetEnd()).first,
        srcMgr->getLineAndColumn(GetStart()).second,
        srcMgr->getLineAndColumn(GetEnd()).second,
        var->ToString(srcMgr, prefix, isTail),
        (type ? ": " + type->ToString(srcMgr, prefix, isTail) : ""),
        (expr ? " = " + expr->ToString(srcMgr, prefix, isTail) : ""));
  }
};

class If : public Statement {
  std::vector<std::unique_ptr<Expression>> conds;
  std::vector<std::unique_ptr<Compound>> blocks;

public:
  If(llvm::SMRange loc, std::vector<std::unique_ptr<Expression>> conds,
     std::vector<std::unique_ptr<Compound>> blocks)
      : Statement(loc), conds(std::move(conds)), blocks(std::move(blocks)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetConds() const
      -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(conds.size());
    for (const auto& cond : conds) {
      result.push_back(cond.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto GetBlocks() const
      -> std::vector<Compound*> {
    std::vector<Compound*> result;
    result.reserve(blocks.size());
    for (const auto& block : blocks) {
      result.push_back(block.get());
    }
    return result;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    auto ret = fmt::format("{}{}If[Line({}-{}):Col({}-{})]:\n", prefix,
                           isTail ? "└──" : "├──",
                           srcMgr->getLineAndColumn(GetStart()).first,
                           srcMgr->getLineAndColumn(GetEnd()).first,
                           srcMgr->getLineAndColumn(GetStart()).second,
                           srcMgr->getLineAndColumn(GetEnd()).second);
    for (unsigned long i = 0; i < conds.size(); i++) {
      ret += fmt::format(
          "{}{}Cond: {}\n{}", prefix + (isTail ? "    " : "│   "),
          i == conds.size() - 1 ? "└──" : "├──",
          conds[i]->ToString(srcMgr, prefix + (isTail ? "    " : "│   "),
                             i == conds.size() - 1),
          blocks[i]->ToString(srcMgr,
                              prefix + (isTail ? "    " : "│   ") +
                                  (i == conds.size() - 1 ? "    " : "│   "),
                              true));
    }

    return ret;
  }
};

class While : public Statement {
  std::unique_ptr<Expression> cond;
  std::unique_ptr<Compound> block;

public:
  While(llvm::SMRange loc, std::unique_ptr<Expression> cond,
        std::unique_ptr<Compound> block)
      : Statement(loc), cond(std::move(cond)), block(std::move(block)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetCond() const -> Expression* {
    return cond.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetBlock() const -> Compound* {
    return block.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format(
        "{}{}While[Line({}-{}):Col({}-{})]:\n{}{}Cond: {}\n{}", prefix,
        isTail ? "└──" : "├──", srcMgr->getLineAndColumn(GetStart()).first,
        srcMgr->getLineAndColumn(GetEnd()).first,
        srcMgr->getLineAndColumn(GetStart()).second,
        srcMgr->getLineAndColumn(GetEnd()).second,
        prefix + (isTail ? "    " : "│   "), "└──",
        cond->ToString(srcMgr, prefix, true),
        block->ToString(srcMgr, prefix + (isTail ? "        " : "│       "),
                        true));
  }
};

class Parameter {
public:
  std::string name;
  std::unique_ptr<TypeExpr> type;
  bool optional;
  std::unique_ptr<Expression> defaultVal;

  Parameter(std::string name, std::unique_ptr<TypeExpr> type = nullptr,
            bool optional = false,
            std::unique_ptr<Expression> defaultVal = nullptr)
      : name(std::move(name)), type(std::move(type)), optional(optional),
        defaultVal(std::move(defaultVal)) {}

  Parameter(const Parameter&) = delete;
  auto operator=(const Parameter&) -> Parameter& = delete;
  Parameter(Parameter&&) = default;
  auto operator=(Parameter&&) -> Parameter& = default;
  ~Parameter() = default;
};

class FuncDecl : public Statement {
  std::string name;
  std::unique_ptr<TypeExpr> returnType;
  std::vector<std::unique_ptr<Parameter>> parameters;
  std::unique_ptr<Compound> body;
  bool varargs;
  bool exported;

public:
  FuncDecl(llvm::SMRange loc, std::string name,
           std::unique_ptr<TypeExpr> returnType,
           std::vector<std::unique_ptr<Parameter>> parameters,
           std::unique_ptr<Compound> body, bool varargs, bool exported)
      : Statement(loc), name(std::move(name)),
        returnType(std::move(returnType)), parameters(std::move(parameters)),
        body(std::move(body)), varargs(varargs), exported(exported) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetName() const -> std::string {
    return name;
  }
  [[nodiscard]] [[maybe_unused]] auto GetReturnType() const -> TypeExpr* {
    return returnType.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetParameters() const
      -> std::vector<Parameter*> {
    std::vector<Parameter*> result;
    result.reserve(parameters.size());
    for (const auto& param : parameters) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto GetBody() const -> Compound* {
    return body.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetVarArgs() const -> bool {
    return varargs;
  }
  [[nodiscard]] [[maybe_unused]] auto IsExported() const -> bool {
    return exported;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    auto ret = fmt::format("{}{}FuncDecl[Line({}-{}):Col({}-{})]: {}(", prefix,
                           isTail ? "└──" : "├──",
                           srcMgr->getLineAndColumn(GetStart()).first,
                           srcMgr->getLineAndColumn(GetEnd()).first,
                           srcMgr->getLineAndColumn(GetStart()).second,
                           srcMgr->getLineAndColumn(GetEnd()).second, name);
    for (const auto& param : parameters) {
      ret +=
          param->name + ": " + param->type->ToString(srcMgr, prefix, isTail) +
          (param->defaultVal == nullptr
               ? ""
               : fmt::format("= {}", param->defaultVal->ToString(srcMgr, prefix,
                                                                 isTail)));
      if (param.get() != parameters.back().get()) {
        ret += ", ";
      }
    }
    if (varargs) {
      ret += ", ...";
    }
    ret += fmt::format(
        ") -> {}\n{}", returnType->ToString(srcMgr, prefix, isTail),
        body->ToString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
    return ret;
  }
};

class ExternFuncDecl : public Statement {
  std::string name;
  std::unique_ptr<TypeExpr> returnType;
  std::vector<std::unique_ptr<Parameter>> parameters;
  bool varargs;
  bool exported;

public:
  ExternFuncDecl(llvm::SMRange loc, std::string name,
                 std::unique_ptr<TypeExpr> returnType,
                 std::vector<std::unique_ptr<Parameter>> parameters,
                 bool varargs, bool exported)
      : Statement(loc), name(std::move(name)),
        returnType(std::move(returnType)), parameters(std::move(parameters)),
        varargs(varargs), exported(exported) {}

  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetName() const -> std::string {
    return name;
  }
  [[nodiscard]] [[maybe_unused]] auto GetReturnType() const -> TypeExpr* {
    return returnType.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetParameters() const
      -> std::vector<Parameter*> {
    std::vector<Parameter*> result;
    result.reserve(parameters.size());
    for (const auto& param : parameters) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto GetVarArgs() const -> bool {
    return varargs;
  }
  [[nodiscard]] [[maybe_unused]] auto IsExported() const -> bool {
    return exported;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    auto ret = fmt::format("{}{}ExternFuncDecl[Line({}-{}):Col({}-{})]: {}(",
                           prefix, isTail ? "└──" : "├──",
                           srcMgr->getLineAndColumn(GetStart()).first,
                           srcMgr->getLineAndColumn(GetEnd()).first,
                           srcMgr->getLineAndColumn(GetStart()).second,
                           srcMgr->getLineAndColumn(GetEnd()).second, name);
    for (const auto& param : parameters) {
      ret +=
          param->name + ": " + param->type->ToString(srcMgr, prefix, isTail) +
          (param->defaultVal == nullptr
               ? ""
               : fmt::format("= {}", param->defaultVal->ToString(srcMgr, prefix,
                                                                 isTail)));
      if (param.get() != parameters.back().get()) {
        ret += ", ";
      }
    }
    if (varargs) {
      ret += ", ...";
    }
    ret +=
        fmt::format(") -> {}\n", returnType->ToString(srcMgr, prefix, isTail));
    return ret;
  }
};

class FuncCall : public Expression {
  std::string name;
  std::vector<std::unique_ptr<Expression>> arguments;

public:
  FuncCall(llvm::SMRange loc, std::string name,
           std::vector<std::unique_ptr<Expression>> arguments)
      : Expression(loc), name(std::move(name)),
        arguments(std::move(arguments)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetName() const -> std::string {
    return name;
  }
  [[nodiscard]] [[maybe_unused]] auto GetArguments() const
      -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(arguments.size());
    for (const auto& arg : arguments) {
      result.push_back(arg.get());
    }
    return result;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    auto ret = name + "(";
    for (const auto& param : arguments) {
      ret += param->ToString(srcMgr, prefix, isTail);
      if (param.get() != arguments.back().get()) {
        ret += ", ";
      }
    }
    ret += ")";
    return ret;
  }
};

class Assignment : public Statement {
  std::unique_ptr<Expression> lhs;
  TokenType op;
  std::unique_ptr<Expression> rhs;

public:
  Assignment(llvm::SMRange loc, std::unique_ptr<Expression> lhs, TokenType op,
             std::unique_ptr<Expression> rhs)
      : Statement(loc), lhs(std::move(lhs)), op(op), rhs(std::move(rhs)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetLeftHandSide() const -> Expression* {
    return lhs.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetOperator() const -> TokenType {
    return op;
  }
  [[nodiscard]] [[maybe_unused]] auto GetRightHandSide() const -> Expression* {
    return rhs.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format(
        "{}{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n", prefix,
        isTail ? "└──" : "├──", srcMgr->getLineAndColumn(GetStart()).first,
        srcMgr->getLineAndColumn(GetEnd()).first,
        srcMgr->getLineAndColumn(GetStart()).second,
        srcMgr->getLineAndColumn(GetEnd()).second,
        lhs->ToString(srcMgr, prefix, isTail), std::string{NAMEOF_ENUM(op)},
        rhs->ToString(srcMgr, prefix, isTail));
  }
};

class ExpressionStatement : public Statement {
  std::unique_ptr<Expression> expr;

public:
  ExpressionStatement(llvm::SMRange loc, std::unique_ptr<Expression> expr)
      : Statement(loc), expr(std::move(expr)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetExpression() const -> Expression* {
    return expr.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format("{}{}Expression[Line({}-{}):Col({}-{})]: {}\n", prefix,
                       isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second,
                       expr->ToString(srcMgr, prefix, isTail));
  }
};

class BinaryOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<Expression> right;

public:
  BinaryOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
           std::unique_ptr<Expression> right)
      : Expression(loc), left(std::move(left)), op(op),
        right(std::move(right)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetLeft() const -> Expression* {
    return left.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetOperator() const -> TokenType {
    return op;
  }
  [[nodiscard]] [[maybe_unused]] auto GetRight() const -> Expression* {
    return right.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return left->ToString(srcMgr, prefix, isTail) + " " +
           std::string{NAMEOF_ENUM(op)} + " " +
           right->ToString(srcMgr, prefix, isTail);
  }
};

class IsOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<TypeExpr> right;

public:
  IsOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
       std::unique_ptr<TypeExpr> right)
      : Expression(loc), left(std::move(left)), op(op),
        right(std::move(right)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetLeft() const -> Expression* {
    return left.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetOperator() const -> TokenType {
    return op;
  }
  [[nodiscard]] [[maybe_unused]] auto GetRight() const -> TypeExpr* {
    return right.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return left->ToString(srcMgr, prefix, isTail) + " " +
           std::string{NAMEOF_ENUM(op)} + " " +
           right->ToString(srcMgr, prefix, isTail);
  }
};

class CastOp : public Expression {
  std::unique_ptr<Expression> expr;
  std::unique_ptr<TypeExpr> type;

public:
  CastOp(llvm::SMRange loc, std::unique_ptr<Expression> expr,
         std::unique_ptr<TypeExpr> type)
      : Expression(loc), expr(std::move(expr)), type(std::move(type)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetExpression() const -> Expression* {
    return expr.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetType() const -> TypeExpr* {
    return type.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return expr->ToString(srcMgr, prefix, isTail) + " as " +
           type->ToString(srcMgr, prefix, isTail);
  }
};

class UnaryOp : public Expression {
  TokenType op;
  std::unique_ptr<Expression> expr;

public:
  UnaryOp(llvm::SMRange loc, TokenType op, std::unique_ptr<Expression> expr)
      : Expression(loc), op(op), expr(std::move(expr)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetOperator() const -> TokenType {
    return op;
  }
  [[nodiscard]] [[maybe_unused]] auto GetExpression() const -> Expression* {
    return expr.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return std::string{NAMEOF_ENUM(op)} +
           expr->ToString(srcMgr, prefix, isTail);
  }
};

class DotOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<Expression> right;

public:
  DotOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
        std::unique_ptr<Expression> right)
      : Expression(loc), left(std::move(left)), op(op),
        right(std::move(right)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetLeft() const -> Expression* {
    return left.get();
  }
  [[nodiscard]] [[maybe_unused]] auto GetOperator() const -> TokenType {
    return op;
  }
  [[nodiscard]] [[maybe_unused]] auto GetRight() const -> Expression* {
    return right.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return left->ToString(srcMgr, prefix, isTail) + "." +
           right->ToString(srcMgr, prefix, isTail);
  }
};

class Else : public Expression {
public:
  explicit Else(llvm::SMRange loc) : Expression(loc) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  auto ToString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/,
                bool /*isTail*/) const -> std::string override {
    return "Else";
  }
};

class Break : public Statement {
public:
  explicit Break(llvm::SMRange loc) : Statement(loc) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format("{}{}Break[Line({}-{}):Col({}-{})]:\n", prefix,
                       isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second);
  }
};

class Continue : public Statement {
public:
  explicit Continue(llvm::SMRange loc) : Statement(loc) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format("{}{}Continue[Line({}-{}):Col({}-{})]:\n", prefix,
                       isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second);
  }
};

class Return : public Statement {
  std::unique_ptr<Expression> value;

public:
  Return(llvm::SMRange loc, std::unique_ptr<Expression> value)
      : Statement(loc), value(std::move(value)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetValue() const -> Expression* {
    return value.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format("{}{}Return[Line({}-{}):Col({}-{})]: {}\n", prefix,
                       isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second,
                       value ? value->ToString(srcMgr, prefix, true) : "void");
  }
};

class Defer : public Statement {
  std::unique_ptr<Statement> stmt;

public:
  Defer(llvm::SMRange loc, std::unique_ptr<Statement> stmt)
      : Statement(loc), stmt(std::move(stmt)) {}
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetStatement() const -> Statement* {
    return stmt.get();
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    return fmt::format(
        "{}{}Defer[Line({}-{}):Col({}-{})]:\n{}", prefix,
        isTail ? "└──" : "├──", srcMgr->getLineAndColumn(GetStart()).first,
        srcMgr->getLineAndColumn(GetEnd()).first,
        srcMgr->getLineAndColumn(GetStart()).second,
        srcMgr->getLineAndColumn(GetEnd()).second,
        stmt->ToString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
  }
};

class Class : public Statement {
  std::string identifier;
  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  bool exported;

public:
  Class(llvm::SMRange loc, std::string identifier,
        std::vector<std::unique_ptr<VarDecl>> fields,
        std::vector<std::unique_ptr<FuncDecl>> methods, bool exported)
      : Statement(loc), identifier(std::move(identifier)),
        fields(std::move(fields)), methods(std::move(methods)),
        exported(exported) {};
  void Accept(ASTVisitor& visitor) const override { visitor.Visit(this); }

  [[nodiscard]] [[maybe_unused]] auto GetIdentifier() const -> std::string {
    return identifier;
  }
  [[nodiscard]] [[maybe_unused]] auto GetFields() const
      -> std::vector<VarDecl*> {
    std::vector<VarDecl*> result;
    result.reserve(fields.size());
    for (const auto& field : fields) {
      result.push_back(field.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto GetMethods() const
      -> std::vector<FuncDecl*> {
    std::vector<FuncDecl*> result;
    result.reserve(methods.size());
    for (const auto& method : methods) {
      result.push_back(method.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto IsExported() const -> bool {
    return exported;
  }

  auto ToString(llvm::SourceMgr* srcMgr, const std::string& prefix,
                bool isTail) const -> std::string override {
    std::string fieldsStr;
    for (const auto& field : fields) {
      fieldsStr +=
          field->ToString(srcMgr, prefix + (isTail ? "    " : "│   "), false);
    }

    std::string methodsStr;
    for (const auto& method : methods) {
      methodsStr +=
          method->ToString(srcMgr, prefix + (isTail ? "    " : "│   "),
                           method.get() == methods.back().get());
    }
    return fmt::format("{}{}Class[Line({}-{}):Col({}-{})]: {}: \n{}{}", prefix,
                       isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(GetStart()).first,
                       srcMgr->getLineAndColumn(GetEnd()).first,
                       srcMgr->getLineAndColumn(GetStart()).second,
                       srcMgr->getLineAndColumn(GetEnd()).second, identifier,
                       fieldsStr, methodsStr);
  }
};
} // namespace lesma
