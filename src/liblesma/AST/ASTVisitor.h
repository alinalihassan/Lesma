#pragma once


namespace lesma {
    class Statement;
    class Compound;
    class Import;
    class Enum;
    class Class;
    class VarDecl;
    class If;
    class While;
    class FuncDecl;
    class ExternFuncDecl;
    class Assignment;
    class ExpressionStatement;
    class Break;
    class Continue;
    class Return;
    class Defer;
    class Expression;
    class Literal;
    class FuncCall;
    class BinaryOp;
    class DotOp;
    class CastOp;
    class IsOp;
    class UnaryOp;
    class Else;
    class TypeExpr;
    class ASTVisitor {
    public:
        virtual ~ASTVisitor() = default;

        virtual void visit(const Statement *node) = 0;
        virtual void visit(const Compound *node) = 0;
        virtual void visit(const Import *node) = 0;
        virtual void visit(const Enum *node) = 0;
        virtual void visit(const Class *node) = 0;
        virtual void visit(const VarDecl *node) = 0;
        virtual void visit(const If *node) = 0;
        virtual void visit(const While *node) = 0;
        virtual void visit(const FuncDecl *node) = 0;
        virtual void visit(const ExternFuncDecl *node) = 0;
        virtual void visit(const Assignment *node) = 0;
        virtual void visit(const ExpressionStatement *node) = 0;
        virtual void visit(const Break *node) = 0;
        virtual void visit(const Continue *node) = 0;
        virtual void visit(const Return *node) = 0;
        virtual void visit(const Defer *node) = 0;

        virtual void visit(const Expression *node) = 0;
        virtual void visit(const Literal *node) = 0;
        virtual void visit(const FuncCall *node) = 0;
        virtual void visit(const BinaryOp *node) = 0;
        virtual void visit(const DotOp *node) = 0;
        virtual void visit(const CastOp *node) = 0;
        virtual void visit(const IsOp *node) = 0;
        virtual void visit(const UnaryOp *node) = 0;
        virtual void visit(const Else *node) = 0;

        virtual void visit(const TypeExpr *node) = 0;
    };
}// namespace lesma