#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stack>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sysexits.h>

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

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Common/ExportDiscovery.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/UnionNarrowingStableKey.h"
#include "liblesma/Symbol/Value.h"

using namespace llvm;
using namespace llvm::orc;

namespace lesma {
using MainFnTy = int();

class Class;
class Enum;
class TraitDecl;
class FuncDecl;
class FuncCall;
class LambdaExpr;
class VarDecl;
class ForIn;
class DotOp;
class Timer;

struct ImportedSpecializationState {
  std::unordered_map<std::string, const Class*> genericClasses;
  std::unordered_map<std::string, const Enum*> genericEnums;
  std::unordered_map<std::string, lesma::Type*> specializedClassTypesByKey;
  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
      specializedClassTypeEnvs;
  std::unordered_map<lesma::Type*, lesma::Type*> specializedClassTemplateOf;
  std::unordered_map<lesma::Value*, std::unordered_map<std::string, lesma::Type*>>
      specializationEnvs;
  std::unordered_map<std::string, const Class*> codegenClassAstByDisplayName;
  std::unordered_map<std::string, const Enum*> codegenEnumAstByDisplayName;
  std::unordered_map<std::string, std::vector<std::string>> traitRequirementMethodOrder;
  std::unordered_map<std::string, const TraitDecl*> traitDeclByName;
};

enum class SyntheticEnumMethodKind : std::uint8_t { CONSTRUCTOR };

struct SyntheticEnumMethodBody {
  lesma::Value* symbol = nullptr;
  lesma::Type* enumType = nullptr;
  unsigned variantIndex = 0U;
  SyntheticEnumMethodKind kind = SyntheticEnumMethodKind::CONSTRUCTOR;
};

struct ArcTrackedSlot {
  llvm::Value* slot = nullptr;
  lesma::Type* type = nullptr;
  bool storesFuncValuePair = false;
};

struct ModuleArcTrackedRoot {
  llvm::GlobalVariable* slot = nullptr;
  lesma::Type* type = nullptr;
  bool storesFuncValuePair = false;
  std::string debugName;
};

class Codegen final : public ASTVisitor {
  std::shared_ptr<ThreadSafeContext> theContext;
  std::unique_ptr<Module> theModule;
  std::unique_ptr<IRBuilder<>> builder;

  std::shared_ptr<LLLazyJIT> theJit;
  /// JIT: mangled per-import init symbols; run from \c prepareJit (shared across nested imports).
  std::shared_ptr<std::vector<std::string>> pendingJitModuleInits;
  /// JIT: mangled per-import fini symbols; called by the main module in reverse init order.
  std::shared_ptr<std::vector<std::string>> pendingJitModuleFinis;
  std::unique_ptr<llvm::TargetMachine> targetMachine;
  std::shared_ptr<Parser> parser;
  std::shared_ptr<SourceMgr> sourceManager;
  Timer* performanceTimer = nullptr;
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
  /** \c deferStack.size() after \c deferStack.emplace() for the current module/callable unit. */
  std::stack<size_t> deferBaselineStack;
  lesma::Value* currentFunction = nullptr;
  llvm::Value* currentAsyncCoroHandle = nullptr;
  llvm::Value* currentAsyncPromisePtr = nullptr;
  llvm::BasicBlock* currentAsyncReturnBlock = nullptr;
  lesma::Type* currentAsyncReturnPayloadType = nullptr;
  std::unordered_map<lesma::Type*, llvm::StructType*> asyncPromiseTypes;

