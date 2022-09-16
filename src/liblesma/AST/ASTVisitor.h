#pragma once

#include "AST.h"

namespace lesma {
    template<typename StmtRetTy = void, typename ExprRetTy = void, typename TypeRetTy = void>
    class ASTVisitor {
    public:
        ~ASTVisitor() = default;

        virtual StmtRetTy visit(Statement *node) = 0;
        virtual StmtRetTy visit(Compound *node) = 0;
        virtual StmtRetTy visit(Import *node) = 0;
        virtual StmtRetTy visit(Enum *node) = 0;
        virtual StmtRetTy visit(Class *node) = 0;
        virtual StmtRetTy visit(VarDecl *node) = 0;
        virtual StmtRetTy visit(If *node) = 0;
        virtual StmtRetTy visit(While *node) = 0;
        virtual StmtRetTy visit(FuncDecl *node) = 0;
        virtual StmtRetTy visit(ExternFuncDecl *node) = 0;
        virtual StmtRetTy visit(Assignment *node) = 0;
        virtual StmtRetTy visit(ExpressionStatement *node) = 0;
        virtual StmtRetTy visit(Break *node) = 0;
        virtual StmtRetTy visit(Continue *node) = 0;
        virtual StmtRetTy visit(Return *node) = 0;
        virtual StmtRetTy visit(Defer *node) = 0;

        virtual ExprRetTy visit(Expression *node) = 0;
        virtual ExprRetTy visit(Literal *node) = 0;
        virtual ExprRetTy visit(FuncCall *node) = 0;
        virtual ExprRetTy visit(BinaryOp *node) = 0;
        virtual ExprRetTy visit(DotOp *node) = 0;
        virtual ExprRetTy visit(CastOp *node) = 0;
        virtual ExprRetTy visit(IsOp *node) = 0;
        virtual ExprRetTy visit(UnaryOp *node) = 0;
        virtual ExprRetTy visit(Else *node) = 0;

        virtual TypeRetTy visit(Type *node) = 0;
    };
}// namespace lesma