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

class TraitDecl;

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
  /** While visiting a class declaration: template Type* being built (fields added incrementally).
   */
  Type* classTemplateBeingDeclared = nullptr;
  /** Expected field count for `classTemplateBeingDeclared` (for incomplete specialization stubs).
   */
  size_t classFieldCountExpected = 0;
  /** Specialized trait existentials: key = trait name + concrete type strings (see
   * getOrCreateSpecializedTraitExistentialType). */
  std::unordered_map<std::string, Type*> specializedTraitExistentialTypes;
  /** Substitution env for each specialized trait existential (trait generic name -> type). */
  std::unordered_map<Type*, std::unordered_map<std::string, Type*>> specializedTraitExistentialEnv;
  /** Imported types materialized into this typechecker's cache so they outlive imported scopes. */
  std::unordered_map<Type*, Type*> importedTypeCopies;
  std::vector<Type*> expectedTypes;

  /** Registered traits (name → AST) for impl checks and existential method lookup. */
  std::unordered_map<std::string, const TraitDecl*> traitRegistry;
  /** traitName -> methodName -> overload signatures (TY_FUNCTION: self + params, return type;
   *  multiple entries per name preserve overloads; resolved during visit(TraitDecl)). */
  std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Type*>>>
      traitMethodSignatures;
  /** While typechecking a generic function body: generic param name -> trait bound names. */
  std::unordered_map<std::string, std::vector<std::string>> currentGenericParamTraitBounds;

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
  [[nodiscard]] auto resolveImportPath(const std::string& filepath, bool isStd) const
      -> std::string;
  /** Typecheck an imported file and return its root scope (cached). Returns nullptr if path unknown
   * or typecheck fails. */
  auto getOrTypecheckImport(const std::string& absolutePath) -> SymbolTable*;

  void loadImplicitStdModule(const std::string& moduleFilename);
  /** Get or create a specialized class type by substituting env into template's
   * fields. */
  auto getOrCreateSpecializedClassType(Type* classTemplate,
                                       const std::vector<std::string>& genericParamNames,
                                       const std::unordered_map<std::string, Type*>& env) -> Type*;
  /** After all template fields exist, fill in placeholder specialized types created
   * mid-declaration. */
  void finalizeSpecializedTypesForTemplate(Type* classTemplate);
  /** Existential trait type with explicit type args (e.g. Iterator<int>). */
  auto getOrCreateSpecializedTraitExistentialType(Type* traitTemplate,
                                                  const std::string& lookupName,
                                                  const std::vector<std::string>& genericParamNames,
                                                  const std::vector<Type*>& explicitTypeArgs)
      -> Type*;
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
  [[nodiscard]] auto functionTypesMatchForTraitImpl(Type* actualFn, Type* expectedFn) -> bool;
  [[nodiscard]] auto wrapReturnTypeIfNominal(Type* returnType) -> Type*;
  /** Result type of a binary operator (arithmetic, comparison, logical). Throws on unsupported op.
   */
  auto typecheckBinaryOpResult(TokenType op, Type* leftTy, Type* rightTy, llvm::SMRange span)
      -> Type*;
  auto visitExprWithExpectedType(const Expression* node, Type* expected) -> void;
  [[nodiscard]] auto currentExpectedType() const -> Type*;
  auto resolveMethodReturnType(Type* baseType, const std::string& methodName,
                               const std::vector<Type*>& argTypes, llvm::SMRange span) -> Type*;
  auto isMutableListReceiver(const Expression* expr) -> bool;
  [[nodiscard]] auto isMutatingListFunction(const std::string& functionName) const -> bool;
  [[nodiscard]] auto isListIntrinsicName(const std::string& functionName) const -> bool;
  auto visitListIntrinsicCall(const FuncCall* node, const std::vector<Type*>& argTypes) -> bool;
  auto visitListMethodCall(Type* listType, const DotOp* node, const FuncCall* call) -> bool;
  /** Map compound-assignment operator to the corresponding binary operator; nullopt if not
   * compound. */
  auto compoundToBinaryOp(TokenType op) -> std::optional<TokenType>;

  /** Returns true if some path through the statements reaches end of block without a return. */
  auto pathLeadsToEndWithoutReturn(const std::vector<Statement*>& statements, size_t index) -> bool;
  /** Returns true if the block always returns on every path. */
  auto blockAlwaysReturns(const Compound* body) -> bool;

  auto buildMethodFunctionType(FuncDecl* decl, Type* classType) -> Type*;
  auto registerTraitDefaultMethodSymbol(SymbolTable* insertScope, Type* classType, FuncDecl* req)
      -> void;
  auto typecheckTraitDefaultBodies(const Class* classNode, Type* classType,
                                   SymbolTable* methodInsertScope) -> void;
  auto checkTraitImplementation(const Class* classNode, Type* classType,
                                SymbolTable* methodInsertScope) -> void;
  /** Replace `currentGenericTypes` with a copy that includes bindings for the trait's generic
   * parameters from `impl Trait<...>` (e.g. `Iterable<T>` or `Iterable<int>`). */
  auto mergeTraitImplTypeArgsIntoCurrentGenericEnv(const Class* classNode, size_t traitClauseIndex,
                                                   const TraitDecl* trait) -> void;
  auto verifyGenericTraitBounds(Value* callee, const std::unordered_map<std::string, Type*>& subs,
                                llvm::SMRange span) -> void;
  auto classDeclaresTrait(Type* classTy, const std::string& traitName) -> bool;

  /** Register trait AST nodes from an imported file so `impl Trait` resolves in the importer. */
  auto registerTraitsFromImportedModule(const std::string& absolutePath) -> void;

  /** `Iterator<int>` -> `Iterator` for trait registry / implTraitNames lookup. */
  [[nodiscard]] auto traitExistentialBaseName(const std::string& displayName) -> std::string;
  /** Same argument list identity as `SymbolTable::lookupFunction(name, paramTypes)` (self + params).
   */
  [[nodiscard]] auto methodLookupSignatureKey(const std::string& name,
                                              const std::vector<Type*>& lookupArgs) -> std::string;
  /** Move all owning Type nodes from an import analysis tree into \p dest so \c
   * SymbolTable typeRefs remain valid after \c importedModuleCache is cleared. */
  void mergeImportedAnalysisTypeCachesInto(std::vector<std::unique_ptr<Type>>& dest,
                                           const std::shared_ptr<ImportedModuleAnalysis>& mod);
  /** True when \p sym is the nominal type name binding (not a value, function,
   *  or enum member), for CUSTOM_TYPE resolution after lookupStruct / lookup. */
  [[nodiscard]] auto isTypeSymbolForCustomTypeName(Value const* sym) -> bool;

