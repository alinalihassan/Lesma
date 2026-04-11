#include <optional>
#include <string>
#include <utility>

#include "llvm/Support/SMLoc.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/IntegerLiteralParse.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Symbol/TypeUtils.h"

using namespace lesma;

namespace lesma {
namespace detail_index {
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
} // namespace detail_index

auto indexedTokenKindFromResolvedSymbol(const Value* resolvedSymbol, bool isTypePosition,
                                        bool isMemberAccess, IndexedTokenKind fallbackKind)
    -> IndexedTokenKind {
  if (resolvedSymbol == nullptr) {
    return fallbackKind;
  }
  if (std::optional<IndexedTokenKind> declarationKind =
          detail_index::indexedTokenKindFromDeclarationKind(resolvedSymbol->getDeclarationKind())) {
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
} // namespace lesma

namespace {
auto makeNameSpan(llvm::SMLoc start, const std::string& name) -> llvm::SMRange {
  if (!start.isValid()) {
    return {};
  }
  llvm::StringRef const text(start.getPointer(), name.size());
  return llvm::SMRange{start, llvm::SMLoc::getFromPointer(text.end())};
}

auto spanTextEquals(llvm::SMRange span, llvm::StringRef expected) -> bool {
  if (!span.isValid()) {
    return false;
  }
  char const* const startPtr = span.Start.getPointer();
  char const* const endPtr = span.End.getPointer();
  if (startPtr == nullptr || endPtr == nullptr || endPtr < startPtr) {
    return false;
  }
  llvm::StringRef const text(startPtr, static_cast<size_t>(endPtr - startPtr));
  return text == expected;
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

auto declarationIdentityFromType(const Type* type) -> std::optional<IndexedDeclarationIdentity> {
  if (type == nullptr || !type->getDeclarationSpan().isValid() ||
      type->getDeclarationFilePath().empty()) {
    return std::nullopt;
  }
  return IndexedDeclarationIdentity{
      .filePath = type->getDeclarationFilePath(),
      .span = type->getDeclarationSpan(),
  };
}

auto appendIndexedOccurrence(AnalysisIndex& index, const std::string& name,
                             std::optional<std::string> dotBase, llvm::SMRange span,
                             bool isTypePosition, bool isMemberAccess, unsigned modifiers,
                             std::optional<IndexedTokenKind> fallbackTokenKind,
                             const Value* resolvedSymbol = nullptr, Type* resolvedType = nullptr,
                             std::optional<IndexedDeclarationIdentity> declaration = std::nullopt,
                             std::optional<llvm::SMRange> semanticHighlightSpan = std::nullopt,
                             Type* flowSensitiveType = nullptr) -> void {
  if (!span.isValid()) {
    return;
  }
  if (!declaration.has_value()) {
    declaration = declarationIdentityFromValue(resolvedSymbol);
  }
  if (semanticHighlightSpan.has_value() && !semanticHighlightSpan->isValid()) {
    semanticHighlightSpan = std::nullopt;
  }
  if (resolvedType == nullptr && resolvedSymbol != nullptr) {
    resolvedType = resolvedSymbol->getType();
  }
  index.symbolOccurrences.push_back(IndexedSymbolOccurrence{
      .name = name,
      .dotBase = std::move(dotBase),
      .span = span,
      .flowSensitiveType = flowSensitiveType,
      .resolvedType = resolvedType,
      .semanticHighlightSpan = semanticHighlightSpan,
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
  if (expr->getLspFlowSensitiveType() != nullptr) {
    return expr->getLspFlowSensitiveType();
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    Value* const resolvedSymbol = lit->getResolvedSymbol();
    return resolvedSymbol != nullptr ? resolvedSymbol->getType() : nullptr;
  }
  if (auto const* sip = dynamic_cast<const StringInterpolation*>(expr)) {
    Type* const strTy = sip->getResolvedStrClassType();
    return strTy != nullptr ? strTy : nullptr;
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
  if (auto const* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
    Value* const resolvedSymbol = lambda->getResolvedSymbol();
    return resolvedSymbol != nullptr ? resolvedSymbol->getType() : nullptr;
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
auto collectIndexFromExpr(const Expression* expr, AnalysisIndex& index,
                          const std::string& mainFilePath) -> void;
auto collectIndexFromCallOperands(const FuncCall* call, AnalysisIndex& index,
                                  const std::string& mainFilePath) -> void;
auto collectIndexFromStmt(const Statement* stmt, AnalysisIndex& index, bool inClass,
                          const std::string& mainFilePath) -> void;

template <typename FuncLike>
auto collectIndexFromFuncLike(const FuncLike* node, AnalysisIndex& index, bool inClass,
                              const std::string& mainFilePath) -> void {
  if (node == nullptr) {
    return;
  }
  FuncLikeDeclView const view = makeFuncLikeDeclView(node);
  std::optional<llvm::SMRange> semanticHighlightSpan;
  if (auto const* funcDecl = dynamic_cast<const FuncDecl*>(node)) {
    llvm::SMRange const g = funcDecl->getOverloadGlyphSpan();
    if (g.isValid()) {
      semanticHighlightSpan = g;
    }
  }
  appendIndexedOccurrence(index, view.name, std::nullopt, view.nameSpan, false, false,
                          analysis_index_modifier::DECLARATION,
                          inClass ? IndexedTokenKind::Method : IndexedTokenKind::Function,
                          view.resolvedSymbol, nullptr, std::nullopt, semanticHighlightSpan);
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
    collectIndexFromStmt(view.body, index, false, mainFilePath);
  }
}

auto collectIndexFromTypeExpr(const TypeExpr* typeExpr, AnalysisIndex& index) -> void {
  if (typeExpr == nullptr) {
    return;
  }
  if (typeExpr->getType() != TokenType::PTR_TYPE && typeExpr->getType() != TokenType::FUNC_TYPE &&
      typeExpr->getType() != TokenType::TUPLE_TYPE &&
      typeExpr->getType() != TokenType::UNION_TYPE) {
    Value* const resolvedSymbol = typeExpr->getResolvedSymbol();
    llvm::SMRange span = typeExpr->getSpan();
    std::string name = typeExpr->getName();
    if (typeExpr->getType() == TokenType::LIST_TYPE) {
      name = "list";
      span = makeNameSpan(typeExpr->getStart(), name);
    } else if (typeExpr->getType() == TokenType::CUSTOM_TYPE && !typeExpr->getTypeArgs().empty()) {
      // `dict<K,V>` / `Box<T>`: index only the constructor name so hover/definition match
      // `root->lookup("dict")`, not the full spelling `dict<...>`.
      name = typeExpr->getLookupName();
      span = makeNameSpan(typeExpr->getStart(), name);
    }
    // Optional-type sugar parses `T?` as `T | null`, but the synthetic `null` arm only has the
    // `?` token span. Skip indexing that synthetic arm so punctuation is not highlighted as a type.
    if (!(typeExpr->getType() == TokenType::NIL && !spanTextEquals(span, "null"))) {
      appendIndexedOccurrence(
          index, name, std::nullopt, span, true, false, 0U,
          indexedTokenKindFromResolvedSymbol(resolvedSymbol, true, false, IndexedTokenKind::Type),
          resolvedSymbol);
    }
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

auto collectIndexFromExpr(const Expression* expr, AnalysisIndex& index,
                          const std::string& mainFilePath) -> void {
  if (expr == nullptr) {
    return;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    if (lit->getType() == TokenType::IDENTIFIER) {
      Value* const resolvedSymbol = lit->getResolvedSymbol();
      appendIndexedOccurrence(
          index, lit->getValue(), std::nullopt, lit->getSpan(), false, false, 0U,
          indexedTokenKindFromResolvedSymbol(resolvedSymbol, false, false,
                                             IndexedTokenKind::Variable),
          resolvedSymbol, nullptr, std::nullopt, std::nullopt, lit->getLspFlowSensitiveType());
    } else if (lit->getType() == TokenType::INTEGER &&
               integerLiteralHasExplicitRadix(lit->getValue())) {
      appendIndexedOccurrence(index, lit->getValue(), std::nullopt, lit->getSpan(), false, false,
                              0U, std::nullopt, nullptr, nullptr);
    }
    return;
  }
  if (auto const* interp = dynamic_cast<const StringInterpolation*>(expr)) {
    for (Expression* part : interp->getExprs()) {
      collectIndexFromExpr(part, index, mainFilePath);
    }
    return;
  }
  if (auto const* typeExpr = dynamic_cast<const TypeExpr*>(expr)) {
    collectIndexFromTypeExpr(typeExpr, index);
    return;
  }
  if (auto const* call = dynamic_cast<const FuncCall*>(expr)) {
    appendIndexedOccurrence(index, call->getName(), std::nullopt,
                            makeNameSpan(call->getSpan().Start, call->getName()), false, false, 0U,
                            IndexedTokenKind::Function, call->getResolvedSymbol());
    collectIndexFromCallOperands(call, index, mainFilePath);
    return;
  }
  if (auto const* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
    for (Parameter* param : lambda->getParameters()) {
      appendIndexedOccurrence(index, param->name, std::nullopt, param->nameSpan, false, false,
                              analysis_index_modifier::DECLARATION, IndexedTokenKind::Parameter,
                              param->getResolvedSymbol());
      collectIndexFromTypeExpr(param->type.get(), index);
    }
    collectIndexFromTypeExpr(lambda->getReturnType(), index);
    if (lambda->isExpressionBody()) {
      collectIndexFromExpr(lambda->getExpressionBody(), index, mainFilePath);
    } else {
      collectIndexFromStmt(lambda->getBlockBody(), index, false, mainFilePath);
    }
    return;
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    collectIndexFromExpr(dot->getLeft(), index, mainFilePath);
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
            rightLit->getResolvedSymbol(), nullptr, fieldDeclaration);
        return;
      }
    }
    if (auto const* rightCall = dynamic_cast<const FuncCall*>(dot->getRight())) {
      std::optional<IndexedDeclarationIdentity> const memberDeclaration =
          isEnumMemberAccess
              ? fieldDeclarationFromMemberAccess(dot->getLeft(), rightCall->getName())
              : std::nullopt;
      appendIndexedOccurrence(index, rightCall->getName(), dotBase,
                              makeNameSpan(rightCall->getSpan().Start, rightCall->getName()), false,
                              true, 0U, IndexedTokenKind::Method, rightCall->getResolvedSymbol(),
                              nullptr, memberDeclaration);
      collectIndexFromCallOperands(rightCall, index, mainFilePath);
      return;
    }
    collectIndexFromExpr(dot->getRight(), index, mainFilePath);
    return;
  }
  if (auto const* binary = dynamic_cast<const BinaryOp*>(expr)) {
    collectIndexFromExpr(binary->getLeft(), index, mainFilePath);
    collectIndexFromExpr(binary->getRight(), index, mainFilePath);
    return;
  }
  if (auto const* subscript = dynamic_cast<const SubscriptOp*>(expr)) {
    collectIndexFromExpr(subscript->getLeft(), index, mainFilePath);
    collectIndexFromExpr(subscript->getIndex(), index, mainFilePath);
    return;
  }
  if (auto const* unary = dynamic_cast<const UnaryOp*>(expr)) {
    collectIndexFromExpr(unary->getExpression(), index, mainFilePath);
    return;
  }
  if (auto const* blockExpr = dynamic_cast<const BlockExpr*>(expr)) {
    collectIndexFromStmt(blockExpr->getBody(), index, false, mainFilePath);
    collectIndexFromExpr(blockExpr->getTailExpr(), index, mainFilePath);
    return;
  }
  if (auto const* match = dynamic_cast<const MatchExpr*>(expr)) {
    collectIndexFromExpr(match->getScrutinee(), index, mainFilePath);
    for (const MatchArm& arm : match->getArms()) {
      const MatchPattern& pattern = arm.pattern;
      if (pattern.kind == MatchPatternKind::VALUE && pattern.valueExpr != nullptr) {
        collectIndexFromExpr(pattern.valueExpr.get(), index, mainFilePath);
      } else if (pattern.kind == MatchPatternKind::VARIANT) {
        if (!pattern.enumName.empty() && pattern.enumNameSpan.isValid()) {
          appendIndexedOccurrence(index, pattern.enumName, std::nullopt, pattern.enumNameSpan, true,
                                  false, 0U, IndexedTokenKind::Enum, nullptr,
                                  pattern.resolvedEnumType,
                                  declarationIdentityFromType(pattern.resolvedEnumType));
        }
        if (!pattern.variantName.empty() && pattern.variantNameSpan.isValid()) {
          std::optional<IndexedDeclarationIdentity> variantDecl = std::nullopt;
          Type* variantType = pattern.resolvedEnumType;
          if (variantType != nullptr) {
            const auto& variants = variantType->getEnumVariants();
            if (pattern.resolvedVariantIndex < variants.size() &&
                variants[pattern.resolvedVariantIndex] != nullptr &&
                variants[pattern.resolvedVariantIndex]->getDeclarationSpan().isValid() &&
                !variants[pattern.resolvedVariantIndex]->getDeclarationFilePath().empty()) {
              variantDecl = IndexedDeclarationIdentity{
                  .filePath = variants[pattern.resolvedVariantIndex]->getDeclarationFilePath(),
                  .span = variants[pattern.resolvedVariantIndex]->getDeclarationSpan(),
              };
            }
          }
          appendIndexedOccurrence(index, pattern.variantName, pattern.enumName,
                                  pattern.variantNameSpan, false, true, 0U,
                                  IndexedTokenKind::EnumMember, nullptr, variantType, variantDecl);
        }
        Type* bindingEnumType = pattern.resolvedEnumType;
        if (bindingEnumType != nullptr) {
          const auto& variants = bindingEnumType->getEnumVariants();
          if (pattern.resolvedVariantIndex < variants.size() &&
              variants[pattern.resolvedVariantIndex] != nullptr) {
            const auto& payloadTypes = variants[pattern.resolvedVariantIndex]->payloadTypes;
            for (size_t i = 0; i < pattern.bindings.size() && i < payloadTypes.size() &&
                               i < pattern.bindingSpans.size();
                 ++i) {
              if (pattern.bindings[i] == "_" || !pattern.bindingSpans[i].isValid()) {
                continue;
              }
              appendIndexedOccurrence(index, pattern.bindings[i], std::nullopt,
                                      pattern.bindingSpans[i], false, false,
                                      analysis_index_modifier::DECLARATION,
                                      IndexedTokenKind::Variable, nullptr, payloadTypes[i],
                                      IndexedDeclarationIdentity{.filePath = mainFilePath,
                                                                 .span = pattern.bindingSpans[i]});
            }
          }
        }
      }
      collectIndexFromExpr(arm.body.get(), index, mainFilePath);
    }
    return;
  }
  if (auto const* list = dynamic_cast<const ListLiteral*>(expr)) {
    for (Expression* element : list->getElements()) {
      collectIndexFromExpr(element, index, mainFilePath);
    }
    return;
  }
  if (auto const* dict = dynamic_cast<const DictLiteral*>(expr)) {
    for (Expression* k : dict->getKeys()) {
      collectIndexFromExpr(k, index, mainFilePath);
    }
    for (Expression* v : dict->getValues()) {
      collectIndexFromExpr(v, index, mainFilePath);
    }
    return;
  }
  if (auto const* tup = dynamic_cast<const TupleLiteral*>(expr)) {
    for (Expression* element : tup->getElements()) {
      collectIndexFromExpr(element, index, mainFilePath);
    }
    return;
  }
  if (auto const* castOp = dynamic_cast<const CastOp*>(expr)) {
    collectIndexFromExpr(castOp->getExpression(), index, mainFilePath);
    collectIndexFromTypeExpr(castOp->getType(), index);
    return;
  }
  if (auto const* isOp = dynamic_cast<const IsOp*>(expr)) {
    collectIndexFromExpr(isOp->getLeft(), index, mainFilePath);
    collectIndexFromTypeExpr(isOp->getRight(), index);
  }
}

auto collectIndexFromCallOperands(const FuncCall* call, AnalysisIndex& index,
                                  const std::string& mainFilePath) -> void {
  if (call == nullptr) {
    return;
  }
  for (TypeExpr* typeArg : call->getExplicitTypeArgs()) {
    collectIndexFromTypeExpr(typeArg, index);
  }
  for (Expression* arg : call->getArguments()) {
    collectIndexFromExpr(arg, index, mainFilePath);
  }
}

auto collectIndexFromStmt(const Statement* stmt, AnalysisIndex& index, bool inClass,
                          const std::string& mainFilePath) -> void {
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
    collectIndexFromExpr(varDecl->getValue(), index, mainFilePath);
    return;
  }
  if (auto const* func = dynamic_cast<const FuncDecl*>(stmt)) {
    collectIndexFromFuncLike(func, index, inClass, mainFilePath);
    return;
  }
  if (auto const* ext = dynamic_cast<const ExternFuncDecl*>(stmt)) {
    collectIndexFromFuncLike(ext, index, false, mainFilePath);
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
      collectIndexFromFuncLike(req, index, true, mainFilePath);
    }
    return;
  }
  if (auto const* typeAlias = dynamic_cast<const TypeAlias*>(stmt)) {
    appendIndexedOccurrence(index, typeAlias->getIdentifier(), std::nullopt,
                            typeAlias->getNameSpan(), true, false,
                            analysis_index_modifier::DECLARATION, IndexedTokenKind::Type,
                            typeAlias->getResolvedSymbol());
    collectIndexFromTypeExpr(typeAlias->getAliasedType(), index);
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
      collectIndexFromStmt(field, index, true, mainFilePath);
    }
    for (FuncDecl* method : klass->getMethods()) {
      collectIndexFromStmt(method, index, true, mainFilePath);
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
          analysis_index_modifier::DECLARATION, IndexedTokenKind::EnumMember, nullptr, nullptr,
          i < fields.size() ? declarationIdentityFromField(fields[i]) : std::nullopt);
    }
    for (FuncDecl* method : enumNode->getMethods()) {
      collectIndexFromStmt(method, index, true, mainFilePath);
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
    collectIndexFromExpr(exprStmt->getExpression(), index, mainFilePath);
    return;
  }
  if (auto const* ifNode = dynamic_cast<const If*>(stmt)) {
    for (Expression* cond : ifNode->getConds()) {
      collectIndexFromExpr(cond, index, mainFilePath);
    }
    for (Compound* block : ifNode->getBlocks()) {
      collectIndexFromStmt(block, index, false, mainFilePath);
    }
    return;
  }
  if (auto const* whileNode = dynamic_cast<const While*>(stmt)) {
    collectIndexFromExpr(whileNode->getCond(), index, mainFilePath);
    collectIndexFromStmt(whileNode->getBlock(), index, false, mainFilePath);
    return;
  }
  if (auto const* forNode = dynamic_cast<const ForIn*>(stmt)) {
    if (forNode->getIdentifier() != nullptr) {
      appendIndexedOccurrence(index, forNode->getIdentifier()->getValue(), std::nullopt,
                              forNode->getIdentifier()->getSpan(), false, false,
                              analysis_index_modifier::DECLARATION, IndexedTokenKind::Variable,
                              forNode->getIdentifier()->getResolvedSymbol());
    }
    collectIndexFromExpr(forNode->getIterable(), index, mainFilePath);
    collectIndexFromStmt(forNode->getBlock(), index, false, mainFilePath);
    return;
  }
  if (auto const* assign = dynamic_cast<const Assignment*>(stmt)) {
    collectIndexFromExpr(assign->getLeftHandSide(), index, mainFilePath);
    collectIndexFromExpr(assign->getRightHandSide(), index, mainFilePath);
    return;
  }
  if (auto const* ret = dynamic_cast<const Return*>(stmt)) {
    collectIndexFromExpr(ret->getValue(), index, mainFilePath);
    return;
  }
  if (auto const* defer = dynamic_cast<const Defer*>(stmt)) {
    collectIndexFromStmt(defer->getStatement(), index, false, mainFilePath);
    return;
  }
  if (auto const* compound = dynamic_cast<const Compound*>(stmt)) {
    for (Statement* child : compound->getChildren()) {
      collectIndexFromStmt(child, index, false, mainFilePath);
    }
  }
}
} // namespace

auto lesma::buildAnalysisIndex(const Compound* ast, llvm::SourceMgr* /*srcMgr*/,
                               unsigned /*bufferId*/, const std::string& mainFilePath)
    -> AnalysisIndex {
  AnalysisIndex index;
  if (ast == nullptr) {
    return index;
  }
  for (Statement* stmt : ast->getChildren()) {
    collectIndexFromStmt(stmt, index, false, mainFilePath);
  }
  return index;
}
