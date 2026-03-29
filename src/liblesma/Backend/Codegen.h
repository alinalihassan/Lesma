#pragma once

#include <deque>
#include <memory>
#include <stack>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llvm {
class DIBuilder;
class DICompileUnit;
class DIFile;
class DISubroutineType;
class DIType;
class GlobalVariable;
class Instruction;
class AllocaInst;
} // namespace llvm

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Target/TargetMachine.h>

#include <sysexits.h>

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

using namespace llvm;
using namespace llvm::orc;

namespace lesma {
using MainFnTy = int();

class Class;
class TraitDecl;
class FuncDecl;

struct ImportedSpecializationState {
  std::unordered_map<std::string, const Class*> genericClasses;
  std::unordered_map<std::string, lesma::Type*> specializedClassTypesByKey;
  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
      specializedClassTypeEnvs;
  std::unordered_map<lesma::Type*, lesma::Type*> specializedClassTemplateOf;
  std::unordered_map<lesma::Value*, std::unordered_map<std::string, lesma::Type*>>
      specializationEnvs;
  std::unordered_map<std::string, const Class*> codegenClassAstByDisplayName;
};

class Codegen final : public ASTVisitor {
  std::shared_ptr<ThreadSafeContext> theContext;
  std::unique_ptr<Module> theModule;
  std::unique_ptr<IRBuilder<>> builder;

  std::unique_ptr<LLJIT> theJit;
  /// JIT: mangled per-import init symbols; run from \c prepareJit (shared across nested imports).
  std::shared_ptr<std::vector<std::string>> pendingJitModuleInits;
  std::unique_ptr<llvm::TargetMachine> targetMachine;
  std::shared_ptr<Parser> parser;
  std::shared_ptr<SourceMgr> sourceManager;
  // deque so push_back never invalidates Type* pointers stored in scope (from
  // typecheck). Declared before \c rootScope so symbols are destroyed before the
  // type cache.
  std::deque<std::unique_ptr<lesma::Type>> typeCache;
  std::unique_ptr<SymbolTable> rootScope; // Owns the root scope
  SymbolTable* scope{};                   // Non-owning navigation pointer
  std::string filename;
  std::string alias;
  std::unique_ptr<lesma::Value> result;

  std::stack<llvm::BasicBlock*> breakBlocks;
  std::stack<llvm::BasicBlock*> continueBlocks;
  std::stack<std::vector<Statement*>> deferStack;
  lesma::Value* currentFunction = nullptr;

