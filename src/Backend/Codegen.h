#pragma once

#include "LesmaJIT.h"
#include "Frontend/Parser.h"
#include "Symbol/SymbolTable.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Host.h>

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
        TargetMachine* TargetMachine;
        std::unique_ptr<Parser> Parser_;
        SymbolTable* Scope;
        std::string filename;
    public:
        Codegen(std::unique_ptr<Parser> parser, const std::string& filename);

        void Dump();
        void Run();
        void Compile(const std::string& output);
        int JIT();

    protected:
        llvm::TargetMachine *InitializeTargetMachine();

        void InitializePasses();

        llvm::Value *Visit(Statement *node);
        llvm::Value *Visit(Expression *node);
        llvm::Value *Visit(Program *node);
        llvm::Value *Visit(Compound *node);
        llvm::Value *Visit(VarDecl *node);
        llvm::Value *Visit(If *node);
        llvm::Value *Visit(While *node);
        llvm::Value *Visit(FuncDecl *node);
        llvm::Value *Visit(ExternFuncDecl *node);
        llvm::Value *Visit(Assignment *node);
        llvm::Value *Visit(Break *node);
        llvm::Value *Visit(Continue *node);
        llvm::Value *Visit(Return *node);
        llvm::Value *Visit(ExpressionStatement *node);
        llvm::Value *Visit(Var *node);
        llvm::Type  *Visit(lesma::Type *node);
        llvm::Value *Visit(FuncCall *node);
        llvm::Value *Visit(BinaryOp *node);
        llvm::Value *Visit(CastOp *node);
        llvm::Value *Visit(UnaryOp *node);
        llvm::Value *Visit(Literal *node);
        llvm::Value *Visit(Else *node);

        llvm::Value *cast(llvm::Value* val, llvm::Type* type);
    };
}