public:
  /** Typecheck with no import * resolution. */
  explicit Typechecker();
  /** Typecheck with import * resolution; mainFilePath used for relative
   * imports. */
  Typechecker(std::string mainFilePath, GetExportsFn getExports);
  ~Typechecker() override = default;

  Typechecker(const Typechecker&) = delete;
  auto operator=(const Typechecker&) -> Typechecker& = delete;
  Typechecker(Typechecker&&) = delete;
  auto operator=(Typechecker&&) -> Typechecker& = delete;

  /** Run typecheck on the given AST. Throws TypeCheckError on first error. */
  auto run(const Compound* ast) -> void;

  /** Take ownership of the symbol table built during typecheck (call after
   * run()). */
  auto takeRootScope() -> std::unique_ptr<SymbolTable>;
  /** Take ownership of the type cache built during typecheck (call after
   * run()). */
  auto takeTypeCache() -> std::vector<std::unique_ptr<Type>>;
  /** Per-specialized-class and trait-existential generic bindings (e.g. T -> int), for codegen. */
  auto takeSpecializedTypeEnv()
      -> std::unordered_map<Type*, std::unordered_map<std::string, Type*>>;
  auto takeImportAliasToPath() -> ImportAliasMap;
  auto takeImportedNameToSource() -> ImportedNameSourceMap;
  auto takeImportedModules()
      -> std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>>;

  auto visit(const Statement* node) -> void override;
  auto visit(const Compound* node) -> void override;
  auto visit(const VarDecl* node) -> void override;
  auto visit(const If* node) -> void override;
  auto visit(const While* node) -> void override;
  auto visit(const ForIn* node) -> void override;
  auto visit(const Import* node) -> void override;
  auto visit(const Enum* node) -> void override;
  auto visit(const Class* node) -> void override;
  auto visit(const TraitDecl* node) -> void override;
  auto visit(const FuncDecl* node) -> void override;
  auto visit(const ExternFuncDecl* node) -> void override;
  auto visit(const Assignment* node) -> void override;
  auto visit(const Break* node) -> void override;
  auto visit(const Continue* node) -> void override;
  auto visit(const Pass* node) -> void override;
  auto visit(const Return* node) -> void override;
  auto visit(const Defer* node) -> void override;
  auto visit(const UnimplementedStatement* node) -> void override;
  auto visit(const ExpressionStatement* node) -> void override;

  auto visit(const Expression* node) -> void override;
  auto visit(const FuncCall* node) -> void override;
  auto visit(const BinaryOp* node) -> void override;
  auto visit(const SubscriptOp* node) -> void override;
  auto visit(const DotOp* node) -> void override;
  auto visit(const CastOp* node) -> void override;
  auto visit(const IsOp* node) -> void override;
  auto visit(const UnaryOp* node) -> void override;
  auto visit(const Literal* node) -> void override;
  auto visit(const ListLiteral* node) -> void override;
  auto visit(const Else* node) -> void override;

  auto visit(const TypeExpr* node) -> void override;

  auto getStdStrType(llvm::SMRange span) -> Type*;
  auto isStdStrClassType(Type* type) const -> bool;
};

} // namespace lesma
