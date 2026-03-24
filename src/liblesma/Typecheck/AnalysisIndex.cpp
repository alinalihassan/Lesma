#include <optional>
#include <string>
#include <utility>

#include "llvm/Support/SMLoc.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/AnalysisResult.h"
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

auto declarationIdentityFromField(const Field* field) -> std::optional<IndexedDeclarationIdentity> {
  if (field == nullptr || !field->getDeclarationSpan().isValid() ||
      field->getDeclarationFilePath().empty()) {
    return std::nullopt;
  }
  return IndexedDeclarationIdentity{
      .filePath = field->getDeclarationFilePath(),
      .span = field->getDeclarationSpan(),
  };
}

auto indexedTokenKindFromDeclarationKind(ValueDeclarationKind declarationKind)
    -> std::optional<IndexedTokenKind> {
  switch (declarationKind) {
  case ValueDeclarationKind::UNKNOWN:
    return std::nullopt;
  case ValueDeclarationKind::NAMESPACE:
    return IndexedTokenKind::Namespace;
  case ValueDeclarationKind::CLASS:
    return IndexedTokenKind::Class;
  case ValueDeclarationKind::ENUM:
    return IndexedTokenKind::Enum;
  case ValueDeclarationKind::ENUM_MEMBER:
    return IndexedTokenKind::EnumMember;
  case ValueDeclarationKind::TYPE:
    return IndexedTokenKind::Type;
  case ValueDeclarationKind::TRAIT:
    return IndexedTokenKind::Type;
  case ValueDeclarationKind::TYPE_PARAMETER:
    return IndexedTokenKind::TypeParameter;
  case ValueDeclarationKind::FUNCTION:
    return IndexedTokenKind::Function;
  case ValueDeclarationKind::METHOD:
    return IndexedTokenKind::Method;
  case ValueDeclarationKind::PARAMETER:
    return IndexedTokenKind::Parameter;
  case ValueDeclarationKind::VARIABLE:
    return IndexedTokenKind::Variable;
  case ValueDeclarationKind::PROPERTY:
    return IndexedTokenKind::Property;
  }
}

auto indexedTokenKindFromResolvedSymbol(const Value* resolvedSymbol, bool isTypePosition,
                                        bool isMemberAccess, IndexedTokenKind fallbackKind)
    -> IndexedTokenKind {
  if (resolvedSymbol == nullptr) {
    return fallbackKind;
  }
  if (std::optional<IndexedTokenKind> declarationKind =
          indexedTokenKindFromDeclarationKind(resolvedSymbol->getDeclarationKind())) {
    return *declarationKind;
  }
  Type* const resolvedType = resolvedSymbol->getType();
  if (isTypePosition) {
    if (resolvedType != nullptr && resolvedType->is(BaseType::TY_GENERIC)) {
      return IndexedTokenKind::TypeParameter;
    }
    if (resolvedSymbol->getCategory() == ValueCategory::TYPE_SYMBOL && resolvedType != nullptr) {
      if (resolvedType->is(BaseType::TY_CLASS)) {
        return IndexedTokenKind::Class;
      }
      if (resolvedType->is(BaseType::TY_ENUM)) {
        return IndexedTokenKind::Enum;
      }
    }
    return IndexedTokenKind::Type;
  }
  switch (resolvedSymbol->getCategory()) {
  case ValueCategory::MODULE_SYMBOL:
    return IndexedTokenKind::Namespace;
  case ValueCategory::TYPE_SYMBOL:
    if (resolvedType != nullptr && resolvedType->is(BaseType::TY_GENERIC)) {
      return IndexedTokenKind::TypeParameter;
    }
    if (resolvedType != nullptr && resolvedType->is(BaseType::TY_CLASS)) {
      return IndexedTokenKind::Class;
    }
    if (resolvedType != nullptr && resolvedType->is(BaseType::TY_ENUM)) {
      return IndexedTokenKind::Enum;
    }
    return IndexedTokenKind::Type;
  case ValueCategory::CALLABLE_SYMBOL:
    return isMemberAccess ? IndexedTokenKind::Method : IndexedTokenKind::Function;
  case ValueCategory::ADDRESSABLE_STORAGE:
  case ValueCategory::DIRECT_VALUE:
    return isMemberAccess ? IndexedTokenKind::Property : fallbackKind;
  }
  return fallbackKind;
}