  std::vector<std::string> objectFiles;
  std::shared_ptr<std::vector<std::string>> importedModules;
  std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>>
      importedScopes; // Shared so child (e.g. B) sees parent's (A) imports
                      // (e.g. math)
  std::shared_ptr<std::vector<ImportedSpecializationState>> importedSpecializationStates;
  std::vector<std::unique_ptr<Codegen>> importedCodegens; // Keep imported module codegens alive so
                                                          // Class* in symbols stay valid
  /** Maps `import "m"` alias -> absolute path of `m` (for resolving exported globals). */
  std::unordered_map<std::string, std::string> importAliasToModulePath;
  std::unordered_map<std::string, llvm::StructType*> listStructTypes;
  std::vector<std::tuple<lesma::Value*, const FuncDecl*, Value*>> prototypes;
  /** Class AST + constructor symbol for default `new` bodies (no FuncDecl). */
  std::vector<std::pair<lesma::Value*, const Class*>> syntheticConstructorBodies;
  std::unordered_map<std::string, const FuncDecl*> genericFunctions;
  std::unordered_map<std::string, std::unordered_map<std::string, const FuncDecl*>> genericMethods;
  std::unordered_map<std::string, const Class*> genericClasses;
  std::unordered_map<std::string, lesma::Type*> currentGenericTypes;
  std::unordered_map<std::string, lesma::Value*> specializedFunctions;
  std::unordered_map<std::string, lesma::Value*> specializedClasses;
  std::unordered_map<lesma::Type*, lesma::Value*> specializedClassSymbolsByType;
  std::unordered_map<lesma::Value*, std::unordered_map<std::string, lesma::Type*>>
      specializationEnvs;
  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
      specializedClassTypeEnvs;
  /** From typecheck: specialized class → template (for lowering `Base<T>` field/super types). */
  std::unordered_map<lesma::Type*, lesma::Type*> specializedClassTemplateOf;
  /** From typecheck: stable specialization key -> canonical specialized class type. */
  std::unordered_map<std::string, lesma::Type*> specializedClassTypesByKey;
  std::unordered_map<std::string, std::vector<std::string>> traitRequirementMethodOrder;
  std::unordered_map<std::string, const TraitDecl*> traitDeclByName;
  std::unordered_map<std::string, llvm::GlobalVariable*> witnessGlobalCache;
  std::unordered_map<lesma::Type*, llvm::GlobalVariable*> classVtableGlobals;
  std::unordered_map<lesma::Type*, const Class*> codegenClassAstByType;
  std::unordered_map<std::string, const Class*> codegenClassAstByDisplayName;
  std::unordered_map<std::string, llvm::Function*> traitThunkCache;
  // deque so push_back never invalidates pointers to existing elements (used in
  // prototypes)
  std::deque<std::unique_ptr<lesma::Value>> methodSelfSymbols;
  llvm::Function* topLevelFunc;
  MainFnTy* mainFuncAddress = nullptr;
  Value* selfSymbol = nullptr;
  bool isBreak = false;
  bool isReturn = false;
  bool isAssignment = false;
  bool isJit = false;
  bool isMain = true;
  bool emitDebugInfo = false;
  llvm::OptimizationLevel optimizationLevelForDebug = llvm::OptimizationLevel::O3;

  std::unique_ptr<llvm::DIBuilder> diBuilder;
  llvm::DICompileUnit* diCompileUnit = nullptr;
  llvm::DIFile* moduleDiFile = nullptr;
  llvm::DISubroutineType* emptyDiSubroutineType = nullptr;
  std::unordered_map<unsigned, llvm::DIFile*> diFileByBufferId;

public:
  /** Codegen lowers a module using the semantic scope and type cache produced
   * by the typechecker. */
  Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr,
          const std::string& filename, std::vector<std::string> imports, bool jit, bool main,
          std::string alias = "", const std::shared_ptr<ThreadSafeContext>& = nullptr,
          std::shared_ptr<std::vector<std::string>> sharedModules = nullptr,
          std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>> sharedScopes = nullptr,
          std::shared_ptr<std::vector<ImportedSpecializationState>>
              sharedImportedSpecializationStates = nullptr,
          std::unique_ptr<SymbolTable> preScope = nullptr,
          std::vector<std::unique_ptr<lesma::Type>> preTypeCache = {},
          std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
              preSpecializedClassTypeEnvs = {},
          std::unordered_map<lesma::Type*, lesma::Type*> preSpecializedClassTemplateOf = {},
          std::unordered_map<std::string, lesma::Type*> preSpecializedClassTypesByKey = {},
          bool emitDebug = false,
          llvm::OptimizationLevel optimizationLevelForDebugArg = llvm::OptimizationLevel::O3,
          std::shared_ptr<std::vector<std::string>> sharedPendingJitModuleInits = nullptr);
  ~Codegen() override;

  Codegen(const Codegen&) = delete;
  auto operator=(const Codegen&) -> Codegen& = delete;
  Codegen(Codegen&&) = delete;
  auto operator=(Codegen&&) -> Codegen& = delete;

  auto dump() -> void;
  auto run() -> void;
  auto prepareJit() -> void;
  auto executeJit() -> int;
  auto writeToObjectFile(const std::string& output) -> void;
  auto linkObjectFile(const std::string& objFilename) -> void;
  auto optimize(OptimizationLevel opt) -> void;

