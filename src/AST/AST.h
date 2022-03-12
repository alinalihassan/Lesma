#pragma once

#include <iostream>
#include <map>
#include <utility>
#include <vector>

#include "Common/Utils.h"
#include "Token/Token.h"
#include "Token/TokenType.h"
#include "nameof.hpp"

namespace lesma {
    class AST {
        Span Loc;

    public:
        explicit AST(Span Loc) : Loc(Loc) {}
        virtual ~AST() = default;

        [[nodiscard]] [[maybe_unused]] Span getSpan() const { return Loc; }
        [[nodiscard]] [[maybe_unused]] SourceLocation getStart() const { return Loc.Start; }
        [[nodiscard]] [[maybe_unused]] SourceLocation getEnd() const { return Loc.End; }

        virtual std::string toString(int ind) {
            return fmt::format("{}AST[Line({}-{}):Col({}-{})]:\n",
                               std::string(ind, ' '),
                               Loc.Start.Line,
                               Loc.End.Line,
                               Loc.Start.Col,
                               Loc.End.Col);
        }
    };

    class Expression : public AST {
    public:
        explicit Expression(Span Loc) : AST(Loc) {}
        ~Expression() override = default;
    };

    class Statement : public AST {
    public:
        explicit Statement(Span Loc) : AST(Loc) {}
        ~Statement() override = default;
    };


    class Literal : public Expression {
        std::string value;
        TokenType type;

    public:
        Literal(Span Loc, std::string value, TokenType type) : Expression(Loc), value(std::move(value)),
                                                               type(type) {}

        ~Literal() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getValue() const { return value; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }

        std::string toString(int /*ind*/) override {
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
        explicit Compound(Span Loc) : Statement(Loc) {}
        explicit Compound(Span Loc, std::vector<Statement *> children) : Statement(Loc), children(std::move(children)) {}
        ~Compound() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Statement *> getChildren() const { return children; }

        void addChildren(Statement *ast) {
            this->children.push_back(ast);
        }

        std::string toString(int ind) override {
            auto ret = fmt::format("{}Compound Statement[Line({}-{}):Col({}-{})]:\n",
                                   std::string(ind, ' '),
                                   getStart().Line,
                                   getEnd().Line,
                                   getStart().Col,
                                   getEnd().Col);
            for (auto child: children)
                ret += child->toString(ind + 2);
            return ret;
        }
    };

    class Type : public Expression {
        std::string name;
        TokenType type;

    public:
        Type(Span Loc, std::string name, TokenType type) : Expression(Loc), name(std::move(name)), type(type) {}
        ~Type() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }

        std::string toString(int /*ind*/) override {
            return name;
        }
    };

    class Import : public Statement {
        std::string file_path;
        std::string alias;

    public:
        Import(Span Loc, std::string file_path, std::string alias) : Statement(Loc), file_path(std::move(file_path)), alias(std::move(alias)){};
        ~Import() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getFilePath() const { return file_path; }
        [[nodiscard]] [[maybe_unused]] std::string getAlias() const { return alias; }

        std::string toString(int ind) override {
            return fmt::format("{}Import[Line({}-{}):Col({}-{})]: {} as {}\n",
                               std::string(ind, ' '),
                               getStart().Line,
                               getEnd().Line,
                               getStart().Col,
                               getEnd().Col,
                               file_path,
                               alias);
        }
    };

    class VarDecl : public Statement {
        Literal *var;
        std::optional<Type *> type;
        std::optional<Expression *> expr;
        bool mutable_;

