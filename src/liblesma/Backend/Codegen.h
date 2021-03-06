#pragma once

#include "liblesma/AST/ExprVisitor.h"
#include "liblesma/AST/StmtVisitor.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/JIT/LesmaJIT.h"
#include "liblesma/Symbol/SymbolTable.h"
#include <clang/Driver/Driver.h>
#include <filesystem>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <utility>

namespace lesma {
    class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Codegen final : public ExprVisitor<llvm::Value *, llvm::Type *>, public StmtVisitor<void> {
        std::unique_ptr<LLVMContext> TheContext;
        std::unique_ptr<Module> TheModule;
        std::unique_ptr<IRBuilder<>> Builder;
        ExitOnError ExitOnErr;

        std::unique_ptr<LesmaJIT> TheJIT;
        std::unique_ptr<llvm::TargetMachine> TargetMachine;
        std::unique_ptr<Parser> Parser_;
        std::shared_ptr<SourceMgr> SourceManager;
        SymbolTable *Scope;
        std::string filename;
        std::string alias;

        std::stack<llvm::BasicBlock *> breakBlocks;
        std::stack<llvm::BasicBlock *> continueBlocks;
        std::stack<std::vector<Statement *>> deferStack;

        std::vector<std::string> ObjectFiles;
        std::vector<std::string> ImportedModules;
        std::vector<std::tuple<Function *, FuncDecl *, SymbolTableEntry *>> Prototypes;
        llvm::Function *TopLevelFunc;
        SymbolTableEntry *selfSymbol = nullptr;
        unsigned int bufferId;
        bool isBreak = false;
        bool isReturn = false;
        bool isAssignment = false;
        bool isJIT = false;
        bool isMain = true;

    public:
        Codegen(std::unique_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main, std::string alias = "");

        void Dump();
        void Run();
        int JIT();
        void WriteToObjectFile(const std::string &output);
        void LinkObjectFile(const std::string &obj_filename);
        void Optimize(OptimizationLevel opt);

    protected:
        std::unique_ptr<llvm::TargetMachine> InitializeTargetMachine();
        llvm::Function *InitializeTopLevel();
        void CompileModule(llvm::SMRange span, const std::string &filepath, bool isStd, std::string alias, bool importAll, bool importToScope, std::vector<std::pair<std::string, std::string>> imported_names);

        void visit(Statement *node) override;
        void visit(Compound *node) override;
        void visit(VarDecl *node) override;
        void visit(If *node) override;
        void visit(While *node) override;
        void visit(Import *node) override;
        void visit(Enum *node) override;
        void visit(Class *node) override;
        void visit(FuncDecl *node) override;
        void visit(ExternFuncDecl *node) override;
        void visit(Assignment *node) override;
        void visit(Break *node) override;
        void visit(Continue *node) override;
        void visit(Return *node) override;
        void visit(Defer *node) override;
        void visit(ExpressionStatement *node) override;
        llvm::Type *visit(lesma::Type *node) override;
        llvm::Value *visit(Expression *node) override;
        llvm::Value *visit(FuncCall *node) override;
        llvm::Value *visit(BinaryOp *node) override;
        llvm::Value *visit(DotOp *node) override;
        llvm::Value *visit(CastOp *node) override;
        llvm::Value *visit(IsOp *node) override;
        llvm::Value *visit(UnaryOp *node) override;
        llvm::Value *visit(Literal *node) override;
        llvm::Value *visit(Else *node) override;

        // TODO: Helper functions, move them out somewhere
        static SymbolType *getType(llvm::Type *type);
        llvm::Value *Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type);
        llvm::Value *Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type, bool isStore);
        static llvm::Type *GetExtendedType(llvm::Type *left, llvm::Type *right);
        bool isMethod(const std::string &mangled_name);
        std::string getMangledName(llvm::SMRange span, std::string func_name, const std::vector<llvm::Type *> &paramTypes, bool isMethod = false, std::string alias = "");
        bool isMangled(std::string name);
        std::string getDemangledName(const std::string &mangled_name);
        std::string getTypeMangledName(llvm::SMRange span, llvm::Type *type);
        llvm::Value *genFuncCall(FuncCall *node, const std::vector<llvm::Value *> &extra_params);
        static int FindIndexInFields(SymbolType *_struct, const std::string &field);
        void defineFunction(Function *F, FuncDecl *node, SymbolTableEntry *clsSymbol);
    };
}// namespace lesma