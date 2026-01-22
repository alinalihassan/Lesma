#pragma once

#include <memory>
#include <stack>
#include <string>
#include <tuple>
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
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

using namespace llvm;
using namespace llvm::orc;

namespace lesma {
class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
public:
  using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
};

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
  std::vector<std::string> importedModules;
  std::vector<std::unique_ptr<SymbolTable>>
      importedScopes; // Keep imported scopes alive
  std::vector<std::unique_ptr<lesma::Type>>
      typeCache; // Cache for primitive types to prevent dangling pointers
  std::vector<std::tuple<lesma::Value*, const FuncDecl*, Value*>> prototypes;
  llvm::Function* topLevelFunc;
  MainFnTy* mainFuncAddress = nullptr;
  Value* selfSymbol = nullptr;
  bool isBreak = false;
  bool isReturn = false;
  bool isAssignment = false;
  bool isJit = false;
  bool isMain = true;

public:
  Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr,
          const std::string& filename, std::vector<std::string> imports,
          bool jit, bool main, std::string alias = "",
          const std::shared_ptr<ThreadSafeContext>& = nullptr);
  ~Codegen() override = default;

  Codegen(const Codegen&) = delete;
  auto operator=(const Codegen&) -> Codegen& = delete;
  Codegen(Codegen&&) = delete;
  auto operator=(Codegen&&) -> Codegen& = delete;

  auto Dump() -> void;
  auto Run() -> void;
  auto PrepareJit() -> void;
  auto ExecuteJit() -> int;
  auto WriteToObjectFile(const std::string& output) -> void;
  auto LinkObjectFile(const std::string& objFilename) -> void;
  auto Optimize(OptimizationLevel opt) -> void;

protected:
  auto InitializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine>;
  auto InitializeModule() -> std::unique_ptr<Module>;
  auto InitializeJit() -> std::unique_ptr<LLJIT>;
  auto InitializeTopLevel() -> llvm::Function*;

  [[maybe_unused]] auto LinkObjectFileWithClang(const std::string& objFilename)
      -> void;
  [[maybe_unused]] auto LinkObjectFileWithLld(const std::string& objFilename)
      -> void;

  auto CompileModule(
      llvm::SMRange span, const std::string& filepath, bool isStd,
      const std::string& alias, bool importAll, bool importToScope,
      const std::vector<std::pair<std::string, std::string>>& importedNames)
      -> void;

  auto Visit(const Statement* node) -> void override;
  auto Visit(const Compound* node) -> void override;
  auto Visit(const VarDecl* node) -> void override;
  auto Visit(const If* node) -> void override;
  auto Visit(const While* node) -> void override;
  auto Visit(const Import* node) -> void override;
  auto Visit(const Enum* node) -> void override;
  auto Visit(const Class* node) -> void override;
  auto Visit(const FuncDecl* node) -> void override;
  auto Visit(const ExternFuncDecl* node) -> void override;
  auto Visit(const Assignment* node) -> void override;
  auto Visit(const Break* node) -> void override;
  auto Visit(const Continue* node) -> void override;
  auto Visit(const Return* node) -> void override;
  auto Visit(const Defer* node) -> void override;
  auto Visit(const ExpressionStatement* node) -> void override;

  auto Visit(const Expression* node) -> void override;
  auto Visit(const FuncCall* node) -> void override;
  auto Visit(const BinaryOp* node) -> void override;
  auto Visit(const DotOp* node) -> void override;
  auto Visit(const CastOp* node) -> void override;
  auto Visit(const IsOp* node) -> void override;
  auto Visit(const UnaryOp* node) -> void override;
  auto Visit(const Literal* node) -> void override;
  auto Visit(const Else* node) -> void override;

  auto Visit(const TypeExpr* node) -> void override;

  // TODO: Helper functions, move them out somewhere
  // Type related helper functions
  auto Cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
      -> std::unique_ptr<lesma::Value>;
  static auto GetExtendedType(lesma::Type* left, lesma::Type* right)
      -> lesma::Type*;

  // Name mangling functions and such
  static auto IsMethod(const std::string& mangledName) -> bool;
  auto GetMangledName(llvm::SMRange span, std::string funcName,
                      const std::vector<lesma::Type*>& paramTypes,
                      bool isMethod = false, std::string alias = "")
      -> std::string;
  [[maybe_unused]] static auto IsMangled(std::string name) -> bool;
  static auto GetDemangledName(const std::string& mangledName) -> std::string;
  auto GetTypeMangledName(llvm::SMRange span, lesma::Type* type) -> std::string;

  // Other
  auto GenFuncCall(const FuncCall* node,
                   const std::vector<lesma::Value*>& extraParams)
      -> std::unique_ptr<lesma::Value>;
  static auto FindIndexInFields(Type* structType, const std::string& field)
      -> int;
  static auto FindTypeInFields(Type* structType, const std::string& field)
      -> lesma::Type*;
  auto DefineFunction(lesma::Value* value, const FuncDecl* node,
                      Value* clsSymbol) -> void;

  // Cache a type to keep it alive - returns raw pointer to the cached type
  auto CacheType(std::unique_ptr<lesma::Type> type) -> lesma::Type* {
    typeCache.push_back(std::move(type));
    return typeCache.back().get();
  }
};
} // namespace lesma