auto appendIndexedOccurrence(AnalysisIndex& index, const std::string& name,
                             std::optional<std::string> dotBase, llvm::SMRange span,
                             bool isTypePosition, bool isMemberAccess, unsigned modifiers,
                             std::optional<IndexedTokenKind> fallbackTokenKind,
                             const Value* resolvedSymbol = nullptr,
                             std::optional<IndexedDeclarationIdentity> declaration = std::nullopt)
    -> void {
  if (!span.isValid()) {
    return;
  }
  if (!declaration.has_value()) {
    declaration = declarationIdentityFromValue(resolvedSymbol);
  }
  index.symbolOccurrences.push_back(IndexedSymbolOccurrence{
      .name = name,
      .dotBase = std::move(dotBase),
      .span = span,
      .declaration = std::move(declaration),
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
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    return memberReceiverName(dot->getLeft());
  }
  auto const* lit = dynamic_cast<const Literal*>(expr);
  if (lit == nullptr || lit->getType() != TokenType::IDENTIFIER) {
    return std::nullopt;
  }
  return lit->getValue();
}

auto memberBaseName(const Expression* expr) -> std::optional<std::string> {
  Expression const* baseExpr = expr;
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    baseExpr = dot->getLeft();
  }
  std::optional<std::string> receiverName = memberReceiverName(baseExpr);
  if (!receiverName.has_value()) {
    return std::nullopt;
  }
  Type* resolvedType = resolvedTypeForExpr(baseExpr);
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

auto fieldDeclarationFromMemberAccess(const Expression* expr, const std::string& memberName)
    -> std::optional<IndexedDeclarationIdentity> {
  Type* resolvedType = resolvedTypeForExpr(expr);
  if (resolvedType == nullptr) {
    return std::nullopt;
  }
  if (resolvedType->is(BaseType::TY_PTR) && resolvedType->getElementType() != nullptr) {
    resolvedType = resolvedType->getElementType();
  }
  return declarationIdentityFromField(TypeUtils::findFieldInFields(resolvedType, memberName));
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
      appendIndexedOccurrence(
          index, genericParam.name, std::nullopt, genericParam.span, true, false,
          analysis_index_modifier::DECLARATION, IndexedTokenKind::TypeParameter,
          view.genericScope != nullptr ? view.genericScope->lookup(genericParam.name) : nullptr);
      for (size_t bi = 0; bi < genericParam.traitBounds.size(); ++bi) {
        if (bi >= genericParam.traitBoundSpans.size()) {
          break;
        }
        llvm::SMRange const boundSpan = genericParam.traitBoundSpans[bi];
        if (!boundSpan.isValid()) {
          continue;
        }
        Value* boundSym = view.genericScope != nullptr
                              ? view.genericScope->lookup(genericParam.traitBounds[bi])
                              : nullptr;
        appendIndexedOccurrence(
            index, genericParam.traitBounds[bi], std::nullopt, boundSpan, true, false, 0U,
            indexedTokenKindFromResolvedSymbol(boundSym, true, false, IndexedTokenKind::Type),
            boundSym);
      }
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
  if (typeExpr->getType() != TokenType::PTR_TYPE && typeExpr->getType() != TokenType::FUNC_TYPE &&
      typeExpr->getType() != TokenType::TUPLE_TYPE) {
    Value* const resolvedSymbol = typeExpr->getResolvedSymbol();
    llvm::SMRange span = typeExpr->getSpan();
    std::string name = typeExpr->getName();
    if (typeExpr->getType() == TokenType::LIST_TYPE) {
      name = "list";
      span = makeNameSpan(typeExpr->getStart(), name);
    }
    appendIndexedOccurrence(
        index, name, std::nullopt, span, true, false, 0U,
        indexedTokenKindFromResolvedSymbol(resolvedSymbol, true, false, IndexedTokenKind::Type),
        resolvedSymbol);
  }
  collectIndexFromTypeExpr(typeExpr->getElementType(), index);
  for (TypeExpr* typeArg : typeExpr->getTypeArgs()) {
    collectIndexFromTypeExpr(typeArg, index);
  }
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
      Value* const resolvedSymbol = lit->getResolvedSymbol();
      appendIndexedOccurrence(index, lit->getValue(), std::nullopt, lit->getSpan(), false, false,
                              0U,
                              indexedTokenKindFromResolvedSymbol(resolvedSymbol, false, false,
                                                                 IndexedTokenKind::Variable),
                              resolvedSymbol);
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
        std::optional<IndexedDeclarationIdentity> const fieldDeclaration =
            fieldDeclarationFromMemberAccess(dot->getLeft(), rightLit->getValue());
        appendIndexedOccurrence(
            index, rightLit->getValue(), dotBase, rightLit->getSpan(), false, true, 0U,
            isEnumMemberAccess ? IndexedTokenKind::EnumMember : IndexedTokenKind::Property,
            rightLit->getResolvedSymbol(), fieldDeclaration);
        return;
      }
    }
    if (auto const* rightCall = dynamic_cast<const FuncCall*>(dot->getRight())) {
      appendIndexedOccurrence(index, rightCall->getName(), dotBase,
                              makeNameSpan(rightCall->getSpan().Start, rightCall->getName()), false,
                              true, 0U, IndexedTokenKind::Method, rightCall->getResolvedSymbol());
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
  if (auto const* subscript = dynamic_cast<const SubscriptOp*>(expr)) {
    collectIndexFromExpr(subscript->getLeft(), index);
    collectIndexFromExpr(subscript->getIndex(), index);
    return;
  }
  if (auto const* unary = dynamic_cast<const UnaryOp*>(expr)) {
    collectIndexFromExpr(unary->getExpression(), index);
    return;
  }
  if (auto const* list = dynamic_cast<const ListLiteral*>(expr)) {
    for (Expression* element : list->getElements()) {
      collectIndexFromExpr(element, index);
    }
    return;
  }
  if (auto const* tup = dynamic_cast<const TupleLiteral*>(expr)) {
    for (Expression* element : tup->getElements()) {
      collectIndexFromExpr(element, index);
    }
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
    std::vector<Literal*> const names = varDecl->getVarLiterals();
    std::vector<Value*> const rs = varDecl->getResolvedSymbols();
    for (size_t i = 0; i < names.size(); ++i) {
      Literal* lit = names[i];
      if (lit == nullptr) {
        continue;
      }
      Value* sym = nullptr;
      if (i < rs.size()) {
        sym = rs[i];
      } else if (names.size() == 1U) {
        sym = varDecl->getResolvedSymbol();
      }
      appendIndexedOccurrence(index, lit->getValue(), std::nullopt, lit->getSpan(), false, false,
                              analysis_index_modifier::DECLARATION, IndexedTokenKind::Variable,
                              sym);
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
  if (auto const* traitNode = dynamic_cast<const TraitDecl*>(stmt)) {
    appendIndexedOccurrence(index, traitNode->getIdentifier(), std::nullopt,
                            traitNode->getNameSpan(), true, false,
                            analysis_index_modifier::DECLARATION, IndexedTokenKind::Type,
                            traitNode->getResolvedSymbol());
    for (const GenericParamDecl& genericParam : traitNode->getGenericParamDecls()) {
      appendIndexedOccurrence(index, genericParam.name, std::nullopt, genericParam.span, true,
                              false, analysis_index_modifier::DECLARATION,
                              IndexedTokenKind::TypeParameter, nullptr);
      for (size_t bi = 0; bi < genericParam.traitBounds.size(); ++bi) {
        if (bi >= genericParam.traitBoundSpans.size()) {
          break;
        }
        llvm::SMRange const boundSpan = genericParam.traitBoundSpans[bi];
        if (!boundSpan.isValid()) {
          continue;
        }
        appendIndexedOccurrence(
            index, genericParam.traitBounds[bi], std::nullopt, boundSpan, true, false, 0U,
            indexedTokenKindFromResolvedSymbol(nullptr, true, false, IndexedTokenKind::Type),
            nullptr);
      }
    }
    for (FuncDecl* req : traitNode->getRequirements()) {
      collectIndexFromFuncLike(req, index, true);
    }
    return;
  }
  if (auto const* klass = dynamic_cast<const Class*>(stmt)) {
    appendIndexedOccurrence(index, klass->getIdentifier(), std::nullopt, klass->getNameSpan(), true,
                            false, analysis_index_modifier::DECLARATION, IndexedTokenKind::Class,
                            klass->getResolvedSymbol());
    for (const GenericParamDecl& genericParam : klass->getGenericParamDecls()) {
      appendIndexedOccurrence(
          index, genericParam.name, std::nullopt, genericParam.span, true, false,
          analysis_index_modifier::DECLARATION, IndexedTokenKind::TypeParameter,
          klass->getGenericScope() != nullptr ? klass->getGenericScope()->lookup(genericParam.name)
                                              : nullptr);
      for (size_t bi = 0; bi < genericParam.traitBounds.size(); ++bi) {
        if (bi >= genericParam.traitBoundSpans.size()) {
          break;
        }
        llvm::SMRange const boundSpan = genericParam.traitBoundSpans[bi];
        if (!boundSpan.isValid()) {
          continue;
        }
        Value* boundSym = klass->getGenericScope() != nullptr
                              ? klass->getGenericScope()->lookup(genericParam.traitBounds[bi])
                              : nullptr;
        appendIndexedOccurrence(
            index, genericParam.traitBounds[bi], std::nullopt, boundSpan, true, false, 0U,
            indexedTokenKindFromResolvedSymbol(boundSym, true, false, IndexedTokenKind::Type),
            boundSym);
      }
    }
    {
      std::vector<std::string> const& implNames = klass->getImplTraitNames();
      std::vector<llvm::SMRange> const& implSpans = klass->getImplTraitSpans();
      std::vector<std::vector<std::unique_ptr<TypeExpr>>> const& implTypeArgLists =
          klass->getImplTraitTypeArgs();
      for (size_t ti = 0; ti < implNames.size(); ++ti) {
        if (ti >= implSpans.size()) {
          break;
        }
        llvm::SMRange const traitSpan = implSpans[ti];
        if (!traitSpan.isValid()) {
          continue;
        }
        Value* traitSym = klass->getGenericScope() != nullptr
                              ? klass->getGenericScope()->lookup(implNames[ti])
                              : nullptr;
        appendIndexedOccurrence(
            index, implNames[ti], std::nullopt, traitSpan, true, false, 0U,
            indexedTokenKindFromResolvedSymbol(traitSym, true, false, IndexedTokenKind::Type),
            traitSym);
        if (ti < implTypeArgLists.size()) {
          for (std::unique_ptr<TypeExpr> const& typeArg : implTypeArgLists[ti]) {
            collectIndexFromTypeExpr(typeArg.get(), index);
          }
        }
      }
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
    std::vector<Field*> const fields = enumNode->getResolvedSymbol() != nullptr &&
                                               enumNode->getResolvedSymbol()->getType() != nullptr
                                           ? enumNode->getResolvedSymbol()->getType()->getFields()
                                           : std::vector<Field*>{};
    std::vector<NamedSpan> const valueDecls = getEnumValueDecls(enumNode);
    for (size_t i = 0; i < valueDecls.size(); ++i) {
      const NamedSpan& valueDecl = valueDecls[i];
      appendIndexedOccurrence(
          index, valueDecl.name, std::nullopt, valueDecl.span, false, false,
          analysis_index_modifier::DECLARATION, IndexedTokenKind::EnumMember, nullptr,
          i < fields.size() ? declarationIdentityFromField(fields[i]) : std::nullopt);
    }
    return;
  }
  if (auto const* importNode = dynamic_cast<const Import*>(stmt)) {
    llvm::SMRange const aliasSpan = importNode->getAliasSpan();
    for (const NamedSpan& binding : getImportLocalBindings(importNode)) {
      bool const isModuleAlias = aliasSpan.isValid() && binding.name == importNode->getAlias() &&
                                 binding.span.isValid() && binding.span.Start == aliasSpan.Start &&
                                 binding.span.End == aliasSpan.End;
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
  if (auto const* forNode = dynamic_cast<const ForIn*>(stmt)) {
    if (forNode->getIdentifier() != nullptr) {
      appendIndexedOccurrence(index, forNode->getIdentifier()->getValue(), std::nullopt,
                              forNode->getIdentifier()->getSpan(), false, false,
                              analysis_index_modifier::DECLARATION, IndexedTokenKind::Variable,
                              forNode->getIdentifier()->getResolvedSymbol());
    }
    collectIndexFromExpr(forNode->getIterable(), index);
    collectIndexFromStmt(forNode->getBlock(), index, false);
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

auto lesma::buildAnalysisIndex(const Compound* ast, llvm::SourceMgr* /*srcMgr*/,
                               unsigned /*bufferId*/) -> AnalysisIndex {
  AnalysisIndex index;
  if (ast == nullptr) {
    return index;
  }
  for (Statement* stmt : ast->getChildren()) {
    collectIndexFromStmt(stmt, index, false);
  }
  return index;
}
