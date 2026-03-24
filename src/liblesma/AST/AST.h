#pragma once

#include <algorithm>
#include <cassert>
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
class Value;
class SymbolTable;
class Type;

class AST {
  llvm::SMRange loc;

public:
  explicit AST(llvm::SMRange loc) : loc(loc) {}
  virtual ~AST() = default;
  AST(const AST&) = delete;
  auto operator=(const AST&) -> AST& = delete;
  AST(AST&&) = default;
  auto operator=(AST&&) -> AST& = default;
  virtual void accept(ASTVisitor& visitor) const = 0;

  [[nodiscard]] [[maybe_unused]] auto getSpan() const -> llvm::SMRange { return loc; }
  [[nodiscard]] [[maybe_unused]] auto getStart() const -> llvm::SMLoc { return loc.Start; }
  [[nodiscard]] [[maybe_unused]] auto getEnd() const -> llvm::SMLoc { return loc.End; }

  virtual auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string {
    return fmt::format(
        "{}{}AST[Line({}-{}):Col({}-{})]:\n", prefix, (isTail ? "└──" : "├──"),
        srcMgr->getLineAndColumn(loc.Start).first, srcMgr->getLineAndColumn(loc.End).first,
        srcMgr->getLineAndColumn(loc.Start).second, srcMgr->getLineAndColumn(loc.End).second);
  }
};

class Expression : public AST {
public:
  explicit Expression(llvm::SMRange loc) : AST(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }
};

class Statement : public AST {
public:
  explicit Statement(llvm::SMRange loc) : AST(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }
};

class Literal : public Expression {
  std::string value;
  TokenType type;
  mutable Value* resolvedSymbol = nullptr;
  /** If non-null for STRING literals, codegen emits a boxed stdlib str instance. */
  mutable Type* resolvedStrClassType = nullptr;

public:
  Literal(llvm::SMRange loc, std::string value, TokenType type)
      : Expression(loc), value(std::move(value)), type(type) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getValue() const -> std::string { return value; }
  [[nodiscard]] [[maybe_unused]] auto getType() const -> TokenType { return type; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] auto getResolvedStrClassType() const -> Type* { return resolvedStrClassType; }
  auto setResolvedStrClassType(Type* t) const -> void { resolvedStrClassType = t; }

  auto toString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/, bool /*isTail*/) const
      -> std::string override {
    if (type == TokenType::STRING) {
      return '"' + value + '"';
    }
    if (type == TokenType::NIL || type == TokenType::INTEGER || type == TokenType::DOUBLE ||
        type == TokenType::IDENTIFIER || type == TokenType::BOOL) {
      return value;
    }
    return "Unknown literal";
  }
};

class Compound : public Statement {
  std::vector<std::unique_ptr<Statement>> children;

public:
  explicit Compound(llvm::SMRange loc) : Statement(loc) {}
  explicit Compound(llvm::SMRange loc, std::vector<std::unique_ptr<Statement>> children)
      : Statement(loc), children(std::move(children)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getChildren() const -> std::vector<Statement*> {
    std::vector<Statement*> result;
    result.reserve(children.size());
    for (const auto& child : children) {
      result.push_back(child.get());
    }
    return result;
  }

  [[maybe_unused]] auto addChildren(std::unique_ptr<Statement> ast) -> void {
    children.push_back(std::move(ast));
  }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    auto ret = fmt::format(
        "{}{}Compound[Line({}-{}):Col({}-{})]:\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second);
    for (const auto& child : children) {
      ret += child->toString(srcMgr, prefix + (isTail ? "    " : "│   "),
                             child.get() == children.back().get());
    }
    return ret;
  }
};

class TypeExpr : public Expression {
  std::string name;
  TokenType type;
  mutable Value* resolvedSymbol = nullptr;

  // Pointer fields
  std::unique_ptr<TypeExpr> elementType;
  std::vector<std::unique_ptr<TypeExpr>> typeArgs;

