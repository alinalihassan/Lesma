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
class ForIn;
class FuncDecl;
class ExternFuncDecl;
class Assignment;
class ExpressionStatement;
class Break;
class Continue;
class Return;
class Defer;
class UnimplementedStatement;
class Expression;
class Literal;
class FuncCall;
class BinaryOp;
class DotOp;
class CastOp;
class IsOp;
class UnaryOp;
class ListLiteral;
class Else;
class TypeExpr;
class ASTVisitor {
public:
  virtual ~ASTVisitor() = default;
  ASTVisitor(const ASTVisitor&) = delete;
  auto operator=(const ASTVisitor&) -> ASTVisitor& = delete;
  ASTVisitor(ASTVisitor&&) = delete;
  auto operator=(ASTVisitor&&) -> ASTVisitor& = delete;

  virtual auto visit(const Statement* node) -> void = 0;
  virtual auto visit(const Compound* node) -> void = 0;
  virtual auto visit(const Import* node) -> void = 0;
  virtual auto visit(const Enum* node) -> void = 0;
  virtual auto visit(const Class* node) -> void = 0;
  virtual auto visit(const VarDecl* node) -> void = 0;
  virtual auto visit(const If* node) -> void = 0;
  virtual auto visit(const While* node) -> void = 0;
  virtual auto visit(const ForIn* node) -> void = 0;
  virtual auto visit(const FuncDecl* node) -> void = 0;
  virtual auto visit(const ExternFuncDecl* node) -> void = 0;
  virtual auto visit(const Assignment* node) -> void = 0;
  virtual auto visit(const ExpressionStatement* node) -> void = 0;
  virtual auto visit(const Break* node) -> void = 0;
  virtual auto visit(const Continue* node) -> void = 0;
  virtual auto visit(const Return* node) -> void = 0;
  virtual auto visit(const Defer* node) -> void = 0;
  virtual auto visit(const UnimplementedStatement* node) -> void = 0;

  virtual auto visit(const Expression* node) -> void = 0;
  virtual auto visit(const Literal* node) -> void = 0;
  virtual auto visit(const FuncCall* node) -> void = 0;
  virtual auto visit(const BinaryOp* node) -> void = 0;
  virtual auto visit(const DotOp* node) -> void = 0;
  virtual auto visit(const CastOp* node) -> void = 0;
  virtual auto visit(const IsOp* node) -> void = 0;
  virtual auto visit(const UnaryOp* node) -> void = 0;
  virtual auto visit(const ListLiteral* node) -> void = 0;
  virtual auto visit(const Else* node) -> void = 0;

  virtual auto visit(const TypeExpr* node) -> void = 0;

protected:
  ASTVisitor() = default;
};
} // namespace lesma