  std::vector<std::string> objectFiles;
  std::shared_ptr<std::vector<std::string>> importedModules;
  std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>>
      importedScopes; // Shared so child (e.g. B) sees parent's (A) imports
                      // (e.g. math)
  std::shared_ptr<std::vector<ImportedSpecializationState>> importedSpecializationStates;
  /** Keeps imported-module typecheck analyses alive while lowering imported ASTs. */
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModuleAnalyses;
  std::vector<std::unique_ptr<Codegen>> importedCodegens; // Keep imported module codegens alive so
                                                          // Class* in symbols stay valid
  /** Maps `import "m"` alias -> absolute path of `m` (for resolving exported globals). */
  std::unordered_map<std::string, std::string> importAliasToModulePath;
  std::unordered_map<std::string, llvm::StructType*> listStructTypes;
  std::vector<std::tuple<lesma::Value*, const FuncDecl*, Value*>> prototypes;
  /** Class AST + constructor symbol for default `new` bodies (no FuncDecl). */
  std::vector<std::pair<lesma::Value*, const Class*>> syntheticConstructorBodies;
  std::vector<SyntheticEnumMethodBody> syntheticEnumMethodBodies;
  std::unordered_map<std::string, const FuncDecl*> genericFunctions;
  std::unordered_map<std::string, const LambdaExpr*> genericLambdas;
  std::vector<std::pair<lesma::Value*, const LambdaExpr*>> lambdaPrototypes;
  llvm::StructType* funcValuePairLlvmType = nullptr;
  std::unordered_map<std::string, std::unordered_map<std::string, const FuncDecl*>> genericMethods;
  std::unordered_map<std::string, const Class*> genericClasses;
  std::unordered_map<std::string, const Enum*> genericEnums;
  std::unordered_map<std::string, lesma::Type*> currentGenericTypes;
  /** Stack of call-site binding maps for `getOrCreateLlvmType` when `currentGenericTypes` is empty
   * (e.g. `Cell.of(7)` nested inside `main`). */
  std::vector<const std::unordered_map<std::string, lesma::Type*>*> genericTypeFallbackStack;
  std::unordered_map<std::string, lesma::Value*> specializedFunctions;
  std::unordered_map<std::string, lesma::Value*> specializedClasses;
  std::unordered_map<lesma::Type*, lesma::Value*> specializedClassSymbolsByType;
  std::unordered_map<lesma::Value*, std::unordered_map<std::string, lesma::Type*>>
      specializationEnvs;
  /** While emitting a specialized generic function, maps template \c SymbolTable* from typecheck to
   *  this emission's cloned tables (see \c defineFunction). */
  std::unordered_map<lesma::SymbolTable*, lesma::SymbolTable*> codegenTemplateBodyScopeRemap;
  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
      specializedClassTypeEnvs;
  /** From typecheck: specialized class → template (for lowering `Base<T>` field/super types). */
  std::unordered_map<lesma::Type*, lesma::Type*> specializedClassTemplateOf;
  /** From typecheck: stable specialization key -> canonical specialized class type. */
  std::unordered_map<std::string, lesma::Type*> specializedClassTypesByKey;
  std::unordered_map<std::string, std::vector<std::string>> traitRequirementMethodOrder;
  std::unordered_map<std::string, const TraitDecl*> traitDeclByName;
  std::unordered_map<std::string, llvm::GlobalVariable*> witnessGlobalCache;
  std::unordered_map<std::string, llvm::GlobalVariable*> anyTypeInfoGlobals;
  std::unordered_map<std::string, llvm::Function*> anyTypeInfoRetainFns;
  std::unordered_map<std::string, llvm::Function*> anyTypeInfoReleaseFns;
  std::unordered_map<lesma::Type*, llvm::GlobalVariable*> classVtableGlobals;
  std::unordered_map<std::string, llvm::Function*> arcDestroyFns;
  std::unordered_map<std::string, llvm::Function*> arcPayloadDestroyFns;
  std::unordered_map<std::string, llvm::Function*> arcStorageRetainFns;
  std::unordered_map<std::string, llvm::Function*> arcStorageReleaseFns;
  std::unordered_map<std::string, llvm::Function*> arcClosureDestroyFns;
  std::unordered_map<lesma::Type*, const Class*> codegenClassAstByType;
  std::unordered_map<std::string, const Class*> codegenClassAstByDisplayName;
  std::unordered_map<lesma::Type*, const Enum*> codegenEnumAstByType;
  std::unordered_map<std::string, const Enum*> codegenEnumAstByDisplayName;
  std::unordered_map<std::string, llvm::Function*> traitThunkCache;
  // deque so push_back never invalidates pointers to existing elements (used in
  // prototypes)
  std::deque<std::unique_ptr<lesma::Value>> methodSelfSymbols;
  llvm::Function* topLevelFunc;
  MainFnTy* mainFuncAddress = nullptr;
  Value* selfSymbol = nullptr;
  llvm::StructType* arcHeaderLlvmType = nullptr;
  llvm::Function* arcDebugDeltaFn = nullptr;
  llvm::Function* arcDebugReportFn = nullptr;
  llvm::Function* arcDebugCleanupBeginFn = nullptr;
  llvm::Function* arcDebugCleanupStepFn = nullptr;
  llvm::Function* moduleCleanupFn = nullptr;
  std::vector<std::vector<ArcTrackedSlot>> arcOwnedSlotFrames;
  std::vector<ModuleArcTrackedRoot> moduleArcTrackedRoots;
  bool isBreak = false;
  bool isReturn = false;
  bool isAssignment = false;
  bool isJit = false;
  bool isMain = true;
  bool emitDebugInfo = false;
  bool emitArcDebug = false;
  bool emitArcTrace = false;
  std::size_t lambdaCounter = 0U;
  llvm::OptimizationLevel optimizationLevelForDebug = llvm::OptimizationLevel::O3;
  /** `if x is T` / else: maps parameter/local symbol → union variant index for narrowed loads. */
  std::vector<std::unordered_map<UnionNarrowingStableKey, unsigned, UnionNarrowingStableKeyHash,
                                 UnionNarrowingStableKeyEq>>
      unionNarrowVariantStack;