  /** Verify the current module's IR (JIT imports). The main program module is not verified: it
   *  may reference Function values owned by other modules in the same LLVMContext, which the IR
   *  verifier rejects while ORC still loads those modules together correctly. */
  auto verifyIrModuleOrThrow(const std::string& contextLabel) const -> void;

protected:
  auto initializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine>;
  auto initializeModule() -> std::unique_ptr<Module>;
  auto initializeJit() -> std::unique_ptr<LLJIT>;
  auto initializeTopLevel() -> llvm::Function*;

  auto initializeDebugMetadata() -> void;
  auto finalizeDebugMetadata() -> void;
  auto getOrCreateDiFileForBuffer(unsigned bufferId) -> llvm::DIFile*;
  auto getDiTypeForLlvmType(llvm::Type* t) -> llvm::DIType*;
  auto attachFunctionDebugInfo(llvm::Function* f, llvm::StringRef displayName,
                               llvm::StringRef linkageName, llvm::SMRange declSpan,
                               llvm::GlobalValue::LinkageTypes linkage, bool isMainSubprogram)
      -> void;
  auto emitParameterDebugDeclare(llvm::Function* fn, llvm::Value* storage, llvm::StringRef name,
                                 unsigned dwArgNo, llvm::DIFile* file, unsigned line,
                                 llvm::Type* paramLlvmTy, llvm::Instruction* insertBefore) -> void;
  auto emitAutoVarDebugDeclare(llvm::AllocaInst* allocaInst, llvm::StringRef name,
                               llvm::SMRange span, llvm::Instruction* insertBefore) -> void;
  auto setDebugLoc(llvm::SMRange span) -> void;
  /** Alloca in \p fn's entry block (after PHIs) so LLVM mem2reg can promote loop/stack slots. */
  auto createAllocaInEntry(llvm::Function* fn, llvm::Type* elemTy, const std::string& name)
      -> llvm::AllocaInst*;

  auto linkObjectFileWithLld(const std::string& objFilename) -> void;

  auto compileModule(llvm::SMRange span, const std::string& filepath, bool isStd,
                     const std::string& alias, bool importAll, bool importToScope,
                     const std::vector<ImportedNameBinding>& importedNames) -> void;
  auto getExportsFromFile(const std::string& filepath, bool isStd, const std::string& mainFilePath)
      -> std::vector<std::string>;
  auto typecheckModule(const Compound* ast, const std::string& modulePath)
      -> std::tuple<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>,
                    std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>,
                    std::unordered_map<lesma::Type*, lesma::Type*>,
                    std::unordered_map<std::string, lesma::Type*>>;
  [[nodiscard]] auto isImported(const std::vector<ImportedNameBinding>& importedNames,
                                const std::string& importName) const -> bool;
  [[nodiscard]] auto getImportedLocalName(const std::vector<ImportedNameBinding>& importedNames,
                                          const std::string& importName) const -> std::string;
  auto insertImportAlias(const std::string& moduleAlias, bool importToScope,
                         const std::string& importedModuleAbsolutePath) -> void;
  auto exposeImportedSymbols(llvm::SMRange span, SymbolTable* importedScope, bool importAll,
                             bool importToScope,
                             const std::vector<ImportedNameBinding>& importedNames) -> void;

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
  auto visit(const SuperExpr* node) -> void override;
  auto visit(const StringInterpolation* node) -> void override;
  auto visit(const ListLiteral* node) -> void override;
  auto visit(const DictLiteral* node) -> void override;
  auto visit(const TupleLiteral* node) -> void override;
  auto visit(const Else* node) -> void override;

  auto visit(const TypeExpr* node) -> void override;

  auto cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
      -> std::unique_ptr<lesma::Value>;

  auto getMangledName(llvm::SMRange span, std::string funcName,
                      const std::vector<lesma::Type*>& paramTypes, bool isMethod = false,
                      std::string alias = "") -> std::string;

