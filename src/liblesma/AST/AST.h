#pragma once

#include <iostream>
#include <map>
#include <utility>
#include <vector>

#include "liblesma/Common/Utils.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"
#include "nameof.hpp"

namespace lesma {
    class AST {
        llvm::SMRange Loc;

    public:
        explicit AST(llvm::SMRange Loc) : Loc(Loc) {}
        virtual ~AST() = default;

        [[nodiscard]] [[maybe_unused]] llvm::SMRange getSpan() const { return Loc; }
        [[nodiscard]] [[maybe_unused]] llvm::SMLoc getStart() const { return Loc.Start; }
        [[nodiscard]] [[maybe_unused]] llvm::SMLoc getEnd() const { return Loc.End; }

        virtual std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) {
            return fmt::format("{}{}AST[Line({}-{}):Col({}-{})]:\n",
                               prefix, (isTail ? "└──" : "├──"),
                               srcMgr->getLineAndColumn(Loc.Start).first,
                               srcMgr->getLineAndColumn(Loc.End).first,
                               srcMgr->getLineAndColumn(Loc.Start).second,
                               srcMgr->getLineAndColumn(Loc.End).second);
        }
    };

    class Expression : public AST {
    public:
        explicit Expression(llvm::SMRange Loc) : AST(Loc) {}
        ~Expression() override = default;
    };

    class Statement : public AST {
    public:
        explicit Statement(llvm::SMRange Loc) : AST(Loc) {}
        ~Statement() override = default;
    };


    class Literal : public Expression {
        std::string value;
        TokenType type;

    public:
        Literal(llvm::SMRange Loc, std::string value, TokenType type) : Expression(Loc), value(std::move(value)),
                                                                        type(type) {}

        ~Literal() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getValue() const { return value; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) override {
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
        explicit Compound(llvm::SMRange Loc) : Statement(Loc) {}
        explicit Compound(llvm::SMRange Loc, std::vector<Statement *> children) : Statement(Loc), children(std::move(children)) {}
        ~Compound() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Statement *> getChildren() const { return children; }

        [[maybe_unused]] void addChildren(Statement *ast) {
            this->children.push_back(ast);
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            auto ret = fmt::format("{}{}Compound[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second);
            for (auto child: children)
                ret += child->toString(srcMgr, prefix + (isTail ? "    " : "│   "), children.back() == child);
            return ret;
        }
    };

    class Type : public Expression {
        std::string name;
        TokenType type;

        // Pointer fields
        Type *elementType;

        // Function fields
        std::vector<Type *> params;
        Type *ret;

    public:
        Type(llvm::SMRange Loc, std::string name, TokenType type) : Expression(Loc), name(std::move(name)), type(type), elementType(nullptr), ret(nullptr) {}
        Type(llvm::SMRange Loc, std::string name, TokenType type, Type *elementType) : Expression(Loc), name(std::move(name)), type(type), elementType(elementType), ret(nullptr) {}
        Type(llvm::SMRange Loc, std::string name, TokenType type, std::vector<Type *> params, Type *ret) : Expression(Loc), name(std::move(name)), type(type), params(std::move(params)), ret(ret) {}
        ~Type() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] Type *getElementType() const { return elementType; }
        [[nodiscard]] [[maybe_unused]] std::vector<Type *> getParams() const { return params; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return ret; }

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) override {
            return name;
        }
    };

    class Enum : public Statement {
        std::string identifier;
        std::vector<std::string> values;

    public:
        Enum(llvm::SMRange Loc, std::string identifier, std::vector<std::string> values) : Statement(Loc), identifier(std::move(identifier)), values(std::move(values)){};
        ~Enum() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::string> getValues() const { return values; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            std::ostringstream imploded;
            std::copy(values.begin(), values.end(),
                      std::ostream_iterator<std::string>(imploded, ", "));
            return fmt::format("{}{}Enum[Line({}-{}):Col({}-{})]: {} with: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               identifier,
                               imploded.str());
        }
    };

    class Import : public Statement {
        std::string file_path;
        std::string alias;
        bool std;

    public:
        Import(llvm::SMRange Loc, std::string file_path, std::string alias, bool std) : Statement(Loc), file_path(std::move(file_path)), alias(std::move(alias)), std(std){};
        ~Import() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getFilePath() const { return file_path; }
        [[nodiscard]] [[maybe_unused]] std::string getAlias() const { return alias; }
        [[nodiscard]] [[maybe_unused]] bool isStd() const { return std; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Import[Line({}-{}):Col({}-{})]: {} as {} from {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               file_path,
                               alias,
                               std ? "std" : "file");
        }
    };

    class VarDecl : public Statement {
        Literal *var;
        std::optional<Type *> type;
        std::optional<Expression *> expr;
        bool mutable_;

    public:
        VarDecl(llvm::SMRange Loc, Literal *var, std::optional<Type *> type, std::optional<Expression *> expr, bool readonly) : Statement(Loc), var(var), type(type), expr(expr), mutable_(readonly) {}
        ~VarDecl() override = default;

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var; }
        [[nodiscard]] [[maybe_unused]] std::optional<Type *> getType() const { return type; }
        [[nodiscard]] [[maybe_unused]] std::optional<Expression *> getValue() const { return expr; }
        [[nodiscard]] [[maybe_unused]] bool getMutability() const { return mutable_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               var->toString(srcMgr, prefix, isTail),
                               (type.has_value() ? ": " + type.value()->toString(srcMgr, prefix, isTail) : ""),
                               (expr.has_value() ? " = " + expr.value()->toString(srcMgr, prefix, isTail) : ""));
        }
    };

    class If : public Statement {
        std::vector<Expression *> conds;
        std::vector<Compound *> blocks;

    public:
        If(llvm::SMRange Loc, std::vector<Expression *> conds, std::vector<Compound *> blocks) : Statement(Loc),
                                                                                                 conds(std::move(
                                                                                                         conds)),
                                                                                                 blocks(std::move(
                                                                                                         blocks)) {}

        ~If() override = default;

        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getConds() const { return conds; }
        [[nodiscard]] [[maybe_unused]] std::vector<Compound *> getBlocks() const { return blocks; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            auto ret = fmt::format("{}{}If[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second);
            for (unsigned long i = 0; i < conds.size(); i++)
                ret += fmt::format("{}{}Cond: {}\n{}",
                                   prefix + (isTail ? "    " : "│   "), i == conds.size() - 1 ? "└──" : "├──",
                                   conds[i]->toString(srcMgr, prefix + (isTail ? "    " : "│   "), i == conds.size() - 1),
                                   blocks[i]->toString(srcMgr, prefix + (isTail ? "    " : "│   ") + (i == conds.size() - 1 ? "    " : "│   "), true));

            return ret;
        }
    };

    class While : public Statement {
        Expression *cond;
        Compound *block;

    public:
        While(llvm::SMRange Loc, Expression *cond, Compound *block) : Statement(Loc), cond(cond), block(block) {}
        ~While() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getCond() const { return cond; }
        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}While[Line({}-{}):Col({}-{})]:\n{}{}Cond: {}\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               prefix + (isTail ? "    " : "│   "),
                               "└──",
                               cond->toString(srcMgr, prefix, true),
                               block->toString(srcMgr, prefix + (isTail ? "        " : "│       "), true));
        }
    };

    class FuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;
        Compound *body;
        bool varargs;

    public:
        FuncDecl(llvm::SMRange Loc, std::string name, Type *return_type,
                 std::vector<std::pair<std::string, Type *>> parameters, Compound *body, bool varargs) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)),
                                                                                                         body(body), varargs(varargs) {}

        ~FuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] Compound *getBody() const { return body; }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            auto ret = fmt::format("{}{}FuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second,
                                   name);
            for (auto &param: parameters) {
                ret += param.first + ": " + param.second->toString(srcMgr, prefix, isTail);
                if (parameters.back() != param) ret += ", ";
            }
            if (varargs)
                ret += ", ...";
            ret += fmt::format(") -> {}\n{}", return_type->toString(srcMgr, prefix, isTail),
                               body->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
            return ret;
        }
    };

    class ExternFuncDecl : public Statement {
        std::string name;
        Type *return_type;
        std::vector<std::pair<std::string, Type *>> parameters;
        bool varargs;

    public:
        ExternFuncDecl(llvm::SMRange Loc, std::string name, Type *return_type,
                       std::vector<std::pair<std::string, Type *>> parameters, bool varargs) : Statement(Loc), name(std::move(name)), return_type(return_type), parameters(std::move(parameters)), varargs(varargs) {}

        ~ExternFuncDecl() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] Type *getReturnType() const { return return_type; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, Type *>> getParameters() const { return parameters; }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            auto ret = fmt::format("{}{}ExternFuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second,
                                   name);
            for (auto &param: parameters) {
                ret += param.first + ": " + param.second->toString(srcMgr, prefix, isTail);
                if (parameters.back() != param) ret += ", ";
            }
            if (varargs)
                ret += ", ...";
            ret += fmt::format(") -> {}\n", return_type->toString(srcMgr, prefix, isTail));
            return ret;
        }
    };

    class FuncCall : public Expression {
        std::string name;
        std::vector<Expression *> arguments;

    public:
        FuncCall(llvm::SMRange Loc, std::string name, std::vector<Expression *> arguments) : Expression(Loc), name(std::move(name)), arguments(std::move(arguments)) {}

        ~FuncCall() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name; }
        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getArguments() const { return arguments; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            auto ret = name + "(";
            for (auto param: arguments) {
                ret += param->toString(srcMgr, prefix, isTail);
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
        Assignment(llvm::SMRange Loc, Expression *lhs, TokenType op, Expression *rhs) : Statement(Loc), lhs(lhs), op(op), rhs(rhs) {}

        ~Assignment() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getLeftHandSide() const { return lhs; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRightHandSide() const { return rhs; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               lhs->toString(srcMgr, prefix, isTail),
                               std::string{NAMEOF_ENUM(op)},
                               rhs->toString(srcMgr, prefix, isTail));
        }
    };

    class ExpressionStatement : public Statement {
        Expression *expr;

    public:
        ExpressionStatement(llvm::SMRange Loc, Expression *expr) : Statement(Loc), expr(expr) {}
        ~ExpressionStatement() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Expression[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               expr->toString(srcMgr, prefix, isTail));
        }
    };

    class BinaryOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        BinaryOp(llvm::SMRange Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left),
                                                                                         op(op), right(right) {}

        ~BinaryOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return left->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " + right->toString(srcMgr, prefix, isTail);
        }
    };

    class IsOp : public Expression {
        Expression *left;
        TokenType op;
        Type *right;

    public:
        IsOp(llvm::SMRange Loc, Expression *left, TokenType op, Type *right) : Expression(Loc), left(left),
                                                                               op(op), right(right) {}

        ~IsOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Type *getRight() const { return right; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return left->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op)} + " " + right->toString(srcMgr, prefix, isTail);
        }
    };

    class CastOp : public Expression {
        Expression *expr;
        Type *type;

    public:
        CastOp(llvm::SMRange Loc, Expression *expr, Type *type) : Expression(Loc), expr(expr), type(type) {}

        ~CastOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }
        [[nodiscard]] [[maybe_unused]] Type *getType() const { return type; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return expr->toString(srcMgr, prefix, isTail) + " as " + type->toString(srcMgr, prefix, isTail);
        }
    };

    class UnaryOp : public Expression {
        TokenType op;
        Expression *expr;

    public:
        UnaryOp(llvm::SMRange Loc, TokenType op, Expression *expr) : Expression(Loc), op(op), expr(expr) {}
        ~UnaryOp() override = default;

        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return std::string{NAMEOF_ENUM(op)} + expr->toString(srcMgr, prefix, isTail);
        }
    };

    class DotOp : public Expression {
        Expression *left;
        TokenType op;
        Expression *right;

    public:
        DotOp(llvm::SMRange Loc, Expression *left, TokenType op, Expression *right) : Expression(Loc), left(left),
                                                                                      op(op), right(right) {}

        ~DotOp() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left; }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return left->toString(srcMgr, prefix, isTail) + "." + right->toString(srcMgr, prefix, isTail);
        }
    };

    class Else : public Expression {
    public:
        explicit Else(llvm::SMRange Loc) : Expression(Loc) {}
        ~Else() override = default;

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) override {
            return "Else";
        }
    };

    class Break : public Statement {
    public:
        explicit Break(llvm::SMRange Loc) : Statement(Loc) {}
        ~Break() override = default;

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Break[Line({}-{}):Col({}-{})]:\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second);
        }
    };

    class Continue : public Statement {
    public:
        explicit Continue(llvm::SMRange Loc) : Statement(Loc) {}
        ~Continue() override = default;

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Continue[Line({}-{}):Col({}-{})]:\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second);
        }
    };

    class Return : public Statement {
        Expression *value;

    public:
        Return(llvm::SMRange Loc, Expression *value) : Statement(Loc), value(value) {}
        ~Return() override = default;

        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return value; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Return[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               value->toString(srcMgr, prefix, true));
        }
    };

    class Defer : public Statement {
        Statement *stmt;

    public:
        Defer(llvm::SMRange Loc, Statement *stmt) : Statement(Loc), stmt(stmt) {}
        ~Defer() override = default;

        [[nodiscard]] [[maybe_unused]] Statement *getStatement() const { return stmt; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            return fmt::format("{}{}Defer[Line({}-{}):Col({}-{})]:\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               stmt->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
        }
    };

    class Class : public Statement {
        std::string identifier;
        std::vector<VarDecl *> fields;
        std::vector<FuncDecl *> methods;

    public:
        Class(llvm::SMRange Loc, std::string identifier, std::vector<VarDecl *> fields, std::vector<FuncDecl *> methods) : Statement(Loc), identifier(std::move(identifier)), fields(std::move(fields)), methods(std::move(methods)){};
        ~Class() override = default;

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier; }
        [[nodiscard]] [[maybe_unused]] std::vector<VarDecl *> getFields() const { return fields; }
        [[nodiscard]] [[maybe_unused]] std::vector<FuncDecl *> getMethods() const { return methods; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) override {
            std::string fields_str;
            for (auto field: fields)
                fields_str += field->toString(srcMgr, prefix + (isTail ? "    " : "│   "), false);

            std::string methods_str;
            for (auto method: methods)
                methods_str += method->toString(srcMgr, prefix + (isTail ? "    " : "│   "), method == methods.back());
            return fmt::format("{}{}Class[Line({}-{}):Col({}-{})]: {}: \n{}{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               identifier,
                               fields_str,
                               methods_str);
        }
    };
}// namespace lesma