  struct AsyncCodegenStateSnapshot {
    llvm::Value* coroHandle = nullptr;
    llvm::Value* promisePtr = nullptr;
    llvm::BasicBlock* returnBlock = nullptr;
    lesma::Type* returnPayloadType = nullptr;
  };

  struct AsyncCoroutineBlocks {
    llvm::BasicBlock* suspendBlock = nullptr;
    llvm::BasicBlock* resumeBlock = nullptr;
    llvm::BasicBlock* cleanupBlock = nullptr;
    llvm::BasicBlock* trapBlock = nullptr;
    llvm::BasicBlock* dynAllocBlock = nullptr;
    llvm::BasicBlock* coroBeginBlock = nullptr;
    llvm::Value* coroId = nullptr;
  };

  /** Pushes a non-empty narrow map onto \c unionNarrowVariantStack in the ctor and pops in the
   * dtor so the stack stays balanced if nested codegen throws (e.g. \c CodegenError). */
  struct UnionNarrowingScope {
    using MapTy = std::unordered_map<UnionNarrowingStableKey, unsigned, UnionNarrowingStableKeyHash,
                                     UnionNarrowingStableKeyEq>;
    UnionNarrowingScope(std::vector<MapTy>& stackRef, MapTy&& map)
        : stack(stackRef), pushed(!map.empty()) {
      if (pushed) {
        stack.push_back(std::move(map));
      }
    }
    ~UnionNarrowingScope() {
      if (pushed) {
        stack.pop_back();
      }
    }
    UnionNarrowingScope(const UnionNarrowingScope&) = delete;
    auto operator=(const UnionNarrowingScope&) -> UnionNarrowingScope& = delete;
    UnionNarrowingScope(UnionNarrowingScope&&) = delete;
    auto operator=(UnionNarrowingScope&&) -> UnionNarrowingScope& = delete;

  private:
    std::vector<MapTy>& stack;
    bool pushed;
  };

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
          std::shared_ptr<LLLazyJIT> sharedJit = nullptr,
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
          std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>>
              preImportedModuleAnalyses = {},
          Timer* performanceTimer = nullptr, bool emitDebug = false, bool emitArcDebug = false,
          bool emitArcTrace = false,
          llvm::OptimizationLevel optimizationLevelForDebugArg = llvm::OptimizationLevel::O3,
          std::shared_ptr<std::vector<std::string>> sharedPendingJitModuleInits = nullptr,
          std::shared_ptr<std::vector<std::string>> sharedPendingJitModuleFinis = nullptr);
  ~Codegen() override;

  Codegen(const Codegen&) = delete;
  auto operator=(const Codegen&) -> Codegen& = delete;
  Codegen(Codegen&&) = delete;
  auto operator=(Codegen&&) -> Codegen& = delete;

  auto dump() -> void;
  [[nodiscard]] auto moduleToString() const -> std::string;
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
  auto initializeJit() -> std::unique_ptr<LLLazyJIT>;
  auto initializeTopLevel() -> llvm::Function*;
  [[nodiscard]] static auto statementRequiresModuleInit(const Statement* stmt) -> bool;
  [[nodiscard]] auto moduleNeedsJitInit() const -> bool;

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
  auto emitEntryNullInit(llvm::AllocaInst* slot, llvm::Type* storageTy) -> void;

