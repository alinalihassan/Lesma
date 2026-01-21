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
        SymbolTable *Scope_;
        std::string filename_;
        std::string alias_;
        lesma::Value *result_ = nullptr;

        std::stack<llvm::BasicBlock *> breakBlocks_;
        std::stack<llvm::BasicBlock *> continueBlocks_;
        std::stack<std::vector<Statement *>> deferStack_;
        lesma::Value *currentFunction_ = nullptr;

        std::vector<std::string> ObjectFiles_;
        std::vector<std::string> ImportedModules_;
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
        Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main, std::string alias = "", const std::shared_ptr<ThreadSafeContext> & = nullptr);
        ~Codegen() override {
            delete selfSymbol_;
            delete Scope_;
        }

        Codegen(const Codegen &) = delete;
        Codegen &operator=(const Codegen &) = delete;
        Codegen(Codegen &&) = default;
        Codegen &operator=(Codegen &&) = default;

        void dump();
        void run();
        void prepareJit();
        int executeJit();
        void writeToObjectFile(const std::string &output);
        void linkObjectFile(const std::string &obj_filename);
        void optimize(OptimizationLevel opt);

    protected:
        std::unique_ptr<llvm::TargetMachine> initializeTargetMachine();
        std::unique_ptr<Module> initializeModule();
        std::unique_ptr<LLJIT> initializeJit();
        llvm::Function *initializeTopLevel();

        [[maybe_unused]] void linkObjectFileWithClang(const std::string &obj_filename);
        [[maybe_unused]] void linkObjectFileWithLld(const std::string &obj_filename);

        void compileModule(llvm::SMRange span, const std::string &filepath, bool isStd, const std::string &alias, bool importAll, bool importToScope, const std::vector<std::pair<std::string, std::string>> &imported_names);

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
        lesma::Value *cast(llvm::SMRange span, lesma::Value *val, lesma::Type *type);
        static lesma::Type *getExtendedType(lesma::Type *left, lesma::Type *right);

        // Name mangling functions and such
        static bool isMethod(const std::string &mangled_name);
        std::string getMangledName(llvm::SMRange span, std::string func_name, const std::vector<lesma::Type *> &paramTypes, bool isMethod = false, std::string alias = "");
        [[maybe_unused]] static bool isMangled(std::string name);
        static std::string getDemangledName(const std::string &mangled_name);
        std::string getTypeMangledName(llvm::SMRange span, lesma::Type *type);

        // Other
        lesma::Value *genFuncCall(const FuncCall *node, const std::vector<lesma::Value *> &extra_params);
        static int findIndexInFields(Type *_struct, const std::string &field);
        static lesma::Type *findTypeInFields(Type *_struct, const std::string &field);
        void defineFunction(lesma::Value *value, const FuncDecl *node, Value *clsSymbol);
    };
}// namespace lesma