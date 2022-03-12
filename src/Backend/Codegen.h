#pragma once

#include "AST/ExprVisitor.h"
#include "AST/StmtVisitor.h"
#include "Frontend/Parser.h"
#include "JIT/LesmaJIT.h"
#include "Symbol/SymbolTable.h"
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
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

namespace lesma {
    class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Codegen : public ExprVisitor<llvm::Value *, llvm::Type *>, public StmtVisitor<void> {
        std::unique_ptr<LLVMContext> TheContext;
        std::unique_ptr<Module> TheModule;
        std::unique_ptr<IRBuilder<>> Builder;
        //        std::unique_ptr<PassManagerBuilder> PMB;
        //        std::unique_ptr<ModulePassManager> MPM;
        //        std::unique_ptr<FunctionPassManager> FPM;
        ExitOnError ExitOnErr;

        std::unique_ptr<LesmaJIT> TheJIT;
        std::unique_ptr<llvm::TargetMachine> TargetMachine;
        std::unique_ptr<Parser> Parser_;
        SymbolTable *Scope;
        std::string filename;

        std::stack<llvm::BasicBlock *> breakBlocks;
        std::stack<llvm::BasicBlock *> continueBlocks;
        std::stack<std::vector<llvm::Value *>> deferStack;

        std::vector<std::string> ObjectFiles;
        llvm::Function *TopLevelFunc;
        bool isBreak = false;
        bool isReturn = false;
        bool isJIT = false;
        bool isMain = true;

    public:
        Codegen(std::unique_ptr<Parser> parser, const std::string &filename, bool jit, bool main);

        void Dump();
        void Run();
        int JIT();
        void WriteToObjectFile(const std::string &output);
        void LinkObjectFile(const std::string &obj_filename);
        void Optimize(llvm::PassBuilder::OptimizationLevel opt);
        ThreadSafeModule getModule() { return {std::move(TheModule), std::move(TheContext)}; };

    protected:
        std::unique_ptr<llvm::TargetMachine> InitializeTargetMachine();
        llvm::Function *InitializeTopLevel();
        void CompileModule(Span span, const std::string &filepath);

        void visit(Statement *node) override;
        void visit(Compound *node) override;
        void visit(VarDecl *node) override;
        void visit(If *node) override;
        void visit(While *node) override;
        void visit(Import *node) override;
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
        llvm::Value *visit(CastOp *node) override;
        llvm::Value *visit(UnaryOp *node) override;
        llvm::Value *visit(Literal *node) override;
        llvm::Value *visit(Else *node) override;

        // TODO: Helper functions, move them out somewhere
        llvm::Value *Cast(Span span, llvm::Value *val, llvm::Type *type);
        llvm::Type *GetExtendedType(llvm::Type *left, llvm::Type *right);
        std::string getMangledName(Span span, std::string func_name, const std::vector<llvm::Type *> &paramTypes);
        std::string getTypeMangledName(Span span, llvm::Type *type);
    };
}// namespace lesma