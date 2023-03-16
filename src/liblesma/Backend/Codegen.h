#pragma once

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/JIT/LesmaJIT.h"
#include "liblesma/Symbol/SymbolTable.h"
#include <clang/Driver/Driver.h>
#include <filesystem>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
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
#include <regex>
#include <utility>

namespace lesma {
    class CodegenError : public LesmaErrorWithExitCode<EX_DATAERR> {
    public:
        using LesmaErrorWithExitCode<EX_DATAERR>::LesmaErrorWithExitCode;
    };

    class Codegen final : public ASTVisitor {
        std::shared_ptr<ThreadSafeContext> TheContext;
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
        llvm::Value *result;
        llvm::Type *result_type;

        std::stack<llvm::BasicBlock *> breakBlocks;
        std::stack<llvm::BasicBlock *> continueBlocks;
        std::stack<std::vector<Statement *>> deferStack;

        std::vector<std::string> ObjectFiles;
        std::vector<std::string> ImportedModules;
        std::vector<std::tuple<Function *, const FuncDecl *, SymbolTableEntry *>> Prototypes;
        llvm::Function *TopLevelFunc;
        SymbolTableEntry *selfSymbol = nullptr;
        bool isBreak = false;
        bool isReturn = false;
        bool isAssignment = false;
        bool isJIT = false;
        bool isMain = true;

    public:
        Codegen(std::unique_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main, std::string alias = "", const std::shared_ptr<ThreadSafeContext> & = nullptr);
        ~Codegen() {
            delete selfSymbol;
            delete Scope;
        }

        void Dump();
        void Run();
        int JIT();
        void WriteToObjectFile(const std::string &output);
        void LinkObjectFile(const std::string &obj_filename);
        void Optimize(OptimizationLevel opt);

    protected:
        std::unique_ptr<llvm::TargetMachine> InitializeTargetMachine();
        llvm::Function *InitializeTopLevel();
        void CompileModule(llvm::SMRange span, const std::string &filepath, bool isStd, const std::string &alias, bool importAll, bool importToScope, const std::vector<std::pair<std::string, std::string>> &imported_names);

        void visit(const Statement *node) override;
        void visit(const Compound *node) override;
        void visit(const VarDecl *node) override;
        void visit(const If *node) override;
        void visit(const While *node) override;
        void visit(const Import *node) override;
        void visit(const Enum *node) override;
        void visit(const Class *node) override;
        void visit(const FuncDecl *node) override;
        void visit(const ExternFuncDecl *node) override;
        void visit(const Assignment *node) override;
        void visit(const Break *node) override;
        void visit(const Continue *node) override;
        void visit(const Return *node) override;
        void visit(const Defer *node) override;
        void visit(const ExpressionStatement *node) override;

        void visit(const Expression *node) override;
        void visit(const FuncCall *node) override;
        void visit(const BinaryOp *node) override;
        void visit(const DotOp *node) override;
        void visit(const CastOp *node) override;
        void visit(const IsOp *node) override;
        void visit(const UnaryOp *node) override;
        void visit(const Literal *node) override;
        void visit(const Else *node) override;

        void visit(const lesma::Type *node) override;

        // TODO: Helper functions, move them out somewhere
        static SymbolType *getType(llvm::Type *type);
        llvm::Value *Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type);
        llvm::Value *Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type, bool isStore);
        static llvm::Type *GetExtendedType(llvm::Type *left, llvm::Type *right);
        static bool isMethod(const std::string &mangled_name);
        std::string getMangledName(llvm::SMRange span, std::string func_name, const std::vector<llvm::Type *> &paramTypes, bool isMethod = false, std::string alias = "");
        [[maybe_unused]] static bool isMangled(std::string name);
        static std::string getDemangledName(const std::string &mangled_name);
        std::string getTypeMangledName(llvm::SMRange span, llvm::Type *type);
        llvm::Value *genFuncCall(const FuncCall *node, const std::vector<llvm::Value *> &extra_params);
        static int FindIndexInFields(SymbolType *_struct, const std::string &field);
        void defineFunction(Function *F, const FuncDecl *node, SymbolTableEntry *clsSymbol);
    };
}// namespace lesma