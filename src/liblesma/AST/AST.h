#pragma once

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "fmt/core.h"
#include "fmt/format.h"
#include "nameof.hpp"

#include "liblesma/AST/ASTVisitor.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {
    class AST {
        llvm::SMRange loc_;

    public:
        explicit AST(llvm::SMRange Loc) : loc_(Loc) {}
        virtual ~AST() = default;
        AST(const AST &) = delete;
        AST &operator=(const AST &) = delete;
        AST(AST &&) = default;
        AST &operator=(AST &&) = default;
        virtual void accept(ASTVisitor &visitor) const = 0;

        [[nodiscard]] [[maybe_unused]] llvm::SMRange getSpan() const { return loc_; }
        [[nodiscard]] [[maybe_unused]] llvm::SMLoc getStart() const { return loc_.Start; }
        [[nodiscard]] [[maybe_unused]] llvm::SMLoc getEnd() const { return loc_.End; }

        virtual std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const {
            return fmt::format("{}{}AST[Line({}-{}):Col({}-{})]:\n",
                               prefix, (isTail ? "└──" : "├──"),
                               srcMgr->getLineAndColumn(loc_.Start).first,
                               srcMgr->getLineAndColumn(loc_.End).first,
                               srcMgr->getLineAndColumn(loc_.Start).second,
                               srcMgr->getLineAndColumn(loc_.End).second);
        }
    };

    class Expression : public AST {
    public:
        explicit Expression(llvm::SMRange Loc) : AST(Loc) {}
        ~Expression() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }
    };

    class Statement : public AST {
    public:
        explicit Statement(llvm::SMRange Loc) : AST(Loc) {}
        ~Statement() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }
    };


    class Literal : public Expression {
        std::string value_;
        TokenType type_;

    public:
        Literal(llvm::SMRange Loc, std::string value, TokenType type) : Expression(Loc), value_(std::move(value)),
                                                                        type_(type) {}
        ~Literal() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getValue() const { return value_; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type_; }

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) const override {
            if (type_ == TokenType::STRING) {
                return '"' + value_ + '"';
            }
            if (type_ == TokenType::NIL || type_ == TokenType::INTEGER || type_ == TokenType::DOUBLE ||
                type_ == TokenType::IDENTIFIER || type_ == TokenType::BOOL) {
                return value_;
            }
            return "Unknown literal";
        }
    };

    class Compound : public Statement {
        std::vector<std::unique_ptr<Statement>> children_;

    public:
        explicit Compound(llvm::SMRange Loc) : Statement(Loc) {}
        explicit Compound(llvm::SMRange Loc, std::vector<std::unique_ptr<Statement>> children) : Statement(Loc), children_(std::move(children)) {}
        ~Compound() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::vector<Statement *> getChildren() const {
            std::vector<Statement *> result;
            result.reserve(children_.size());
            for (const auto &child: children_) {
                result.push_back(child.get());
            }
            return result;
        }

        [[maybe_unused]] void addChildren(std::unique_ptr<Statement> ast) {
            children_.push_back(std::move(ast));
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            auto ret = fmt::format("{}{}Compound[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second);
            for (const auto &child: children_) {
                ret += child->toString(srcMgr, prefix + (isTail ? "    " : "│   "), child.get() == children_.back().get());
            }
            return ret;
        }
    };

    class TypeExpr : public Expression {
        std::string name_;
        TokenType type_;

        // Pointer fields
        std::unique_ptr<TypeExpr> element_type_;

        // Function fields
        std::vector<std::unique_ptr<TypeExpr>> params_;
        std::unique_ptr<TypeExpr> ret_;

    public:
        TypeExpr(llvm::SMRange Loc, std::string name, TokenType type) : Expression(Loc), name_(std::move(name)), type_(type), element_type_(nullptr), ret_(nullptr) {}
        TypeExpr(llvm::SMRange Loc, std::string name, TokenType type, std::unique_ptr<TypeExpr> elementType) : Expression(Loc), name_(std::move(name)), type_(type), element_type_(std::move(elementType)), ret_(nullptr) {}
        TypeExpr(llvm::SMRange Loc, std::string name, TokenType type, std::vector<std::unique_ptr<TypeExpr>> params, std::unique_ptr<TypeExpr> ret) : Expression(Loc), name_(std::move(name)), type_(type), element_type_(nullptr), params_(std::move(params)), ret_(std::move(ret)) {}
        ~TypeExpr() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name_; }
        [[nodiscard]] [[maybe_unused]] TokenType getType() const { return type_; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getElementType() const { return element_type_.get(); }
        [[nodiscard]] [[maybe_unused]] std::vector<TypeExpr *> getParams() const {
            std::vector<TypeExpr *> result;
            result.reserve(params_.size());
            for (const auto &param: params_) {
                result.push_back(param.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return ret_.get(); }

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) const override {
            return name_;
        }
    };

    class Enum : public Statement {
        std::string identifier_;
        std::vector<std::string> values_;
        bool exported_;

    public:
        Enum(llvm::SMRange Loc, std::string identifier, std::vector<std::string> values, bool exported) : Statement(Loc), identifier_(std::move(identifier)), values_(std::move(values)), exported_(exported) {};
        ~Enum() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier_; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::string> getValues() const { return values_; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            std::ostringstream imploded;
            std::copy(values_.begin(), values_.end(),
                      std::ostream_iterator<std::string>(imploded, ", "));
            return fmt::format("{}{}Enum[Line({}-{}):Col({}-{})]: {} with: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               identifier_,
                               imploded.str());
        }
    };

    class Import : public Statement {
        std::string file_path_;
        std::string alias_;
        std::vector<std::pair<std::string, std::string>> imported_names_;
        bool std_;
        bool import_all_;
        bool import_to_scope_;

    public:
        Import(llvm::SMRange Loc, std::string file_path, std::string alias, bool std, bool import_all, bool import_to_scope, std::vector<std::pair<std::string, std::string>> imported_names) : Statement(Loc), file_path_(std::move(file_path)), alias_(std::move(alias)), imported_names_(std::move(imported_names)), std_(std), import_all_(import_all), import_to_scope_(import_to_scope) {};
        ~Import() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getFilePath() const { return file_path_; }
        [[nodiscard]] [[maybe_unused]] std::string getAlias() const { return alias_; }
        [[nodiscard]] [[maybe_unused]] bool getImportAll() const { return import_all_; }
        [[nodiscard]] [[maybe_unused]] bool getImportScope() const { return import_to_scope_; }
        [[nodiscard]] [[maybe_unused]] std::vector<std::pair<std::string, std::string>> getImportedNames() const { return imported_names_; }
        [[nodiscard]] [[maybe_unused]] bool isStd() const { return std_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Import[Line({}-{}):Col({}-{})]: {} as {} from {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               file_path_,
                               alias_,
                               std_ ? "std" : "file");
        }
    };

    class VarDecl : public Statement {
        std::unique_ptr<Literal> var_;
        std::unique_ptr<TypeExpr> type_;
        std::unique_ptr<Expression> expr_;
        bool mutable_;

    public:
        VarDecl(llvm::SMRange Loc, std::unique_ptr<Literal> var, std::unique_ptr<TypeExpr> type, std::unique_ptr<Expression> expr, bool mutable_val) : Statement(Loc), var_(std::move(var)), type_(std::move(type)), expr_(std::move(expr)), mutable_(mutable_val) {}
        ~VarDecl() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Literal *getIdentifier() const { return var_.get(); }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getType() const { return type_.get(); }
        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return expr_.get(); }
        [[nodiscard]] [[maybe_unused]] bool getMutability() const { return mutable_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}VarDecl[Line({}-{}):Col({}-{})]: {}{}{}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               var_->toString(srcMgr, prefix, isTail),
                               (type_ ? ": " + type_->toString(srcMgr, prefix, isTail) : ""),
                               (expr_ ? " = " + expr_->toString(srcMgr, prefix, isTail) : ""));
        }
    };

    class If : public Statement {
        std::vector<std::unique_ptr<Expression>> conds_;
        std::vector<std::unique_ptr<Compound>> blocks_;

    public:
        If(llvm::SMRange Loc, std::vector<std::unique_ptr<Expression>> conds, std::vector<std::unique_ptr<Compound>> blocks) : Statement(Loc),
                                                                                                                               conds_(std::move(conds)),
                                                                                                                               blocks_(std::move(blocks)) {}
        ~If() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getConds() const {
            std::vector<Expression *> result;
            result.reserve(conds_.size());
            for (const auto &cond: conds_) {
                result.push_back(cond.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] std::vector<Compound *> getBlocks() const {
            std::vector<Compound *> result;
            result.reserve(blocks_.size());
            for (const auto &block: blocks_) {
                result.push_back(block.get());
            }
            return result;
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            auto ret = fmt::format("{}{}If[Line({}-{}):Col({}-{})]:\n",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second);
            for (unsigned long i = 0; i < conds_.size(); i++) {
                ret += fmt::format("{}{}Cond: {}\n{}",
                                   prefix + (isTail ? "    " : "│   "), i == conds_.size() - 1 ? "└──" : "├──",
                                   conds_[i]->toString(srcMgr, prefix + (isTail ? "    " : "│   "), i == conds_.size() - 1),
                                   blocks_[i]->toString(srcMgr, prefix + (isTail ? "    " : "│   ") + (i == conds_.size() - 1 ? "    " : "│   "), true));
            }

            return ret;
        }
    };

    class While : public Statement {
        std::unique_ptr<Expression> cond_;
        std::unique_ptr<Compound> block_;

    public:
        While(llvm::SMRange Loc, std::unique_ptr<Expression> cond, std::unique_ptr<Compound> block) : Statement(Loc), cond_(std::move(cond)), block_(std::move(block)) {}
        ~While() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getCond() const { return cond_.get(); }
        [[nodiscard]] [[maybe_unused]] Compound *getBlock() const { return block_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}While[Line({}-{}):Col({}-{})]:\n{}{}Cond: {}\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               prefix + (isTail ? "    " : "│   "),
                               "└──",
                               cond_->toString(srcMgr, prefix, true),
                               block_->toString(srcMgr, prefix + (isTail ? "        " : "│       "), true));
        }
    };

    class Parameter {
    public:
        std::string name;
        std::unique_ptr<TypeExpr> type;
        bool optional;
        std::unique_ptr<Expression> default_val;

        Parameter(std::string name, std::unique_ptr<TypeExpr> type = nullptr, bool optional = false, std::unique_ptr<Expression> default_val = nullptr) : name(std::move(name)), type(std::move(type)), optional(optional), default_val(std::move(default_val)) {}

        ~Parameter() = default;
    };

    class FuncDecl : public Statement {
        std::string name_;
        std::unique_ptr<TypeExpr> return_type_;
        std::vector<std::unique_ptr<Parameter>> parameters_;
        std::unique_ptr<Compound> body_;
        bool varargs_;
        bool exported_;

    public:
        FuncDecl(llvm::SMRange Loc, std::string name, std::unique_ptr<TypeExpr> return_type,
                 std::vector<std::unique_ptr<Parameter>> parameters, std::unique_ptr<Compound> body, bool varargs, bool exported) : Statement(Loc), name_(std::move(name)), return_type_(std::move(return_type)), parameters_(std::move(parameters)),
                                                                                                                                    body_(std::move(body)), varargs_(varargs), exported_(exported) {}
        ~FuncDecl() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name_; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return return_type_.get(); }
        [[nodiscard]] [[maybe_unused]] std::vector<Parameter *> getParameters() const {
            std::vector<Parameter *> result;
            result.reserve(parameters_.size());
            for (const auto &param: parameters_) {
                result.push_back(param.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] Compound *getBody() const { return body_.get(); }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs_; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            auto ret = fmt::format("{}{}FuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second,
                                   name_);
            for (const auto &param: parameters_) {
                ret += param->name + ": " + param->type->toString(srcMgr, prefix, isTail) +
                       (param->default_val == nullptr ? "" : fmt::format("= {}", param->default_val->toString(srcMgr, prefix, isTail)));
                if (param.get() != parameters_.back().get()) {
                    ret += ", ";
                }
            }
            if (varargs_) {
                ret += ", ...";
            }
            ret += fmt::format(") -> {}\n{}", return_type_->toString(srcMgr, prefix, isTail),
                               body_->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
            return ret;
        }
    };

    class ExternFuncDecl : public Statement {
        std::string name_;
        std::unique_ptr<TypeExpr> return_type_;
        std::vector<std::unique_ptr<Parameter>> parameters_;
        bool varargs_;
        bool exported_;

    public:
        ExternFuncDecl(llvm::SMRange Loc, std::string name, std::unique_ptr<TypeExpr> return_type,
                       std::vector<std::unique_ptr<Parameter>> parameters, bool varargs, bool exported) : Statement(Loc), name_(std::move(name)), return_type_(std::move(return_type)), parameters_(std::move(parameters)), varargs_(varargs), exported_(exported) {}

        ~ExternFuncDecl() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name_; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getReturnType() const { return return_type_.get(); }
        [[nodiscard]] [[maybe_unused]] std::vector<Parameter *> getParameters() const {
            std::vector<Parameter *> result;
            result.reserve(parameters_.size());
            for (const auto &param: parameters_) {
                result.push_back(param.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] bool getVarArgs() const { return varargs_; }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            auto ret = fmt::format("{}{}ExternFuncDecl[Line({}-{}):Col({}-{})]: {}(",
                                   prefix, isTail ? "└──" : "├──",
                                   srcMgr->getLineAndColumn(getStart()).first,
                                   srcMgr->getLineAndColumn(getEnd()).first,
                                   srcMgr->getLineAndColumn(getStart()).second,
                                   srcMgr->getLineAndColumn(getEnd()).second,
                                   name_);
            for (const auto &param: parameters_) {
                ret += param->name + ": " + param->type->toString(srcMgr, prefix, isTail) +
                       (param->default_val == nullptr ? "" : fmt::format("= {}", param->default_val->toString(srcMgr, prefix, isTail)));
                if (param.get() != parameters_.back().get()) {
                    ret += ", ";
                }
            }
            if (varargs_) {
                ret += ", ...";
            }
            ret += fmt::format(") -> {}\n", return_type_->toString(srcMgr, prefix, isTail));
            return ret;
        }
    };

    class FuncCall : public Expression {
        std::string name_;
        std::vector<std::unique_ptr<Expression>> arguments_;

    public:
        FuncCall(llvm::SMRange Loc, std::string name, std::vector<std::unique_ptr<Expression>> arguments) : Expression(Loc), name_(std::move(name)), arguments_(std::move(arguments)) {}
        ~FuncCall() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getName() const { return name_; }
        [[nodiscard]] [[maybe_unused]] std::vector<Expression *> getArguments() const {
            std::vector<Expression *> result;
            result.reserve(arguments_.size());
            for (const auto &arg: arguments_) {
                result.push_back(arg.get());
            }
            return result;
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            auto ret = name_ + "(";
            for (const auto &param: arguments_) {
                ret += param->toString(srcMgr, prefix, isTail);
                if (param.get() != arguments_.back().get()) {
                    ret += ", ";
                }
            }
            ret += ")";
            return ret;
        }
    };

    class Assignment : public Statement {
        std::unique_ptr<Expression> lhs_;
        TokenType op_;
        std::unique_ptr<Expression> rhs_;

    public:
        Assignment(llvm::SMRange Loc, std::unique_ptr<Expression> lhs, TokenType op, std::unique_ptr<Expression> rhs) : Statement(Loc), lhs_(std::move(lhs)), op_(op), rhs_(std::move(rhs)) {}
        ~Assignment() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeftHandSide() const { return lhs_.get(); }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op_; }
        [[nodiscard]] [[maybe_unused]] Expression *getRightHandSide() const { return rhs_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Assignment[Line({}-{}):Col({}-{})]: {} {} {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               lhs_->toString(srcMgr, prefix, isTail),
                               std::string{NAMEOF_ENUM(op_)},
                               rhs_->toString(srcMgr, prefix, isTail));
        }
    };

    class ExpressionStatement : public Statement {
        std::unique_ptr<Expression> expr_;

    public:
        ExpressionStatement(llvm::SMRange Loc, std::unique_ptr<Expression> expr) : Statement(Loc), expr_(std::move(expr)) {}
        ~ExpressionStatement() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Expression[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               expr_->toString(srcMgr, prefix, isTail));
        }
    };

    class BinaryOp : public Expression {
        std::unique_ptr<Expression> left_;
        TokenType op_;
        std::unique_ptr<Expression> right_;

    public:
        BinaryOp(llvm::SMRange Loc, std::unique_ptr<Expression> left, TokenType op, std::unique_ptr<Expression> right) : Expression(Loc), left_(std::move(left)), op_(op), right_(std::move(right)) {}
        ~BinaryOp() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left_.get(); }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op_; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return left_->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op_)} + " " + right_->toString(srcMgr, prefix, isTail);
        }
    };

    class IsOp : public Expression {
        std::unique_ptr<Expression> left_;
        TokenType op_;
        std::unique_ptr<TypeExpr> right_;

    public:
        IsOp(llvm::SMRange Loc, std::unique_ptr<Expression> left, TokenType op, std::unique_ptr<TypeExpr> right) : Expression(Loc), left_(std::move(left)), op_(op), right_(std::move(right)) {}
        ~IsOp() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left_.get(); }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op_; }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getRight() const { return right_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return left_->toString(srcMgr, prefix, isTail) + " " + std::string{NAMEOF_ENUM(op_)} + " " + right_->toString(srcMgr, prefix, isTail);
        }
    };

    class CastOp : public Expression {
        std::unique_ptr<Expression> expr_;
        std::unique_ptr<TypeExpr> type_;

    public:
        CastOp(llvm::SMRange Loc, std::unique_ptr<Expression> expr, std::unique_ptr<TypeExpr> type) : Expression(Loc), expr_(std::move(expr)), type_(std::move(type)) {}
        ~CastOp() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr_.get(); }
        [[nodiscard]] [[maybe_unused]] TypeExpr *getType() const { return type_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return expr_->toString(srcMgr, prefix, isTail) + " as " + type_->toString(srcMgr, prefix, isTail);
        }
    };

    class UnaryOp : public Expression {
        TokenType op_;
        std::unique_ptr<Expression> expr_;

    public:
        UnaryOp(llvm::SMRange Loc, TokenType op, std::unique_ptr<Expression> expr) : Expression(Loc), op_(op), expr_(std::move(expr)) {}
        ~UnaryOp() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op_; }
        [[nodiscard]] [[maybe_unused]] Expression *getExpression() const { return expr_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return std::string{NAMEOF_ENUM(op_)} + expr_->toString(srcMgr, prefix, isTail);
        }
    };

    class DotOp : public Expression {
        std::unique_ptr<Expression> left_;
        TokenType op_;
        std::unique_ptr<Expression> right_;

    public:
        DotOp(llvm::SMRange Loc, std::unique_ptr<Expression> left, TokenType op, std::unique_ptr<Expression> right) : Expression(Loc), left_(std::move(left)), op_(op), right_(std::move(right)) {}
        ~DotOp() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getLeft() const { return left_.get(); }
        [[nodiscard]] [[maybe_unused]] TokenType getOperator() const { return op_; }
        [[nodiscard]] [[maybe_unused]] Expression *getRight() const { return right_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return left_->toString(srcMgr, prefix, isTail) + "." + right_->toString(srcMgr, prefix, isTail);
        }
    };

    class Else : public Expression {
    public:
        explicit Else(llvm::SMRange Loc) : Expression(Loc) {}
        ~Else() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        std::string toString(llvm::SourceMgr * /*srcMgr*/, const std::string & /*prefix*/, bool /*isTail*/) const override {
            return "Else";
        }
    };

    class Break : public Statement {
    public:
        explicit Break(llvm::SMRange Loc) : Statement(Loc) {}
        ~Break() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
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
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Continue[Line({}-{}):Col({}-{})]:\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second);
        }
    };

    class Return : public Statement {
        std::unique_ptr<Expression> value_;

    public:
        Return(llvm::SMRange Loc, std::unique_ptr<Expression> value) : Statement(Loc), value_(std::move(value)) {}
        ~Return() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Expression *getValue() const { return value_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Return[Line({}-{}):Col({}-{})]: {}\n",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               value_ ? value_->toString(srcMgr, prefix, true) : "void");
        }
    };

    class Defer : public Statement {
        std::unique_ptr<Statement> stmt_;

    public:
        Defer(llvm::SMRange Loc, std::unique_ptr<Statement> stmt) : Statement(Loc), stmt_(std::move(stmt)) {}
        ~Defer() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] Statement *getStatement() const { return stmt_.get(); }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            return fmt::format("{}{}Defer[Line({}-{}):Col({}-{})]:\n{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               stmt_->toString(srcMgr, prefix + (isTail ? "    " : "│   "), true));
        }
    };

    class Class : public Statement {
        std::string identifier_;
        std::vector<std::unique_ptr<VarDecl>> fields_;
        std::vector<std::unique_ptr<FuncDecl>> methods_;
        bool exported_;

    public:
        Class(llvm::SMRange Loc, std::string identifier, std::vector<std::unique_ptr<VarDecl>> fields, std::vector<std::unique_ptr<FuncDecl>> methods, bool exported) : Statement(Loc), identifier_(std::move(identifier)), fields_(std::move(fields)), methods_(std::move(methods)), exported_(exported) {};
        ~Class() override = default;
        void accept(ASTVisitor &visitor) const override {
            visitor.visit(this);
        }

        [[nodiscard]] [[maybe_unused]] std::string getIdentifier() const { return identifier_; }
        [[nodiscard]] [[maybe_unused]] std::vector<VarDecl *> getFields() const {
            std::vector<VarDecl *> result;
            result.reserve(fields_.size());
            for (const auto &field: fields_) {
                result.push_back(field.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] std::vector<FuncDecl *> getMethods() const {
            std::vector<FuncDecl *> result;
            result.reserve(methods_.size());
            for (const auto &method: methods_) {
                result.push_back(method.get());
            }
            return result;
        }
        [[nodiscard]] [[maybe_unused]] bool isExported() const { return exported_; }

        std::string toString(llvm::SourceMgr *srcMgr, const std::string &prefix, bool isTail) const override {
            std::string fieldsStr;
            for (const auto &field: fields_) {
                fieldsStr += field->toString(srcMgr, prefix + (isTail ? "    " : "│   "), false);
            }

            std::string methodsStr;
            for (const auto &method: methods_) {
                methodsStr += method->toString(srcMgr, prefix + (isTail ? "    " : "│   "), method.get() == methods_.back().get());
            }
            return fmt::format("{}{}Class[Line({}-{}):Col({}-{})]: {}: \n{}{}",
                               prefix, isTail ? "└──" : "├──",
                               srcMgr->getLineAndColumn(getStart()).first,
                               srcMgr->getLineAndColumn(getEnd()).first,
                               srcMgr->getLineAndColumn(getStart()).second,
                               srcMgr->getLineAndColumn(getEnd()).second,
                               identifier_,
                               fieldsStr,
                               methodsStr);
        }
    };
}// namespace lesma
