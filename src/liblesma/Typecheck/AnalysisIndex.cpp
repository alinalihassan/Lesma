#include "liblesma/Driver/AnalysisResult.h"

#include <optional>
#include <string>
#include <utility>

#include "llvm/Support/SMLoc.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Symbol/TypeUtils.h"

using namespace lesma;

namespace {
auto makeNameSpan(llvm::SMLoc start, const std::string& name) -> llvm::SMRange {
  if (!start.isValid()) {
    return {};
  }
  llvm::StringRef const text(start.getPointer(), name.size());
  return llvm::SMRange{start, llvm::SMLoc::getFromPointer(text.end())};
}

auto declarationIdentityFromValue(const Value* resolvedSymbol)
    -> std::optional<IndexedDeclarationIdentity> {
  if (resolvedSymbol == nullptr) {
    return std::nullopt;
  }
  llvm::SMRange const declarationSpan = resolvedSymbol->getDeclarationSpan();
  std::string const& declarationFilePath = resolvedSymbol->getDeclarationFilePath();
  if (!declarationSpan.isValid() || declarationFilePath.empty()) {
    return std::nullopt;
  }
  return IndexedDeclarationIdentity{
      .filePath = declarationFilePath,
      .span = declarationSpan,
  };
}

auto appendIndexedOccurrence(AnalysisIndex& index, const std::string& name,
                             std::optional<std::string> dotBase, llvm::SMRange span,
                             bool isTypePosition, bool isMemberAccess, unsigned modifiers,
                             std::optional<IndexedTokenKind> fallbackTokenKind,
                             const Value* resolvedSymbol = nullptr) -> void {
  if (!span.isValid()) {
    return;
  }
  index.symbolOccurrences.push_back(IndexedSymbolOccurrence{
      .name = name,
      .dotBase = std::move(dotBase),
      .span = span,
      .declaration = declarationIdentityFromValue(resolvedSymbol),
      .isTypePosition = isTypePosition,
      .isMemberAccess = isMemberAccess,
      .modifiers = modifiers,
      .fallbackTokenKind = fallbackTokenKind,
  });
}

auto resolvedTypeForExpr(const Expression* expr) -> Type* {
  if (expr == nullptr) {
    return nullptr;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    Value* const resolvedSymbol = lit->getResolvedSymbol();
    return resolvedSymbol != nullptr ? resolvedSymbol->getType() : nullptr;
  }
  if (auto const* typeExpr = dynamic_cast<const TypeExpr*>(expr)) {
    Value* const resolvedSymbol = typeExpr->getResolvedSymbol();
    return resolvedSymbol != nullptr ? resolvedSymbol->getType() : nullptr;
  }
  if (auto const* call = dynamic_cast<const FuncCall*>(expr)) {
    Value* const resolvedSymbol = call->getResolvedSymbol();
    return resolvedSymbol != nullptr && resolvedSymbol->getType() != nullptr
               ? resolvedSymbol->getType()->getReturnType()
               : nullptr;
  }
  if (auto const* castOp = dynamic_cast<const CastOp*>(expr)) {
    return resolvedTypeForExpr(castOp->getType());
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    Type* baseType = resolvedTypeForExpr(dot->getLeft());
    if (baseType == nullptr) {
      return nullptr;
    }
    if (baseType->is(BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
      baseType = baseType->getElementType();
    }
    if (auto const* rightCall = dynamic_cast<const FuncCall*>(dot->getRight())) {
      Value* const resolvedSymbol = rightCall->getResolvedSymbol();
      return resolvedSymbol != nullptr && resolvedSymbol->getType() != nullptr
                 ? resolvedSymbol->getType()->getReturnType()
                 : nullptr;
    }
    auto const* rightLit = dynamic_cast<const Literal*>(dot->getRight());
    if (rightLit == nullptr || rightLit->getType() != TokenType::IDENTIFIER ||
        !baseType->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
      return nullptr;
    }
    return TypeUtils::findTypeInFields(baseType, rightLit->getValue());
  }
  return nullptr;
}

auto memberReceiverName(const Expression* expr) -> std::optional<std::string> {
  auto const* lit = dynamic_cast<const Literal*>(expr);
  if (lit == nullptr || lit->getType() != TokenType::IDENTIFIER) {
    return std::nullopt;
  }
  return lit->getValue();
}

auto memberBaseName(const Expression* expr) -> std::optional<std::string> {
  std::optional<std::string> receiverName = memberReceiverName(expr);
  if (!receiverName.has_value()) {
    return std::nullopt;
  }
  Type* resolvedType = resolvedTypeForExpr(expr);
  if (resolvedType == nullptr) {
    return std::nullopt;
  }
  if (resolvedType->is(BaseType::TY_PTR) && resolvedType->getElementType() != nullptr) {
    resolvedType = resolvedType->getElementType();
  }
  if (!resolvedType->is(BaseType::TY_ENUM)) {
    return std::nullopt;
  }
  return receiverName;
}

auto collectIndexFromTypeExpr(const TypeExpr* typeExpr, AnalysisIndex& index) -> void;
auto collectIndexFromExpr(const Expression* expr, AnalysisIndex& index) -> void;
auto collectIndexFromStmt(const Statement* stmt, AnalysisIndex& index, bool inClass) -> void;

template <typename FuncLike>
auto collectIndexFromFuncLike(const FuncLike* node, AnalysisIndex& index, bool inClass) -> void {
  if (node == nullptr) {
    return;
  }
  FuncLikeDeclView const view = makeFuncLikeDeclView(node);
  appendIndexedOccurrence(index, view.name, std::nullopt, view.nameSpan, false, false,
                          analysis_index_modifier::DECLARATION,
                          inClass ? IndexedTokenKind::Method : IndexedTokenKind::Function,
                          view.resolvedSymbol);
  if (view.genericParams != nullptr) {
    for (const GenericParamDecl& genericParam : *view.genericParams) {
      appendIndexedOccurrence(index, genericParam.name, std::nullopt, genericParam.span, true,
                              false, analysis_index_modifier::DECLARATION,
                              IndexedTokenKind::TypeParameter,
                              view.genericScope != nullptr ? view.genericScope->lookup(genericParam.name)
                                                           : nullptr);
    }
  }
  for (Parameter* param : view.parameters) {
    if (param != nullptr && param->nameSpan.isValid()) {
      appendIndexedOccurrence(index, param->name, std::nullopt, param->nameSpan, false, false,
                              analysis_index_modifier::DECLARATION, IndexedTokenKind::Parameter,
                              param->getResolvedSymbol());
    }
    if (param != nullptr) {
      collectIndexFromTypeExpr(param->type.get(), index);
    }
  }
  collectIndexFromTypeExpr(view.returnType, index);
  if (view.body != nullptr) {
    collectIndexFromStmt(view.body, index, false);
  }
}

auto collectIndexFromTypeExpr(const TypeExpr* typeExpr, AnalysisIndex& index) -> void {
  if (typeExpr == nullptr) {
    return;
  }
  if (typeExpr->getType() != TokenType::PTR_TYPE && typeExpr->getType() != TokenType::FUNC_TYPE) {
    appendIndexedOccurrence(index, typeExpr->getName(), std::nullopt, typeExpr->getSpan(), true,
                            false, 0U, IndexedTokenKind::Type, typeExpr->getResolvedSymbol());
  }
  collectIndexFromTypeExpr(typeExpr->getElementType(), index);
  for (TypeExpr* param : typeExpr->getParams()) {
    collectIndexFromTypeExpr(param, index);
  }
  collectIndexFromTypeExpr(typeExpr->getReturnType(), index);
}

auto collectIndexFromExpr(const Expression* expr, AnalysisIndex& index) -> void {
  if (expr == nullptr) {
    return;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    if (lit->getType() == TokenType::IDENTIFIER) {
      appendIndexedOccurrence(index, lit->getValue(), std::nullopt, lit->getSpan(), false, false,
                              0U, IndexedTokenKind::Variable, lit->getResolvedSymbol());
    }
    return;
  }
  if (auto const* call = dynamic_cast<const FuncCall*>(expr)) {
    appendIndexedOccurrence(index, call->getName(), std::nullopt,
                            makeNameSpan(call->getSpan().Start, call->getName()), false, false, 0U,
                            IndexedTokenKind::Function, call->getResolvedSymbol());
    for (TypeExpr* typeArg : call->getExplicitTypeArgs()) {
      collectIndexFromTypeExpr(typeArg, index);
    }
    for (Expression* arg : call->getArguments()) {
      collectIndexFromExpr(arg, index);
    }
    return;
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    collectIndexFromExpr(dot->getLeft(), index);
    std::optional<std::string> const dotBase = memberReceiverName(dot->getLeft());
    std::optional<std::string> const enumBase = memberBaseName(dot->getLeft());
    bool const isEnumMemberAccess = enumBase.has_value();
    if (auto const* rightLit = dynamic_cast<const Literal*>(dot->getRight())) {
      if (rightLit->getType() == TokenType::IDENTIFIER) {
        if (isEnumMemberAccess) {
          index.enumMemberOccurrences.push_back(IndexedEnumMemberOccurrence{
              .enumName = *enumBase,
              .memberName = rightLit->getValue(),
              .span = rightLit->getSpan(),
          });
        }
        appendIndexedOccurrence(index, rightLit->getValue(), dotBase, rightLit->getSpan(), false,
                                true, 0U, isEnumMemberAccess ? IndexedTokenKind::EnumMember
                                                             : IndexedTokenKind::Property,
                                rightLit->getResolvedSymbol());
        return;
      }
    }
    if (auto const* rightCall = dynamic_cast<const FuncCall*>(dot->getRight())) {
      appendIndexedOccurrence(index, rightCall->getName(), dotBase,
                              makeNameSpan(rightCall->getSpan().Start, rightCall->getName()), false,
                              true, 0U, IndexedTokenKind::Method,
                              rightCall->getResolvedSymbol());
      for (TypeExpr* typeArg : rightCall->getExplicitTypeArgs()) {
        collectIndexFromTypeExpr(typeArg, index);
      }
      for (Expression* arg : rightCall->getArguments()) {
        collectIndexFromExpr(arg, index);
      }
      return;
    }
    collectIndexFromExpr(dot->getRight(), index);
    return;
  }
  if (auto const* binary = dynamic_cast<const BinaryOp*>(expr)) {
    collectIndexFromExpr(binary->getLeft(), index);
    collectIndexFromExpr(binary->getRight(), index);
    return;
  }
  if (auto const* unary = dynamic_cast<const UnaryOp*>(expr)) {
    collectIndexFromExpr(unary->getExpression(), index);
    return;
  }
  if (auto const* castOp = dynamic_cast<const CastOp*>(expr)) {
    collectIndexFromExpr(castOp->getExpression(), index);
    collectIndexFromTypeExpr(castOp->getType(), index);
    return;
  }
  if (auto const* isOp = dynamic_cast<const IsOp*>(expr)) {
    collectIndexFromExpr(isOp->getLeft(), index);
    collectIndexFromTypeExpr(isOp->getRight(), index);
  }
}

auto collectIndexFromStmt(const Statement* stmt, AnalysisIndex& index, bool inClass) -> void {
  if (stmt == nullptr) {
    return;
  }
  if (auto const* varDecl = dynamic_cast<const VarDecl*>(stmt)) {
    if (varDecl->getIdentifier() != nullptr) {
      appendIndexedOccurrence(index, varDecl->getIdentifier()->getValue(), std::nullopt,
                              varDecl->getIdentifier()->getSpan(), false, false,
                              analysis_index_modifier::DECLARATION,
                              inClass ? IndexedTokenKind::Property : IndexedTokenKind::Variable,
                              varDecl->getResolvedSymbol());
    }
    collectIndexFromTypeExpr(varDecl->getType(), index);
    collectIndexFromExpr(varDecl->getValue(), index);
    return;
  }
  if (auto const* func = dynamic_cast<const FuncDecl*>(stmt)) {
    collectIndexFromFuncLike(func, index, inClass);
    return;
  }
  if (auto const* ext = dynamic_cast<const ExternFuncDecl*>(stmt)) {
    collectIndexFromFuncLike(ext, index, false);
    return;
  }
  if (auto const* klass = dynamic_cast<const Class*>(stmt)) {
    appendIndexedOccurrence(index, klass->getIdentifier(), std::nullopt, klass->getNameSpan(), true,
                            false, analysis_index_modifier::DECLARATION, IndexedTokenKind::Class,
                            klass->getResolvedSymbol());
    for (const GenericParamDecl& genericParam : klass->getGenericParamDecls()) {
      appendIndexedOccurrence(index, genericParam.name, std::nullopt, genericParam.span, true, false,
                              analysis_index_modifier::DECLARATION,
                              IndexedTokenKind::TypeParameter,
                              klass->getGenericScope() != nullptr
                                  ? klass->getGenericScope()->lookup(genericParam.name)
                                  : nullptr);
    }
    for (VarDecl* field : klass->getFields()) {
      collectIndexFromStmt(field, index, true);
    }
    for (FuncDecl* method : klass->getMethods()) {
      collectIndexFromStmt(method, index, true);
    }
    return;
  }
  if (auto const* enumNode = dynamic_cast<const Enum*>(stmt)) {
    appendIndexedOccurrence(index, enumNode->getIdentifier(), std::nullopt, enumNode->getNameSpan(),
                            true, false, analysis_index_modifier::DECLARATION,
                            IndexedTokenKind::Enum, enumNode->getResolvedSymbol());
    for (const NamedSpan& valueDecl : getEnumValueDecls(enumNode)) {
      appendIndexedOccurrence(index, valueDecl.name, std::nullopt, valueDecl.span, false, false,
                              analysis_index_modifier::DECLARATION,
                              IndexedTokenKind::EnumMember);
      index.enumMemberOccurrences.push_back(IndexedEnumMemberOccurrence{
          .enumName = enumNode->getIdentifier(),
          .memberName = valueDecl.name,
          .span = valueDecl.span,
      });
    }
    return;
  }
  if (auto const* importNode = dynamic_cast<const Import*>(stmt)) {
    llvm::SMRange const aliasSpan = importNode->getAliasSpan();
    for (const NamedSpan& binding : getImportLocalBindings(importNode)) {
      bool const isModuleAlias =
          aliasSpan.isValid() && binding.name == importNode->getAlias() && binding.span.isValid() &&
          binding.span.Start == aliasSpan.Start && binding.span.End == aliasSpan.End;
      appendIndexedOccurrence(index, binding.name, std::nullopt, binding.span, false, false,
                              analysis_index_modifier::DECLARATION,
                              isModuleAlias
                                  ? std::optional<IndexedTokenKind>(IndexedTokenKind::Namespace)
                                  : std::nullopt);
    }
    return;
  }
  if (auto const* exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
    collectIndexFromExpr(exprStmt->getExpression(), index);
    return;
  }
  if (auto const* ifNode = dynamic_cast<const If*>(stmt)) {
    for (Expression* cond : ifNode->getConds()) {
      collectIndexFromExpr(cond, index);
    }
    for (Compound* block : ifNode->getBlocks()) {
      collectIndexFromStmt(block, index, false);
    }
    return;
  }
  if (auto const* whileNode = dynamic_cast<const While*>(stmt)) {
    collectIndexFromExpr(whileNode->getCond(), index);
    collectIndexFromStmt(whileNode->getBlock(), index, false);
    return;
  }
  if (auto const* assign = dynamic_cast<const Assignment*>(stmt)) {
    collectIndexFromExpr(assign->getLeftHandSide(), index);
    collectIndexFromExpr(assign->getRightHandSide(), index);
    return;
  }
  if (auto const* ret = dynamic_cast<const Return*>(stmt)) {
    collectIndexFromExpr(ret->getValue(), index);
    return;
  }
  if (auto const* defer = dynamic_cast<const Defer*>(stmt)) {
    collectIndexFromStmt(defer->getStatement(), index, false);
    return;
  }
  if (auto const* compound = dynamic_cast<const Compound*>(stmt)) {
    for (Statement* child : compound->getChildren()) {
      collectIndexFromStmt(child, index, false);
    }
  }
}
} // namespace

auto lesma::buildAnalysisIndex(const Compound* ast, llvm::SourceMgr* /*srcMgr*/, unsigned /*bufferId*/)
    -> AnalysisIndex {
  AnalysisIndex index;
  if (ast == nullptr) {
    return index;
  }
  for (Statement* stmt : ast->getChildren()) {
    collectIndexFromStmt(stmt, index, false);
  }
  return index;
}
