#pragma once

#include "AST.h"

namespace lesma {
    template<typename T1, typename T2>
    class ExprVisitor {
    public:
        ~ExprVisitor() = default;

        virtual T1 visit(Expression *node) = 0;
        virtual T1 visit(Literal *node) = 0;
        virtual T2 visit(Type *node) = 0;
        virtual T1 visit(FuncCall *node) = 0;
        virtual T1 visit(BinaryOp *node) = 0;
        virtual T1 visit(CastOp *node) = 0;
        virtual T1 visit(UnaryOp *node) = 0;
        virtual T1 visit(Else *node) = 0;
    };
}