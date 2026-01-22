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
  ASTVisitor(const ASTVisitor&) = delete;
  auto operator=(const ASTVisitor&) -> ASTVisitor& = delete;
  ASTVisitor(ASTVisitor&&) = delete;
  auto operator=(ASTVisitor&&) -> ASTVisitor& = delete;

  virtual auto Visit(const Statement* node) -> void = 0;
  virtual auto Visit(const Compound* node) -> void = 0;
  virtual auto Visit(const Import* node) -> void = 0;
  virtual auto Visit(const Enum* node) -> void = 0;
  virtual auto Visit(const Class* node) -> void = 0;
  virtual auto Visit(const VarDecl* node) -> void = 0;
  virtual auto Visit(const If* node) -> void = 0;
  virtual auto Visit(const While* node) -> void = 0;
  virtual auto Visit(const FuncDecl* node) -> void = 0;
  virtual auto Visit(const ExternFuncDecl* node) -> void = 0;
  virtual auto Visit(const Assignment* node) -> void = 0;
  virtual auto Visit(const ExpressionStatement* node) -> void = 0;
  virtual auto Visit(const Break* node) -> void = 0;
  virtual auto Visit(const Continue* node) -> void = 0;
  virtual auto Visit(const Return* node) -> void = 0;
  virtual auto Visit(const Defer* node) -> void = 0;

  virtual auto Visit(const Expression* node) -> void = 0;
  virtual auto Visit(const Literal* node) -> void = 0;
  virtual auto Visit(const FuncCall* node) -> void = 0;
  virtual auto Visit(const BinaryOp* node) -> void = 0;
  virtual auto Visit(const DotOp* node) -> void = 0;
  virtual auto Visit(const CastOp* node) -> void = 0;
  virtual auto Visit(const IsOp* node) -> void = 0;
  virtual auto Visit(const UnaryOp* node) -> void = 0;
  virtual auto Visit(const Else* node) -> void = 0;

  virtual auto Visit(const TypeExpr* node) -> void = 0;

protected:
  ASTVisitor() = default;
};
} // namespace lesma