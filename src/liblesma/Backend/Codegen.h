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
    std::shared_ptr<ThreadSafeContext> TheContext_;
    std::unique_ptr<Module> TheModule_;
    std::unique_ptr<IRBuilder<>> Builder_;

    std::unique_ptr<LLJIT> TheJIT_;
    std::unique_ptr<llvm::TargetMachine> TargetMachine_;
    std::shared_ptr<Parser> Parser_;
    std::shared_ptr<SourceMgr> SourceManager_;
    std::unique_ptr<SymbolTable> rootScope_;  // Owns the root scope
    SymbolTable *Scope_;                      // Non-owning navigation pointer
    std::string filename_;
    std::string alias_;
    std::unique_ptr<lesma::Value> result_;

    std::stack<llvm::BasicBlock *> breakBlocks_;
    std::stack<llvm::BasicBlock *> continueBlocks_;
    std::stack<std::vector<Statement *>> deferStack_;
    lesma::Value *currentFunction_ = nullptr;

    std::vector<std::string> ObjectFiles_;
    std::vector<std::string> ImportedModules_;
    std::vector<std::unique_ptr<SymbolTable>> ImportedScopes_;  // Keep imported scopes alive
    std::vector<std::unique_ptr<lesma::Type>> typeCache_;  // Cache for primitive types to prevent dangling pointers
    std::vector<std::tuple<lesma::Value *, const FuncDecl *, Value *>> Prototypes_;
    llvm::Function *TopLevelFunc_;
    MainFnTy *mainFuncAddress_ = nullptr;
    Value *selfSymbol_ = nullptr;
    bool isBreak_ = false;
    bool isReturn_ = false;
    bool isAssignment_ = false;
    bool isJIT_ = false;
    bool isMain_ = true;

public:
    Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename,
            std::vector<std::string> imports, bool jit, bool main, std::string alias = "",
            const std::shared_ptr<ThreadSafeContext> & = nullptr);
    ~Codegen() override = default;

    Codegen(const Codegen &) = delete;
    auto operator=(const Codegen &) -> Codegen & = delete;
    Codegen(Codegen &&) = delete;
    auto operator=(Codegen &&) -> Codegen & = delete;

    auto dump() -> void;
    auto run() -> void;
    auto prepareJit() -> void;
    auto executeJit() -> int;
    auto writeToObjectFile(const std::string &output) -> void;
    auto linkObjectFile(const std::string &obj_filename) -> void;
    auto optimize(OptimizationLevel opt) -> void;

protected:
    auto initializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine>;
    auto initializeModule() -> std::unique_ptr<Module>;
    auto initializeJit() -> std::unique_ptr<LLJIT>;
    auto initializeTopLevel() -> llvm::Function *;

    [[maybe_unused]] auto linkObjectFileWithClang(const std::string &objFilename) -> void;
    [[maybe_unused]] auto linkObjectFileWithLld(const std::string &obj_filename) -> void;

    auto compileModule(llvm::SMRange span, const std::string &filepath, bool isStd, const std::string &alias,
                       bool importAll, bool importToScope,
                       const std::vector<std::pair<std::string, std::string>> &imported_names) -> void;

    auto visit(const Statement *node) -> void override;
    auto visit(const Compound *node) -> void override;
    auto visit(const VarDecl *node) -> void override;
    auto visit(const If *node) -> void override;
    auto visit(const While *node) -> void override;
    auto visit(const Import *node) -> void override;
    auto visit(const Enum *node) -> void override;
    auto visit(const Class *node) -> void override;
    auto visit(const FuncDecl *node) -> void override;
    auto visit(const ExternFuncDecl *node) -> void override;
    auto visit(const Assignment *node) -> void override;
    auto visit(const Break *node) -> void override;
    auto visit(const Continue *node) -> void override;
    auto visit(const Return *node) -> void override;
    auto visit(const Defer *node) -> void override;
    auto visit(const ExpressionStatement *node) -> void override;

    auto visit(const Expression *node) -> void override;
    auto visit(const FuncCall *node) -> void override;
    auto visit(const BinaryOp *node) -> void override;
    auto visit(const DotOp *node) -> void override;
    auto visit(const CastOp *node) -> void override;
    auto visit(const IsOp *node) -> void override;
    auto visit(const UnaryOp *node) -> void override;
    auto visit(const Literal *node) -> void override;
    auto visit(const Else *node) -> void override;

    auto visit(const TypeExpr *node) -> void override;

    // TODO: Helper functions, move them out somewhere
    // Type related helper functions
    auto cast(llvm::SMRange span, lesma::Value *val, lesma::Type *type) -> std::unique_ptr<lesma::Value>;
    static auto getExtendedType(lesma::Type *left, lesma::Type *right) -> lesma::Type *;

    // Name mangling functions and such
    static auto isMethod(const std::string &mangled_name) -> bool;
    auto getMangledName(llvm::SMRange span, std::string func_name, const std::vector<lesma::Type *> &paramTypes,
                        bool isMethod = false, std::string alias = "") -> std::string;
    [[maybe_unused]] static auto isMangled(std::string name) -> bool;
    static auto getDemangledName(const std::string &mangled_name) -> std::string;
    auto getTypeMangledName(llvm::SMRange span, lesma::Type *type) -> std::string;

    // Other
    auto genFuncCall(const FuncCall *node, const std::vector<lesma::Value *> &extra_params)
        -> std::unique_ptr<lesma::Value>;
    static auto findIndexInFields(Type *_struct, const std::string &field) -> int;
    static auto findTypeInFields(Type *_struct, const std::string &field) -> lesma::Type *;
    auto defineFunction(lesma::Value *value, const FuncDecl *node, Value *clsSymbol) -> void;

    // Cache a type to keep it alive - returns raw pointer to the cached type
    auto cacheType(std::unique_ptr<lesma::Type> type) -> lesma::Type * {
        typeCache_.push_back(std::move(type));
        return typeCache_.back().get();
    }
};
}  // namespace lesma