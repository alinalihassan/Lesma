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

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
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

class Codegen final : public ASTVisitor {
  std::shared_ptr<ThreadSafeContext> theContext;
  std::unique_ptr<Module> theModule;
  std::unique_ptr<IRBuilder<>> builder;

  std::unique_ptr<LLJIT> theJit;
  std::unique_ptr<llvm::TargetMachine> targetMachine;
  std::shared_ptr<Parser> parser;
  std::shared_ptr<SourceMgr> sourceManager;
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
  std::vector<std::unique_ptr<Codegen>> importedCodegens; // Keep imported module codegens alive so
                                                          // Class* in symbols stay valid
  // deque so push_back never invalidates Type* pointers stored in scope (from
  // typecheck)
  std::deque<std::unique_ptr<lesma::Type>> typeCache;
  std::vector<std::tuple<lesma::Value*, const FuncDecl*, Value*>> prototypes;
  std::unordered_map<std::string, const FuncDecl*> genericFunctions;
  std::unordered_map<std::string, std::unordered_map<std::string, const FuncDecl*>> genericMethods;
  std::unordered_map<std::string, const Class*> genericClasses;
  std::unordered_map<std::string, lesma::Type*> currentGenericTypes;
  std::unordered_map<std::string, lesma::Value*> specializedFunctions;
  std::unordered_map<std::string, lesma::Value*> specializedClasses;
  std::unordered_map<lesma::Value*, std::unordered_map<std::string, lesma::Type*>>
      specializationEnvs;
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

public:
  /** Codegen lowers a module using the semantic scope and type cache produced
   * by the typechecker. */
  Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr,
          const std::string& filename, std::vector<std::string> imports, bool jit, bool main,
          std::string alias = "", const std::shared_ptr<ThreadSafeContext>& = nullptr,
          std::shared_ptr<std::vector<std::string>> sharedModules = nullptr,
          std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>> sharedScopes = nullptr,
          std::unique_ptr<SymbolTable> preScope = nullptr,
          std::vector<std::unique_ptr<lesma::Type>> preTypeCache = {});
  ~Codegen() override = default;

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

protected:
  auto initializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine>;
  auto initializeModule() -> std::unique_ptr<Module>;
  auto initializeJit() -> std::unique_ptr<LLJIT>;
  auto initializeTopLevel() -> llvm::Function*;

  auto linkObjectFileWithClang(const std::string& objFilename) -> void;
  auto linkObjectFileWithLld(const std::string& objFilename) -> void;

  auto compileModule(llvm::SMRange span, const std::string& filepath, bool isStd,
                     const std::string& alias, bool importAll, bool importToScope,
                     const std::vector<ImportedNameBinding>& importedNames) -> void;
  auto getExportsFromFile(const std::string& filepath, bool isStd, const std::string& mainFilePath)
      -> std::vector<std::string>;
  auto typecheckModule(const Compound* ast, const std::string& modulePath)
      -> std::pair<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>>;
  auto insertImportAlias(const std::string& moduleAlias, bool importToScope) -> void;
  auto exposeImportedSymbols(llvm::SMRange span, SymbolTable* importedScope, bool importAll,
                             bool importToScope, const std::vector<ImportedNameBinding>& importedNames)
      -> void;

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

  auto cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
      -> std::unique_ptr<lesma::Value>;

  auto getMangledName(llvm::SMRange span, std::string funcName,
                      const std::vector<lesma::Type*>& paramTypes, bool isMethod = false,
                      std::string alias = "") -> std::string;

  auto genFuncCall(const FuncCall* node, const std::vector<lesma::Value*>& extraParams)
      -> std::unique_ptr<lesma::Value>;
  auto defineFunction(lesma::Value* value, const FuncDecl* node, Value* clsSymbol) -> void;
  auto specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                          const std::vector<std::string>& genericNames,
                          const std::vector<lesma::Type*>& explicitTypeArgs = {}) -> lesma::Value*;
  auto specializeClass(const Class* node, const std::vector<lesma::Type*>& constructorArgTypes,
                       const std::vector<lesma::Type*>& explicitTypeArgs = {}) -> lesma::Value*;

  auto emitCompoundAssign(llvm::SMRange span, TokenType op, lesma::Value* lhs, lesma::Value* value)
      -> void;
  auto symbolUsesDirectLlvmValue(const lesma::Value* symbol) const -> bool;
  auto materializeSymbolValue(lesma::Value* symbol) -> std::unique_ptr<lesma::Value>;

  // Cache a type to keep it alive - returns raw pointer to the cached type
  auto cacheType(std::unique_ptr<lesma::Type> type) -> lesma::Type* {
    typeCache.push_back(std::move(type));
    return typeCache.back().get();
  }

  /** Ensure \p type has an LLVM type (fill in when from typechecker). */
  auto getOrCreateLlvmType(lesma::Type* type) -> llvm::Type*;

  /** Populate \p env by structurally matching declared (TypeExpr) vs actual
   * (lesma::Type), binding generic names from \p genericNameSet. */
  static void bindGenericsFromTypePair(const TypeExpr* declared, lesma::Type* actual,
                                       const std::unordered_set<std::string>& genericNameSet,
                                       std::unordered_map<std::string, lesma::Type*>& env);
};
} // namespace lesma