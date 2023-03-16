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
    class Type;
    class ASTVisitor {
    public:
        ~ASTVisitor() = default;

        virtual void visit(Statement *node) = 0;
        virtual void visit(Compound *node) = 0;
        virtual void visit(Import *node) = 0;
        virtual void visit(Enum *node) = 0;
        virtual void visit(Class *node) = 0;
        virtual void visit(VarDecl *node) = 0;
        virtual void visit(If *node) = 0;
        virtual void visit(While *node) = 0;
        virtual void visit(FuncDecl *node) = 0;
        virtual void visit(ExternFuncDecl *node) = 0;
        virtual void visit(Assignment *node) = 0;
        virtual void visit(ExpressionStatement *node) = 0;
        virtual void visit(Break *node) = 0;
        virtual void visit(Continue *node) = 0;
        virtual void visit(Return *node) = 0;
        virtual void visit(Defer *node) = 0;

        virtual void visit(Expression *node) = 0;
        virtual void visit(Literal *node) = 0;
        virtual void visit(FuncCall *node) = 0;
        virtual void visit(BinaryOp *node) = 0;
        virtual void visit(DotOp *node) = 0;
        virtual void visit(CastOp *node) = 0;
        virtual void visit(IsOp *node) = 0;
        virtual void visit(UnaryOp *node) = 0;
        virtual void visit(Else *node) = 0;

        virtual void visit(Type *node) = 0;
    };
}// namespace lesma