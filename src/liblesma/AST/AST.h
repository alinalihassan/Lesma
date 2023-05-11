#pragma once

#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"
#include "nameof.hpp"

namespace lesma {
    class AST {
        SMRange Loc;

    public:
        explicit AST(SMRange Loc) : Loc(Loc) {}
        virtual ~AST() = default;
        virtual void accept(ASTVisitor &visitor) const = 0;

        [[nodiscard]] [[maybe_unused]] SMRange getSpan() const { return Loc; }
        [[nodiscard]] [[maybe_unused]] SMLoc getStart() const { return Loc.Start; }
        [[nodiscard]] [[maybe_unused]] SMLoc getEnd() const { return Loc.End; }

        [[nodiscard]] virtual std::string toString(const std::string &prefix, bool isTail) const {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}AST[Line({}-{}):Col({}-{})]:\n",
                               prefix, (isTail ? "└──" : "├──"),
                               srcMgr.getLineAndColumn(Loc.Start).first,
                               srcMgr.getLineAndColumn(Loc.End).first,
                               srcMgr.getLineAndColumn(Loc.Start).second,
                               srcMgr.getLineAndColumn(Loc.End).second);
        }
    };

    class Expression : public AST {
    public:
        explicit Expression(SMRange Loc) : AST(Loc) {}
        ~Expression() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }
    };

    class Statement : public AST {
    public:
        explicit Statement(SMRange Loc) : AST(Loc) {}
        ~Statement() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }
    };


    class Literal : public Expression {
        std::string value;
        TokenType type;

    public:
        Literal(SMRange Loc, std::string value, TokenType type) : Expression(Loc), value(std::move(value)),
                                                                  type(type) {}
        ~Literal() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getValue() const { return value; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }

        [[nodiscard]] std::string toString(const std::string & /*prefix*/, bool /*isTail*/) const override {
            if (type == TokenType::STRING)
                return '"' + value + '"';
            else if (type == TokenType::NIL || type == TokenType::INTEGER || type == TokenType::DOUBLE ||
                     type == TokenType::IDENTIFIER || type == TokenType::BOOL)
                return value;
            else
                return "Unknown literal";
        }
    };

    class Compound : public Statement {
        std::vector<Statement *> children;

    public:
        explicit Compound(SMRange Loc) : Statement(Loc) {}
        explicit Compound(SMRange Loc, std::vector<Statement *> children) : Statement(Loc), children(std::move(children)) {}
        ~Compound() override {
            for (auto stmt: children)
                delete stmt;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::vector<Statement *> getChildren() const { return children; }

        [[maybe_unused]] void addChildren(Statement *ast) {
            this->children.push_back(ast);
        }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            auto ret = fmt::format("{}{}Compound[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr.getLineAndColumn(getStart()).first,
                                   srcMgr.getLineAndColumn(getEnd()).first,
                                   srcMgr.getLineAndColumn(getStart()).second,
                                   srcMgr.getLineAndColumn(getEnd()).second);
            for (auto child: children)
                ret += child->toString(prefix + (isTail ? "    " : "│   "), children.back() == child);
            return ret;
        }
    };

    class TypeExpr : public Expression {
        std::string name;
        TokenType type;

        // Pointer fields
        TypeExpr *elementType;

        // Function fields
        std::vector<TypeExpr *> params;
        TypeExpr *ret;

    public:
        TypeExpr(SMRange Loc, std::string name, TokenType type) : Expression(Loc), name(std::move(name)), type(type), elementType(nullptr), ret(nullptr) {}
        TypeExpr(SMRange Loc, std::string name, TokenType type, TypeExpr *elementType) : Expression(Loc), name(std::move(name)), type(type), elementType(elementType), ret(nullptr) {}
        TypeExpr(SMRange Loc, std::string name, TokenType type, std::vector<TypeExpr *> params, TypeExpr *ret) : Expression(Loc), name(std::move(name)), type(type), elementType(nullptr), params(std::move(params)), ret(ret) {}
        ~TypeExpr() override {
            for (auto param: params)
                delete param;

            delete ret;
            delete elementType;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getElementType() const { return elementType; }
        [[nodiscard]] [[maybe_unused]] std::vector<TypeExpr *> getParams() const { return params; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return ret; }

        [[nodiscard]] std::string toString(const std::string & /*prefix*/, bool /*isTail*/) const override {
            return name;
        }
    };

    class Enum : public Statement {
        std::string identifier;
        std::vector<std::string> values;
        bool exported;

    public:
        Enum(SMRange Loc, std::string identifier, std::vector<std::string> values, bool exported) : Statement(Loc), identifier(std::move(identifier)), values(std::move(values)), exported(exported){};
        ~Enum() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::string> getValues() const { return values; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            std::ostringstream imploded;
            std::copy(values.begin(), values.end(),
                      std::ostream_iterator<std::string>(imploded, ", "));

            return fmt::format("{}{}Enum[Line({}-{}):Col({}-{})]: {} with: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               identifier,
                               imploded.str());
        }
    };

    class Import : public Statement {
        std::string file_path;
        std::string alias;
        std::vector<std::pair<std::string, std::string>> imported_names;
        bool std;
        bool import_all;
        bool import_to_scope;

    public:
        Import(SMRange Loc, std::string file_path, std::string alias, bool std, bool import_all, bool import_to_scope, std::vector<std::pair<std::string, std::string>> imported_names) : Statement(Loc), file_path(std::move(file_path)), alias(std::move(alias)), imported_names(std::move(imported_names)), std(std), import_all(import_all), import_to_scope(import_to_scope){};
        ~Import() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getFilePath() const { return file_path; }
        [[nodiscard]] [[maybe_unused]] std::string getAlias() const { return alias; }
        [[nodiscard]] [[maybe_unused]] bool getImportAll() const { return import_all; }
        [[nodiscard]] [[maybe_unused]] bool getImportScope() const { return import_to_scope; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, std::string>> getImportedNames() const { return imported_names; }
        [[nodiscard]] [[maybe_unused]] bool isStd() const { return std; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Import[Line({}-{}):Col({}-{})]: {} as {} from {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               file_path,
                               alias,
                               std ? "std" : "file");
        }
    };

    class VarDecl : public Statement {
        Literal *var;
        std::optional<TypeExpr *> type;
        std::optional<Expression *> expr;
        bool mutable_;

    public:
        VarDecl(SMRange Loc, Literal *var, std::optional<TypeExpr *> type, std::optional<Expression *> expr, bool readonly) : Statement(Loc), var(var), type(type), expr(expr), mutable_(readonly) {}
        ~VarDecl() override {
            delete var;
            if (type.has_value())
                delete type.value();
            if (expr.has_value())
                delete expr.value();
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] std::optional<TypeExpr *> getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] std::optional<Expression *> getValue() const { return expr; }
        [[nodiscard]] [[maybe_unused]] bool getMutability() const { return mutable_; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               var->toString(prefix, isTail),
                               (type.has_value() ? ": " + type.value()->toString(prefix, isTail) : ""),
                               (expr.has_value() ? " = " + expr.value()->toString(prefix, isTail) : ""));
        }
    };

    class If : public Statement {
        std::vector<Expression *> conds;
        std::vector<Compound *> blocks;

    public:
        If(SMRange Loc, std::vector<Expression *> conds, std::vector<Compound *> blocks) : Statement(Loc),
                                                                                           conds(std::move(conds)),
                                                                                           blocks(std::move(blocks)) {}
        ~If() override {
            for (auto cond: conds)
                delete cond;
            for (auto block: blocks)
                delete block;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getConds() const { return conds; }
        [[nodiscard]] [[maybe_unused]] std::vector<Compound *> getBlocks() const { return blocks; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            auto ret = fmt::format("{}{}If[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr.getLineAndColumn(getStart()).first,
                                   srcMgr.getLineAndColumn(getEnd()).first,
                                   srcMgr.getLineAndColumn(getStart()).second,
                                   srcMgr.getLineAndColumn(getEnd()).second);
            for (unsigned long i = 0; i < conds.size(); i++)
                ret += fmt::format("{}{}Cond: {}\n{}",
                                   prefix + (isTail ? "    " : "│   "), i == conds.size() - 1 ? "└──" : "├──",
                                   conds[i]->toString(prefix + (isTail ? "    " : "│   "), i == conds.size() - 1),
                                   blocks[i]->toString(prefix + (isTail ? "    " : "│   ") + (i == conds.size() - 1 ? "    " : "│   "), true));

            return ret;
        }
    };

    class While : public Statement {
        Expression *cond;
        Compound *block;

    public:
        While(SMRange Loc, Expression *cond, Compound *block) : Statement(Loc), cond(cond), block(block) {}
        ~While() override {
            delete cond;
            delete block;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getCond() const { return cond; }
        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}While[Line({}-{}):Col({}-{})]:\n{}{}Cond: {}\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               prefix + (isTail ? "    " : "│   "),
                               "└──",
                               cond->toString(prefix, true),
                               block->toString(prefix + (isTail ? "        " : "│       "), true));
        }
    };

    class Parameter {
    public:
        std::string name;
        TypeExpr *type;
        bool optional;
        Expression *default_val;

        explicit Parameter(std::string name, TypeExpr *type = nullptr, bool optional = false, Expression *default_val = nullptr) : name(std::move(name)), type(type), optional(optional), default_val(default_val) {}

        ~Parameter() {
            delete type;
            delete default_val;
        }
    };

    class FuncDecl : public Statement {
        std::string name;
        TypeExpr *return_type;
        std::vector<Parameter *> parameters;
        Compound *body;
        bool varargs;
        bool exported;

    public:
        FuncDecl(SMRange Loc, std::string name, TypeExpr *return_type,
                 std::vector<Parameter *> parameters, Compound *body, bool varargs, bool exported) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)),
                                                                                                     body(body), varargs(varargs), exported(exported) {}
        ~FuncDecl() override {
            for (auto param: parameters)
                delete param;

            delete return_type;
            delete body;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<Parameter *> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] Compound *getBody() const { return body; }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            auto ret = fmt::format("{}{}FuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr.getLineAndColumn(getStart()).first,
                                   srcMgr.getLineAndColumn(getEnd()).first,
                                   srcMgr.getLineAndColumn(getStart()).second,
                                   srcMgr.getLineAndColumn(getEnd()).second,
                                   name);
            for (auto &param: parameters) {
                ret += param->name + ": " + param->type->toString(prefix, isTail) +
                       (param->default_val == nullptr ? "" : fmt::format("= {}", param->default_val->toString(prefix, isTail)));
                if (parameters.back() != param) ret += ", ";
            }
            if (varargs)
                ret += ", ...";
            ret += fmt::format(") -> {}\n{}", return_type->toString(prefix, isTail),
                               body->toString(prefix + (isTail ? "    " : "│   "), true));
            return ret;
        }
    };

    class ExternFuncDecl : public Statement {
        std::string name;
        TypeExpr *return_type;
        std::vector<Parameter *> parameters;
        bool varargs;
        bool exported;

    public:
        ExternFuncDecl(SMRange Loc, std::string name, TypeExpr *return_type,
                       std::vector<Parameter *> parameters, bool varargs, bool exported) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)), varargs(varargs), exported(exported) {}

        ~ExternFuncDecl() override {
            for (auto param: parameters)
                delete param;

            delete return_type;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<Parameter *> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            auto ret = fmt::format("{}{}ExternFuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr.getLineAndColumn(getStart()).first,
                                   srcMgr.getLineAndColumn(getEnd()).first,
                                   srcMgr.getLineAndColumn(getStart()).second,
                                   srcMgr.getLineAndColumn(getEnd()).second,
                                   name);
            for (auto &param: parameters) {
                ret += param->name + ": " + param->type->toString(prefix, isTail) +
                       (param->default_val == nullptr ? "" : fmt::format("= {}", param->default_val->toString(prefix, isTail)));
                if (parameters.back() != param) ret += ", ";
            }
            if (varargs)
                ret += ", ...";
            ret += fmt::format(") -> {}\n", return_type->toString(prefix, isTail));
            return ret;
        }
    };

    class FuncCall : public Expression {
        std::string name;
        std::vector<Expression *> arguments;

    public:
        FuncCall(SMRange Loc, std::string name, std::vector<Expression *> arguments) : Expression(Loc), name(std::move(name)), arguments(std::move(arguments)) {}
        ~FuncCall() override {
            for (auto arg: arguments)
                delete arg;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getArguments() const { return arguments; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto ret = name + "(";
            for (auto param: arguments) {
                ret += param->toString(prefix, isTail);
                if (arguments.back() != param) ret += ", ";
            }
            ret += ")";
            return ret;
        }
    };

    class Assignment : public Statement {
        Expression *lhs;
        TokenType op;
        Expression *rhs;

    public:
        Assignment(SMRange Loc, Expression *lhs, TokenType op, Expression *rhs) : Statement(Loc), lhs(lhs), op(op), rhs(rhs) {}
        ~Assignment() override {
            delete lhs;
            delete rhs;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeftHandSide() const { return lhs; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRightHandSide() const { return rhs; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               lhs->toString(prefix, isTail),
                               std::string{NAMEOF_ENUM(op)},
                               rhs->toString(prefix, isTail));
        }
    };

    class ExpressionStatement : public Statement {
        Expression *expr;

    public:
        ExpressionStatement(SMRange Loc, Expression *expr) : Statement(Loc), expr(expr) {}
        ~ExpressionStatement() override {
            delete expr;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Expression[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               expr->toString(prefix, isTail));
        }
    };

    class BinaryOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        BinaryOp(SMRange Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left), op(op), right(right) {}
        ~BinaryOp() override {
            delete left;
            delete right;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            return left->toString(prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " + right->toString(prefix, isTail);
        }
    };

    class IsOp : public Expression {
        Expression *left;
        TokenType op;
        TypeExpr *right;

    public:
        IsOp(SMRange Loc, Expression *left, TokenType op, TypeExpr *right) : Expression(Loc), left(left), op(op), right(right) {}
        ~IsOp() override {
            delete left;
            delete right;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getRight() const { return right; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            return left->toString(prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " + right->toString(prefix, isTail);
        }
    };

    class CastOp : public Expression {
        Expression *expr;
        TypeExpr *type;

    public:
        CastOp(SMRange Loc, Expression *expr, TypeExpr *type) : Expression(Loc), expr(expr), type(type) {}
        ~CastOp() override {
            delete expr;
            delete type;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getType() const { return type; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            return expr->toString(prefix, isTail) + " as " + type->toString(prefix, isTail);
        }
    };

    class UnaryOp : public Expression {
        TokenType op;
        Expression *expr;

    public:
        UnaryOp(SMRange Loc, TokenType op, Expression *expr) : Expression(Loc), op(op), expr(expr) {}
        ~UnaryOp() override {
            delete expr;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            return std::string{NAMEOF_ENUM(op)} + expr->toString(prefix, isTail);
        }
    };

    class DotOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        DotOp(SMRange Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left), op(op), right(right) {}
        ~DotOp() override {
            delete left;
            delete right;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            return left->toString(prefix, isTail) + "." + right->toString(prefix, isTail);
        }
    };

    class Else : public Expression {
    public:
        explicit Else(SMRange Loc) : Expression(Loc) {}
        ~Else() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] std::string toString(const std::string & /*prefix*/, bool /*isTail*/) const override {
            return "Else";
        }
    };

    class Break : public Statement {
    public:
        explicit Break(SMRange Loc) : Statement(Loc) {}
        ~Break() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Break[Line({}-{}):Col({}-{})]:\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second);
        }
    };

    class Continue : public Statement {
    public:
        explicit Continue(SMRange Loc) : Statement(Loc) {}
        ~Continue() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Continue[Line({}-{}):Col({}-{})]:\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second);
        }
    };

    class Return : public Statement {
        Expression *value;

    public:
        Return(SMRange Loc, Expression *value) : Statement(Loc), value(value) {}
        ~Return() override {
            delete value;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return value; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Return[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               value->toString(prefix, true));
        }
    };

    class Defer : public Statement {
        Statement *stmt;

    public:
        Defer(SMRange Loc, Statement *stmt) : Statement(Loc), stmt(stmt) {}
        ~Defer() override {
            delete stmt;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Statement *getStatement() const { return stmt; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            return fmt::format("{}{}Defer[Line({}-{}):Col({}-{})]:\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               stmt->toString(prefix + (isTail ? "    " : "│   "), true));
        }
    };

    class Class : public Statement {
        std::string identifier;
        std::vector<VarDecl *> fields;
        std::vector<FuncDecl *> methods;
        bool exported;

    public:
        Class(SMRange Loc, std::string identifier, std::vector<VarDecl *> fields, std::vector<FuncDecl *> methods, bool exported) : Statement(Loc), identifier(std::move(identifier)), fields(std::move(fields)), methods(std::move(methods)), exported(exported){};
        ~Class() override {
            for (auto field: fields)
                delete field;
            for (auto method: methods)
                delete method;
        }
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier; }
        [[nodiscard]] [[maybe_unused]] std::vector<VarDecl *> getFields() const { return fields; }
        [[nodiscard]] [[maybe_unused]] std::vector<FuncDecl *> getMethods() const { return methods; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported; }

        [[nodiscard]] std::string toString(const std::string &prefix, bool isTail) const override {
            auto &srcMgr = ServiceLocator::getSourceManager();
            std::string fields_str;
            for (auto field: fields)
                fields_str += field->toString(prefix + (isTail ? "    " : "│   "), false);

            std::string methods_str;
            for (auto method: methods)
                methods_str += method->toString(prefix + (isTail ? "    " : "│   "), method == methods.back());
            return fmt::format("{}{}Class[Line({}-{}):Col({}-{})]: {}: \n{}{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr.getLineAndColumn(getStart()).first,
                               srcMgr.getLineAndColumn(getEnd()).first,
                               srcMgr.getLineAndColumn(getStart()).second,
                               srcMgr.getLineAndColumn(getEnd()).second,
                               identifier,
                               fields_str,
                               methods_str);
        }
    };
}// namespace lesma