  // Function fields
  std::vector<std::unique_ptr<TypeExpr>> params;
  std::unique_ptr<TypeExpr> ret;

public:
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type)
      : Expression(loc), name(std::move(name)), type(type), elementType(nullptr), ret(nullptr) {}
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type,
           std::unique_ptr<TypeExpr> elementType)
      : Expression(loc), name(std::move(name)), type(type), elementType(std::move(elementType)),
        ret(nullptr) {}
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type,
           std::vector<std::unique_ptr<TypeExpr>> typeArgs)
      : Expression(loc), name(std::move(name)), type(type), elementType(nullptr),
        typeArgs(std::move(typeArgs)), ret(nullptr) {}
  TypeExpr(llvm::SMRange loc, std::string name, TokenType type,
           std::vector<std::unique_ptr<TypeExpr>> params, std::unique_ptr<TypeExpr> ret)
      : Expression(loc), name(std::move(name)), type(type), elementType(nullptr),
        params(std::move(params)), ret(std::move(ret)) {}
  /** Tuple type `(T1, T2, ...)` / `(T,)`: same `params` as function types, `ret` is null. */
  static auto makeTupleType(llvm::SMRange loc, std::string displayName,
                            std::vector<std::unique_ptr<TypeExpr>> elements)
      -> std::unique_ptr<TypeExpr> {
    return std::make_unique<TypeExpr>(loc, std::move(displayName), TokenType::TUPLE_TYPE,
                                      std::move(elements), std::unique_ptr<TypeExpr>(nullptr));
  }
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getName() const -> std::string { return name; }
  /// Base identifier for symbol lookup (e.g. `Pair` for `Pair<int>`). Plain `getName()` keeps the
  /// full generic spelling for display and diagnostics.
  [[nodiscard]] auto getLookupName() const -> std::string {
    if (typeArgs.empty()) {
      return name;
    }
    const auto angle = name.find('<');
    if (angle == std::string::npos) {
      return name;
    }
    return name.substr(0, angle);
  }
  [[nodiscard]] [[maybe_unused]] auto getType() const -> TokenType { return type; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] [[maybe_unused]] auto getElementType() const -> TypeExpr* {
    return elementType.get();
  }
  [[nodiscard]] auto getTypeArgs() const -> std::vector<TypeExpr*> {
    std::vector<TypeExpr*> result;
    result.reserve(typeArgs.size());
    for (const auto& typeArg : typeArgs) {
      result.push_back(typeArg.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getParams() const -> std::vector<TypeExpr*> {
    std::vector<TypeExpr*> result;
    result.reserve(params.size());
    for (const auto& param : params) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getReturnType() const -> TypeExpr* { return ret.get(); }

  auto toString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/, bool /*isTail*/) const
      -> std::string override {
    return name;
  }
};

struct GenericParamDecl {
  std::string name;
  llvm::SMRange span;
  /** Intersection bounds: `T: A & B` → {"A","B"}. Empty means no trait bound. */
  std::vector<std::string> traitBounds;
  /** Spans for each name in `traitBounds` (same order). */
  std::vector<llvm::SMRange> traitBoundSpans;
};

class Enum : public Statement {
  std::string identifier;
  llvm::SMRange nameSpan;
  std::vector<std::string> values;
  std::vector<llvm::SMRange> valueSpans;
  bool exported;
  mutable Value* resolvedSymbol = nullptr;

public:
  Enum(llvm::SMRange loc, std::string identifier, llvm::SMRange nameSpan,
       std::vector<std::string> values, std::vector<llvm::SMRange> valueSpans, bool exported)
      : Statement(loc), identifier(std::move(identifier)), nameSpan(nameSpan),
        values(std::move(values)), valueSpans(std::move(valueSpans)), exported(exported) {};
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getIdentifier() const -> std::string { return identifier; }
  [[nodiscard]] [[maybe_unused]] auto getNameSpan() const -> llvm::SMRange { return nameSpan; }
  [[nodiscard]] [[maybe_unused]] auto getValues() const -> std::vector<std::string> {
    return values;
  }
  [[nodiscard]] [[maybe_unused]] auto getValueSpans() const -> const std::vector<llvm::SMRange>& {
    return valueSpans;
  }
  [[nodiscard]] [[maybe_unused]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    std::ostringstream imploded;
    std::copy(values.begin(), values.end(), std::ostream_iterator<std::string>(imploded, ", "));
    return fmt::format("{}{}Enum[Line({}-{}):Col({}-{})]: {} with: {}\n", prefix,
                       isTail ? "└──" : "├──", srcMgr->getLineAndColumn(getStart()).first,
                       srcMgr->getLineAndColumn(getEnd()).first,
                       srcMgr->getLineAndColumn(getStart()).second,
                       srcMgr->getLineAndColumn(getEnd()).second, identifier, imploded.str());
  }
};

struct ImportedNameBinding {
  std::string name;
  std::string alias;
  llvm::SMRange nameSpan;
  llvm::SMRange aliasSpan;
};

class Import : public Statement {
  std::string filePath;
  std::string alias;
  llvm::SMRange aliasSpan;
  std::vector<ImportedNameBinding> importedNames;
  bool std;
  bool importAll;
  bool importToScope;

public:
  Import(llvm::SMRange loc, std::string filePath, std::string alias, llvm::SMRange aliasSpan,
         bool std, bool importAll, bool importToScope,
         std::vector<ImportedNameBinding> importedNames)
      : Statement(loc), filePath(std::move(filePath)), alias(std::move(alias)),
        aliasSpan(aliasSpan), importedNames(std::move(importedNames)), std(std),
        importAll(importAll), importToScope(importToScope) {};
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getFilePath() const -> std::string { return filePath; }
  [[nodiscard]] [[maybe_unused]] auto getAlias() const -> std::string { return alias; }
  [[nodiscard]] [[maybe_unused]] auto getAliasSpan() const -> llvm::SMRange { return aliasSpan; }
  [[nodiscard]] [[maybe_unused]] auto getImportAll() const -> bool { return importAll; }
  [[nodiscard]] [[maybe_unused]] auto getImportScope() const -> bool { return importToScope; }
  [[nodiscard]] [[maybe_unused]] auto getImportedNames() const
      -> const std::vector<ImportedNameBinding>& {
    return importedNames;
  }
  [[nodiscard]] [[maybe_unused]] auto isStd() const -> bool { return std; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Import[Line({}-{}):Col({}-{})]: {} as {} from {}\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        filePath, alias, std ? "std" : "file");
  }
};

class VarDecl : public Statement {
  std::vector<std::unique_ptr<Literal>> vars;
  std::unique_ptr<TypeExpr> type;
  std::unique_ptr<Expression> expr;
  bool isMutable;
  /** Set by typechecker: one entry per `vars` (unpack) or one for a simple `let`. */
  mutable std::vector<Value*> resolvedSymbols;

public:
  VarDecl(llvm::SMRange loc, std::vector<std::unique_ptr<Literal>> vars,
          std::unique_ptr<TypeExpr> type, std::unique_ptr<Expression> expr, bool isMutable)
      : Statement(loc), vars(std::move(vars)), type(std::move(type)), expr(std::move(expr)),
        isMutable(isMutable) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getIdentifier() const -> Literal* {
    if (vars.empty()) {
      return nullptr;
    }
    assert(vars.size() == 1U && "getIdentifier() is only valid for a single-name VarDecl");
    return vars.front().get();
  }
  [[nodiscard]] auto getVarLiterals() const -> std::vector<Literal*> {
    std::vector<Literal*> out;
    out.reserve(vars.size());
    for (const auto& v : vars) {
      out.push_back(v.get());
    }
    return out;
  }
  [[nodiscard]] [[maybe_unused]] auto getType() const -> TypeExpr* { return type.get(); }
  [[nodiscard]] [[maybe_unused]] auto getValue() const -> Expression* { return expr.get(); }
  [[nodiscard]] [[maybe_unused]] auto getMutability() const -> bool { return isMutable; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* {
    if (resolvedSymbols.empty()) {
      return nullptr;
    }
    assert(resolvedSymbols.size() == 1U &&
           "getResolvedSymbol() is only valid when VarDecl has a single resolved symbol");
    return resolvedSymbols.front();
  }
  [[nodiscard]] auto getResolvedSymbols() const -> const std::vector<Value*>& {
    return resolvedSymbols;
  }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbols = {v}; }
  auto setResolvedSymbols(std::vector<Value*> syms) const -> void {
    resolvedSymbols = std::move(syms);
  }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        [&]() -> std::string {
          std::string names;
          for (size_t i = 0; i < vars.size(); ++i) {
            if (i > 0) {
              names += ", ";
            }
            names += vars[i]->toString(srcMgr, prefix, isTail);
          }
          return names;
        }(),
        (type ? ": " + type->toString(srcMgr, prefix, isTail) : ""),
        (expr ? " = " + expr->toString(srcMgr, prefix, isTail) : ""));
  }
};