  auto genFuncCall(const FuncCall* node, const std::vector<lesma::Value*>& extraParams)
      -> std::unique_ptr<lesma::Value>;
  auto appendCallableArgument(lesma::Value* arg, std::vector<lesma::Type*>& paramTypes,
                              std::vector<llvm::Value*>& paramsLLVM) -> void;
  auto callNamedFunction(llvm::SMRange span, const std::string& functionName,
                         const std::vector<lesma::Type*>& paramTypes,
                         const std::vector<llvm::Value*>& paramsLLVM,
                         const std::vector<lesma::Type*>& explicitTypeArgs = {},
                         Value* typecheckCalleeFallback = nullptr,
                         lesma::Type* allocatedClassMonomorph = nullptr,
                         const std::vector<std::pair<std::string, lesma::Type*>>*
                             genericBindingHint = nullptr)
      -> std::unique_ptr<lesma::Value>;
  auto callListMethodByName(llvm::SMRange span, lesma::Value* receiver,
                            const std::string& methodName,
                            const std::vector<lesma::Value*>& args = {},
                            const std::vector<lesma::Type*>& explicitTypeArgs = {})
      -> std::unique_ptr<lesma::Value>;
  /** True for methods lowered via buffer unwrapping (list-like class layout). */
  [[nodiscard]] auto isBuiltinListBuiltinMethodName(const std::string& methodName) const -> bool;
  /** True when class layout matches stdlib list (single __buffer field; not dict keys+vals). */
  [[nodiscard]] auto classHasSingleBufferStorageField(lesma::Type* classTy) const -> bool;
  auto callMethodByName(llvm::SMRange span, lesma::Value* receiver, const std::string& methodName,
                        const std::vector<lesma::Value*>& args = {},
                        const std::vector<lesma::Type*>& explicitTypeArgs = {})
      -> std::unique_ptr<lesma::Value>;
  auto defineFunction(lesma::Value* value, const FuncDecl* node, Value* clsSymbol) -> void;
  auto declareSynthesizedClassConstructor(const Class* astNode, lesma::Type* classType,
                                          lesma::Value* classStructSym) -> lesma::Value*;
  auto defineSynthesizedClassConstructor(lesma::Value* ctorSym, const Class* astNode) -> void;
  auto getOrEmitClassVtableGlobal(lesma::Type* classTy, const Class* astNode)
      -> llvm::GlobalVariable*;
  auto emitInitClassVtablePointer(lesma::Type* classTy, llvm::Value* objectPtr) -> void;
  auto computeGenericFunctionBindingEnv(const FuncDecl* node,
                                        const std::vector<lesma::Type*>& paramTypes,
                                        const std::vector<std::string>& genericNames,
                                        const std::vector<lesma::Type*>& explicitTypeArgs)
      -> std::unordered_map<std::string, lesma::Type*>;
  auto appendGenericBindingSuffix(llvm::SMRange span, std::string& base,
                                  const std::vector<std::string>& genericNames,
                                  const std::unordered_map<std::string, lesma::Type*>& env) -> void;
  auto specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                          const std::vector<std::string>& genericNames,
                          const std::vector<lesma::Type*>& explicitTypeArgs = {},
                          const std::unordered_map<std::string, lesma::Type*>* bindingEnvHint = nullptr)
      -> lesma::Value*;
  auto specializeClass(const Class* node, const std::vector<lesma::Type*>& constructorArgTypes,
                       const std::vector<lesma::Type*>& explicitTypeArgs = {}) -> lesma::Value*;
  auto emitClassMonomorph(lesma::Type* specialized, const Class* templateAst) -> lesma::Value*;
  [[nodiscard]] auto wrapNominalReturnAsPointer(Type* t) -> Type*;
  /** Match `super` callee receiver type (mirrors Typechecker::superMethodReceiverMatchesFormal). */
  [[nodiscard]] auto superMethodReceiverMatchesFormalCodegen(lesma::Type* formalReceiverClass,
                                                             lesma::Type* staticSuperType) const
      -> bool;

  auto emitCompoundAssign(llvm::SMRange span, TokenType op, lesma::Value* lhs, lesma::Value* value)
      -> void;
  auto emitCompoundAssignArithmetic(llvm::SMRange span, TokenType compoundOp, lesma::Value* loaded,
                                    lesma::Value* rhs) -> std::unique_ptr<lesma::Value>;
  auto emitCompoundSubscriptNewValue(llvm::SMRange span, TokenType compoundOp,
                                     lesma::Value* currentElem, lesma::Value* rhs)
      -> std::unique_ptr<lesma::Value>;
  auto getOrCreateListStructType(lesma::Type* listType) -> llvm::StructType*;
  auto getListStoredElementType(lesma::Type* listType) -> llvm::Type*;
  auto getListStoredElementValue(llvm::SMRange span, lesma::Value* value, lesma::Type* elementType)
      -> llvm::Value*;
  auto emitCalloc(llvm::Value* count, llvm::Value* size, const llvm::Twine& name = "calloc.tmp")
      -> llvm::Value*;
  auto emitMalloc(llvm::Value* size, const llvm::Twine& name = "malloc.tmp") -> llvm::Value*;
  auto emitCstrConcatValues(llvm::SMRange span, llvm::Value* a, llvm::Value* b) -> llvm::Value*;
  auto emitFormatIntegerToCstr(llvm::SMRange span, llvm::Value* intVal, lesma::Type* intTy)
      -> llvm::Value*;
  auto emitFormatFloatToCstr(llvm::SMRange span, llvm::Value* floatVal, lesma::Type* floatTy)
      -> llvm::Value*;
  auto emitBoxedStrLiteralText(llvm::SMRange span, const std::string& text, lesma::Type* strClass)
      -> std::unique_ptr<lesma::Value>;
  auto emitBoxedStrWithCstrField(llvm::SMRange span, llvm::Value* nulTerminatedPtr,
                                 lesma::Type* strClass) -> std::unique_ptr<lesma::Value>;
  auto emitInterpolationExprToBoxedStr(llvm::SMRange span, const Expression* expr,
                                       lesma::Type* exprTy, lesma::Type* strClass)
      -> std::unique_ptr<lesma::Value>;
  auto emitInterpolationExprToCstr(llvm::SMRange span, const Expression* expr, lesma::Type* exprTy)
      -> llvm::Value*;
  auto emitRealloc(llvm::Value* ptr, llvm::Value* size, const llvm::Twine& name = "realloc.tmp")
      -> llvm::Value*;
  auto emitFree(llvm::Value* ptr) -> void;
  auto emitExit(int code) -> void;
  auto emitListLength(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value*;
  auto emitListCapacity(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value*;
  auto emitListDataPtr(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value*;
  auto emitStoreListDataPtr(lesma::Type* listType, llvm::Value* listHandle, llvm::Value* dataValue)
      -> void;
  auto emitStoreListLength(lesma::Type* listType, llvm::Value* listHandle, llvm::Value* length)
      -> void;
  auto emitStoreListCapacity(lesma::Type* listType, llvm::Value* listHandle, llvm::Value* capacity)
      -> void;
  auto emitListBoundsCheck(llvm::SMRange span, lesma::Type* listType, llvm::Value* listHandle,
                           llvm::Value* index) -> void;
  auto emitListElementPointer(llvm::SMRange span, lesma::Type* listType, llvm::Value* listHandle,
                              llvm::Value* index) -> llvm::Value*;
  auto emitListEnsureCapacity(lesma::Type* listType, llvm::Value* listHandle,
                              llvm::Value* minCapacity) -> void;
  auto emitListDeepCopy(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value*;
  [[nodiscard]] auto isListIntrinsicName(const std::string& functionName) const -> bool;
  auto genListIntrinsicCall(const FuncCall* node, const std::vector<lesma::Type*>& paramTypes,
                            const std::vector<llvm::Value*>& paramsLLVM)
      -> std::unique_ptr<lesma::Value>;
  auto symbolUsesDirectLlvmValue(const lesma::Value* symbol) const -> bool;
  auto materializeSymbolValue(lesma::Value* symbol) -> std::unique_ptr<lesma::Value>;
  /** True if class type lists `impl Iterable` (used for buffer-backed for-in lowering). */
  [[nodiscard]] auto classTypeDeclaresIterable(lesma::Type* classTy) const -> bool;

  // Cache a type to keep it alive - returns raw pointer to the cached type
  auto cacheType(std::unique_ptr<lesma::Type> type) -> lesma::Type* {
    typeCache.push_back(std::move(type));
    return typeCache.back().get();
  }

  /** Ensure \p type has an LLVM type (fill in when from typechecker). */
  auto getOrCreateLlvmType(lesma::Type* type) -> llvm::Type*;
  /** LLVM storage type for aggregate fields/slots after ABI lowering. */
  [[nodiscard]] auto getStoredAggregateFieldLlvmType(lesma::Type* fieldType) -> llvm::Type*;
  /** Load an aggregate field/slot value using the ABI-lowered storage type. */
  auto loadStoredAggregateFieldValue(llvm::Value* slotPtr, lesma::Type* fieldType,
                                     const llvm::Twine& name = "") -> llvm::Value*;

  /** Resolve the class template symbol for codegen; prefers Type display name (imported classes may
   * not have a named LLVM struct yet). */
  auto lookupClassStructSymbol(lesma::Type* classTy) -> Value*;
  [[nodiscard]] auto specializedClassEnvFor(lesma::Type* classTy)
      -> const std::unordered_map<std::string, lesma::Type*>*;
  [[nodiscard]] auto lookupClassVtableGlobal(lesma::Type* classTy) -> llvm::GlobalVariable*;

  auto collectTraitMetadataFromAst() -> void;
  auto mergeImportedTraitMetadata(Codegen const& imported) -> void;
  [[nodiscard]] auto captureImportedSpecializationState() const -> ImportedSpecializationState;
  auto mergeImportedSpecializationState(ImportedSpecializationState const& imported) -> void;
  /// Copy specialization env maps from an imported module codegen so call sites in this module
  /// can resolve TY_GENERIC when invoking methods on specialized types from the import.
  auto mergeImportedSpecializationState(Codegen const& imported) -> void;
  auto emitErasedThunkForTraitMethod(lesma::Type* classType, const std::string& traitName,
                                     const FuncDecl* req) -> llvm::Function*;
  auto getOrEmitWitnessTable(lesma::Type* classType, const std::string& traitName)
      -> llvm::GlobalVariable*;
  auto emitBoxClassToExistential(lesma::Type* existentialType, lesma::Type* classPtrLesmaType,
                                 llvm::Value* classPtrVal) -> llvm::Value*;
  auto callExistentialMethod(llvm::SMRange span, lesma::Value* receiver,
                             const std::string& methodName, const std::vector<lesma::Value*>& args,
                             const std::vector<lesma::Type*>& explicitTypeArgs)
      -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto findTraitRequirement(const TraitDecl* trait,
                                          const std::string& methodName) const -> const FuncDecl*;

  /** Populate \p env by structurally matching declared (TypeExpr) vs actual
   * (lesma::Type), binding generic names from \p genericNameSet. If \p bindingConflict
   * is non-null, a conflicting second binding for the same generic is reported there. */
  auto bindGenericsFromTypePair(const TypeExpr* declared, lesma::Type* actual,
                                const std::unordered_set<std::string>& genericNameSet,
                                std::unordered_map<std::string, lesma::Type*>& env,
                                bool* bindingConflict = nullptr) -> void;

  /** Self + parameter types for class method overload resolution; must match \c visit(FuncDecl). */
  auto buildClassMethodParamTypesForLookup(const FuncDecl* node) -> std::vector<lesma::Type*>;

  [[nodiscard]] auto findGenericClassAstForTemplateType(lesma::Type* classTemplateTy) const
      -> const Class*;
  /** Substitute generic parameters (and nested specialized classes) for lowering a template field
   * or superclass type. */
  auto substituteTypeForSpecializationEnv(
      lesma::Type* t, const std::unordered_map<std::string, lesma::Type*>& env) -> lesma::Type*;
};
} // namespace lesma