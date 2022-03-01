#pragma once

#include "Frontend/Parser.h"
#include "LesmaJIT.h"
#include "Symbol/SymbolTable.h"
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

namespace lesma {
    class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Codegen {
        std::unique_ptr<LLVMContext> TheContext;
        std::unique_ptr<Module> TheModule;
        std::unique_ptr<IRBuilder<>> Builder;
        //        std::unique_ptr<PassManagerBuilder> PMB;
        //        std::unique_ptr<ModulePassManager> MPM;
        //        std::unique_ptr<FunctionPassManager> FPM;
        ExitOnError ExitOnErr;

        std::unique_ptr<LesmaJIT> TheJIT;
        TargetMachine *TargetMachine;
        std::unique_ptr<Parser> Parser_;
        SymbolTable *Scope;
        std::string filename;

        std::stack<llvm::BasicBlock *> breakBlocks;
        std::stack<llvm::BasicBlock *> continueBlocks;
        std::stack<std::vector<llvm::Value *>> deferStack;
        llvm::Function *TopLevelFunc;
        bool isBreak = false;

    public:
        std::vector<ThreadSafeModule> Modules;
        Codegen(std::unique_ptr<Parser> parser, const std::string &filename);

        void Dump();
        void Run();
        void Compile(const std::string &output);
        void Optimize(llvm::PassBuilder::OptimizationLevel opt);
        int JIT(std::vector<ThreadSafeModule> modules);
        ThreadSafeModule getModule() { return {std::move(TheModule), std::move(TheContext)}; };

    protected:
        llvm::TargetMachine *InitializeTargetMachine();
        llvm::Function *InitializeTopLevel();
        void CompileModule(const std::string &filepath);

        llvm::Value *Visit(Statement *node);
        llvm::Value *Visit(Expression *node);
        llvm::Value *Visit(Program *node);
        llvm::Value *Visit(Compound *node);
        llvm::Value *Visit(VarDecl *node);
        llvm::Value *Visit(If *node);
        llvm::Value *Visit(While *node);
        llvm::Value *Visit(Import *node);
        llvm::Value *Visit(FuncDecl *node);
        llvm::Value *Visit(ExternFuncDecl *node);
        llvm::Value *Visit(Assignment *node);
        llvm::Value *Visit(Break *node);
        llvm::Value *Visit(Continue *node);
        llvm::Value *Visit(Return *node);
        llvm::Value *Visit(Defer *node);
        llvm::Value *Visit(ExpressionStatement *node);
        llvm::Type *Visit(lesma::Type *node);
        llvm::Value *Visit(FuncCall *node);
        llvm::Value *Visit(BinaryOp *node);
        llvm::Value *Visit(CastOp *node);
        llvm::Value *Visit(UnaryOp *node);
        llvm::Value *Visit(Literal *node);
        llvm::Value *Visit(Else *node);

        // TODO: Helper functions, move them out somewhere
        llvm::Value *Cast(llvm::Value *val, llvm::Type *type);
        llvm::Type *GetExtendedType(llvm::Type *left, llvm::Type *right);
        std::string getMangledName(std::string func_name, const std::vector<llvm::Type *> &paramTypes);
        std::string getTypeMangledName(llvm::Type *type);
    };
}// namespace lesma