    public:
        VarDecl(Span Loc, Literal *var, std::optional<Type *> type, std::optional<Expression *> expr, bool readonly) : Statement(Loc), var(var), type(type), expr(expr), mutable_(readonly) {}
        ~VarDecl() override = default;

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] std::optional<Type *> getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] std::optional<Expression *> getValue() const { return expr; }
        [[nodiscard]] [[maybe_unused]] bool getMutability() const { return mutable_; }

        std::string toString(int ind) override {
            return fmt::format("{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n",
                               std::string(ind, ' '),
                               getStart().Line,
                               getEnd().Line,
                               getStart().Col,
                               getEnd().Col,
                               var->toString(ind),
                               (type.has_value() ? ": " + type.value()->toString(ind) : ""),
                               (expr.has_value() ? " = " + expr.value()->toString(ind) : ""));
        }
    };

    class If : public Statement {
        std::vector<Expression *> conds;
        std::vector<Compound *> blocks;

    public:
        If(Span Loc, std::vector<Expression *> conds, std::vector<Compound *> blocks) : Statement(Loc),
                                                                                        conds(std::move(
                                                                                                conds)),
                                                                                        blocks(std::move(
                                                                                                blocks)) {}

        ~If() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getConds() const { return conds; }
        [[nodiscard]] [[maybe_unused]] std::vector<Compound *> getBlocks() const { return blocks; }

        std::string toString(int ind) override {
            auto ret = fmt::format("{}If[Line({}-{}):Col({}-{})]:\n",
                                   std::string(ind, ' '),
                                   getStart().Line,
                                   getEnd().Line,
                                   getStart().Col,
                                   getEnd().Col);
            for (unsigned long i = 0; i < conds.size(); i++)
                ret += fmt::format("{}Cond: {}\n{}",
                                   std::string(ind + 2, ' '),
                                   conds[i]->toString(ind + 2),
                                   blocks[i]->toString(ind + 2));

            return ret;
        }
    };

    class While : public Statement {
        Expression *cond;
        Compound *block;

    public:
        While(Span Loc, Expression *cond, Compound *block) : Statement(Loc), cond(cond), block(block) {}
        ~While() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getCond() const { return cond; }
        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block; }

        std::string toString(int ind) override {
            return fmt::format("{}While[Line({}-{}):Col({}-{})]:\n{}Cond: {}\n{}",
                               std::string(ind, ' '),
                               getStart().Line,
                               getEnd().Line,
                               getStart().Col,
                               getEnd().Col,
                               std::string(ind + 2, ' '),
                               cond->toString(ind + 2),
                               block->toString(ind + 2));
        }
    };

    class FuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;
        Compound *body;

    public:
        FuncDecl(Span Loc, std::string name, Type *return_type,
                 std::vector<std::pair<std::string, Type *>> parameters, Compound *body) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)),
                                                                                           body(body) {}

        ~FuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] Compound *getBody() const { return body; }

        std::string toString(int ind) override {
            auto ret = fmt::format("{}FuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   std::string(ind, ' '),
                                   getStart().Line,
                                   getEnd().Line,
                                   getStart().Col,
                                   getEnd().Col,
                                   name);
            for (auto &param: parameters) {
                ret += param.first + ": " + param.second->toString(ind);
                if (parameters.back() != param) ret += ", ";
            }
            ret += fmt::format(") -> {}\n{}", return_type->toString(ind), body->toString(ind + 2));
            return ret;
        }
    };

    class ExternFuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;

    public:
        ExternFuncDecl(Span Loc, std::string name, Type *return_type,
                       std::vector<std::pair<std::string, Type *>> parameters) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)) {}

        ~ExternFuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }

        std::string toString(int ind) override {
            auto ret = fmt::format("{}FuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   std::string(ind, ' '),
                                   getStart().Line,
                                   getEnd().Line,
                                   getStart().Col,
                                   getEnd().Col,
                                   name);
            for (auto &param: parameters) {
                ret += param.first + ": " + param.second->toString(ind);
                if (parameters.back() != param) ret += ", ";
            }
            ret += fmt::format(") -> {}\n", return_type->toString(ind));
            return ret;
        }
    };

    class FuncCall : public Expression {
        std::string name;
        std::vector<Expression *> arguments;

    public:
        FuncCall(Span Loc, std::string name, std::vector<Expression *> arguments) : Expression(Loc), name(std::move(name)), arguments(std::move(arguments)) {}

        ~FuncCall() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getArguments() const { return arguments; }

        std::string toString(int ind) override {
            auto ret = name + "(";
            for (auto param: arguments) {
                ret += param->toString(ind);
                if (arguments.back() != param) ret += ", ";
            }
            ret += ")";
            return ret;
        }
    };

    class Assignment : public Statement {
        Literal *var;
        TokenType op;
        Expression *expr;

    public:
        Assignment(Span Loc, Literal *var, TokenType op, Expression *expr) : Statement(Loc), var(var), op(op), expr(expr) {}

        ~Assignment() override = default;

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            return fmt::format("{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n",
                               std::string(ind, ' '),
                               getStart().Line,
                               getEnd().Line,
                               getStart().Col,
                               getEnd().Col,
                               var->toString(ind),
                               std::string{NAMEOF_ENUM(op)},
                               expr->toString(ind));
        }
    };

    class ExpressionStatement : public Statement {
        Expression *expr;

    public:
        ExpressionStatement(Span Loc, Expression *expr) : Statement(Loc), expr(expr) {}
        ~ExpressionStatement() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            return fmt::format("{}Expression[Line({}-{}):Col({}-{})]: {}\n",
                               std::string(ind, ' '),
                               getStart().Line,
                               getEnd().Line,
                               getStart().Col,
                               getEnd().Col,
                               expr->toString(ind));
        }
    };

    class BinaryOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        BinaryOp(Span Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left),
                                                                                op(op), right(right) {}

        ~BinaryOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right; }

        std::string toString(int ind) override {
            return left->toString(ind) + " " + std::string{NAMEOF_ENUM(op)} + " " + right->toString(ind);
        }
    };

    class CastOp : public Expression {
        Expression *expr;
        Type *type;

    public:
        CastOp(Span Loc, Expression *expr, Type *type) : Expression(Loc), expr(expr), type(type) {}

        ~CastOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }
        [[nodiscard]] [[maybe_unused]] Type *getType() const { return type; }

        std::string toString(int ind) override {
            return expr->toString(ind) + " as " + type->toString(ind);
        }
    };

    class UnaryOp : public Expression {
        TokenType op;
        Expression *expr;

    public:
        UnaryOp(Span Loc, TokenType op, Expression *expr) : Expression(Loc), op(op), expr(expr) {}
        ~UnaryOp() override = default;

        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            return std::string{NAMEOF_ENUM(op)} + expr->toString(ind);
        }
    };

    class Else : public Expression {
    public:
        explicit Else(Span Loc) : Expression(Loc) {}
        ~Else() override = default;

        std::string toString(int /*ind*/) override {
            return "Else";
        }
    };

    class Break : public Statement {
    public:
        explicit Break(Span Loc) : Statement(Loc) {}
        ~Break() override = default;

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Break\n";
        }
    };

    class Continue : public Statement {
    public:
        explicit Continue(Span Loc) : Statement(Loc) {}
        ~Continue() override = default;

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Continue\n";
        }
    };

    class Return : public Statement {
        Expression *value;

    public:
        Return(Span Loc, Expression *value) : Statement(Loc), value(value) {}
        ~Return() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return value; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Return " + value->toString(ind) + '\n';
        }
    };

    class Defer : public Statement {
        Statement *stmt;

    public:
        Defer(Span Loc, Statement *stmt) : Statement(Loc), stmt(stmt) {}
        ~Defer() override = default;

        [[nodiscard]] [[maybe_unused]] Statement *getStatement() const { return stmt; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Defer " + stmt->toString(0);
        }
    };
}// namespace lesma