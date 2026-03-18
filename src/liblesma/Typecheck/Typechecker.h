#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

namespace lesma {

/** Callback to resolve import *: (filepath, isStd, mainFilePath) -> exported names. */
using GetExportsFn =
    std::function<std::vector<std::string>(const std::string&, bool,
                                           const std::string&)>;

/**
 * Semantic typecheck pass. Runs after parsing, before codegen.
 * Resolves types (without LLVM), builds symbol table, and checks:
 * - Type existence and resolution
 * - Variable declarations (init type matches declared type)
 * - Assignments (mutability, type compatibility)
 * - Return types vs function signature
 * - Binary/unary op operand types
 * - Function call argument counts and types
 * When getExports is provided, import * adds exported names from the imported file.
 */
class Typechecker final : public ASTVisitor {
  std::unique_ptr<SymbolTable> rootScope;
  SymbolTable* scope = nullptr;
  std::vector<std::unique_ptr<Type>> typeCache;
  std::unique_ptr<Value> result; // Last expression type (type only, no LLVM value)

  std::string mainFilePath;
  GetExportsFn getExports;

  Value* currentFunction = nullptr;
  Type* currentClassType = nullptr; // Set when visiting class methods, for self
  bool inTopLevel = true;
  std::unordered_map<std::string, Type*> currentGenericTypes;

  void registerBaseStubs();

  auto cacheType(std::unique_ptr<Type> type) -> Type*;
  auto resolveType(const TypeExpr* node) -> Type*;
  /** Returns the unified type for binary ops, or nullptr if incompatible. */
  auto getExtendedType(Type* left, Type* right) -> Type*;
  /** Whether a value of type 'from' can be assigned/cast to type 'to'. */
  auto isAssignableTo(Type* from, Type* to) -> bool;

public:
  /** Typecheck with no import * resolution. */
  explicit Typechecker();
  /** Typecheck with import * resolution; mainFilePath used for relative imports. */
  Typechecker(std::string mainFilePath, GetExportsFn getExports);
  ~Typechecker() override = default;

  Typechecker(const Typechecker&) = delete;
  auto operator=(const Typechecker&) -> Typechecker& = delete;

  /** Run typecheck on the given AST. Throws TypeCheckError on first error. */
  auto run(const Compound* ast) -> void;

  /** Take ownership of the symbol table built during typecheck (call after run()). */
  auto takeRootScope() -> std::unique_ptr<SymbolTable>;
  /** Take ownership of the type cache built during typecheck (call after run()). */
  auto takeTypeCache() -> std::vector<std::unique_ptr<Type>>;

  auto visit(const Statement* node) -> void override;
  auto visit(const Compound* node) -> void override;
  auto visit(const VarDecl* node) -> void override;
  auto visit(const If* node) -> void override;
  auto visit(const While* node) -> void override;
  auto visit(const Import* node) -> void override;
  auto visit(const Enum* node) -> void override;
  auto visit(const Class* node) -> void override;
  auto visit(const FuncDecl* node) -> void override;
  auto visit(const ExternFuncDecl* node) -> void override;
  auto visit(const Assignment* node) -> void override;
  auto visit(const Break* node) -> void override;
  auto visit(const Continue* node) -> void override;
  auto visit(const Return* node) -> void override;
  auto visit(const Defer* node) -> void override;
  auto visit(const UnimplementedStatement* node) -> void override;
  auto visit(const ExpressionStatement* node) -> void override;

  auto visit(const Expression* node) -> void override;
  auto visit(const FuncCall* node) -> void override;
  auto visit(const BinaryOp* node) -> void override;
  auto visit(const DotOp* node) -> void override;
  auto visit(const CastOp* node) -> void override;
  auto visit(const IsOp* node) -> void override;
  auto visit(const UnaryOp* node) -> void override;
  auto visit(const Literal* node) -> void override;
  auto visit(const Else* node) -> void override;

  auto visit(const TypeExpr* node) -> void override;
};

} // namespace lesma
