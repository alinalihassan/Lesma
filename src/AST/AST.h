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
        SourceLocation Loc;

    public:
        explicit AST(SourceLocation Loc) : Loc(Loc) {}
        virtual ~AST() = default;

        [[nodiscard]] [[maybe_unused]] unsigned int getLine() const { return Loc.Line; }
        [[nodiscard]] [[maybe_unused]] unsigned int getCol() const { return Loc.Col; }

        virtual std::string toString(int ind) {
            return std::string(ind, ' ') + "AST[" + std::to_string(getLine()) + ':' +
                   std::to_string(getCol()) + "]";
        }
    };

    class Expression : public AST {
    public:
        explicit Expression(SourceLocation Loc) : AST(Loc) {}
        ~Expression() override = default;
    };

    class Statement : public AST {
    public:
        explicit Statement(SourceLocation Loc) : AST(Loc) {}
        ~Statement() override = default;
    };


    class Literal : public Expression {
        std::string value;
        TokenType type;

    public:
        Literal(SourceLocation Loc, std::string value, TokenType type) : Expression(Loc), value(std::move(value)),
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
        explicit Compound(SourceLocation Loc) : Statement(Loc) {}
        ~Compound() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Statement *> getChildren() const { return children; }

        void addChildren(Statement *ast) {
            this->children.push_back(ast);
        }

        std::string toString(int ind) override {
            auto ret = std::string(ind, ' ') + "Compound Statement" +
                       "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]:" + '\n';
            for (auto child: children) {
                ret += child->toString(ind + 2);
            }
            return ret;
        }
    };

    class Program : public AST {
        Compound *block = {};

    public:
        Program(SourceLocation Loc, Compound *block) : AST(Loc), block(block) {}
        ~Program() override = default;

        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Program" + '\n' + block->toString(ind + 2);
        }
    };

    class Type : public Expression {
        std::string name;
        TokenType type;

    public:
        Type(SourceLocation Loc, std::string name, TokenType type) : Expression(Loc), name(std::move(name)), type(type) {}
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
        Import(SourceLocation Loc, std::string file_path, std::string alias) : Statement(Loc), file_path(std::move(file_path)), alias(std::move(alias)){};
        ~Import() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getFilePath() const { return file_path; }
        [[nodiscard]] [[maybe_unused]] std::string getAlias() const { return alias; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Import" +
                   "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]: " +
                   file_path + " as " + alias + '\n';
        }
    };

    class VarDecl : public Statement {
        Literal *var;
        std::optional<Type *> type;
        std::optional<Expression *> expr;
        bool mutable_;

    public:
        VarDecl(SourceLocation Loc, Literal *var, std::optional<Type *> type, std::optional<Expression *> expr, bool readonly) : Statement(Loc), var(var), type(type), expr(expr), mutable_(readonly) {}
        ~VarDecl() override = default;

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] std::optional<Type *> getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] std::optional<Expression *> getValue() const { return expr; }
        [[nodiscard]] [[maybe_unused]] bool getMutability() const { return mutable_; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "VarDecl" +
                   "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]: " +
                   var->toString(ind) + (type.has_value() ? ": " + type.value()->toString(ind) : "") +
                   (expr.has_value() ? " = " + expr.value()->toString(ind) : "") + '\n';
        }
    };

    class If : public Statement {
        std::vector<Expression *> conds;
        std::vector<Compound *> blocks;

    public:
        If(SourceLocation Loc, std::vector<Expression *> conds, std::vector<Compound *> blocks) : Statement(Loc),
                                                                                                  conds(std::move(
                                                                                                          conds)),
                                                                                                  blocks(std::move(
                                                                                                          blocks)) {}

        ~If() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getConds() const { return conds; }
        [[nodiscard]] [[maybe_unused]] std::vector<Compound *> getBlocks() const { return blocks; }

        std::string toString(int ind) override {
            auto ret = std::string(ind, ' ') + "If" +
                       "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]:\n";
            for (unsigned long i = 0; i < conds.size(); i++) {
                ret += std::string(ind + 2, ' ') + "Cond: " + conds[i]->toString(ind + 2) + '\n' +
                       blocks[i]->toString(ind + 2);
            }

            return ret;
        }
    };

    class While : public Statement {
        Expression *cond;
        Compound *block;

    public:
        While(SourceLocation Loc, Expression *cond, Compound *block) : Statement(Loc), cond(cond), block(block) {}
        ~While() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getCond() const { return cond; }
        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "While" +
                   "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]:\n" +
                   std::string(ind + 2, ' ') + "Cond: " + cond->toString(ind + 2) + '\n' +
                   block->toString(ind + 2);
        }
    };

    class FuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;
        Compound *body;

    public:
        FuncDecl(SourceLocation Loc, std::string name, Type *return_type,
                 std::vector<std::pair<std::string, Type *>> parameters, Compound *body) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)),
                                                                                           body(body) {}

        ~FuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] Compound *getBody() const { return body; }

        std::string toString(int ind) override {
            auto ret = std::string(ind, ' ') + "FuncDecl" +
                       "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]:" + name;
            ret += "(";
            for (auto &param: parameters) {
                ret += param.first + ": " + param.second->toString(ind);
                if (parameters.back() != param) ret += ", ";
            }
            ret += ") -> " + return_type->toString(ind) + '\n';
            ret += body->toString(ind + 2);
            return ret;
        }
    };

    class ExternFuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;

    public:
        ExternFuncDecl(SourceLocation Loc, std::string name, Type *return_type,
                       std::vector<std::pair<std::string, Type *>> parameters) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)) {}

        ~ExternFuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }

        std::string toString(int ind) override {
            auto ret = std::string(ind, ' ') + "ExternFuncDecl" +
                       "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]:" + name;
            ret += "(";
            for (auto param: parameters) {
                ret += param.first + ": " + param.second->toString(ind);
                if (parameters.back() != param) ret += ", ";
            }
            ret += ") -> " + return_type->toString(ind) + '\n';
            return ret;
        }
    };

    class FuncCall : public Expression {
        std::string name;
        std::vector<Expression *> arguments;

    public:
        FuncCall(SourceLocation Loc, std::string name, std::vector<Expression *> arguments) : Expression(Loc), name(std::move(name)), arguments(std::move(arguments)) {}

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
        Assignment(SourceLocation Loc, Literal *var, TokenType op, Expression *expr) : Statement(Loc), var(var), op(op), expr(expr) {}

        ~Assignment() override = default;

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            auto ret = std::string(ind, ' ') + "Assignment" +
                       "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]: ";
            ret += var->toString(ind) + " " + std::string{NAMEOF_ENUM(op)} + " " + expr->toString(ind) + '\n';
            return ret;
        }
    };

    class ExpressionStatement : public Statement {
        Expression *expr;

    public:
        ExpressionStatement(SourceLocation Loc, Expression *expr) : Statement(Loc), expr(expr) {}
        ~ExpressionStatement() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Expression" +
                   "[" + std::to_string(getLine()) + ':' + std::to_string(getCol()) + "]: " +
                   expr->toString(ind) + '\n';
        }
    };

    class BinaryOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        BinaryOp(SourceLocation Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left),
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
        CastOp(SourceLocation Loc, Expression *expr, Type *type) : Expression(Loc), expr(expr), type(type) {}

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
        UnaryOp(SourceLocation Loc, TokenType op, Expression *expr) : Expression(Loc), op(op), expr(expr) {}
        ~UnaryOp() override = default;

        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(int ind) override {
            return std::string{NAMEOF_ENUM(op)} + expr->toString(ind);
        }
    };

    class Else : public Expression {
    public:
        explicit Else(SourceLocation Loc) : Expression(Loc) {}
        ~Else() override = default;

        std::string toString(int /*ind*/) override {
            return "Else";
        }
    };

    class Break : public Statement {
    public:
        explicit Break(SourceLocation Loc) : Statement(Loc) {}
        ~Break() override = default;

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Break\n";
        }
    };

    class Continue : public Statement {
    public:
        explicit Continue(SourceLocation Loc) : Statement(Loc) {}
        ~Continue() override = default;

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Continue\n";
        }
    };

    class Return : public Statement {
        Expression *value;

    public:
        Return(SourceLocation Loc, Expression *value) : Statement(Loc), value(value) {}
        ~Return() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return value; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Return " + value->toString(ind) + '\n';
        }
    };

    class Defer : public Statement {
        Statement *stmt;

    public:
        Defer(SourceLocation Loc, Statement *stmt) : Statement(Loc), stmt(stmt) {}
        ~Defer() override = default;

        [[nodiscard]] [[maybe_unused]] Statement *getStatement() const { return stmt; }

        std::string toString(int ind) override {
            return std::string(ind, ' ') + "Defer " + stmt->toString(0);
        }
    };
}// namespace lesma