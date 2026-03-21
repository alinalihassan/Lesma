#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/Support/SMLoc.h"

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {

/** Callback to resolve import *: (filepath, isStd, mainFilePath) -> exported
 * names. */
using GetExportsFn =
    std::function<std::vector<std::string>(const std::string&, bool, const std::string&)>;

/**
 * Semantic typecheck pass. Runs after parsing, before codegen.
 * Resolves types (without LLVM), builds symbol table, and checks:
 * - Type existence and resolution
 * - Variable declarations (init type matches declared type)
 * - Assignments (mutability, type compatibility)
 * - Return types vs function signature
 * - Binary/unary op operand types
 * - Function call argument counts and types
 * When getExports is provided, import * adds exported names from the imported
 * file.
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
  SymbolTable* currentMethodInsertScope = nullptr;
  bool inTopLevel = true;
  bool declarationPass = false;
  std::unordered_map<std::string, Type*> currentGenericTypes;
  /** Specialized class types: key = template toString + "|" + concrete types,
   * value = Type* with concrete fields. */
  std::unordered_map<std::string, Type*> specializedClassTypes;
  /** For each specialized class type, the substitution map (generic name ->
   * concrete type) used to create it. */
  std::unordered_map<Type*, std::unordered_map<std::string, Type*>> specializedTypeEnv;
  /** For each specialized class type, the template class type it was created
   * from. */
  std::unordered_map<Type*, Type*> specializedTypeToTemplate;
  /** Imported types materialized into this typechecker's cache so they outlive imported scopes. */
  std::unordered_map<Type*, Type*> importedTypeCopies;

  /** Declared generic param list for a class or function type (resolves to template for specialized
   * classes). */
  auto getDeclaredGenericParams(Type* type) const -> const std::vector<std::string>&;

  /** Import alias (e.g. "import_math") -> absolute path, for resolving return types of
   * import_math.func(). */
  ImportAliasMap importAliasToPath;
  /** Named import/local binding -> (absolute path, exported name). */
  ImportedNameSourceMap importedNameToSource;
  /** Cache of fully analyzed imported modules for import-aware symbol resolution. */
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModuleCache;

  /** Resolve absolute path for an import (same logic as Driver getExportsFromFile). */
  auto resolveImportPath(const std::string& filepath, bool isStd) const -> std::string;
  /** Typecheck an imported file and return its root scope (cached). Returns nullptr if path unknown
   * or typecheck fails. */
  auto getOrTypecheckImport(const std::string& absolutePath) -> SymbolTable*;

  void loadImplicitBaseModule();
  /** Get or create a specialized class type by substituting env into template's
   * fields. */
  auto getOrCreateSpecializedClassType(Type* classTemplate,
                                       const std::vector<std::string>& genericParamNames,
                                       const std::unordered_map<std::string, Type*>& env) -> Type*;
  /** Substitute env into type (for fields); returns cached type. */
  auto substituteInType(Type* t, const std::unordered_map<std::string, Type*>& env) -> Type*;
  /** Infer generic bindings from a parameter/argument type pair. */
  auto inferGenericBindings(Type* pattern, Type* actual,
                            std::unordered_map<std::string, Type*>& bindings, llvm::SMRange span)
      -> void;

  auto cacheType(std::unique_ptr<Type> type) -> Type*;
  auto materializeImportedType(Type* type) -> Type*;
  auto resolveType(const TypeExpr* node) -> Type*;
  /** Returns the unified type for binary ops, or nullptr if incompatible. */
  auto getExtendedType(Type* left, Type* right) -> Type*;
  /** Whether a value of type 'from' can be assigned/cast to type 'to'. */
  auto isAssignableTo(Type* from, Type* to) -> bool;
  /** Result type of a binary operator (arithmetic, comparison, logical). Throws on unsupported op.
   */
  auto typecheckBinaryOpResult(TokenType op, Type* leftTy, Type* rightTy, llvm::SMRange span)
      -> Type*;
  /** Map compound-assignment operator to the corresponding binary operator; nullopt if not
   * compound. */
  auto compoundToBinaryOp(TokenType op) -> std::optional<TokenType>;

  /** Returns true if some path through the statements reaches end of block without a return. */
  auto pathLeadsToEndWithoutReturn(const std::vector<Statement*>& statements, size_t index) -> bool;
  /** Returns true if the block always returns on every path. */
  auto blockAlwaysReturns(const Compound* body) -> bool;

public:
  /** Typecheck with no import * resolution. */
  explicit Typechecker();
  /** Typecheck with import * resolution; mainFilePath used for relative
   * imports. */
  Typechecker(std::string mainFilePath, GetExportsFn getExports);
  ~Typechecker() override = default;

  Typechecker(const Typechecker&) = delete;
  auto operator=(const Typechecker&) -> Typechecker& = delete;

  /** Run typecheck on the given AST. Throws TypeCheckError on first error. */
  auto run(const Compound* ast) -> void;

  /** Take ownership of the symbol table built during typecheck (call after
   * run()). */
  auto takeRootScope() -> std::unique_ptr<SymbolTable>;
  /** Take ownership of the type cache built during typecheck (call after
   * run()). */
  auto takeTypeCache() -> std::vector<std::unique_ptr<Type>>;
  auto takeImportAliasToPath() -> ImportAliasMap;
  auto takeImportedNameToSource() -> ImportedNameSourceMap;
  auto takeImportedModules() -> std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>>;

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