  [[nodiscard]] auto lookupUnionNarrowVariant(lesma::Value* sym) const -> std::optional<unsigned>;
  auto fillCodegenUnionNarrowVariantMap(
      const If* node, unsigned blockIndex,
      std::unordered_map<UnionNarrowingStableKey, unsigned, UnionNarrowingStableKeyHash,
                         UnionNarrowingStableKeyEq>& out) -> void;
  auto emitUnionWrapValue(llvm::SMRange span, lesma::Value* val, lesma::Type* unionTy,
                          unsigned variantIndex) -> std::unique_ptr<lesma::Value>;
  /** Wrap \p val into \p unionTy at \p variantIndex using existing alloca \p destSlot (union
   * struct). */
  auto emitUnionWrapValueToSlot(llvm::SMRange span, lesma::Value* val, lesma::Type* unionTy,
                                unsigned variantIndex, llvm::Value* destSlot,
                                bool retainBorrowedPayload = true)
      -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto unionVariantIndexOf(lesma::Type* unionTy, lesma::Type* memberTy) const
      -> std::optional<unsigned>;
  auto emitUnionPayloadLoadFromSlot(llvm::Value* unionAllocaPtr, lesma::Type* unionTy,
                                    lesma::Type* memberTy) -> llvm::Value*;
  [[nodiscard]] auto getOrCreateEnumTagLlvmType(lesma::Type* enumTy) -> llvm::Type*;
  [[nodiscard]] auto getEnumPayloadLlvmType(lesma::Type* enumTy) -> llvm::Type*;
  [[nodiscard]] auto getEnumVariantAggregatePayloadType(lesma::Type* enumTy, unsigned variantIndex)
      -> lesma::Type*;
  auto emitEnumPayloadLoadFromSlot(llvm::Value* enumAllocaPtr, lesma::Type* enumTy,
                                   unsigned variantIndex) -> llvm::Value*;
  auto emitEnumConstructValue(llvm::SMRange span, lesma::Type* enumTy, unsigned variantIndex,
                              const std::vector<lesma::Value*>& payloadValues)
      -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto getOptionalPayloadType(lesma::Type* type) const -> lesma::Type*;
  auto materializeNarrowedUnionValue(lesma::Value* value, lesma::Type* narrowedType,
                                     const std::string& tempName) -> std::unique_ptr<lesma::Value>;
  auto getOrCreateAnyTypeInfoGlobal(lesma::Type* type) -> llvm::GlobalVariable*;
  auto emitAnyTypeInfoPtr(lesma::Type* type) -> llvm::Value*;
  auto emitAnyTypeInfoMatches(llvm::Value* typeInfo, lesma::Type* candidate,
                              const llvm::Twine& name = "any.type.match") -> llvm::Value*;
  auto emitBoxToAny(llvm::SMRange span, lesma::Value* value, lesma::Type* anyType)
      -> std::unique_ptr<lesma::Value>;
  auto emitUnboxFromAny(llvm::SMRange span, lesma::Value* value, lesma::Type* targetType)
      -> std::unique_ptr<lesma::Value>;
  auto emitAnyIsCheck(llvm::SMRange span, lesma::Value* value, lesma::Type* testType, bool negate)
      -> std::unique_ptr<lesma::Value>;

  auto linkObjectFileWithLld(const std::string& objFilename) -> void;

  auto compileModule(llvm::SMRange span, const std::string& filepath, bool isStd,
                     const std::string& alias, bool importAll, bool importToScope,
                     const std::vector<ImportedNameBinding>& importedNames) -> void;
  auto getExportsFromFile(const std::string& filepath, bool isStd, const std::string& mainFilePath)
      -> ExportDiscoveryResult;
  auto typecheckModule(const Compound* ast, const std::string& modulePath)
      -> std::tuple<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>,
                    std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>,
                    std::unordered_map<lesma::Type*, lesma::Type*>,
                    std::unordered_map<std::string, lesma::Type*>,
                    std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>>>;
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
  auto visit(const TypeAlias* node) -> void override;
  auto visit(const Enum* node) -> void override;
  auto visit(const Class* node) -> void override;
  auto visit(const TraitDecl* node) -> void override;
  auto visit(const FuncDecl* node) -> void override;
  auto visit(const ExternFuncDecl* node) -> void override;
  auto visit(const Assignment* node) -> void override;
  auto visit(const Break* node) -> void override;
  auto visit(const Continue* node) -> void override;
  auto visit(const Return* node) -> void override;
  auto visit(const Defer* node) -> void override;
  /** Emit deferred statements in LIFO order (last \c defer registered runs first). */
  auto runDeferredStatements(std::vector<Statement*> const& stmts) -> void;
  /** Call after \c deferStack.emplace() for module / function / lambda / ctor bodies. */
  auto pushDeferBaseline() -> void;
  /** Run defers for all active loop frames, then this callable's defer list (for \c return). */
  auto flushDeferredFramesForReturn() -> void;
  /** End of loop iteration / \c break / \c continue: run and pop one loop defer frame if any. */
  auto finishLoopDeferFrameIfAny() -> void;
  auto visit(const UnimplementedStatement* node) -> void override;
  auto visit(const ExpressionStatement* node) -> void override;

