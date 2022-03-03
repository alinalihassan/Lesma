#pragma once

#include "AST.h"

namespace lesma {
    template<typename T>
    class StmtVisitor {
    public:
        ~StmtVisitor() = default;

        virtual T visit(Statement *node) = 0;
        virtual T visit(Compound *node) = 0;
        virtual T visit(Import *node) = 0;
        virtual T visit(VarDecl *node) = 0;
        virtual T visit(If *node) = 0;
        virtual T visit(While *node) = 0;
        virtual T visit(FuncDecl *node) = 0;
        virtual T visit(ExternFuncDecl *node) = 0;
        virtual T visit(Assignment *node) = 0;
        virtual T visit(ExpressionStatement *node) = 0;
        virtual T visit(Break *node) = 0;
        virtual T visit(Continue *node) = 0;
        virtual T visit(Return *node) = 0;
        virtual T visit(Defer *node) = 0;
    };
}