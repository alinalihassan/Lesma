#pragma once

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/FileSystemOptions.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <filesystem>
#include <lld/Common/Driver.h>
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
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <regex>
#include <utility>

using namespace llvm;
using namespace llvm::orc;

namespace lesma {
    using MainFnTy = int();

    class Codegen final : public ASTVisitor {
        std::shared_ptr<ThreadSafeContext> TheContext;
        std::unique_ptr<Module> TheModule;
        std::unique_ptr<IRBuilder<>> Builder;

        std::unique_ptr<LLJIT> TheJIT;
        std::unique_ptr<llvm::TargetMachine> TargetMachine;
        SourceManager &SrcMgr;
        DiagnosticManager &DiagMgr;
        SymbolTable *Scope;
        Compound *tree;
        std::string alias;
        lesma::Value *result = nullptr;

        std::stack<llvm::BasicBlock *> breakBlocks;
        std::stack<llvm::BasicBlock *> continueBlocks;
        std::stack<std::vector<Statement *>> deferStack;
        lesma::Value *currentFunction = nullptr;

        std::vector<std::string> ObjectFiles;
        std::vector<std::string> ImportedModules;
        std::vector<std::tuple<lesma::Value *, const FuncDecl *, Value *>> Prototypes;
        llvm::Function *TopLevelFunc;
        MainFnTy *mainFuncAddress = nullptr;
        Value *selfSymbol = nullptr;
        bool isBreak = false;
        bool isReturn = false;
        bool isAssignment = false;
        bool isJIT = false;
        bool isMain = true;

    public:
        Codegen(Compound *tree, std::vector<std::string> imports, bool jit, bool main, std::string alias = "", const std::shared_ptr<ThreadSafeContext> & = nullptr);
        ~Codegen() override {
            delete selfSymbol;
            delete Scope;
        }

        void Dump();
        void Run();
        void PrepareJIT();
        int ExecuteJIT();
        void WriteToObjectFile(const std::string &output);
        void LinkObjectFile(const std::string &obj_filename);
        void Optimize(OptimizationLevel opt);

    protected:
        std::unique_ptr<llvm::TargetMachine> InitializeTargetMachine();
        std::unique_ptr<Module> InitializeModule();
        std::unique_ptr<LLJIT> InitializeJIT();
        llvm::Function *InitializeTopLevel();

        [[maybe_unused]] void LinkObjectFileWithClang(const std::string &obj_filename);
        [[maybe_unused]] void LinkObjectFileWithLLD(const std::string &obj_filename);

        void CompileModule(SMRange span, const std::string &filepath, bool isStd, const std::string &alias, bool importAll, bool importToScope, const std::vector<std::pair<std::string, std::string>> &imported_names);

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

        void visit(const TypeExpr *node) override;

        // TODO: Helper functions, move them out somewhere
        // Type related helper functions
        lesma::Value *Cast(SMRange span, lesma::Value *val, lesma::Type *type);
        static lesma::Type *GetExtendedType(lesma::Type *left, lesma::Type *right);

        // Name mangling functions and such
        static bool isMethod(const std::string &mangled_name);
        std::string getMangledName(SMRange span, std::string func_name, const std::vector<lesma::Type *> &paramTypes, bool isMethod = false, std::string alias = "");
        [[maybe_unused]] static bool isMangled(std::string name);
        static std::string getDemangledName(const std::string &mangled_name);
        std::string getTypeMangledName(SMRange span, lesma::Type *type);

        // Other
        lesma::Value *genFuncCall(const FuncCall *node, const std::vector<lesma::Value *> &extra_params);
        static int FindIndexInFields(Type *_struct, const std::string &field);
        static lesma::Type *FindTypeInFields(Type *_struct, const std::string &field);
        void defineFunction(lesma::Value *value, const FuncDecl *node, Value *clsSymbol);

        // Error handling
        void Error(SMRange span, const std::string &message);

        template<typename... Args>
        void Error(SMRange span, const std::string &format, Args &&...args);
    };
}// namespace lesma