class If : public Statement {
  std::vector<std::unique_ptr<Expression>> conds;
  std::vector<std::unique_ptr<Compound>> blocks;

public:
  If(llvm::SMRange loc, std::vector<std::unique_ptr<Expression>> conds,
     std::vector<std::unique_ptr<Compound>> blocks)
      : Statement(loc), conds(std::move(conds)), blocks(std::move(blocks)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getConds() const -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(conds.size());
    for (const auto& cond : conds) {
      result.push_back(cond.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getBlocks() const -> std::vector<Compound*> {
    std::vector<Compound*> result;
    result.reserve(blocks.size());
    for (const auto& block : blocks) {
      result.push_back(block.get());
    }
    return result;
  }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    auto ret = fmt::format(
        "{}{}If[Line({}-{}):Col({}-{})]:\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second);
    for (unsigned long i = 0; i < conds.size(); i++) {
      ret += fmt::format(
          "{}{}Cond: {}\n{}", prefix + (isTail ? "    " : "│   "),
          i == conds.size() - 1 ? "└──" : "├──",
          conds[i]->toString(srcMgr, prefix + (isTail ? "    " : "│   "), i == conds.size() - 1),
          blocks[i]->toString(srcMgr,
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
  While(llvm::SMRange loc, std::unique_ptr<Expression> cond, std::unique_ptr<Compound> block)
      : Statement(loc), cond(std::move(cond)), block(std::move(block)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getCond() const -> Expression* { return cond.get(); }
  [[nodiscard]] [[maybe_unused]] auto getBlock() const -> Compound* { return block.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}While[Line({}-{}):Col({}-{})]:\n{}{}Cond: {}\n{}", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        prefix + (isTail ? "    " : "│   "), "└──", cond->toString(srcMgr, prefix, true),
        block->toString(srcMgr, prefix + (isTail ? "        " : "│       "), true));
  }
};

class ForIn : public Statement {
  std::unique_ptr<Literal> var;
  std::unique_ptr<Expression> iterable;
  std::unique_ptr<Compound> block;
  mutable SymbolTable* bodyScope = nullptr;

public:
  ForIn(llvm::SMRange loc, std::unique_ptr<Literal> var, std::unique_ptr<Expression> iterable,
        std::unique_ptr<Compound> block)
      : Statement(loc), var(std::move(var)), iterable(std::move(iterable)),
        block(std::move(block)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getIdentifier() const -> Literal* { return var.get(); }
  [[nodiscard]] auto getIterable() const -> Expression* { return iterable.get(); }
  [[nodiscard]] auto getBlock() const -> Compound* { return block.get(); }
  [[nodiscard]] auto getBodyScope() const -> SymbolTable* { return bodyScope; }
  auto setBodyScope(SymbolTable* scope) const -> void { bodyScope = scope; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}ForIn[Line({}-{}):Col({}-{})]: {} in {}\n{}", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        var->toString(srcMgr, prefix, true), iterable->toString(srcMgr, prefix, true),
        block->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
  }
};

class Parameter {
public:
  std::string name;
  llvm::SMRange nameSpan;
  std::unique_ptr<TypeExpr> type;
  bool optional;
  std::unique_ptr<Expression> defaultVal;
  mutable Value* resolvedSymbol = nullptr;

  Parameter(std::string name, llvm::SMRange nameSpan, std::unique_ptr<TypeExpr> type = nullptr,
            bool optional = false, std::unique_ptr<Expression> defaultVal = nullptr)
      : name(std::move(name)), nameSpan(nameSpan), type(std::move(type)), optional(optional),
        defaultVal(std::move(defaultVal)) {}

  Parameter(const Parameter&) = delete;
  auto operator=(const Parameter&) -> Parameter& = delete;
  Parameter(Parameter&&) = default;
  auto operator=(Parameter&&) -> Parameter& = default;
  ~Parameter() = default;
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
};

class FuncDecl : public Statement {
  std::string name;
  llvm::SMRange nameSpan;
  std::vector<GenericParamDecl> genericParams;
  std::unique_ptr<TypeExpr> returnType;
  std::vector<std::unique_ptr<Parameter>> parameters;
  std::unique_ptr<Compound> body;
  bool varargs;
  bool exported;
  /** Set by typechecker: the symbol for this overload (used by LSP for hover/definition). */
  mutable Value* resolvedSymbol = nullptr;
  mutable SymbolTable* genericScope = nullptr;

public:
  FuncDecl(llvm::SMRange loc, std::string name, llvm::SMRange nameSpan,
           std::vector<GenericParamDecl> genericParams, std::unique_ptr<TypeExpr> returnType,
           std::vector<std::unique_ptr<Parameter>> parameters, std::unique_ptr<Compound> body,
           bool varargs, bool exported)
      : Statement(loc), name(std::move(name)), nameSpan(nameSpan),
        genericParams(std::move(genericParams)), returnType(std::move(returnType)),
        parameters(std::move(parameters)), body(std::move(body)), varargs(varargs),
        exported(exported) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getName() const -> std::string { return name; }
  [[nodiscard]] [[maybe_unused]] auto getNameSpan() const -> llvm::SMRange { return nameSpan; }
  [[nodiscard]] [[maybe_unused]] auto getGenericParams() const -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(genericParams.size());
    for (const auto& param : genericParams) {
      result.push_back(param.name);
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getGenericParamDecls() const
      -> const std::vector<GenericParamDecl>& {
    return genericParams;
  }
  [[nodiscard]] [[maybe_unused]] auto getReturnType() const -> TypeExpr* {
    return returnType.get();
  }
  [[nodiscard]] [[maybe_unused]] auto getParameters() const -> std::vector<Parameter*> {
    std::vector<Parameter*> result;
    result.reserve(parameters.size());
    for (const auto& param : parameters) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getBody() const -> Compound* { return body.get(); }
  [[nodiscard]] [[maybe_unused]] auto getVarArgs() const -> bool { return varargs; }
  [[nodiscard]] [[maybe_unused]] auto isExported() const -> bool { return exported; }
  /** Symbol for this declaration (set by typechecker; used by LSP for overload resolution). */
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] auto getGenericScope() const -> SymbolTable* { return genericScope; }
  auto setGenericScope(SymbolTable* scopePtr) const -> void { genericScope = scopePtr; }
  /** Trait method with a body is a default implementation; signature-only (no body) is a
   * requirement (same shape as `def extern`). */
  [[nodiscard]] auto hasTraitDefaultImplementation() const -> bool { return body != nullptr; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    auto ret = fmt::format("{}{}FuncDecl[Line({}-{}):Col({}-{})]: {}(", prefix,
                           isTail ? "└──" : "├──", srcMgr->getLineAndColumn(getStart()).first,
                           srcMgr->getLineAndColumn(getEnd()).first,
                           srcMgr->getLineAndColumn(getStart()).second,
                           srcMgr->getLineAndColumn(getEnd()).second, name);
    for (const auto& param : parameters) {
      ret += param->name + ": " +
             (param->type != nullptr ? param->type->toString(srcMgr, prefix, isTail) : "?") +
             (param->defaultVal == nullptr
                  ? ""
                  : fmt::format("= {}", param->defaultVal->toString(srcMgr, prefix, isTail)));
      if (param.get() != parameters.back().get()) {
        ret += ", ";
      }
    }
    if (!genericParams.empty()) {
      ret += "[";
      for (size_t i = 0; i < genericParams.size(); ++i) {
        ret += genericParams[i].name;
        if (i + 1U < genericParams.size()) {
          ret += ", ";
        }
      }
      ret += "]";
    }
    if (varargs) {
      ret += ", ...";
    }
    ret += fmt::format(") -> {}", returnType->toString(srcMgr, prefix, isTail));
    if (body != nullptr) {
      ret += fmt::format("\n{}", body->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
    } else {
      ret += "\n";
    }
    return ret;
  }
};

class TraitDecl : public Statement {
  std::string identifier;
  llvm::SMRange nameSpan;
  std::vector<GenericParamDecl> genericParams;
  std::vector<std::unique_ptr<FuncDecl>> requirements;
  bool exported;
  mutable Value* resolvedSymbol = nullptr;

public:
  TraitDecl(llvm::SMRange loc, std::string identifier, llvm::SMRange nameSpan,
            std::vector<GenericParamDecl> genericParams,
            std::vector<std::unique_ptr<FuncDecl>> requirements, bool exported)
      : Statement(loc), identifier(std::move(identifier)), nameSpan(nameSpan),
        genericParams(std::move(genericParams)), requirements(std::move(requirements)),
        exported(exported) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getIdentifier() const -> std::string { return identifier; }
  [[nodiscard]] auto getNameSpan() const -> llvm::SMRange { return nameSpan; }
  [[nodiscard]] auto getGenericParamDecls() const -> const std::vector<GenericParamDecl>& {
    return genericParams;
  }
  [[nodiscard]] auto getGenericParams() const -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(genericParams.size());
    for (const auto& param : genericParams) {
      result.push_back(param.name);
    }
    return result;
  }
  [[nodiscard]] auto getRequirements() const -> std::vector<FuncDecl*> {
    std::vector<FuncDecl*> out;
    out.reserve(requirements.size());
    for (const auto& r : requirements) {
      out.push_back(r.get());
    }
    return out;
  }
  [[nodiscard]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    std::string req;
    for (const auto& r : requirements) {
      req += r->toString(srcMgr, prefix + (isTail ? "    " : "│   "),
                         r.get() == requirements.back().get());
    }
    return fmt::format("{}{}Trait[Line({}-{}):Col({}-{})]: {}\n{}", prefix, isTail ? "└──" : "├──",
                       srcMgr->getLineAndColumn(getStart()).first,
                       srcMgr->getLineAndColumn(getEnd()).first,
                       srcMgr->getLineAndColumn(getStart()).second,
                       srcMgr->getLineAndColumn(getEnd()).second, identifier, req);
  }
};

class ExternFuncDecl : public Statement {
  std::string name;
  llvm::SMRange nameSpan;
  std::vector<GenericParamDecl> genericParams;
  std::unique_ptr<TypeExpr> returnType;
  std::vector<std::unique_ptr<Parameter>> parameters;
  bool varargs;
  bool exported;
  /** Set by typechecker: resolved symbol for this declaration. */
  mutable Value* resolvedSymbol = nullptr;
  mutable SymbolTable* genericScope = nullptr;

public:
  ExternFuncDecl(llvm::SMRange loc, std::string name, llvm::SMRange nameSpan,
                 std::vector<GenericParamDecl> genericParams, std::unique_ptr<TypeExpr> returnType,
                 std::vector<std::unique_ptr<Parameter>> parameters, bool varargs, bool exported)
      : Statement(loc), name(std::move(name)), nameSpan(nameSpan),
        genericParams(std::move(genericParams)), returnType(std::move(returnType)),
        parameters(std::move(parameters)), varargs(varargs), exported(exported) {}

  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getName() const -> std::string { return name; }
  [[nodiscard]] [[maybe_unused]] auto getNameSpan() const -> llvm::SMRange { return nameSpan; }
  [[nodiscard]] [[maybe_unused]] auto getGenericParams() const -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(genericParams.size());
    for (const auto& param : genericParams) {
      result.push_back(param.name);
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getGenericParamDecls() const
      -> const std::vector<GenericParamDecl>& {
    return genericParams;
  }
  [[nodiscard]] [[maybe_unused]] auto getReturnType() const -> TypeExpr* {
    return returnType.get();
  }
  [[nodiscard]] [[maybe_unused]] auto getParameters() const -> std::vector<Parameter*> {
    std::vector<Parameter*> result;
    result.reserve(parameters.size());
    for (const auto& param : parameters) {
      result.push_back(param.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getVarArgs() const -> bool { return varargs; }
  [[nodiscard]] [[maybe_unused]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] auto getGenericScope() const -> SymbolTable* { return genericScope; }
  auto setGenericScope(SymbolTable* scopePtr) const -> void { genericScope = scopePtr; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    auto ret = fmt::format("{}{}ExternFuncDecl[Line({}-{}):Col({}-{})]: {}(", prefix,
                           isTail ? "└──" : "├──", srcMgr->getLineAndColumn(getStart()).first,
                           srcMgr->getLineAndColumn(getEnd()).first,
                           srcMgr->getLineAndColumn(getStart()).second,
                           srcMgr->getLineAndColumn(getEnd()).second, name);
    for (const auto& param : parameters) {
      ret += param->name + ": " +
             (param->type != nullptr ? param->type->toString(srcMgr, prefix, isTail) : "?") +
             (param->defaultVal == nullptr
                  ? ""
                  : fmt::format("= {}", param->defaultVal->toString(srcMgr, prefix, isTail)));
      if (param.get() != parameters.back().get()) {
        ret += ", ";
      }
    }
    if (varargs) {
      ret += ", ...";
    }
    ret += fmt::format(") -> {}\n", returnType->toString(srcMgr, prefix, isTail));
    return ret;
  }
};

class FuncCall : public Expression {
  std::string name;
  std::vector<std::unique_ptr<TypeExpr>> explicitTypeArgs;
  std::vector<std::unique_ptr<Expression>> arguments;
  mutable Value* resolvedSymbol = nullptr;

public:
  FuncCall(llvm::SMRange loc, std::string name,
           std::vector<std::unique_ptr<TypeExpr>> explicitTypeArgs,
           std::vector<std::unique_ptr<Expression>> arguments)
      : Expression(loc), name(std::move(name)), explicitTypeArgs(std::move(explicitTypeArgs)),
        arguments(std::move(arguments)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getName() const -> std::string { return name; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] [[maybe_unused]] auto getExplicitTypeArgs() const -> std::vector<TypeExpr*> {
    std::vector<TypeExpr*> result;
    result.reserve(explicitTypeArgs.size());
    for (const auto& t : explicitTypeArgs) {
      result.push_back(t.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getArguments() const -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(arguments.size());
    for (const auto& arg : arguments) {
      result.push_back(arg.get());
    }
    return result;
  }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    auto ret = name + "(";
    for (const auto& param : arguments) {
      ret += param->toString(srcMgr, prefix, isTail);
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
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getLeftHandSide() const -> Expression* { return lhs.get(); }
  [[nodiscard]] [[maybe_unused]] auto getOperator() const -> TokenType { return op; }
  [[nodiscard]] [[maybe_unused]] auto getRightHandSide() const -> Expression* { return rhs.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        lhs->toString(srcMgr, prefix, isTail), std::string{NAMEOF_ENUM(op)},
        rhs->toString(srcMgr, prefix, isTail));
  }
};

class ExpressionStatement : public Statement {
  std::unique_ptr<Expression> expr;

public:
  ExpressionStatement(llvm::SMRange loc, std::unique_ptr<Expression> expr)
      : Statement(loc), expr(std::move(expr)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getExpression() const -> Expression* { return expr.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Expression[Line({}-{}):Col({}-{})]: {}\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        expr->toString(srcMgr, prefix, isTail));
  }
};

class BinaryOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<Expression> right;

public:
  BinaryOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
           std::unique_ptr<Expression> right)
      : Expression(loc), left(std::move(left)), op(op), right(std::move(right)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getLeft() const -> Expression* { return left.get(); }
  [[nodiscard]] [[maybe_unused]] auto getOperator() const -> TokenType { return op; }
  [[nodiscard]] [[maybe_unused]] auto getRight() const -> Expression* { return right.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return left->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " +
           right->toString(srcMgr, prefix, isTail);
  }
};

class SubscriptOp : public Expression {
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> index;

public:
  SubscriptOp(llvm::SMRange loc, std::unique_ptr<Expression> left,
              std::unique_ptr<Expression> index)
      : Expression(loc), left(std::move(left)), index(std::move(index)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getLeft() const -> Expression* { return left.get(); }
  [[nodiscard]] auto getIndex() const -> Expression* { return index.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return left->toString(srcMgr, prefix, isTail) + "[" + index->toString(srcMgr, prefix, isTail) +
           "]";
  }
};

class IsOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<TypeExpr> right;

public:
  IsOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
       std::unique_ptr<TypeExpr> right)
      : Expression(loc), left(std::move(left)), op(op), right(std::move(right)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getLeft() const -> Expression* { return left.get(); }
  [[nodiscard]] [[maybe_unused]] auto getOperator() const -> TokenType { return op; }
  [[nodiscard]] [[maybe_unused]] auto getRight() const -> TypeExpr* { return right.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return left->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " +
           right->toString(srcMgr, prefix, isTail);
  }
};

class CastOp : public Expression {
  std::unique_ptr<Expression> expr;
  std::unique_ptr<TypeExpr> type;

public:
  CastOp(llvm::SMRange loc, std::unique_ptr<Expression> expr, std::unique_ptr<TypeExpr> type)
      : Expression(loc), expr(std::move(expr)), type(std::move(type)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getExpression() const -> Expression* { return expr.get(); }
  [[nodiscard]] [[maybe_unused]] auto getType() const -> TypeExpr* { return type.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return expr->toString(srcMgr, prefix, isTail) + " as " + type->toString(srcMgr, prefix, isTail);
  }
};

class UnaryOp : public Expression {
  TokenType op;
  std::unique_ptr<Expression> expr;

public:
  UnaryOp(llvm::SMRange loc, TokenType op, std::unique_ptr<Expression> expr)
      : Expression(loc), op(op), expr(std::move(expr)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getOperator() const -> TokenType { return op; }
  [[nodiscard]] [[maybe_unused]] auto getExpression() const -> Expression* { return expr.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return std::string{NAMEOF_ENUM(op)} + expr->toString(srcMgr, prefix, isTail);
  }
};

class ListLiteral : public Expression {
  std::vector<std::unique_ptr<Expression>> elements;
  mutable Type* resolvedType = nullptr;

public:
  ListLiteral(llvm::SMRange loc, std::vector<std::unique_ptr<Expression>> elements)
      : Expression(loc), elements(std::move(elements)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getElements() const -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(elements.size());
    for (const auto& element : elements) {
      result.push_back(element.get());
    }
    return result;
  }
  [[nodiscard]] auto getResolvedType() const -> Type* { return resolvedType; }
  auto setResolvedType(Type* type) const -> void { resolvedType = type; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    std::string result = "[";
    for (const auto& element : elements) {
      result += element->toString(srcMgr, prefix, isTail);
      if (element.get() != elements.back().get()) {
        result += ", ";
      }
    }
    result += "]";
    return result;
  }
};

class TupleLiteral : public Expression {
  std::vector<std::unique_ptr<Expression>> elements;
  mutable Type* resolvedType = nullptr;

public:
  TupleLiteral(llvm::SMRange loc, std::vector<std::unique_ptr<Expression>> elements)
      : Expression(loc), elements(std::move(elements)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getElements() const -> std::vector<Expression*> {
    std::vector<Expression*> result;
    result.reserve(elements.size());
    for (const auto& element : elements) {
      result.push_back(element.get());
    }
    return result;
  }
  [[nodiscard]] auto getResolvedType() const -> Type* { return resolvedType; }
  auto setResolvedType(Type* type) const -> void { resolvedType = type; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    std::string result = "(";
    for (const auto& element : elements) {
      result += element->toString(srcMgr, prefix, isTail);
      if (element.get() != elements.back().get()) {
        result += ", ";
      }
    }
    if (elements.size() == 1U) {
      result += ",";
    }
    result += ")";
    return result;
  }
};

class DotOp : public Expression {
  std::unique_ptr<Expression> left;
  TokenType op;
  std::unique_ptr<Expression> right;

public:
  DotOp(llvm::SMRange loc, std::unique_ptr<Expression> left, TokenType op,
        std::unique_ptr<Expression> right)
      : Expression(loc), left(std::move(left)), op(op), right(std::move(right)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getLeft() const -> Expression* { return left.get(); }
  [[nodiscard]] [[maybe_unused]] auto getOperator() const -> TokenType { return op; }
  [[nodiscard]] [[maybe_unused]] auto getRight() const -> Expression* { return right.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return left->toString(srcMgr, prefix, isTail) + "." + right->toString(srcMgr, prefix, isTail);
  }
};

class Else : public Expression {
public:
  explicit Else(llvm::SMRange loc) : Expression(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  auto toString(llvm::SourceMgr* /*srcMgr*/, const std::string& /*prefix*/, bool /*isTail*/) const
      -> std::string override {
    return "Else";
  }
};

class Break : public Statement {
public:
  explicit Break(llvm::SMRange loc) : Statement(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Break[Line({}-{}):Col({}-{})]:\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second);
  }
};

class Continue : public Statement {
public:
  explicit Continue(llvm::SMRange loc) : Statement(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Continue[Line({}-{}):Col({}-{})]:\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second);
  }
};

/** No-op statement (Python-style); valid wherever a statement is allowed. */
class Pass : public Statement {
public:
  explicit Pass(llvm::SMRange loc) : Statement(loc) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Pass[Line({}-{}):Col({}-{})]:\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second);
  }
};

class Return : public Statement {
  std::unique_ptr<Expression> value;

public:
  Return(llvm::SMRange loc, std::unique_ptr<Expression> value)
      : Statement(loc), value(std::move(value)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getValue() const -> Expression* { return value.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Return[Line({}-{}):Col({}-{})]: {}\n", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        value ? value->toString(srcMgr, prefix, true) : "void");
  }
};

class Defer : public Statement {
  std::unique_ptr<Statement> stmt;

public:
  Defer(llvm::SMRange loc, std::unique_ptr<Statement> stmt)
      : Statement(loc), stmt(std::move(stmt)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getStatement() const -> Statement* { return stmt.get(); }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format(
        "{}{}Defer[Line({}-{}):Col({}-{})]:\n{}", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        stmt->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
  }
};

class UnimplementedStatement : public Statement {
  std::string message;

public:
  UnimplementedStatement(llvm::SMRange loc, std::string message)
      : Statement(loc), message(std::move(message)) {}
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] auto getMessage() const -> const std::string& { return message; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    return fmt::format("{}{}Unimplemented[Line({}-{}):Col({}-{})]: {}\n", prefix,
                       isTail ? "└──" : "├──", srcMgr->getLineAndColumn(getStart()).first,
                       srcMgr->getLineAndColumn(getEnd()).first,
                       srcMgr->getLineAndColumn(getStart()).second,
                       srcMgr->getLineAndColumn(getEnd()).second, message);
  }
};

class Class : public Statement {
  std::string identifier;
  llvm::SMRange nameSpan;
  std::vector<GenericParamDecl> genericParams;
  /** Explicit `impl Trait1, Trait2` names (must match declared traits). */
  std::vector<std::string> implTraitNames;
  /** Source span for each name in `implTraitNames` (same order). */
  std::vector<llvm::SMRange> implTraitSpans;
  /** Type arguments for each `impl Trait<...>` (same order as `implTraitNames`; empty if no `<>`).
   */
  std::vector<std::vector<std::unique_ptr<TypeExpr>>> implTraitTypeArgs;
  std::vector<std::unique_ptr<VarDecl>> fields;
  std::vector<std::unique_ptr<FuncDecl>> methods;
  bool exported;
  mutable Value* resolvedSymbol = nullptr;
  mutable SymbolTable* genericScope = nullptr;

public:
  Class(llvm::SMRange loc, std::string identifier, llvm::SMRange nameSpan,
        std::vector<GenericParamDecl> genericParams, std::vector<std::string> implTraitNames,
        std::vector<llvm::SMRange> implTraitSpans,
        std::vector<std::vector<std::unique_ptr<TypeExpr>>> implTraitTypeArgs,
        std::vector<std::unique_ptr<VarDecl>> fields,
        std::vector<std::unique_ptr<FuncDecl>> methods, bool exported)
      : Statement(loc), identifier(std::move(identifier)), nameSpan(nameSpan),
        genericParams(std::move(genericParams)), implTraitNames(std::move(implTraitNames)),
        implTraitSpans(std::move(implTraitSpans)), implTraitTypeArgs(std::move(implTraitTypeArgs)),
        fields(std::move(fields)), methods(std::move(methods)), exported(exported) {};
  void accept(ASTVisitor& visitor) const override { visitor.visit(this); }

  [[nodiscard]] [[maybe_unused]] auto getIdentifier() const -> std::string { return identifier; }
  [[nodiscard]] [[maybe_unused]] auto getNameSpan() const -> llvm::SMRange { return nameSpan; }
  [[nodiscard]] [[maybe_unused]] auto getGenericParams() const -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(genericParams.size());
    for (const auto& param : genericParams) {
      result.push_back(param.name);
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getGenericParamDecls() const
      -> const std::vector<GenericParamDecl>& {
    return genericParams;
  }
  [[nodiscard]] auto getImplTraitNames() const -> const std::vector<std::string>& {
    return implTraitNames;
  }
  [[nodiscard]] auto getImplTraitSpans() const -> const std::vector<llvm::SMRange>& {
    return implTraitSpans;
  }
  [[nodiscard]] auto getImplTraitTypeArgs() const
      -> const std::vector<std::vector<std::unique_ptr<TypeExpr>>>& {
    return implTraitTypeArgs;
  }
  [[nodiscard]] [[maybe_unused]] auto getFields() const -> std::vector<VarDecl*> {
    std::vector<VarDecl*> result;
    result.reserve(fields.size());
    for (const auto& field : fields) {
      result.push_back(field.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto getMethods() const -> std::vector<FuncDecl*> {
    std::vector<FuncDecl*> result;
    result.reserve(methods.size());
    for (const auto& method : methods) {
      result.push_back(method.get());
    }
    return result;
  }
  [[nodiscard]] [[maybe_unused]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto getResolvedSymbol() const -> Value* { return resolvedSymbol; }
  auto setResolvedSymbol(Value* v) const -> void { resolvedSymbol = v; }
  [[nodiscard]] auto getGenericScope() const -> SymbolTable* { return genericScope; }
  auto setGenericScope(SymbolTable* scopePtr) const -> void { genericScope = scopePtr; }

  auto toString(llvm::SourceMgr* srcMgr, const std::string& prefix, bool isTail) const
      -> std::string override {
    std::string fieldsStr;
    for (const auto& field : fields) {
      fieldsStr += field->toString(srcMgr, prefix + (isTail ? "    " : "│   "), false);
    }

    std::string methodsStr;
    for (const auto& method : methods) {
      methodsStr += method->toString(srcMgr, prefix + (isTail ? "    " : "│   "),
                                     method.get() == methods.back().get());
    }
    return fmt::format(
        "{}{}Class[Line({}-{}):Col({}-{})]: {}: \n{}{}", prefix, isTail ? "└──" : "├──",
        srcMgr->getLineAndColumn(getStart()).first, srcMgr->getLineAndColumn(getEnd()).first,
        srcMgr->getLineAndColumn(getStart()).second, srcMgr->getLineAndColumn(getEnd()).second,
        identifier, fieldsStr, methodsStr);
  }
};

struct NamedSpan {
  std::string name;
  llvm::SMRange span;
};

struct FuncLikeDeclView {
  std::string name;
  llvm::SMRange nameSpan;
  const std::vector<GenericParamDecl>* genericParams = nullptr;
  std::vector<Parameter*> parameters;
  TypeExpr* returnType = nullptr;
  Compound* body = nullptr;
  Value* resolvedSymbol = nullptr;
  SymbolTable* genericScope = nullptr;
};

[[nodiscard]] inline auto makeFuncLikeDeclView(const FuncDecl* node) -> FuncLikeDeclView {
  if (node == nullptr) {
    return {};
  }
  return FuncLikeDeclView{
      .name = node->getName(),
      .nameSpan = node->getNameSpan(),
      .genericParams = &node->getGenericParamDecls(),
      .parameters = node->getParameters(),
      .returnType = node->getReturnType(),
      .body = node->getBody(),
      .resolvedSymbol = node->getResolvedSymbol(),
      .genericScope = node->getGenericScope(),
  };
}

[[nodiscard]] inline auto makeFuncLikeDeclView(const ExternFuncDecl* node) -> FuncLikeDeclView {
  if (node == nullptr) {
    return {};
  }
  return FuncLikeDeclView{
      .name = node->getName(),
      .nameSpan = node->getNameSpan(),
      .genericParams = &node->getGenericParamDecls(),
      .parameters = node->getParameters(),
      .returnType = node->getReturnType(),
      .body = nullptr,
      .resolvedSymbol = node->getResolvedSymbol(),
      .genericScope = node->getGenericScope(),
  };
}

[[nodiscard]] inline auto getEnumValueDecls(const Enum* node) -> std::vector<NamedSpan> {
  std::vector<NamedSpan> out;
  if (node == nullptr) {
    return out;
  }
  std::vector<std::string> const values = node->getValues();
  std::vector<llvm::SMRange> const& valueSpans = node->getValueSpans();
  out.reserve(std::min(values.size(), valueSpans.size()));
  for (size_t i = 0; i < values.size() && i < valueSpans.size(); ++i) {
    out.push_back(NamedSpan{
        .name = values[i],
        .span = valueSpans[i],
    });
  }
  return out;
}

[[nodiscard]] inline auto getImportLocalBindings(const Import* node) -> std::vector<NamedSpan> {
  std::vector<NamedSpan> out;
  if (node == nullptr) {
    return out;
  }
  if (!node->getAlias().empty() && node->getAliasSpan().isValid()) {
    out.push_back(NamedSpan{
        .name = node->getAlias(),
        .span = node->getAliasSpan(),
    });
  }
  out.reserve(out.size() + node->getImportedNames().size());
  for (const ImportedNameBinding& binding : node->getImportedNames()) {
    llvm::SMRange const span = binding.aliasSpan.isValid() ? binding.aliasSpan : binding.nameSpan;
    if (!span.isValid()) {
      continue;
    }
    out.push_back(NamedSpan{
        .name = binding.alias.empty() ? binding.name : binding.alias,
        .span = span,
    });
  }
  return out;
}

} // namespace lesma