  auto visit(const Expression* node) -> void override;
  auto visit(const FuncCall* node) -> void override;
  auto visit(const LambdaExpr* node) -> void override;
  auto visit(const BinaryOp* node) -> void override;
  auto visit(const SubscriptOp* node) -> void override;
  auto visit(const DotOp* node) -> void override;
  void lowerDotOpSuperMethodCall(const DotOp* node);
  auto visit(const CastOp* node) -> void override;
  auto visit(const IsOp* node) -> void override;
  auto visit(const MatchExpr* node) -> void override;
  auto visit(const BlockExpr* node) -> void override;
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
  /** Visit positional args; \p storage owns values, \p argsOut holds raw pointers into it. */
  auto evaluateCallArgValues(const FuncCall* call,
                             std::vector<std::unique_ptr<lesma::Value>>& storage,
                             std::vector<lesma::Value*>& argsOut) -> void;
  auto evaluateCallExplicitTypeArgs(const FuncCall* call, std::vector<lesma::Type*>& typesOut)
      -> void;
  auto callNamedFunction(
      llvm::SMRange span, const std::string& functionName,
      const std::vector<lesma::Type*>& paramTypes, const std::vector<llvm::Value*>& paramsLLVM,
      const std::vector<lesma::Type*>& explicitTypeArgs = {},
      Value* typecheckCalleeFallback = nullptr, lesma::Type* allocatedClassMonomorph = nullptr,
      const std::vector<std::pair<std::string, lesma::Type*>>* genericBindingHint = nullptr)
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
  /** `unionValue.method(...)` when every union member is a class with a compatible method. */
  auto emitUnionClassMethodDispatch(llvm::SMRange span, lesma::Value* unionValue,
                                    const std::string& methodName,
                                    const std::vector<lesma::Value*>& args,
                                    const std::vector<lesma::Type*>& explicitTypeArgs)
      -> std::unique_ptr<lesma::Value>;
  auto callMethodByName(llvm::SMRange span, lesma::Value* receiver, const std::string& methodName,
                        const std::vector<lesma::Value*>& args = {},
                        const std::vector<lesma::Type*>& explicitTypeArgs = {},
                        lesma::Value* resolvedCallee = nullptr,
                        const FuncCall* callSiteForGenericEnv = nullptr)
      -> std::unique_ptr<lesma::Value>;
  /** Lower a static class field (TYPE_SYMBOL receiver); uses \c isAssignment. */
  auto emitClassStaticFieldValue(Type* classTy, const std::string& field,
                                 llvm::SMRange unknownFieldSpan, llvm::SMRange storageDiagSpan)
      -> std::unique_ptr<lesma::Value>;
  /** Instance data field GEP; \p classStructSym from \c lookupClassStructSymbol. */
  auto emitClassInstanceDataField(Value* classStructSym, llvm::Value* objectBase,
                                  const std::string& field, llvm::SMRange span,
                                  bool baseMayBeNonPointer) -> std::unique_ptr<lesma::Value>;
  void emitClassStaticMethodCall(const DotOp* node, Type* classTy, const FuncCall* method);
  void emitClassInstanceMethodCall(const DotOp* node, lesma::Value* receiver,
                                   const FuncCall* method);
  auto emitClassStaticFieldGlobals(lesma::Type* classTy, const Class* astNode) -> void;
  /** LLVM global for a static field; uses the class template symbol when \p classTy is specialized.
   */
  [[nodiscard]] auto llvmGlobalForClassStaticField(lesma::Type* classTy,
                                                   const std::string& fieldName) -> llvm::Value*;
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
  auto computeGenericLambdaBindingEnv(const LambdaExpr* node,
                                      const std::vector<lesma::Type*>& paramTypes,
                                      const std::vector<std::string>& genericNames,
                                      const std::vector<lesma::Type*>& explicitTypeArgs)
      -> std::unordered_map<std::string, lesma::Type*>;
  auto appendGenericBindingSuffix(llvm::SMRange span, std::string& base,
                                  const std::vector<std::string>& genericNames,
                                  const std::unordered_map<std::string, lesma::Type*>& env) -> void;
  auto
  specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                     const std::vector<std::string>& genericNames,
                     const std::vector<lesma::Type*>& explicitTypeArgs = {},
                     const std::unordered_map<std::string, lesma::Type*>* bindingEnvHint = nullptr)
      -> lesma::Value*;
  auto
  specializeLambda(const LambdaExpr* node, const std::vector<lesma::Type*>& paramTypes,
                   const std::vector<std::string>& genericNames,
                   const std::vector<lesma::Type*>& explicitTypeArgs = {},
                   const std::unordered_map<std::string, lesma::Type*>* bindingEnvHint = nullptr)
      -> lesma::Value*;
  auto defineLambdaFunction(lesma::Value* value, const LambdaExpr* node) -> void;
  [[nodiscard]] auto getFuncValuePairLlvmType() -> llvm::StructType*;
  [[nodiscard]] auto isAsyncTaskType(lesma::Type* type) const -> bool;
  [[nodiscard]] auto getAsyncTaskPayloadType(lesma::Type* type) const -> lesma::Type*;
  [[nodiscard]] auto getOrCreateAsyncPromiseLlvmType(lesma::Type* payloadType)
      -> llvm::StructType*;
  [[nodiscard]] auto saveAsyncCodegenState() const -> AsyncCodegenStateSnapshot;
  auto restoreAsyncCodegenState(const AsyncCodegenStateSnapshot& state) -> void;
  auto resetAsyncCodegenState() -> void;
  auto initializeAsyncCoroutine(llvm::Function* f, lesma::Type* callableReturnType,
                                llvm::SMRange span, llvm::StringRef callableKind)
      -> AsyncCoroutineBlocks;
  auto emitCurrentAsyncReadyFlag(lesma::Type* payloadType, bool isReady, llvm::StringRef name) -> void;
  auto emitCurrentAsyncReturnValue(llvm::SMRange span, std::unique_ptr<lesma::Value>& value,
                                   llvm::StringRef missingValueMessage) -> void;
  auto emitImplicitAsyncVoidCompletion() -> void;
  auto finalizeAsyncCoroutine(llvm::Function* f, const AsyncCoroutineBlocks& blocks) -> void;
  auto getCoroutineIntrinsic(llvm::Intrinsic::ID id,
                             llvm::ArrayRef<llvm::Type*> overloadTypes = {}) -> llvm::FunctionCallee;
  auto emitAsyncTaskPromisePointer(llvm::Value* taskHandle, lesma::Type* taskType, bool fromCaller)
      -> llvm::Value*;
  auto emitDrainAsyncTask(llvm::SMRange span, std::unique_ptr<lesma::Value> taskValue,
                          bool destroyTask) -> std::unique_ptr<lesma::Value>;
  auto
  specializeClass(const Class* node, const std::vector<lesma::Type*>& constructorArgTypes,
                  const std::vector<lesma::Type*>& explicitTypeArgs = {},
                  const std::unordered_map<std::string, lesma::Type*>* prebuiltClassEnv = nullptr)
      -> lesma::Value*;
  auto emitClassMonomorph(lesma::Type* specialized, const Class* templateAst) -> lesma::Value*;
  auto emitEnumMonomorph(lesma::Type* specialized, const Enum* templateAst) -> lesma::Value*;
  auto declareOrDefineSyntheticEnumMethod(lesma::Type* enumType, const Enum* astNode,
                                          EnumVariant* variant, unsigned variantIndex,
                                          SyntheticEnumMethodKind kind) -> lesma::Value*;
  auto defineSyntheticEnumMethod(const SyntheticEnumMethodBody& body) -> void;
  [[nodiscard]] auto specializedNominalEnvFor(lesma::Type* nominalTy)
      -> const std::unordered_map<std::string, lesma::Type*>*;
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
  auto getOrCreateArcHeaderType() -> llvm::StructType*;
  auto emitArcAlloc(llvm::Value* payloadSize, llvm::Function* destroyFn, const llvm::Twine& name)
      -> llvm::Value*;
  auto emitArcRetain(llvm::Value* payloadPtr) -> void;
  auto emitArcRelease(llvm::Value* payloadPtr) -> void;
  auto emitArcReleaseNullable(llvm::Value* payloadPtr) -> void;
  auto emitArcFreePayload(llvm::Value* payloadPtr) -> void;
  auto emitRetainLoadedValue(lesma::Type* type, llvm::Value* value,
                             bool storesFuncValuePair = false) -> void;
  auto emitReleaseLoadedValue(lesma::Type* type, llvm::Value* value,
                              bool storesFuncValuePair = false) -> void;
  auto emitReleaseTrackedSlot(const ArcTrackedSlot& tracked) -> void;
  auto pushArcOwnedSlotFrame() -> void;
  auto popArcOwnedSlotFrame(bool emitCleanup) -> void;
  auto registerArcOwnedSlot(llvm::Value* slot, lesma::Type* type, bool storesFuncValuePair = false)
      -> void;
  auto emitReleaseCurrentArcOwnedSlots() -> void;
  auto registerModuleArcRoot(llvm::GlobalVariable* slot, lesma::Type* type,
                             const std::string& debugName, bool storesFuncValuePair = false)
      -> void;
  auto emitReleaseRegisteredModuleArcRoots() -> void;
  auto getOrCreateArcStorageRetainFunction(lesma::Type* type) -> llvm::Function*;
  auto getOrCreateArcStorageReleaseFunction(lesma::Type* type) -> llvm::Function*;
  auto getOrCreateArcPayloadDestroyFunction(lesma::Type* type) -> llvm::Function*;
  auto getOrCreateArcDestroyFunction(lesma::Type* type) -> llvm::Function*;
  auto getOrCreateArcClosureDestroyFunction(const std::string& key, llvm::StructType* envStructTy,
                                            const std::vector<lesma::Type*>& captureTypes)
      -> llvm::Function*;
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
  auto emitRuntimeStderrMessage(std::string_view message) -> void;
  auto emitExit(int code) -> void;
  auto getOrCreateArcDebugDeltaFunction() -> llvm::Function*;
  auto getOrCreateArcDebugReportFunction() -> llvm::Function*;
  auto getOrCreateArcDebugCleanupBeginFunction() -> llvm::Function*;
  auto getOrCreateArcDebugCleanupStepFunction() -> llvm::Function*;
  auto emitArcDebugDelta(std::int64_t delta, llvm::Value* payloadPtr,
                         std::string_view traceMessagePrefix) -> void;
  auto emitArcDebugTraceCounts(std::string_view traceMessagePrefix, llvm::Value* payloadPtr,
                               llvm::Value* before, llvm::Value* after) -> void;
  auto emitArcDebugValidateRefcount(llvm::Value* refCount, std::string_view message) -> void;
  auto emitArcDebugTraceModuleRoot(const ModuleArcTrackedRoot& tracked) -> void;
  auto getOrCreateModuleCleanupFunction() -> llvm::Function*;
  auto emitCallPendingJitModuleFinis() -> void;
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
  auto pushGenericTypeFallback(const std::unordered_map<std::string, lesma::Type*>* env) -> void;
  auto popGenericTypeFallback() -> void;
  [[nodiscard]] auto lookupGenericTypeFallback(const std::string& name) const -> lesma::Type*;
  /** LLVM integer tag type for \c TY_UNION (first struct field); requires \p unionTy to be a union.
   */
  [[nodiscard]] auto getOrCreateUnionTagLlvmType(lesma::Type* unionTy) -> llvm::Type*;
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
  [[nodiscard]] auto specializedTraitExistentialEnvFor(lesma::Type* existentialTy)
      -> const std::unordered_map<std::string, lesma::Type*>*;
  [[nodiscard]] auto lookupClassVtableGlobal(lesma::Type* classTy) -> llvm::GlobalVariable*;

  auto collectTraitMetadataFromAst() -> void;
  auto mergeImportedTraitMetadata(ImportedSpecializationState const& imported) -> void;
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
  [[nodiscard]] auto isTypeFullyConcrete(lesma::Type* t) const -> bool;
  /** Substitute generic parameters (and nested specialized classes) for lowering a template field
   * or superclass type. */
  auto substituteTypeForSpecializationEnv(lesma::Type* t,
                                          const std::unordered_map<std::string, lesma::Type*>& env)
      -> lesma::Type*;
  /** Peel singleton TY_UNION chains and rebuild ptr/array when the element changes (types from
   *  Typechecker::substituteInType before singleton collapse, or imports) so emitClassMonomorph
   *  matches substituteTypeForSpecializationEnv lowering. */
  auto typeWithSingletonUnionsCollapsed(lesma::Type* t) -> lesma::Type*;

private:
  auto substituteTypeForSpecializationEnv(lesma::Type* t,
                                          const std::unordered_map<std::string, lesma::Type*>& env,
                                          std::set<lesma::Type const*>& active) -> lesma::Type*;
  auto tryReuseActiveSpecializedNominalType(
      lesma::Type* t, const std::unordered_map<std::string, lesma::Type*>& env,
      std::set<lesma::Type const*>& active) -> lesma::Type*;
  /** True when \p type can be passed to \c MangleUtils::getTypeMangledName for ARC storage helpers.
   */
  [[nodiscard]] static auto isLesmaTypeReadyForArcTypeMangling(lesma::Type* type) -> bool;
  [[nodiscard]] static auto isLesmaPtrToClass(lesma::Type* t) -> bool;
  [[nodiscard]] auto genericMethodMapForSelf(
      std::unordered_map<std::string, std::unordered_map<std::string, const FuncDecl*>>&
          genericMethods,
      lesma::Value* selfSymbol) -> std::unordered_map<std::string, const FuncDecl*>*;
  /** Map a typecheck body-scope pointer to this specialized emission's clone when active. */
  [[nodiscard]] auto remapCodegenTemplateBodyScope(lesma::SymbolTable* t) const -> lesma::SymbolTable*;
  /** Stack/global slot LLVM type for a local or exported variable (class-as-ptr ABI, func pair). */
  [[nodiscard]] auto llvmStorageTypeForVarSlot(lesma::Type* storedType, lesma::Value* existing)
      -> llvm::Type*;
  /** Ptr-to-class direct store vs cast-then-store (shared by globals and simple locals). */
  auto emitSimpleClassPtrOrCastStore(llvm::SMRange span, llvm::Value* destPtr,
                                     std::unique_ptr<lesma::Value>& valueResult,
                                     lesma::Type* storedType, bool releasePrevious = false,
                                     bool destStoresFuncValuePair = false) -> llvm::Instruction*;
  /** Initial store when reusing a typecheck symbol (generic func pair, ptr-to-class, etc.). */
  auto emitExistingVarSlotInitializerStore(const VarDecl* node, llvm::Value* destPtr,
                                           std::unique_ptr<lesma::Value>& valueResult,
                                           lesma::Type* storedType, const std::string& dbgName,
                                           bool destStoresFuncValuePair = false)
      -> llvm::Instruction*;
  auto emitForEachEnumVariantPayloadWithTagDispatch(
      lesma::Type* enumTy, llvm::Value* enumSlot, llvm::Value* tagVal, std::string_view blockStem,
      const std::function<void(lesma::Type*, llvm::Value*)>& callback) -> void;
  auto emitForEachUnionMemberWithTagDispatch(
      lesma::Type* unionTy, llvm::Value* unionSlot, llvm::Value* tagVal, std::string_view blockStem,
      const std::function<void(lesma::Type*, llvm::Value*)>& callback) -> void;
  [[nodiscard]] auto makeBoolCompareResult(llvm::Value* cmpVal) -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto
  emitPromotedArithmetic(llvm::SMRange span, TokenType op, std::unique_ptr<lesma::Value>& left,
                         std::unique_ptr<lesma::Value>& right, lesma::Type* finalType)
      -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto emitPromotedBitwise(llvm::SMRange span, TokenType op,
                                         std::unique_ptr<lesma::Value>& left,
                                         std::unique_ptr<lesma::Value>& right,
                                         lesma::Type* finalType) -> std::unique_ptr<lesma::Value>;
  [[nodiscard]] auto emitPowerOperation(llvm::SMRange span, std::unique_ptr<lesma::Value>& left,
                                        std::unique_ptr<lesma::Value>& right,
                                        lesma::Type* finalType) -> std::unique_ptr<lesma::Value>;
  void emitForInLoopIteration(llvm::Function* parentFct, const ForIn* node, SymbolTable* outerScope,
                              SymbolTable* loopBodyScope, llvm::BasicBlock* bLoop,
                              llvm::BasicBlock* bInc,
                              const std::function<void()>& loadElementIntoLoopVar);

  [[nodiscard]] auto classStaticFieldGlobalName(lesma::Type* classTy, const std::string& fieldName,
                                                llvm::SMRange reportSpan) const -> std::string;
  [[nodiscard]] auto llvmStorageTypeForClassStaticField(lesma::Type* fieldTy, Value* fieldSym)
      -> llvm::Type*;
  [[nodiscard]] auto materializeClassStaticFieldGlobalInCurrentModule(lesma::Type* templateClassTy,
                                                                      Field* tf)
      -> llvm::GlobalVariable*;
  [[nodiscard]] auto traitExistentialBaseName(const std::string& displayName) const -> std::string;

  /** Minimum tag bits: ceil(log2(memberCount)), at least 1 (memberCount must be > 0). */
  [[nodiscard]] static auto unionDiscriminantMinBits(std::size_t memberCount) -> unsigned;
  /** Round up to a whole number of bytes (8, 16, 32, 64); 0 if \p minBits > 64. */
  [[nodiscard]] static auto roundUnionTagToSupportedBitWidth(unsigned minBits) -> unsigned;

  [[nodiscard]] auto cgUnionTryGetIsOpVarSymbol(const IsOp* is, SymbolTable* scope)
      -> lesma::Value*;
  [[nodiscard]] auto cgUnionComplementMemberIndex(lesma::Type* unionTy, lesma::Type* excluded)
      -> std::optional<unsigned>;

  /** Merge per-arm \c Value metadata into the PHI result of a \c match (ARC, function pair,
   * closure).
   */
  static auto mergeMatchPhiArmMetadataIntoResult(std::vector<std::unique_ptr<lesma::Value>> sources,
                                                 lesma::Value* out) -> void;
};
} // namespace lesma