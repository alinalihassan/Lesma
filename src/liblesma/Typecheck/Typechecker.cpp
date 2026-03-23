#include "Typechecker.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "fmt/format.h"
#include "nameof.hpp"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/TypeCheckError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {

namespace {

auto makeGenericDisplaySuffix(const std::vector<std::string>& genericParamNames) -> std::string {
  if (genericParamNames.empty()) {
    return {};
  }
  std::string suffix = "<";
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    if (i > 0U) {
      suffix += ", ";
    }
    suffix += genericParamNames[i];
  }
  suffix += ">";
  return suffix;
}

auto makeSpecializedDisplayName(Type* classTemplate,
                                const std::vector<std::string>& genericParamNames,
                                const std::unordered_map<std::string, Type*>& env) -> std::string {
  if (classTemplate == nullptr) {
    return {};
  }
  std::string baseName = classTemplate->getDisplayName();
  if (baseName.empty()) {
    baseName = classTemplate->toString();
  }
  size_t genericStart = baseName.find('<');
  if (genericStart != std::string::npos) {
    baseName = baseName.substr(0U, genericStart);
  }
  if (genericParamNames.empty()) {
    return baseName;
  }
  std::string result = baseName + "<";
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    if (i > 0U) {
      result += ", ";
    }
    auto it = env.find(genericParamNames[i]);
    result +=
        it != env.end() && it->second != nullptr ? it->second->toString() : genericParamNames[i];
  }
  result += ">";
  return result;
}

void insertGenericParamSymbols(SymbolTable* genericsScope,
                               const std::vector<lesma::GenericParamDecl>& genericParams,
                               const std::unordered_map<std::string, Type*>& genericTypes,
                               const std::string& mainFilePath) {
  if (genericsScope == nullptr) {
    return;
  }
  for (const auto& param : genericParams) {
    auto it = genericTypes.find(param.name);
    if (it == genericTypes.end() || it->second == nullptr) {
      continue;
    }
    auto genericSymbol = std::make_unique<Value>(param.name, it->second);
    genericSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    genericSymbol->setDeclarationKind(ValueDeclarationKind::TYPE_PARAMETER);
    genericSymbol->setDeclarationSpan(param.span);
    genericSymbol->setDeclarationFilePath(mainFilePath);
    genericsScope->insertSymbol(std::move(genericSymbol));
  }
}

} // namespace

auto Typechecker::pathLeadsToEndWithoutReturn(const std::vector<Statement*>& statements,
                                              size_t index) -> bool {
  if (index >= statements.size()) {
    return true; // fell off the end
  }
  Statement* s = statements[index];
  if (dynamic_cast<Return*>(s) != nullptr) {
    return false; // this path returns
  }
  if (auto* comp = dynamic_cast<Compound*>(s)) {
    // If inner block always returns, we never fall off it.
    if (!pathLeadsToEndWithoutReturn(comp->getChildren(), 0)) {
      return false;
    }
    return pathLeadsToEndWithoutReturn(statements, index + 1);
  }
  if (auto* ifStmt = dynamic_cast<If*>(s)) {
    const auto& blocks = ifStmt->getBlocks();
    const bool someBranchFallsThrough =
        std::any_of(blocks.begin(), blocks.end(), [this](Compound* block) {
          return pathLeadsToEndWithoutReturn(block->getChildren(), 0);
        });
    // Any branch that falls through continues with the following statements.
    // Without an else-branch, skipping the whole if also falls through.
    if (blocks.size() == 1U || someBranchFallsThrough) {
      return pathLeadsToEndWithoutReturn(statements, index + 1);
    }
    return false; // all branches return
  }
  if (dynamic_cast<While*>(s) != nullptr) {
    // Can skip loop (0 iterations) and reach next statement.
    return pathLeadsToEndWithoutReturn(statements, index + 1);
  }
  if (dynamic_cast<ForIn*>(s) != nullptr) {
    return pathLeadsToEndWithoutReturn(statements, index + 1);
  }
  // VarDecl, Assignment, ExpressionStatement, Break, Continue, Defer, etc.: fall through
  return pathLeadsToEndWithoutReturn(statements, index + 1);
}

auto Typechecker::blockAlwaysReturns(const Compound* body) -> bool {
  return !pathLeadsToEndWithoutReturn(body->getChildren(), 0);
}

auto Typechecker::visitExprWithExpectedType(const Expression* node, Type* expected) -> void {
  expectedTypes.push_back(expected);
  node->accept(*this);
  expectedTypes.pop_back();
}

auto Typechecker::currentExpectedType() const -> Type* {
  if (expectedTypes.empty()) {
    return nullptr;
  }
  return expectedTypes.back();
}

auto Typechecker::resolveMethodReturnType(Type* baseType, const std::string& methodName,
                                          const std::vector<Type*>& argTypes, llvm::SMRange span)
    -> Type* {
  if (baseType == nullptr) {
    return nullptr;
  }
  Type* base = baseType;
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (base->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    const std::string& traitName = base->getDisplayName();
    auto trIt = traitMethodReturnTypes.find(traitName);
    if (trIt == traitMethodReturnTypes.end()) {
      return nullptr;
    }
    auto methIt = trIt->second.find(methodName);
    if (methIt == trIt->second.end()) {
      return nullptr;
    }
    return methIt->second;
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    return nullptr;
  }

  Type* receiverForLookup = base;
  if (auto templateIt = specializedTypeToTemplate.find(base);
      templateIt != specializedTypeToTemplate.end()) {
    receiverForLookup = templateIt->second;
  }
  Type* selfType =
      receiverForLookup->is(BaseType::TY_PTR)
          ? receiverForLookup
          : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, receiverForLookup));
  std::vector<Type*> methodArgTypes = {selfType};
  for (Type* argType : argTypes) {
    if (argType != nullptr && argType->is(BaseType::TY_CLASS)) {
      methodArgTypes.push_back(
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, argType)));
    } else {
      methodArgTypes.push_back(argType);
    }
  }

  std::unordered_map<std::string, Type*> methodTypeEnv;
  if (auto specializedIt = specializedTypeEnv.find(base);
      specializedIt != specializedTypeEnv.end()) {
    methodTypeEnv = specializedIt->second;
  }

  bool importedMethod = false;
  Value* method = scope->lookupFunction(methodName, methodArgTypes);
  if (method == nullptr) {
    for (auto& [_, cachedModule] : importedModuleCache) {
      if (cachedModule == nullptr || cachedModule->rootScope == nullptr) {
        continue;
      }
      method = cachedModule->rootScope->lookupFunction(methodName, methodArgTypes);
      if (method != nullptr) {
        importedMethod = true;
        break;
      }
    }
  }
  if (method == nullptr || !method->getType()->is(BaseType::TY_FUNCTION)) {
    return nullptr;
  }

  auto fields = method->getType()->getFields();
  for (size_t i = 0; i < fields.size() && i < methodArgTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, methodArgTypes[i], methodTypeEnv, span);
  }
  Type* retType = method->getType()->getReturnType();
  if (!methodTypeEnv.empty() && retType != nullptr) {
    retType = substituteInType(retType, methodTypeEnv);
  }
  if (importedMethod && retType != nullptr) {
    return materializeImportedType(retType);
  }
  return retType;
}

auto Typechecker::isMutableListReceiver(const Expression* expr) -> bool {
  if (expr == nullptr) {
    return true;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    if (lit->getType() != TokenType::IDENTIFIER) {
      return true;
    }
    Value* symbol = scope->lookup(lit->getValue());
    return symbol == nullptr || symbol->getMutability();
  }
  if (auto const* sub = dynamic_cast<const SubscriptOp*>(expr)) {
    return isMutableListReceiver(sub->getLeft());
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    auto const* rightLit = dynamic_cast<const Literal*>(dot->getRight());
    if (rightLit == nullptr || rightLit->getType() != TokenType::IDENTIFIER) {
      return true;
    }
    std::unique_ptr<Value> savedResult = std::move(result);
    try {
      dot->getLeft()->accept(*this);
    } catch (...) {
      result = std::move(savedResult);
      throw;
    }
    Type* base = result != nullptr ? result->getType() : nullptr;
    result = std::move(savedResult);
    if (base == nullptr) {
      return true;
    }
    if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
      base = base->getElementType();
    }
    Field* field = TypeUtils::findFieldInFields(base, rightLit->getValue());
    if (field == nullptr) {
      return true;
    }
    Value* decl = field->getDeclarationSymbol();
    if (decl == nullptr) {
      return true;
    }
    return decl->getMutability();
  }
  return true;
}

auto Typechecker::isMutatingListFunction(const std::string& functionName) const -> bool {
  return functionName == "push" || functionName == "clear" || functionName == "pop";
}

auto Typechecker::isListIntrinsicName(const std::string& functionName) const -> bool {
  return functionName == "__list_len" || functionName == "__list_push" ||
         functionName == "__list_pop" || functionName == "__list_clear" ||
         functionName == "__list_copy" || functionName == "__list_get" ||
         functionName == "__list_set" || functionName == "__buffer_new" ||
         functionName == "__buffer_len" || functionName == "__buffer_push" ||
         functionName == "__buffer_pop" || functionName == "__buffer_clear" ||
         functionName == "__buffer_copy" || functionName == "__buffer_get" ||
         functionName == "__buffer_set";
}

auto Typechecker::visitListIntrinsicCall(const FuncCall* node, const std::vector<Type*>& argTypes)
    -> bool {
  if (!isListIntrinsicName(node->getName())) {
    return false;
  }
  if (node->getName() == "__buffer_new") {
    Type* elementType = nullptr;
    auto explicitTypeArgs = node->getExplicitTypeArgs();
    if (explicitTypeArgs.size() == 1U) {
      explicitTypeArgs.front()->accept(*this);
      elementType = result->getType();
    } else if (!argTypes.empty() && argTypes.front() != nullptr) {
      elementType = argTypes.front();
    }
    if (elementType == nullptr) {
      throw TypeCheckError(node->getSpan(), "__buffer_new<T> requires an explicit element type");
    }
    auto* bufferType = cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, elementType));
    bufferType->setDisplayName("__buffer");
    result = std::make_unique<Value>(bufferType);
    return true;
  }
  if (argTypes.empty() || argTypes.front() == nullptr ||
      !argTypes.front()->is(BaseType::TY_ARRAY) || argTypes.front()->getElementType() == nullptr) {
    throw TypeCheckError(node->getSpan(), "{} requires __buffer<T> as first argument",
                         node->getName());
  }
  Type* listType = argTypes.front();
  if (node->getName() == "__list_len" || node->getName() == "__buffer_len") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    auto intLen = std::make_unique<Type>(BaseType::TY_INT);
    intLen->setIntWidth(64);
    result = std::make_unique<Value>(cacheType(std::move(intLen)));
    return true;
  }
  if (node->getName() == "__list_copy" || node->getName() == "__buffer_copy") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(listType);
    return true;
  }
  if (node->getName() == "__list_clear" || node->getName() == "__buffer_clear") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (node->getName() == "__list_pop" || node->getName() == "__buffer_pop") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(listType->getElementType());
    return true;
  }
  if (node->getName() == "__list_push" || node->getName() == "__buffer_push") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "{} expects buffer and value", node->getName());
    }
    if (!isAssignableTo(argTypes[1], listType->getElementType())) {
      throw TypeCheckError(node->getSpan(), "Cannot push type {} into buffer element type {}",
                           argTypes[1]->toString(), listType->getElementType()->toString());
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (node->getName() == "__list_get" || node->getName() == "__buffer_get") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "{} expects buffer and index", node->getName());
    }
    if (argTypes[1] == nullptr || !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "{} index must be int", node->getName());
    }
    result = std::make_unique<Value>(listType->getElementType());
    return true;
  }
  if (node->getName() == "__list_set" || node->getName() == "__buffer_set") {
    if (argTypes.size() != 3U) {
      throw TypeCheckError(node->getSpan(), "{} expects buffer, index, and value", node->getName());
    }
    if (argTypes[1] == nullptr || !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "{} index must be int", node->getName());
    }
    if (!isAssignableTo(argTypes[2], listType->getElementType())) {
      throw TypeCheckError(node->getSpan(), "Cannot assign type {} into buffer element type {}",
                           argTypes[2]->toString(), listType->getElementType()->toString());
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  return false;
}

auto Typechecker::visitListMethodCall(Type* listType, const DotOp* node, const FuncCall* call)
    -> bool {
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) || call == nullptr) {
    return false;
  }
  std::vector<Type*> argTypes = {listType};
  for (Expression* arg : call->getArguments()) {
    arg->accept(*this);
    Type* argType = result->getType();
    if (argType != nullptr && argType->is(BaseType::TY_CLASS)) {
      argType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, argType));
    }
    argTypes.push_back(argType);
  }
  if (isMutatingListFunction(call->getName()) && !isMutableListReceiver(node->getLeft())) {
    throw TypeCheckError(node->getSpan(), "Cannot call mutating list method {} on immutable value",
                         call->getName());
  }

  const auto listModulePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "list.les").lexically_normal();
  SymbolTable* importedScope = getOrTypecheckImport(listModulePath.string());
  Value* callee =
      importedScope != nullptr ? importedScope->lookupFunction(call->getName(), argTypes) : nullptr;
  if (callee == nullptr || !callee->getType()->is(BaseType::TY_FUNCTION)) {
    return false;
  }

  call->setResolvedSymbol(callee);
  auto* funcType = callee->getType();
  auto fields = funcType->getFields();
  if (!call->getExplicitTypeArgs().empty()) {
    std::vector<Type*> explicitTypes;
    for (TypeExpr* texpr : call->getExplicitTypeArgs()) {
      texpr->accept(*this);
      explicitTypes.push_back(result->getType());
    }
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(funcType);
    if (explicitTypes.size() != genericParamNames.size()) {
      throw TypeCheckError(
          call->getSpan(),
          "Explicit type argument count {} does not match generic parameter count {}",
          explicitTypes.size(), genericParamNames.size());
    }
    std::unordered_map<std::string, Type*> explicitSubst;
    for (size_t i = 0; i < genericParamNames.size(); ++i) {
      explicitSubst[genericParamNames[i]] = explicitTypes[i];
    }
    for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
      Type* expected = substituteInType(fields[i]->type, explicitSubst);
      if (expected != nullptr && !argTypes[i]->isEqual(expected)) {
        throw TypeCheckError(call->getSpan(),
                             "Argument type {} does not match explicit parameter type {}",
                             argTypes[i]->toString(), expected->toString());
      }
    }
    Type* retType = substituteInType(funcType->getReturnType(), explicitSubst);
    Type* finalType =
        retType != nullptr ? retType : cacheType(std::make_unique<Type>(BaseType::TY_VOID));
    result = std::make_unique<Value>(importedScope != nullptr ? materializeImportedType(finalType)
                                                              : finalType);
    return true;
  }

  std::unordered_map<std::string, Type*> localGenericTypes;
  for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, argTypes[i], localGenericTypes, call->getSpan());
  }
  Type* retType = substituteInType(funcType->getReturnType(), localGenericTypes);
  if (retType == nullptr) {
    retType = funcType->getReturnType();
  }
  Type* finalType =
      retType != nullptr ? retType : cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  result = std::make_unique<Value>(importedScope != nullptr ? materializeImportedType(finalType)
                                                            : finalType);
  return true;
}

auto Typechecker::cacheType(std::unique_ptr<Type> type) -> Type* {
  typeCache.push_back(std::move(type));
  return typeCache.back().get();
}

auto Typechecker::materializeImportedType(Type* type) -> Type* {
  if (type == nullptr) {
    return nullptr;
  }
  auto existing = importedTypeCopies.find(type);
  if (existing != importedTypeCopies.end()) {
    return existing->second;
  }
  if (type->is(BaseType::TY_CLASS) || type->is(BaseType::TY_ENUM)) {
    auto placeholder =
        std::make_unique<Type>(type->getBaseType(), nullptr, std::vector<std::unique_ptr<Field>>{});
    Type* copy = cacheType(std::move(placeholder));
    importedTypeCopies[type] = copy;
    copy->setDisplayName(type->getDisplayName());
    copy->setGenericParams(type->getGenericParams());
    copy->setDeclarationSpan(type->getDeclarationSpan());
    copy->setDeclarationFilePath(type->getDeclarationFilePath());
    for (Field* field : type->getFields()) {
      auto fieldCopy = std::make_unique<Field>(field->name, materializeImportedType(field->type));
      fieldCopy->setDeclarationSpan(field->getDeclarationSpan());
      fieldCopy->setDeclarationFilePath(field->getDeclarationFilePath());
      if (Value* declarationSymbol = field->getDeclarationSymbol()) {
        auto symbolCopy = std::make_unique<Value>(declarationSymbol->getName(),
                                                  materializeImportedType(field->type));
        symbolCopy->setCategory(declarationSymbol->getCategory());
        symbolCopy->setDeclarationKind(declarationSymbol->getDeclarationKind());
        symbolCopy->setDeclarationSpan(declarationSymbol->getDeclarationSpan());
        symbolCopy->setDeclarationFilePath(declarationSymbol->getDeclarationFilePath());
        symbolCopy->setMutable(declarationSymbol->getMutability());
        fieldCopy->setDeclarationSymbol(std::move(symbolCopy));
      }
      copy->addField(std::move(fieldCopy));
    }
    if (auto envIt = specializedTypeEnv.find(type); envIt != specializedTypeEnv.end()) {
      std::unordered_map<std::string, Type*> envCopy;
      for (const auto& [name, envType] : envIt->second) {
        envCopy[name] = materializeImportedType(envType);
      }
      specializedTypeEnv[copy] = std::move(envCopy);
    }
    if (auto tmplIt = specializedTypeToTemplate.find(type);
        tmplIt != specializedTypeToTemplate.end()) {
      specializedTypeToTemplate[copy] = materializeImportedType(tmplIt->second);
      copy->setDisplayName(makeSpecializedDisplayName(
          specializedTypeToTemplate[copy], copy->getGenericParams(), specializedTypeEnv[copy]));
    }
    return copy;
  }
  if (type->is(BaseType::TY_PTR)) {
    Type* copy = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr,
                                                  materializeImportedType(type->getElementType())));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_ARRAY)) {
    auto* copy = cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr,
                                                  materializeImportedType(type->getElementType())));
    copy->setDisplayName(type->getDisplayName());
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : type->getFields()) {
      auto fieldCopy = std::make_unique<Field>(field->name, materializeImportedType(field->type));
      fieldCopy->setDeclarationSpan(field->getDeclarationSpan());
      fieldCopy->setDeclarationFilePath(field->getDeclarationFilePath());
      if (Value* declarationSymbol = field->getDeclarationSymbol()) {
        auto symbolCopy = std::make_unique<Value>(declarationSymbol->getName(),
                                                  materializeImportedType(field->type));
        symbolCopy->setCategory(declarationSymbol->getCategory());
        symbolCopy->setDeclarationKind(declarationSymbol->getDeclarationKind());
        symbolCopy->setDeclarationSpan(declarationSymbol->getDeclarationSpan());
        symbolCopy->setDeclarationFilePath(declarationSymbol->getDeclarationFilePath());
        symbolCopy->setMutable(declarationSymbol->getMutability());
        fieldCopy->setDeclarationSymbol(std::move(symbolCopy));
      }
      fields.push_back(std::move(fieldCopy));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(materializeImportedType(type->getReturnType()));
    funcType->setGenericParams(type->getGenericParams());
    funcType->setVarArgs(type->isVarArgs());
    Type* copy = cacheType(std::move(funcType));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_GENERIC)) {
    Type* copy = cacheType(std::make_unique<Type>(type->getGenericName()));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_INT)) {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(static_cast<std::uint16_t>(type->getIntWidth()));
    Type* copy = cacheType(std::move(u));
    importedTypeCopies[type] = copy;
    return copy;
  }
  Type* copy = cacheType(std::make_unique<Type>(type->getBaseType()));
  importedTypeCopies[type] = copy;
  return copy;
}

auto Typechecker::substituteInType(Type* t, const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  if (t->is(BaseType::TY_GENERIC)) {
    auto it = env.find(t->getGenericName());
    if (it != env.end()) {
      return it->second;
    }
    return t;
  }
  if (t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
    Type* elem = substituteInType(t->getElementType(), env);
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (t->is(BaseType::TY_ARRAY) && t->getElementType() != nullptr) {
    Type* elem = substituteInType(t->getElementType(), env);
    return cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, elem));
  }
  if (t->is(BaseType::TY_FUNCTION)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : t->getFields()) {
      fields.push_back(std::make_unique<Field>(field->name, substituteInType(field->type, env)));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(substituteInType(t->getReturnType(), env));
    funcType->setGenericParams(t->getGenericParams());
    funcType->setVarArgs(t->isVarArgs());
    return cacheType(std::move(funcType));
  }
  if (t->is(BaseType::TY_CLASS)) {
    Type* classTemplate = t;
    if (auto tmplIt = specializedTypeToTemplate.find(t);
        tmplIt != specializedTypeToTemplate.end()) {
      classTemplate = tmplIt->second;
    }
    const auto& genericParamNames = classTemplate->getGenericParams();
    if (!genericParamNames.empty()) {
      std::unordered_map<std::string, Type*> classEnv;
      bool changed = false;
      for (const auto& name : genericParamNames) {
        if (auto it = env.find(name); it != env.end()) {
          classEnv[name] = it->second;
          changed = true;
        }
      }
      if (changed) {
        return getOrCreateSpecializedClassType(classTemplate, genericParamNames, classEnv);
      }
    }
  }
  return t;
}

auto Typechecker::inferGenericBindings(Type* pattern, Type* actual,
                                       std::unordered_map<std::string, Type*>& bindings,
                                       llvm::SMRange span) -> void {
  if (pattern == nullptr || actual == nullptr) {
    return;
  }
  if (pattern->is(BaseType::TY_GENERIC)) {
    const std::string genericName = pattern->getGenericName();
    auto it = bindings.find(genericName);
    if (it == bindings.end()) {
      bindings[genericName] = actual;
      return;
    }
    if (!actual->isEqual(it->second)) {
      throw TypeCheckError(span, "Conflicting inferred types for generic parameter {}: {} and {}",
                           genericName, it->second->toString(), actual->toString());
    }
    return;
  }
  if (pattern->getBaseType() != actual->getBaseType()) {
    return;
  }
  if (pattern->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    inferGenericBindings(pattern->getElementType(), actual->getElementType(), bindings, span);
    return;
  }
  if (pattern->is(BaseType::TY_FUNCTION)) {
    auto patternFields = pattern->getFields();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < patternFields.size() && i < actualFields.size(); ++i) {
      inferGenericBindings(patternFields[i]->type, actualFields[i]->type, bindings, span);
    }
    inferGenericBindings(pattern->getReturnType(), actual->getReturnType(), bindings, span);
  }
}

auto Typechecker::getDeclaredGenericParams(Type* type) const -> const std::vector<std::string>& {
  static const std::vector<std::string> emptyParams;
  if (type == nullptr) {
    return emptyParams;
  }
  Type* key = type;
  auto it = specializedTypeToTemplate.find(type);
  if (it != specializedTypeToTemplate.end()) {
    key = it->second;
  }
  return key->getGenericParams();
}

auto Typechecker::getOrCreateSpecializedClassType(Type* classTemplate,
                                                  const std::vector<std::string>& genericParamNames,
                                                  const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  if (genericParamNames.empty()) {
    return classTemplate;
  }

  std::ostringstream key;
  key << classTemplate->toString();
  for (const auto& name : genericParamNames) {
    auto it = env.find(name);
    if (it != env.end()) {
      key << "|" << it->second->toString();
    }
  }
  std::string keyStr = key.str();
  auto it = specializedClassTypes.find(keyStr);
  if (it != specializedClassTypes.end()) {
    return it->second;
  }
  std::vector<std::unique_ptr<Field>> newFields;
  for (Field* f : classTemplate->getFields()) {
    Type* subst = substituteInType(f->type, env);
    newFields.push_back(std::make_unique<Field>(f->name, subst));
  }
  auto specialized = std::make_unique<Type>(BaseType::TY_CLASS, nullptr, std::move(newFields));
  Type* ptr = cacheType(std::move(specialized));
  specializedClassTypes[keyStr] = ptr;
  specializedTypeEnv[ptr] = env;
  specializedTypeToTemplate[ptr] = classTemplate;
  ptr->setGenericParams(genericParamNames);
  ptr->setImplTraitNames(classTemplate->getImplTraitNames());
  ptr->setDeclarationSpan(classTemplate->getDeclarationSpan());
  ptr->setDeclarationFilePath(classTemplate->getDeclarationFilePath());
  ptr->setDisplayName(makeSpecializedDisplayName(classTemplate, genericParamNames, env));
  return ptr;
}

auto Typechecker::getExtendedType(Type* left, Type* right) -> Type* {
  if (left->isEqual(right)) {
    return left;
  }
  if (left->is(BaseType::TY_VOID)) {
    return right;
  }
  if (right->is(BaseType::TY_VOID)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    return left;
  }
  return nullptr;
}

auto Typechecker::typecheckBinaryOpResult(TokenType op, Type* leftTy, Type* rightTy,
                                          llvm::SMRange span) -> Type* {
  bool hasGeneric = leftTy->is(BaseType::TY_GENERIC) || rightTy->is(BaseType::TY_GENERIC);
  Type* unified = getExtendedType(leftTy, rightTy);
  auto tryOverload = [this, op, leftTy, rightTy, span]() -> Type* {
    auto operatorName = OperatorUtils::getBinaryOperatorName(op);
    if (!operatorName.has_value()) {
      return nullptr;
    }
    return resolveMethodReturnType(leftTy, std::string{*operatorName}, {rightTy}, span);
  };

  switch (op) {
  case TokenType::PLUS:
  case TokenType::MINUS:
  case TokenType::STAR:
  case TokenType::SLASH:
  case TokenType::MOD:
  case TokenType::POWER:
    if (hasGeneric) {
      return leftTy->is(BaseType::TY_GENERIC) ? leftTy : rightTy;
    }
    if (unified == nullptr) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        return overloadedType;
      }
      throw TypeCheckError(span, "Operator {} not applicable to {} and {}", NAMEOF_ENUM(op),
                           leftTy->toString(), rightTy->toString());
    }
    if (!unified->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        return overloadedType;
      }
      throw TypeCheckError(span, "Arithmetic operator requires numeric types");
    }
    return unified;
  case TokenType::EQUAL_EQUAL:
  case TokenType::BANG_EQUAL:
  case TokenType::GREATER:
  case TokenType::GREATER_EQUAL:
  case TokenType::LESS:
  case TokenType::LESS_EQUAL:
    if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
      return overloadedType;
    }
    if (leftTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) ||
        rightTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS})) {
      if (leftTy != rightTy) {
        throw TypeCheckError(span, "Comparison requires operands of the same enum or class type");
      }
    }
    if (!hasGeneric && unified == nullptr && !leftTy->isEqual(rightTy)) {
      throw TypeCheckError(span, "Comparison requires compatible types: {} and {}",
                           leftTy->toString(), rightTy->toString());
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  case TokenType::AND:
  case TokenType::OR:
    if (!hasGeneric && (leftTy == nullptr || !leftTy->is(BaseType::TY_BOOL) || rightTy == nullptr ||
                        !rightTy->is(BaseType::TY_BOOL))) {
      throw TypeCheckError(span, "Logical operator requires Bool operands");
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  default:
    throw TypeCheckError(span, "Unsupported binary operator: {}", NAMEOF_ENUM(op));
  }
}

auto Typechecker::isAssignableTo(Type* from, Type* to) -> bool {
  if (to == nullptr) {
    return true;
  }
  if (from == nullptr) {
    return false;
  }
  if (from->is(BaseType::TY_GENERIC) || to->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (from->isEqual(to)) {
    return true;
  }
  if (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr &&
      from->getElementType()->is(BaseType::TY_CLASS) && to->is(BaseType::TY_CLASS)) {
    return from->getElementType()->isEqual(to);
  }
  if (to->is(BaseType::TY_PTR) && to->getElementType() != nullptr &&
      to->getElementType()->is(BaseType::TY_CLASS) && from->is(BaseType::TY_CLASS)) {
    return from->isEqual(to->getElementType());
  }
  // Void is only assignable to Void
  if (from->is(BaseType::TY_VOID)) {
    return to->is(BaseType::TY_VOID);
  }
  if (to->is(BaseType::TY_INT)) {
    return from->is(BaseType::TY_INT) || from->is(BaseType::TY_FLOAT);
  }
  if (to->is(BaseType::TY_FLOAT)) {
    return from->is(BaseType::TY_INT) || from->is(BaseType::TY_FLOAT);
  }
  if (to->is(BaseType::TY_STRING)) {
    return from->is(BaseType::TY_STRING) ||
           (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr &&
            (from->getElementType()->is(BaseType::TY_INT) ||
             from->getElementType()->is(BaseType::TY_VOID)));
  }
  if (to->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    const std::string& want = to->getDisplayName();
    Type* cls = from;
    if (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr &&
        from->getElementType()->is(BaseType::TY_CLASS)) {
      cls = from->getElementType();
    }
    if (cls->is(BaseType::TY_CLASS)) {
      return classDeclaresTrait(cls, want);
    }
    if (from->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
      return from->getDisplayName() == want;
    }
    return false;
  }
  return false;
}

auto Typechecker::resolveType(const TypeExpr* node) -> Type* {
  if (node->getType() == TokenType::INT_TYPE) {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(64);
    return cacheType(std::move(u));
  }
  if (node->getType() == TokenType::INT8_TYPE) {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(8);
    return cacheType(std::move(u));
  }
  if (node->getType() == TokenType::INT16_TYPE) {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(16);
    return cacheType(std::move(u));
  }
  if (node->getType() == TokenType::INT32_TYPE) {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(32);
    return cacheType(std::move(u));
  }
  if (node->getType() == TokenType::FLOAT_TYPE || node->getType() == TokenType::FLOAT32_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_FLOAT));
  }
  if (node->getType() == TokenType::BOOL_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  }
  if (node->getType() == TokenType::STRING_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_STRING));
  }
  if (node->getType() == TokenType::VOID_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  }
  if (node->getType() == TokenType::PTR_TYPE) {
    node->getElementType()->accept(*this);
    Type* elem = result->getType();
    if (elem->is(BaseType::TY_FUNCTION)) {
      return elem; // Function type is already a pointer in Lesma
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (node->getType() == TokenType::FUNC_TYPE) {
    node->getReturnType()->accept(*this);
    Type* retType = result->getType();
    std::vector<std::unique_ptr<Field>> fields;
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      fields.push_back(std::make_unique<Field>(result->getName(), result->getType()));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(retType);
    return cacheType(std::move(funcType));
  }
  if (node->getType() == TokenType::CUSTOM_TYPE) {
    const std::string lookupName = node->getLookupName();
    auto genericIt = currentGenericTypes.find(lookupName);
    if (genericIt != currentGenericTypes.end()) {
      if (Value* genericSymbol = scope->lookup(lookupName)) {
        node->setResolvedSymbol(genericSymbol);
      }
      return genericIt->second;
    }
    std::vector<Type*> explicitTypeArgs;
    for (TypeExpr* typeArg : node->getTypeArgs()) {
      typeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
    }
    if (lookupName == "__buffer") {
      if (explicitTypeArgs.size() != 1U) {
        throw TypeCheckError(node->getSpan(), "__buffer<T> expects exactly one type argument");
      }
      auto* bufferType =
          cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, explicitTypeArgs.front()));
      bufferType->setDisplayName(node->getName());
      return bufferType;
    }
    Type* typ = scope->lookupType(lookupName);
    Value* sym = scope->lookupStruct(lookupName);
    bool resolvedFromImport = false;
    if ((typ == nullptr && sym == nullptr) ||
        (sym != nullptr && sym->getType()->is(BaseType::TY_IMPORT))) {
      auto importedIt = importedNameToSource.find(lookupName);
      if (importedIt != importedNameToSource.end()) {
        SymbolTable* importScope = getOrTypecheckImport(importedIt->second.first);
        if (importScope != nullptr) {
          sym = importScope->lookupStruct(importedIt->second.second);
          typ = importScope->lookupType(importedIt->second.second);
          resolvedFromImport = true;
        }
      }
    }
    if (typ == nullptr && sym == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Type '{}' not found. If you meant a generic type parameter, add it "
                           "to the generic parameter list (e.g. def foo<T>(x: T) -> T).",
                           node->getName());
    }
    if (sym == nullptr) {
      sym = scope->lookup(lookupName);
    }
    node->setResolvedSymbol(sym);
    Type* resolvedType = sym != nullptr ? sym->getType() : typ;
    if (resolvedFromImport) {
      resolvedType = materializeImportedType(resolvedType);
    }
    if (explicitTypeArgs.empty()) {
      return resolvedType;
    }
    if (resolvedType == nullptr || !resolvedType->is(BaseType::TY_CLASS)) {
      throw TypeCheckError(node->getSpan(), "Type {} is not a generic class", node->getName());
    }
    Type* classTemplate = resolvedType;
    auto templateIt = specializedTypeToTemplate.find(classTemplate);
    if (templateIt != specializedTypeToTemplate.end()) {
      classTemplate = templateIt->second;
    }
    const auto& genericParamNames = classTemplate->getGenericParams();
    if (genericParamNames.size() != explicitTypeArgs.size()) {
      throw TypeCheckError(node->getSpan(), "Generic class {} expects {} type arguments, got {}",
                           node->getName(), genericParamNames.size(), explicitTypeArgs.size());
    }
    std::unordered_map<std::string, Type*> env;
    for (size_t i = 0; i < genericParamNames.size(); ++i) {
      env[genericParamNames[i]] = explicitTypeArgs[i];
    }
    return getOrCreateSpecializedClassType(classTemplate, genericParamNames, env);
  }
  throw TypeCheckError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
}

Typechecker::Typechecker()
    : rootScope(std::make_unique<SymbolTable>(nullptr)), scope(rootScope.get()) {}

Typechecker::Typechecker(std::string mainFilePath, GetExportsFn getExports)
    : rootScope(std::make_unique<SymbolTable>(nullptr)), scope(rootScope.get()),
      mainFilePath(std::move(mainFilePath)), getExports(std::move(getExports)) {}

void Typechecker::loadImplicitStdModule(const std::string& moduleFilename) {
  const auto basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / moduleFilename)
          .lexically_normal();
  if (!mainFilePath.empty() &&
      std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal() ==
          basePath) {
    return;
  }

  SymbolTable* baseScope = getOrTypecheckImport(basePath.string());
  if (baseScope == nullptr) {
    return;
  }

  auto* importType = cacheType(std::make_unique<Type>(BaseType::TY_IMPORT));
  for (auto* sym : baseScope->getSymbols()) {
    if (!sym->isExported()) {
      continue;
    }
    const std::string& name = sym->getName();
    if (importedNameToSource.contains(name)) {
      continue;
    }
    auto symbol = std::make_unique<Value>(name, importType);
    symbol->setCategory(ValueCategory::MODULE_SYMBOL);
    scope->insertSymbol(std::move(symbol));
    importedNameToSource[name] = std::make_pair(basePath.string(), name);
  }
  // Non-exported classes/enums may appear in exported members' signatures; resolve them
  // via the same module path as for exported types (not added as top-level module names).
  for (auto* sym : baseScope->getSymbols()) {
    if (sym->isExported()) {
      continue;
    }
    if (!sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS})) {
      continue;
    }
    const std::string& name = sym->getName();
    if (importedNameToSource.contains(name)) {
      continue;
    }
    importedNameToSource[name] = std::make_pair(basePath.string(), name);
  }
}

auto Typechecker::resolveImportPath(const std::string& filepath, bool isStd) const -> std::string {
  if (isStd || mainFilePath.empty()) {
    return filepath;
  }
  return fmt::format("{}/{}", std::filesystem::absolute(mainFilePath).parent_path().string(),
                     filepath);
}

auto Typechecker::getOrTypecheckImport(const std::string& absolutePath) -> SymbolTable* {
  auto it = importedModuleCache.find(absolutePath);
  if (it != importedModuleCache.end()) {
    return it->second != nullptr ? it->second->rootScope.get() : nullptr;
  }
  auto buffer = llvm::MemoryBuffer::getFile(absolutePath);
  if (!buffer) {
    return nullptr;
  }
  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  unsigned const bufferId = srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  auto lexer = std::make_unique<Lexer>(srcMgr);
  lexer->scanAll();
  auto parser = std::make_unique<Parser>(lexer->getTokens());
  parser->parse();
  Compound* ast = parser->getAst();
  if (ast == nullptr) {
    return nullptr;
  }
  Typechecker sub(absolutePath, getExports);
  sub.run(ast);
  auto imported = std::make_shared<ImportedModuleAnalysis>();
  imported->sourceMgr = std::move(srcMgr);
  imported->mainBufferId = bufferId;
  imported->mainFilePath = absolutePath;
  imported->parser = std::move(parser);
  imported->rootScope = sub.takeRootScope();
  imported->typeCache = sub.takeTypeCache();
  imported->index =
      buildAnalysisIndex(imported->parser != nullptr ? imported->parser->getAst() : nullptr,
                         imported->sourceMgr.get(), imported->mainBufferId);
  imported->importAliasToPath = sub.takeImportAliasToPath();
  imported->importedNameToSource = sub.takeImportedNameToSource();
  imported->importedModules = sub.takeImportedModules();
  SymbolTable* scopePtr = imported->rootScope.get();
  importedModuleCache[absolutePath] = std::move(imported);
  return scopePtr;
}

auto Typechecker::run(const Compound* ast) -> void {
  const auto currentPath =
      mainFilePath.empty()
          ? std::filesystem::path()
          : std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal();
  const auto basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  const auto listPath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "list.les").lexically_normal();
  if (currentPath != basePath && currentPath != listPath) {
    loadImplicitStdModule("base.les");
    loadImplicitStdModule("list.les");
  }
  declarationPass = true;
  ast->accept(*this);
  declarationPass = false;
  ast->accept(*this);
}

auto Typechecker::takeRootScope() -> std::unique_ptr<SymbolTable> {
  scope = nullptr;
  return std::move(rootScope);
}

auto Typechecker::takeTypeCache() -> std::vector<std::unique_ptr<Type>> {
  return std::move(typeCache);
}

auto Typechecker::takeImportAliasToPath() -> ImportAliasMap { return std::move(importAliasToPath); }

auto Typechecker::takeImportedNameToSource() -> ImportedNameSourceMap {
  return std::move(importedNameToSource);
}

auto Typechecker::takeImportedModules()
    -> std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> {
  return std::move(importedModuleCache);
}

auto Typechecker::visit(const Statement* /*node*/) -> void {}
auto Typechecker::visit(const Expression* /*node*/) -> void {}
auto Typechecker::visit(const Else* /*node*/) -> void {}

auto Typechecker::visit(const Compound* node) -> void {
  for (Statement* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

auto Typechecker::visit(const VarDecl* node) -> void {
  Type* declType = nullptr;
  if (node->getType() != nullptr) {
    node->getType()->accept(*this);
    declType = result->getType();
  }
  if (node->getValue() != nullptr) {
    visitExprWithExpectedType(node->getValue(), declType);
    Type* initType = result->getType();
    if (declType != nullptr) {
      if (!isAssignableTo(initType, declType)) {
        throw TypeCheckError(node->getSpan(),
                             "Variable initializer type {} is not assignable "
                             "to declared type {}",
                             initType->toString(), declType->toString());
      }
    } else {
      declType = initType;
    }
  }
  if (declType == nullptr) {
    throw TypeCheckError(node->getSpan(), "Variable {} has no type and no initializer",
                         node->getIdentifier()->getValue());
  }
  // Typechecker does not allocate; we only register the symbol for lookup.
  auto symbol = std::make_unique<Value>(node->getIdentifier()->getValue(), declType,
                                        SymbolState::INITIALIZED);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setDeclarationKind(ValueDeclarationKind::VARIABLE);
  symbol->setMutable(node->getMutability());
  symbol->setDeclarationSpan(node->getIdentifier()->getSpan());
  symbol->setDeclarationFilePath(mainFilePath);
  node->setResolvedSymbol(symbol.get());
  scope->insertSymbol(std::move(symbol));
}

auto Typechecker::visit(const If* node) -> void {
  for (Expression* cond : node->getConds()) {
    cond->accept(*this);
    if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
        !result->getType()->is(BaseType::TY_GENERIC)) {
      throw TypeCheckError(cond->getSpan(), "Condition must be Bool, got {}",
                           result->getType()->toString());
    }
  }
  for (Compound* block : node->getBlocks()) {
    block->accept(*this);
  }
}

auto Typechecker::visit(const While* node) -> void {
  node->getCond()->accept(*this);
  if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
      !result->getType()->is(BaseType::TY_GENERIC)) {
    throw TypeCheckError(node->getCond()->getSpan(), "Condition must be Bool, got {}",
                         result->getType()->toString());
  }
  node->getBlock()->accept(*this);
}

auto Typechecker::visit(const ForIn* node) -> void {
  node->getIterable()->accept(*this);
  Type* iterableType = result->getType();
  Type* loopVarType = nullptr;
  auto peelPtr = [](Type* type) -> Type* {
    Type* t = type;
    while (t != nullptr && t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
      t = t->getElementType();
    }
    return t;
  };
  auto isStdListClassType = [this, peelPtr](Type* type) -> bool {
    Type* t = peelPtr(type);
    if (t == nullptr || !t->is(BaseType::TY_CLASS)) {
      return false;
    }
    Type* baseType = t;
    if (auto it = specializedTypeToTemplate.find(t); it != specializedTypeToTemplate.end()) {
      baseType = it->second;
    }
    const std::string& displayName = baseType->getDisplayName();
    return displayName == "list<T>" || displayName == "list";
  };
  auto getStdListBufferType = [peelPtr, &isStdListClassType](Type* type) -> Type* {
    if (!isStdListClassType(type)) {
      return nullptr;
    }
    Type* t = peelPtr(type);
    auto fields = t->getFields();
    if (fields.empty() || fields.front()->type == nullptr ||
        !fields.front()->type->is(BaseType::TY_ARRAY)) {
      return nullptr;
    }
    return fields.front()->type;
  };
  if (iterableType != nullptr && iterableType->is(BaseType::TY_ARRAY) &&
      iterableType->getElementType() != nullptr) {
    loopVarType = iterableType->getElementType();
  } else if (Type* bufferType = getStdListBufferType(iterableType);
             bufferType != nullptr && bufferType->getElementType() != nullptr) {
    loopVarType = bufferType->getElementType();
  } else {
    Type* iteratorType = resolveMethodReturnType(iterableType, "iter", {}, node->getSpan());
    if (iteratorType == nullptr) {
      throw TypeCheckError(node->getIterable()->getSpan(),
                           "For-in requires list<T> or iter()/has_next()/next() protocol, got {}",
                           iterableType != nullptr ? iterableType->toString() : "unknown");
    }
    Type* hasNextType = resolveMethodReturnType(iteratorType, "has_next", {}, node->getSpan());
    if (hasNextType == nullptr || !hasNextType->is(BaseType::TY_BOOL)) {
      throw TypeCheckError(node->getIterable()->getSpan(),
                           "For-in iterator has_next() must return bool");
    }
    loopVarType = resolveMethodReturnType(iteratorType, "next", {}, node->getSpan());
    if (loopVarType == nullptr) {
      throw TypeCheckError(node->getIterable()->getSpan(), "For-in iterator must define next()");
    }
  }

  SymbolTable* child = scope->createChildBlock("for");
  node->setBodyScope(child);
  SymbolTable* savedScope = scope;
  scope = child;

  auto symbol = std::make_unique<Value>(node->getIdentifier()->getValue(), loopVarType,
                                        SymbolState::INITIALIZED);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setMutable(false);
  symbol->setDeclarationSpan(node->getIdentifier()->getSpan());
  symbol->setDeclarationFilePath(mainFilePath);
  node->getIdentifier()->setResolvedSymbol(symbol.get());
  scope->insertSymbol(std::move(symbol));

  node->getBlock()->accept(*this);
  scope = savedScope;
}

auto Typechecker::registerTraitsFromImportedModule(const std::string& absolutePath) -> void {
  if (absolutePath.empty()) {
    return;
  }
  (void) getOrTypecheckImport(absolutePath);
  auto it = importedModuleCache.find(absolutePath);
  if (it == importedModuleCache.end() || it->second == nullptr || it->second->parser == nullptr) {
    return;
  }
  Compound* ast = it->second->parser->getAst();
  if (ast == nullptr || it->second->rootScope == nullptr) {
    return;
  }
  SymbolTable* savedScope = scope;
  scope = it->second->rootScope.get();
  for (Statement* stmt : ast->getChildren()) {
    if (auto* tr = dynamic_cast<TraitDecl*>(stmt)) {
      if (!traitRegistry.contains(tr->getIdentifier())) {
        traitRegistry[tr->getIdentifier()] = tr;
        traitMethodReturnTypes[tr->getIdentifier()].clear();
        auto savedImpTraitG = currentGenericTypes;
        for (const auto& p : tr->getGenericParamDecls()) {
          currentGenericTypes[p.name] = cacheType(std::make_unique<Type>(p.name));
        }
        for (FuncDecl* req : tr->getRequirements()) {
          req->getReturnType()->accept(*this);
          traitMethodReturnTypes[tr->getIdentifier()][req->getName()] = result->getType();
        }
        currentGenericTypes = std::move(savedImpTraitG);
      }
    }
  }
  scope = savedScope;
}

auto Typechecker::visit(const Import* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto* importType = cacheType(std::make_unique<Type>(BaseType::TY_IMPORT));
  const std::string resolvedPath = resolveImportPath(node->getFilePath(), node->isStd());
  if (!node->isStd()) {
    registerTraitsFromImportedModule(resolvedPath);
  }
  auto addImportSymbol = [this, &importType](const std::string& name) {
    if (!name.empty()) {
      auto symbol = std::make_unique<Value>(name, importType);
      symbol->setCategory(ValueCategory::MODULE_SYMBOL);
      symbol->setDeclarationKind(ValueDeclarationKind::NAMESPACE);
      scope->insertSymbol(std::move(symbol));
    }
  };
  if (node->getImportAll() && getExports) {
    std::vector<std::string> names = getExports(node->getFilePath(), node->isStd(), mainFilePath);
    for (const std::string& name : names) {
      addImportSymbol(name);
      importedNameToSource[name] = std::make_pair(resolvedPath, name);
    }
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName);
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  if (node->getImportedNames().empty()) {
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName);
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  for (const ImportedNameBinding& binding : node->getImportedNames()) {
    const std::string& name = binding.name;
    const std::string& alias = binding.alias;
    const std::string localName = alias.empty() ? name : alias;
    addImportSymbol(localName);
    importedNameToSource[localName] = std::make_pair(resolvedPath, name);
  }
}

auto Typechecker::visit(const Enum* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto type =
      std::make_unique<Type>(BaseType::TY_ENUM, nullptr, std::vector<std::unique_ptr<Field>>{});
  Type* typePtr = type.get();
  type->setDisplayName(node->getIdentifier());
  std::vector<std::string> const values = node->getValues();
  std::vector<llvm::SMRange> const& valueSpans = node->getValueSpans();
  for (size_t i = 0; i < values.size(); ++i) {
    auto field = std::make_unique<Field>(values[i], typePtr);
    if (i < valueSpans.size()) {
      field->setDeclarationSpan(valueSpans[i]);
      field->setDeclarationFilePath(mainFilePath);
      auto memberSymbol = std::make_unique<Value>(values[i], typePtr);
      memberSymbol->setCategory(ValueCategory::DIRECT_VALUE);
      memberSymbol->setDeclarationKind(ValueDeclarationKind::ENUM_MEMBER);
      memberSymbol->setDeclarationSpan(valueSpans[i]);
      memberSymbol->setDeclarationFilePath(mainFilePath);
      field->setDeclarationSymbol(std::move(memberSymbol));
    }
    type->addField(std::move(field));
  }
  scope->insertType(node->getIdentifier(), std::move(type));
  auto enumSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
  enumSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  enumSymbol->setDeclarationKind(ValueDeclarationKind::ENUM);
  enumSymbol->setExported(node->isExported());
  enumSymbol->setDeclarationSpan(node->getNameSpan());
  enumSymbol->setDeclarationFilePath(mainFilePath);
  scope->insertSymbol(std::move(enumSymbol));
  node->setResolvedSymbol(scope->lookupStruct(node->getIdentifier()));
}

auto Typechecker::visit(const Class* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* outerScope = scope;
  SymbolTable* classGenericScope = outerScope->createChildBlock("class_generics");
  scope = classGenericScope;
  node->setGenericScope(classGenericScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(classGenericScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);

  Type* classTypePtr = outerScope->lookupType(node->getIdentifier());
  if (declarationPass) {
    std::vector<std::unique_ptr<Field>> fields;
    for (VarDecl* field : node->getFields()) {
      Type* fieldType = nullptr;
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
        fieldType = result->getType();
      }
      if (field->getValue() != nullptr) {
        field->getValue()->accept(*this);
        Type* initType = result->getType();
        if (fieldType != nullptr) {
          if (!isAssignableTo(initType, fieldType)) {
            throw TypeCheckError(
                field->getSpan(),
                "Class field initializer type {} is not assignable to declared type {}",
                initType->toString(), fieldType->toString());
          }
        } else {
          fieldType = initType;
        }
      }
      if (fieldType == nullptr) {
        throw TypeCheckError(field->getSpan(), "Class field has no type");
      }
      auto fieldEntry = std::make_unique<Field>(field->getIdentifier()->getValue(), fieldType);
      fieldEntry->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldEntry->setDeclarationFilePath(mainFilePath);
      auto fieldSymbol = std::make_unique<Value>(field->getIdentifier()->getValue(), fieldType,
                                                 SymbolState::INITIALIZED);
      fieldSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      fieldSymbol->setDeclarationKind(ValueDeclarationKind::PROPERTY);
      fieldSymbol->setMutable(field->getMutability());
      fieldSymbol->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldSymbol->setDeclarationFilePath(mainFilePath);
      field->setResolvedSymbol(fieldSymbol.get());
      fieldEntry->setDeclarationSymbol(std::move(fieldSymbol));
      fields.push_back(std::move(fieldEntry));
    }
    auto type = std::make_unique<Type>(BaseType::TY_CLASS, nullptr, std::move(fields));
    type->setDisplayName(node->getIdentifier() +
                         makeGenericDisplaySuffix(node->getGenericParams()));
    type->setDeclarationSpan(node->getNameSpan());
    type->setDeclarationFilePath(mainFilePath);
    classTypePtr = type.get();
    outerScope->insertType(node->getIdentifier(), std::move(type));
    auto classSymbol = std::make_unique<Value>(node->getIdentifier(), classTypePtr);
    classSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    classSymbol->setDeclarationKind(ValueDeclarationKind::CLASS);
    classSymbol->setExported(node->isExported());
    classSymbol->setDeclarationSpan(node->getNameSpan());
    classSymbol->setDeclarationFilePath(mainFilePath);
    outerScope->insertSymbol(std::move(classSymbol));
    node->setResolvedSymbol(outerScope->lookupStruct(node->getIdentifier()));
  }

  classTypePtr->setGenericParams(node->getGenericParams());
  classTypePtr->setImplTraitNames(node->getImplTraitNames());
  auto* selfPtrType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classTypePtr));
  SymbolTable* savedMethodInsertScope = currentMethodInsertScope;
  for (FuncDecl* func : node->getMethods()) {
    currentClassType = classTypePtr;
    currentMethodInsertScope = outerScope;
    SymbolTable* methodScope = scope->createChildBlock("method");
    scope = methodScope;
    auto selfSymbol = std::make_unique<Value>("self", selfPtrType);
    selfSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    scope->insertSymbol(std::move(selfSymbol));
    func->accept(*this);
    scope = scope->getParent();
    currentClassType = nullptr;
  }
  currentMethodInsertScope = savedMethodInsertScope;

  if (declarationPass) {
    // Impl check runs in the declaration pass so default trait methods are registered before user
    // code in the second pass. Trait requirement *signatures* may reference classes declared later
    // in the file (resolved when checking each class after that class's type exists).
    checkTraitImplementation(node, classTypePtr, outerScope);
  } else {
    typecheckTraitDefaultBodies(node, classTypePtr, outerScope);
  }

  scope = outerScope;
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::visit(const FuncDecl* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* genericsScope = scope->createChildBlock("generics");
  scope = genericsScope;
  node->setGenericScope(genericsScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(genericsScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  if (currentClassType != nullptr) {
    Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, currentClassType));
    paramFields.push_back(std::make_unique<Field>("self", selfPtr));
    paramTypes.push_back(selfPtr);
  }
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    // Match codegen: function params use pointer-to-class so lookup matches
    if (paramType->is(BaseType::TY_CLASS)) {
      paramType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramType));
    }
    if (param->defaultVal != nullptr) {
      param->defaultVal->accept(*this);
      if (!isAssignableTo(result->getType(), paramType)) {
        throw TypeCheckError(param->defaultVal->getSpan(),
                             "Default value type {} is not assignable to parameter type {}",
                             result->getType()->toString(), paramType->toString());
      }
    }
    paramTypes.push_back(paramType);
    std::unique_ptr<Field> field;
    if (param->defaultVal != nullptr) {
      field = std::make_unique<Field>(param->name, paramType, std::make_unique<Value>(paramType));
    } else {
      field = std::make_unique<Field>(param->name, paramType);
    }
    paramFields.push_back(std::move(field));
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  Type* funcTypePtr = cacheType(std::move(funcType));
  funcTypePtr->setGenericParams(node->getGenericParams());
  {
    std::vector<std::vector<std::string>> tb;
    tb.reserve(node->getGenericParamDecls().size());
    for (const auto& p : node->getGenericParamDecls()) {
      tb.push_back(p.traitBounds);
    }
    funcTypePtr->setGenericParamTraitBounds(std::move(tb));
  }
  SymbolTable* insertScope =
      currentMethodInsertScope != nullptr ? currentMethodInsertScope : scope->getParent();
  Value* funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes);

  if (declarationPass) {
    if (funcSymbol == nullptr) {
      auto declaredFunc = std::make_unique<Value>(node->getName(), funcTypePtr);
      declaredFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
      declaredFunc->setDeclarationKind(currentClassType != nullptr
                                           ? ValueDeclarationKind::METHOD
                                           : ValueDeclarationKind::FUNCTION);
      declaredFunc->setExported(node->isExported());
      declaredFunc->setDeclarationSpan(node->getNameSpan());
      declaredFunc->setDeclarationFilePath(mainFilePath);
      insertScope->insertSymbol(std::move(declaredFunc));
      funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes);
      // Set resolvedSymbol immediately after we get the symbol for this exact overload
      if (funcSymbol != nullptr) {
        node->setResolvedSymbol(funcSymbol);
      }
    } else {
      funcSymbol->setType(funcTypePtr);
      funcSymbol->setDeclarationKind(currentClassType != nullptr ? ValueDeclarationKind::METHOD
                                                                 : ValueDeclarationKind::FUNCTION);
      funcSymbol->setExported(node->isExported());
      funcSymbol->setDeclarationSpan(node->getNameSpan());
      funcSymbol->setDeclarationFilePath(mainFilePath);
      // Set resolvedSymbol for existing symbol (this exact overload)
      node->setResolvedSymbol(funcSymbol);
    }

    if (funcSymbol != nullptr && funcSymbol->getBodyScope() == nullptr) {
      SymbolTable* child = scope->createChildBlock("function");
      funcSymbol->setBodyScope(child);
      SymbolTable* savedScopePtr = scope;
      scope = child;
      const size_t paramOffset = (currentClassType != nullptr) ? 1U : 0U;
      for (size_t i = 0; i < node->getParameters().size(); ++i) {
        Parameter* param = node->getParameters()[i];
        auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[paramOffset + i]);
        paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
        paramSymbol->setDeclarationKind(ValueDeclarationKind::PARAMETER);
        paramSymbol->setDeclarationSpan(param->nameSpan);
        paramSymbol->setDeclarationFilePath(mainFilePath);
        param->setResolvedSymbol(paramSymbol.get());
        scope->insertSymbol(std::move(paramSymbol));
      }
      scope = savedScopePtr;
    }
  } else {
    currentFunction = funcSymbol;
    if (currentFunction == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found during definition pass: {}",
                           node->getName());
    }
    // Set resolvedSymbol in definition pass (should already be set in declaration pass, but ensure
    // it)
    node->setResolvedSymbol(currentFunction);
    scope = currentFunction->getBodyScope();
    inTopLevel = false;
    auto savedTraitBounds = currentGenericParamTraitBounds;
    currentGenericParamTraitBounds.clear();
    for (const auto& p : node->getGenericParamDecls()) {
      currentGenericParamTraitBounds[p.name] = p.traitBounds;
    }
    if (node->getBody() != nullptr) {
      node->getBody()->accept(*this);
    }
    currentGenericParamTraitBounds = std::move(savedTraitBounds);
    Type* funcReturnType = currentFunction->getType()->getReturnType();
    if (node->getBody() != nullptr && funcReturnType != nullptr &&
        !funcReturnType->is(BaseType::TY_VOID) && !blockAlwaysReturns(node->getBody())) {
      throw TypeCheckError(node->getSpan(), "Non-void function may reach end without returning");
    }
    scope = scope->getParent();
    currentFunction = nullptr;
    inTopLevel = true;
  }
  scope = scope->getParent(); // pop generics scope so generic param names are not visible to outer
                              // lookups
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::visit(const ExternFuncDecl* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto savedGenerics = currentGenericTypes;
  SymbolTable* genericsScope = scope->createChildBlock("generics");
  scope = genericsScope;
  node->setGenericScope(genericsScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(genericsScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    if (param->defaultVal != nullptr) {
      param->defaultVal->accept(*this);
      if (!isAssignableTo(result->getType(), paramType)) {
        throw TypeCheckError(param->defaultVal->getSpan(),
                             "Default value type {} is not assignable to parameter type {}",
                             result->getType()->toString(), paramType->toString());
      }
    }
    paramTypes.push_back(paramType);
    if (param->defaultVal != nullptr) {
      paramFields.push_back(
          std::make_unique<Field>(param->name, paramType, std::make_unique<Value>(paramType)));
    } else {
      paramFields.push_back(std::make_unique<Field>(param->name, paramType));
    }
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  Type* funcTypePtr = cacheType(std::move(funcType));
  funcTypePtr->setGenericParams(node->getGenericParams());
  {
    std::vector<std::vector<std::string>> tb;
    tb.reserve(node->getGenericParamDecls().size());
    for (const auto& p : node->getGenericParamDecls()) {
      tb.push_back(p.traitBounds);
    }
    funcTypePtr->setGenericParamTraitBounds(std::move(tb));
  }
  Value* existingFunc = scope->getParent()->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    auto funcSymbol = std::make_unique<Value>(node->getName(), funcTypePtr);
    funcSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
    funcSymbol->setDeclarationKind(ValueDeclarationKind::FUNCTION);
    funcSymbol->setExported(node->isExported());
    funcSymbol->setDeclarationSpan(node->getNameSpan());
    funcSymbol->setDeclarationFilePath(mainFilePath);
    scope->getParent()->insertSymbol(
        std::move(funcSymbol)); // insert into enclosing scope, not generics
    existingFunc = scope->getParent()->lookupFunction(node->getName(), paramTypes);
  } else {
    existingFunc->setType(funcTypePtr);
    existingFunc->setDeclarationKind(ValueDeclarationKind::FUNCTION);
    existingFunc->setExported(node->isExported());
    existingFunc->setDeclarationSpan(node->getNameSpan());
    existingFunc->setDeclarationFilePath(mainFilePath);
  }
  if (existingFunc != nullptr) {
    node->setResolvedSymbol(existingFunc);
  }
  if (existingFunc != nullptr && existingFunc->getBodyScope() == nullptr) {
    SymbolTable* child = scope->createChildBlock("extern_function");
    existingFunc->setBodyScope(child);
    SymbolTable* savedScopePtr = scope;
    scope = child;
    for (size_t i = 0; i < node->getParameters().size(); ++i) {
      Parameter* param = node->getParameters()[i];
      auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[i]);
      paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      paramSymbol->setDeclarationKind(ValueDeclarationKind::PARAMETER);
      paramSymbol->setDeclarationSpan(param->nameSpan);
      paramSymbol->setDeclarationFilePath(mainFilePath);
      param->setResolvedSymbol(paramSymbol.get());
      scope->insertSymbol(std::move(paramSymbol));
    }
    scope = savedScopePtr;
  }
  scope = scope->getParent(); // pop generics scope
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::compoundToBinaryOp(TokenType op) -> std::optional<TokenType> {
  switch (op) {
  case TokenType::PLUS_EQUAL:
    return TokenType::PLUS;
  case TokenType::MINUS_EQUAL:
    return TokenType::MINUS;
  case TokenType::STAR_EQUAL:
    return TokenType::STAR;
  case TokenType::SLASH_EQUAL:
    return TokenType::SLASH;
  case TokenType::MOD_EQUAL:
    return TokenType::MOD;
  case TokenType::POWER_EQUAL:
    return TokenType::POWER;
  default:
    return std::nullopt;
  }
}

auto Typechecker::visit(const Assignment* node) -> void {
  TokenType assignOp = node->getOperator();
  std::optional<TokenType> binaryOp = compoundToBinaryOp(assignOp);

  if (auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide())) {
    Value* sym = scope->lookup(lit->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Variable not found: {}", lit->getValue());
    }
    if (!sym->getMutability()) {
      throw TypeCheckError(node->getSpan(), "Cannot assign to immutable variable {}",
                           lit->getValue());
    }
    Type* lhsType = sym->getType();
    visitExprWithExpectedType(node->getRightHandSide(), lhsType);
    Type* rhsType = result->getType();
    if (binaryOp.has_value()) {
      Type* resultType = typecheckBinaryOpResult(*binaryOp, lhsType, rhsType, node->getSpan());
      if (!isAssignableTo(resultType, lhsType)) {
        throw TypeCheckError(
            node->getSpan(),
            "Compound assignment result type {} is not assignable to variable of type {}",
            resultType->toString(), lhsType->toString());
      }
    } else {
      if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      }
      if (!isAssignableTo(rhsType, lhsType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to variable of type {}",
                             rhsType->toString(), lhsType->toString());
      }
    }
    return;
  }
  if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    Type* lhsType = result->getType();
    Type* targetType = lhsType;
    visitExprWithExpectedType(node->getRightHandSide(), targetType);
    Type* rhsType = result->getType();
    if (binaryOp.has_value()) {
      Type* resultType = typecheckBinaryOpResult(*binaryOp, targetType, rhsType, node->getSpan());
      if (targetType != nullptr && !isAssignableTo(resultType, targetType)) {
        throw TypeCheckError(
            node->getSpan(),
            "Compound assignment result type {} is not assignable to field of type {}",
            resultType->toString(), targetType->toString());
      }
    } else {
      if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      }
      if (targetType != nullptr && !isAssignableTo(rhsType, targetType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to field of type {}",
                             rhsType->toString(), targetType->toString());
      }
    }
    return;
  }
  if (auto* subscript = dynamic_cast<SubscriptOp*>(node->getLeftHandSide())) {
    if (auto* baseLit = dynamic_cast<Literal*>(subscript->getLeft())) {
      Value* sym = scope->lookup(baseLit->getValue());
      if (sym != nullptr && !sym->getMutability()) {
        throw TypeCheckError(node->getSpan(), "Cannot assign through immutable variable {}",
                             baseLit->getValue());
      }
    }
    subscript->getLeft()->accept(*this);
    Type* baseType = result->getType();
    subscript->getIndex()->accept(*this);
    Type* indexType = result->getType();
    if (baseType != nullptr && baseType->is(BaseType::TY_ARRAY) &&
        baseType->getElementType() != nullptr) {
      node->getLeftHandSide()->accept(*this);
      Type* lhsType = result->getType();
      visitExprWithExpectedType(node->getRightHandSide(), lhsType);
      Type* rhsType = result->getType();
      if (binaryOp.has_value()) {
        Type* resultType = typecheckBinaryOpResult(*binaryOp, lhsType, rhsType, node->getSpan());
        if (lhsType != nullptr && !isAssignableTo(resultType, lhsType)) {
          throw TypeCheckError(
              node->getSpan(),
              "Compound assignment result type {} is not assignable to list element type {}",
              resultType->toString(), lhsType->toString());
        }
      } else {
        if (assignOp != TokenType::EQUAL) {
          throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                               NAMEOF_ENUM(assignOp));
        }
        if (lhsType != nullptr && !isAssignableTo(rhsType, lhsType)) {
          throw TypeCheckError(node->getSpan(), "Cannot assign type {} to list element of type {}",
                               rhsType->toString(), lhsType->toString());
        }
      }
      return;
    }

    if (binaryOp.has_value()) {
      Type* lhsType = resolveMethodReturnType(
          baseType, std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexType}, node->getSpan());
      if (lhsType == nullptr) {
        throw TypeCheckError(node->getSpan(), "Operator [] not found for assignment target");
      }
      visitExprWithExpectedType(node->getRightHandSide(), lhsType);
      Type* rhsType = result->getType();
      Type* resultType = typecheckBinaryOpResult(*binaryOp, lhsType, rhsType, node->getSpan());
      if (resolveMethodReturnType(baseType, std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                  {indexType, resultType}, node->getSpan()) == nullptr) {
        throw TypeCheckError(node->getSpan(), "Operator []= not found for assignment target");
      }
    } else {
      if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      }
      node->getRightHandSide()->accept(*this);
      Type* rhsType = result->getType();
      if (resolveMethodReturnType(baseType, std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                  {indexType, rhsType}, node->getSpan()) == nullptr) {
        throw TypeCheckError(node->getSpan(), "Operator []= not found for assignment target");
      }
    }
    return;
  }
  throw TypeCheckError(node->getSpan(), "Invalid assignment target");
}

auto Typechecker::visit(const Break* node) -> void {
  (void) node;
  // We don't track break targets in typecheck; Codegen will enforce.
}

auto Typechecker::visit(const Continue* node) -> void { (void) node; }

auto Typechecker::visit(const Return* node) -> void {
  if (currentFunction == nullptr) {
    throw TypeCheckError(node->getSpan(), "Return not allowed outside function");
  }
  Type* expected = currentFunction->getType()->getReturnType();
  if (node->getValue() == nullptr) {
    if (expected == nullptr || !expected->is(BaseType::TY_VOID)) {
      throw TypeCheckError(node->getSpan(), "Return type does not match: expected {}, got void",
                           expected != nullptr ? expected->toString() : "?");
    }
    return;
  }
  visitExprWithExpectedType(node->getValue(), expected);
  if (!isAssignableTo(result->getType(), expected)) {
    throw TypeCheckError(node->getSpan(), "Return type does not match: expected {}, got {}",
                         expected->toString(), result->getType()->toString());
  }
}

auto Typechecker::visit(const Defer* node) -> void { node->getStatement()->accept(*this); }

auto Typechecker::visit(const UnimplementedStatement* node) -> void {
  throw TypeCheckError(node->getSpan(), "{}", node->getMessage());
}

auto Typechecker::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Typechecker::visit(const FuncCall* node) -> void {
  std::vector<Type*> argTypes;
  for (Expression* arg : node->getArguments()) {
    arg->accept(*this);
    Type* t = result->getType();
    if (t->is(BaseType::TY_CLASS)) {
      t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
    }
    argTypes.push_back(t);
  }
  if (visitListIntrinsicCall(node, argTypes)) {
    return;
  }
  if (!node->getExplicitTypeArgs().empty()) {
    Value* classSym = scope->lookup(node->getName());
    if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
      node->setResolvedSymbol(classSym);
      Type* classType = classSym->getType();
      const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
      std::vector<Type*> explicitTypes;
      for (TypeExpr* texpr : node->getExplicitTypeArgs()) {
        texpr->accept(*this);
        explicitTypes.push_back(result->getType());
      }
      if (explicitTypes.size() != genericParamNames.size()) {
        throw TypeCheckError(node->getSpan(),
                             "Explicit type argument count {} does not match "
                             "generic class parameter count {}",
                             explicitTypes.size(), genericParamNames.size());
      }
      std::unordered_map<std::string, Type*> env;
      for (size_t i = 0; i < genericParamNames.size(); ++i) {
        env[genericParamNames[i]] = explicitTypes[i];
      }
      if (!argTypes.empty()) {
        Type* ptrToClass = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
        std::vector<Type*> constructorParamTypes = {ptrToClass};
        constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(), argTypes.end());
        Value* constructor = scope->lookupFunction("new", constructorParamTypes);
        if (constructor == nullptr) {
          throw TypeCheckError(node->getSpan(),
                               "Constructor not found for {} with given type arguments",
                               node->getName());
        }
        auto ctorParams = constructor->getType()->getFields();
        for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
          Type* expected = substituteInType(ctorParams[i]->type, env);
          if (expected != nullptr && !argTypes[i - 1]->isEqual(expected)) {
            throw TypeCheckError(node->getSpan(),
                                 "Argument type {} does not match explicit parameter type {}",
                                 argTypes[i - 1]->toString(), expected->toString());
          }
        }
      }
      Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
      result = std::make_unique<Value>(specialized);
      return;
    }
  }

  SymbolTable* importedScope = nullptr;
  Value* callee = scope->lookupFunction(node->getName(), argTypes);
  if (callee == nullptr) {
    Value* sym = scope->lookup(node->getName());
    std::string importedName;
    if (sym == nullptr || sym->getType()->is(BaseType::TY_IMPORT)) {
      auto importedIt = importedNameToSource.find(node->getName());
      if (importedIt != importedNameToSource.end()) {
        importedScope = getOrTypecheckImport(importedIt->second.first);
        importedName = importedIt->second.second;
        if (importedScope != nullptr) {
          sym = importedScope->lookup(importedName);
          callee = importedScope->lookupFunction(importedName, argTypes);
        }
      }
    }
    if (callee == nullptr && sym != nullptr) {
      node->setResolvedSymbol(sym);
      if (sym->getType()->is(BaseType::TY_CLASS)) {
        Type* classType =
            importedScope != nullptr ? materializeImportedType(sym->getType()) : sym->getType();
        if (!node->getArguments().empty()) {
          const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
          Type* ptrToClass =
              cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
          std::vector<Type*> constructorParamTypes = {ptrToClass};
          for (Type* t : argTypes) {
            constructorParamTypes.push_back(t);
          }
          Value* constructor = importedScope != nullptr
                                   ? importedScope->lookupFunction("new", constructorParamTypes)
                                   : scope->lookupFunction("new", constructorParamTypes);
          if (constructor != nullptr) {
            std::unordered_map<std::string, Type*> env;
            auto* funcType = constructor->getType();
            auto ctorParams = funcType->getFields();
            for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
              inferGenericBindings(ctorParams[i]->type, argTypes[i - 1], env, node->getSpan());
            }
            Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
            result = std::make_unique<Value>(
                importedScope != nullptr ? materializeImportedType(specialized) : specialized);
            return;
          }
        }
        result = std::make_unique<Value>(classType);
        return;
      }
      if (sym->getType()->is(BaseType::TY_ENUM)) {
        result = std::make_unique<Value>(
            importedScope != nullptr ? materializeImportedType(sym->getType()) : sym->getType());
        return;
      }
      if (sym->getType()->is(BaseType::TY_IMPORT)) {
        result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
        return;
      }
    }
    if (callee == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found: {}", node->getName());
    }
  }
  if (!callee->getType()->is(BaseType::TY_FUNCTION)) {
    throw TypeCheckError(node->getSpan(), "Not a function: {}", node->getName());
  }
  node->setResolvedSymbol(callee);

  auto* funcType = callee->getType();
  auto fields = funcType->getFields();

  if (!node->getExplicitTypeArgs().empty()) {
    std::vector<Type*> explicitTypes;
    for (TypeExpr* texpr : node->getExplicitTypeArgs()) {
      texpr->accept(*this);
      explicitTypes.push_back(result->getType());
    }
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(funcType);
    if (explicitTypes.size() != genericParamNames.size()) {
      throw TypeCheckError(node->getSpan(),
                           "Explicit type argument count {} does not match "
                           "generic parameter count {}",
                           explicitTypes.size(), genericParamNames.size());
    }
    std::unordered_map<std::string, Type*> explicitSubst;
    for (size_t i = 0; i < genericParamNames.size(); ++i) {
      explicitSubst[genericParamNames[i]] = explicitTypes[i];
    }
    for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
      Type* expected = substituteInType(fields[i]->type, explicitSubst);
      if (expected != nullptr && !argTypes[i]->isEqual(expected)) {
        throw TypeCheckError(node->getSpan(),
                             "Argument type {} does not match explicit "
                             "parameter type {}",
                             argTypes[i]->toString(), expected->toString());
      }
    }
    if (node->getName() == "new" && !fields.empty() && fields[0]->type->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          substituteInType(fields[0]->type->getElementType(), explicitSubst));
    } else {
      Type* retType = substituteInType(funcType->getReturnType(), explicitSubst);
      result = std::make_unique<Value>(importedScope != nullptr ? materializeImportedType(retType)
                                                                : retType);
    }
    verifyGenericTraitBounds(callee, explicitSubst, node->getSpan());
    return;
  }

  std::unordered_map<std::string, Type*> localGenericTypes;
  for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, argTypes[i], localGenericTypes, node->getSpan());
  }
  // Constructor call: result type is the specialized class type (receiver)
  if (node->getName() == "new" && !funcType->getFields().empty() &&
      funcType->getFields()[0]->type->is(BaseType::TY_PTR)) {
    Type* classType = funcType->getFields()[0]->type->getElementType();
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
    Type* specialized =
        getOrCreateSpecializedClassType(classType, genericParamNames, localGenericTypes);
    result = std::make_unique<Value>(specialized);
  } else {
    Type* retType = substituteInType(funcType->getReturnType(), localGenericTypes);
    if (retType == nullptr) {
      retType = funcType->getReturnType();
    }
    result = std::make_unique<Value>(importedScope != nullptr ? materializeImportedType(retType)
                                                              : retType);
  }
  verifyGenericTraitBounds(callee, localGenericTypes, node->getSpan());
}

auto Typechecker::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  std::unique_ptr<Value> left = std::move(result);
  node->getRight()->accept(*this);
  std::unique_ptr<Value> right = std::move(result);
  Type* resultType = typecheckBinaryOpResult(node->getOperator(), left->getType(), right->getType(),
                                             node->getSpan());
  result = std::make_unique<Value>(resultType);
}

auto Typechecker::visit(const SubscriptOp* node) -> void {
  node->getLeft()->accept(*this);
  Type* baseType = result->getType();
  node->getIndex()->accept(*this);
  Type* indexType = result->getType();
  if (baseType != nullptr && baseType->is(BaseType::TY_ARRAY) &&
      baseType->getElementType() != nullptr) {
    if (indexType == nullptr || !indexType->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getIndex()->getSpan(), "List index must be int, got {}",
                           indexType != nullptr ? indexType->toString() : "unknown");
    }
    result = std::make_unique<Value>(baseType->getElementType());
    return;
  }
  Type* overloadType = resolveMethodReturnType(
      baseType, std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexType}, node->getSpan());
  if (overloadType == nullptr) {
    throw TypeCheckError(node->getSpan(), "Subscript requires an array type or operator [], got {}",
                         baseType != nullptr ? baseType->toString() : "unknown");
  }
  result = std::make_unique<Value>(overloadType);
}

auto Typechecker::visit(const DotOp* node) -> void {
  node->getLeft()->accept(*this);
  Type* base = result->getType();
  auto isStdListClassType = [this](Type* type) -> bool {
    if (type == nullptr) {
      return false;
    }
    if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr) {
      type = type->getElementType();
    }
    if (!type->is(BaseType::TY_CLASS)) {
      return false;
    }
    Type* baseType = type;
    if (auto it = specializedTypeToTemplate.find(type); it != specializedTypeToTemplate.end()) {
      baseType = it->second;
    }
    const std::string& displayName = baseType->getDisplayName();
    return displayName == "list<T>" || displayName == "list";
  };
  if (base == nullptr) {
    throw TypeCheckError(node->getSpan(), "Dot operator on unknown type");
  }
  if (base->is(BaseType::TY_VOID)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
      }
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_IMPORT)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      std::string alias = result->getName();
      auto pathIt = importAliasToPath.find(alias);
      if (pathIt != importAliasToPath.end()) {
        SymbolTable* importScope = getOrTypecheckImport(pathIt->second);
        if (importScope != nullptr) {
          std::vector<Type*> argTypes;
          for (Expression* arg : fc->getArguments()) {
            arg->accept(*this);
            Type* t = result->getType();
            if (t != nullptr && t->is(BaseType::TY_CLASS)) {
              t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
            }
            argTypes.push_back(t);
          }
          Value* func = importScope->lookupFunction(fc->getName(), argTypes);
          if (func == nullptr) {
            Value* sym = importScope->lookup(fc->getName());
            if (sym != nullptr && sym->getType()->is(BaseType::TY_FUNCTION)) {
              func = sym;
            } else if (sym != nullptr && sym->getType()->is(BaseType::TY_CLASS)) {
              fc->setResolvedSymbol(sym);
              Type* classType = materializeImportedType(sym->getType());
              const std::vector<std::string>& genericParamNames =
                  getDeclaredGenericParams(classType);
              if (!fc->getExplicitTypeArgs().empty()) {
                std::vector<Type*> explicitTypes;
                for (TypeExpr* texpr : fc->getExplicitTypeArgs()) {
                  texpr->accept(*this);
                  explicitTypes.push_back(result->getType());
                }
                if (explicitTypes.size() != genericParamNames.size()) {
                  throw TypeCheckError(node->getSpan(),
                                       "Explicit type argument count {} does not match generic "
                                       "class parameter count {}",
                                       explicitTypes.size(), genericParamNames.size());
                }
                std::unordered_map<std::string, Type*> env;
                for (size_t i = 0; i < genericParamNames.size(); ++i) {
                  env[genericParamNames[i]] = explicitTypes[i];
                }
                if (!argTypes.empty()) {
                  Type* ptrToClass =
                      cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
                  std::vector<Type*> constructorParamTypes = {ptrToClass};
                  constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(),
                                               argTypes.end());
                  Value* constructor = importScope->lookupFunction("new", constructorParamTypes);
                  if (constructor == nullptr) {
                    throw TypeCheckError(node->getSpan(),
                                         "Constructor not found for {} with given type arguments",
                                         fc->getName());
                  }
                  auto ctorParams = constructor->getType()->getFields();
                  for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
                    Type* expected = substituteInType(ctorParams[i]->type, env);
                    if (expected != nullptr && !argTypes[i - 1]->isEqual(expected)) {
                      throw TypeCheckError(
                          node->getSpan(),
                          "Argument type {} does not match explicit parameter type {}",
                          argTypes[i - 1]->toString(), expected->toString());
                    }
                  }
                }
                result = std::make_unique<Value>(
                    getOrCreateSpecializedClassType(classType, genericParamNames, env));
                return;
              }
              if (!fc->getArguments().empty()) {
                Type* ptrToClass =
                    cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
                std::vector<Type*> constructorParamTypes = {ptrToClass};
                constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(),
                                             argTypes.end());
                Value* constructor = importScope->lookupFunction("new", constructorParamTypes);
                if (constructor != nullptr) {
                  std::unordered_map<std::string, Type*> env;
                  auto* funcType = constructor->getType();
                  auto ctorParams = funcType->getFields();
                  for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
                    inferGenericBindings(ctorParams[i]->type, argTypes[i - 1], env,
                                         node->getSpan());
                  }
                  result = std::make_unique<Value>(materializeImportedType(
                      getOrCreateSpecializedClassType(classType, genericParamNames, env)));
                  return;
                }
              }
              result = std::make_unique<Value>(materializeImportedType(classType));
              return;
            } else if (sym != nullptr && sym->getType()->is(BaseType::TY_ENUM)) {
              result = std::make_unique<Value>(materializeImportedType(sym->getType()));
              return;
            }
          }
          if (func != nullptr) {
            fc->setResolvedSymbol(func);
            Type* retType = func->getType()->getReturnType();
            result = std::make_unique<Value>(
                retType != nullptr ? materializeImportedType(retType)
                                   : cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
            return;
          }
        }
      }
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
      }
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (base->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      std::vector<Type*> argTypes;
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
        Type* t = result->getType();
        if (t != nullptr && t->is(BaseType::TY_CLASS)) {
          t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
        }
        argTypes.push_back(t);
      }
      Type* retType = resolveMethodReturnType(base, fc->getName(), argTypes, node->getSpan());
      if (retType == nullptr) {
        throw TypeCheckError(node->getSpan(), "Method '{}' not found on trait '{}'", fc->getName(),
                             base->getDisplayName());
      }
      fc->setResolvedSymbol(nullptr);
      result = std::make_unique<Value>(retType);
      return;
    }
    throw TypeCheckError(node->getSpan(), "Expected method call after dot on trait value");
  }
  if (base->is(BaseType::TY_ARRAY)) {
    if (auto* call = dynamic_cast<FuncCall*>(node->getRight())) {
      if (visitListMethodCall(base, node, call)) {
        return;
      }
    }
    throw TypeCheckError(node->getSpan(), "Expected supported list method after dot");
  }
  if (base->is(BaseType::TY_GENERIC)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      const std::string& gname = base->getGenericName();
      auto bit = currentGenericParamTraitBounds.find(gname);
      if (bit != currentGenericParamTraitBounds.end()) {
        for (const std::string& traitName : bit->second) {
          auto trIt = traitMethodReturnTypes.find(traitName);
          if (trIt == traitMethodReturnTypes.end()) {
            continue;
          }
          auto mIt = trIt->second.find(fc->getName());
          if (mIt != trIt->second.end()) {
            fc->setResolvedSymbol(nullptr);
            result = std::make_unique<Value>(mIt->second);
            return;
          }
        }
        throw TypeCheckError(node->getSpan(),
                             "Method '{}' not found on generic parameter '{}' via trait bounds",
                             fc->getName(), gname);
      }
    }
    throw TypeCheckError(node->getSpan(), "Dot operator requires class or enum type, got {}",
                         base->toString());
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    throw TypeCheckError(node->getSpan(), "Dot operator requires class or enum type, got {}",
                         base->toString());
  }
  if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
    if (isStdListClassType(base) && isMutatingListFunction(fc->getName()) &&
        !isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value", fc->getName());
    }
    std::vector<Type*> argTypes;
    for (Expression* arg : fc->getArguments()) {
      arg->accept(*this);
      argTypes.push_back(result->getType());
    }
    Type* receiverForLookup = base;
    auto templateIt = specializedTypeToTemplate.find(base);
    if (templateIt != specializedTypeToTemplate.end()) {
      receiverForLookup = templateIt->second;
    }
    Type* selfType =
        receiverForLookup->is(BaseType::TY_PTR)
            ? receiverForLookup
            : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, receiverForLookup));
    std::vector<Type*> methodArgTypes = {selfType};
    methodArgTypes.insert(methodArgTypes.end(), argTypes.begin(), argTypes.end());
    std::unordered_map<std::string, Type*> methodTypeEnv;
    auto specializedIt = specializedTypeEnv.find(base);
    if (specializedIt != specializedTypeEnv.end()) {
      methodTypeEnv = specializedIt->second;
    }
    Value* method = scope->lookupFunction(fc->getName(), methodArgTypes);
    if (method == nullptr) {
      for (auto& [_, cachedModule] : importedModuleCache) {
        if (cachedModule == nullptr || cachedModule->rootScope == nullptr) {
          continue;
        }
        method = cachedModule->rootScope->lookupFunction(fc->getName(), methodArgTypes);
        if (method != nullptr) {
          break;
        }
      }
    }
    if (method == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found: {}", fc->getName());
    }
    fc->setResolvedSymbol(method);
    auto* methodType = method->getType();
    if (!fc->getExplicitTypeArgs().empty()) {
      std::vector<Type*> explicitTypes;
      explicitTypes.reserve(fc->getExplicitTypeArgs().size());
      for (TypeExpr* texpr : fc->getExplicitTypeArgs()) {
        explicitTypes.push_back(resolveType(texpr));
      }
      const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(methodType);
      if (explicitTypes.size() != genericParamNames.size()) {
        throw TypeCheckError(node->getSpan(),
                             "Explicit type argument count {} does not match "
                             "generic parameter count {}",
                             explicitTypes.size(), genericParamNames.size());
      }
      for (size_t i = 0; i < genericParamNames.size(); ++i) {
        methodTypeEnv[genericParamNames[i]] = explicitTypes[i];
      }
      auto fields = methodType->getFields();
      for (size_t i = 0; i < fields.size() && i < methodArgTypes.size(); ++i) {
        Type* expected = substituteInType(fields[i]->type, methodTypeEnv);
        if (expected != nullptr && !methodArgTypes[i]->isEqual(expected)) {
          throw TypeCheckError(node->getSpan(),
                               "Argument type {} does not match explicit "
                               "parameter type {}",
                               methodArgTypes[i]->toString(), expected->toString());
        }
      }
    }
    Type* retType = methodType->getReturnType();
    if (!methodTypeEnv.empty() && retType != nullptr) {
      retType = substituteInType(retType, methodTypeEnv);
    }
    result = std::make_unique<Value>(retType);
    return;
  }
  if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
    throw TypeCheckError(node->getSpan(), "Expected field name or method call after dot");
  }
  auto* rightLit = dynamic_cast<Literal*>(node->getRight());
  Field* field = TypeUtils::findFieldInFields(base, rightLit->getValue());
  if (field == nullptr || field->type == nullptr) {
    throw TypeCheckError(node->getSpan(), "Unknown field: {}", rightLit->getValue());
  }
  if (field->getDeclarationSymbol() != nullptr) {
    rightLit->setResolvedSymbol(field->getDeclarationSymbol());
  }
  result = std::make_unique<Value>(field->type);
}

auto Typechecker::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* from = result->getType();
  node->getType()->accept(*this);
  Type* to = result->getType();
  if (!isAssignableTo(from, to)) {
    throw TypeCheckError(node->getSpan(), "Cannot cast from {} to {}", from->toString(),
                         to->toString());
  }
  result = std::make_unique<Value>(to);
}

auto Typechecker::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  node->getRight()->accept(*this);
  result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
}

auto Typechecker::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* operand = result->getType();
  auto tryOverload = [this, node, operand]() -> Type* {
    auto operatorName = OperatorUtils::getUnaryOperatorName(node->getOperator());
    if (!operatorName.has_value()) {
      return nullptr;
    }
    return resolveMethodReturnType(operand, std::string{*operatorName}, {}, node->getSpan());
  };
  switch (node->getOperator()) {
  case TokenType::MINUS:
    if (operand != nullptr && operand->is(BaseType::TY_GENERIC)) {
      result = std::make_unique<Value>(operand);
      break;
    }
    if (operand == nullptr || !operand->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        result = std::make_unique<Value>(overloadedType);
        break;
      }
      throw TypeCheckError(node->getSpan(), "Unary minus requires numeric type");
    }
    result = std::make_unique<Value>(operand);
    break;
  case TokenType::BANG:
  case TokenType::NOT:
    if (operand != nullptr && operand->is(BaseType::TY_GENERIC)) {
      result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
      break;
    }
    if (operand == nullptr || !operand->is(BaseType::TY_BOOL)) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        result = std::make_unique<Value>(overloadedType);
        break;
      }
      throw TypeCheckError(node->getSpan(), "Unary not requires Bool type");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::AMPERSAND:
    if (operand == nullptr) {
      throw TypeCheckError(node->getSpan(), "Address-of requires a value");
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, operand)));
    break;
  case TokenType::STAR:
    if (operand == nullptr || !operand->is(BaseType::TY_PTR)) {
      throw TypeCheckError(node->getSpan(), "Dereference requires pointer type");
    }
    result = std::make_unique<Value>(
        operand->getElementType() != nullptr ? operand->getElementType() : [&]() -> Type* {
          auto u = std::make_unique<Type>(BaseType::TY_INT);
          u->setIntWidth(64);
          return cacheType(std::move(u));
        }());
    break;
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported unary operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
  }
}

auto Typechecker::visit(const ListLiteral* node) -> void {
  Type* expectedType = currentExpectedType();
  auto isStdListClassType = [this](Type* type) -> bool {
    if (type == nullptr || !type->is(BaseType::TY_CLASS)) {
      return false;
    }
    Type* baseType = type;
    if (auto it = specializedTypeToTemplate.find(type); it != specializedTypeToTemplate.end()) {
      baseType = it->second;
    }
    const std::string& displayName = baseType->getDisplayName();
    return displayName == "list<T>" || displayName == "list";
  };
  auto getStdListElementType = [this, &isStdListClassType](Type* type) -> Type* {
    if (!isStdListClassType(type)) {
      return nullptr;
    }
    if (auto it = specializedTypeEnv.find(type); it != specializedTypeEnv.end()) {
      auto envIt = it->second.find("T");
      if (envIt != it->second.end()) {
        return envIt->second;
      }
    }
    return nullptr;
  };
  auto getStdListType = [this, node](Type* elementType) -> Type* {
    const auto listPath = std::filesystem::absolute(std::filesystem::path(getStdDir()) / "list.les")
                              .lexically_normal();
    SymbolTable* listScope = getOrTypecheckImport(listPath.string());
    if (listScope == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unable to load stdlib list class");
    }
    Value* listSymbol = listScope->lookupStruct("list");
    Type* listTemplate = listSymbol != nullptr
                             ? materializeImportedType(listSymbol->getType())
                             : materializeImportedType(listScope->lookupType("list"));
    if (listTemplate == nullptr || !listTemplate->is(BaseType::TY_CLASS)) {
      throw TypeCheckError(node->getSpan(), "Stdlib list class not found");
    }
    std::unordered_map<std::string, Type*> env = {{"T", elementType}};
    return getOrCreateSpecializedClassType(listTemplate, listTemplate->getGenericParams(), env);
  };
  Type* expectedElementType = nullptr;
  if (expectedType != nullptr) {
    if (expectedType->is(BaseType::TY_ARRAY)) {
      expectedElementType = expectedType->getElementType();
    } else {
      expectedElementType = getStdListElementType(expectedType);
    }
  }
  std::vector<Expression*> elements = node->getElements();
  if (elements.empty()) {
    if (expectedType == nullptr || expectedElementType == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Empty list literal requires an explicit list<T> context");
    }
    node->setResolvedType(expectedType);
    result = std::make_unique<Value>(expectedType);
    return;
  }

  Type* elementType = expectedElementType;
  for (Expression* element : elements) {
    visitExprWithExpectedType(element, expectedElementType);
    Type* current = result->getType();
    if (current == nullptr) {
      throw TypeCheckError(element->getSpan(), "List element has unknown type");
    }
    if (expectedElementType != nullptr) {
      if (!isAssignableTo(current, expectedElementType)) {
        throw TypeCheckError(element->getSpan(),
                             "List element type {} is not assignable to expected type {}",
                             current->toString(), expectedElementType->toString());
      }
      continue;
    }
    if (elementType == nullptr) {
      elementType = current;
      continue;
    }
    Type* unified = getExtendedType(elementType, current);
    if (unified != nullptr) {
      elementType = unified;
      continue;
    }
    if (!current->isEqual(elementType)) {
      throw TypeCheckError(element->getSpan(),
                           "List elements must have a common type, got {} and {}",
                           elementType->toString(), current->toString());
    }
  }

  if (expectedType != nullptr && !expectedType->is(BaseType::TY_ARRAY) &&
      getStdListElementType(expectedType) == nullptr) {
    throw TypeCheckError(node->getSpan(), "List literal is not compatible with expected type {}",
                         expectedType->toString());
  }

  Type* listType = expectedType;
  if (listType == nullptr) {
    listType = getStdListType(elementType);
  }
  node->setResolvedType(listType);
  result = std::make_unique<Value>(listType);
}

auto Typechecker::visit(const Literal* node) -> void {
  switch (node->getType()) {
  case TokenType::INTEGER: {
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(64);
    result = std::make_unique<Value>(cacheType(std::move(u)));
    break;
  }
  case TokenType::DOUBLE:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_FLOAT)));
    break;
  case TokenType::STRING:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    break;
  case TokenType::BOOL:
  case TokenType::TRUE_:
  case TokenType::FALSE_:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::NIL:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, nullptr)));
    break;
  case TokenType::IDENTIFIER: {
    Value* sym = scope->lookup(node->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unknown name: {}", node->getValue());
    }
    node->setResolvedSymbol(sym);
    result = std::make_unique<Value>(*sym);
    break;
  }
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported literal type: {}",
                         NAMEOF_ENUM(node->getType()));
  }
}

auto Typechecker::visit(const TypeExpr* node) -> void {
  Type* type = resolveType(node);
  result = std::make_unique<Value>(type);
}

auto Typechecker::visit(const TraitDecl* node) -> void {
  if (declarationPass) {
    if (traitRegistry.contains(node->getIdentifier())) {
      throw TypeCheckError(node->getNameSpan(), "Duplicate trait '{}'", node->getIdentifier());
    }
    traitRegistry[node->getIdentifier()] = node;

    {
      auto traitType = std::make_unique<Type>(BaseType::TY_TRAIT_EXISTENTIAL, nullptr);
      traitType->setDisplayName(node->getIdentifier());
      scope->insertType(node->getIdentifier(), std::move(traitType));
    }
    Type* traitTypePtr = scope->lookupType(node->getIdentifier());
    auto traitSym = std::make_unique<Value>(node->getIdentifier(), traitTypePtr);
    traitSym->setCategory(ValueCategory::TYPE_SYMBOL);
    traitSym->setDeclarationKind(ValueDeclarationKind::TRAIT);
    traitSym->setDeclarationSpan(node->getNameSpan());
    traitSym->setDeclarationFilePath(mainFilePath);
    scope->insertSymbol(std::move(traitSym));
    return;
  }

  // Second pass: resolve requirement signatures (may reference classes declared later in the file).
  auto savedTraitGenerics = currentGenericTypes;
  for (const auto& p : node->getGenericParamDecls()) {
    currentGenericTypes[p.name] = cacheType(std::make_unique<Type>(p.name));
  }

  traitMethodReturnTypes[node->getIdentifier()].clear();
  for (FuncDecl* req : node->getRequirements()) {
    req->getReturnType()->accept(*this);
    traitMethodReturnTypes[node->getIdentifier()][req->getName()] = result->getType();
  }
  currentGenericTypes = std::move(savedTraitGenerics);
}

auto Typechecker::buildMethodFunctionType(FuncDecl* decl, Type* classType) -> Type* {
  Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
  std::vector<std::unique_ptr<Field>> paramFields;
  paramFields.push_back(std::make_unique<Field>("self", selfPtr));
  for (Parameter* param : decl->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(decl->getSpan(), "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    if (paramType->is(BaseType::TY_CLASS)) {
      paramType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramType));
    }
    paramFields.push_back(std::make_unique<Field>(param->name, paramType));
  }
  decl->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  return cacheType(std::move(funcType));
}

auto Typechecker::registerTraitDefaultMethodSymbol(SymbolTable* insertScope, Type* classType,
                                                   FuncDecl* req) -> void {
  Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
  std::vector<Type*> lookupArgs = {selfPtr};
  for (Parameter* p : req->getParameters()) {
    if (p->type != nullptr) {
      p->type->accept(*this);
      Type* pt = result->getType();
      if (pt->is(BaseType::TY_CLASS)) {
        pt = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, pt));
      }
      lookupArgs.push_back(pt);
    }
  }
  if (insertScope->lookupFunction(req->getName(), lookupArgs) != nullptr) {
    return;
  }
  Type* funcTypePtr = buildMethodFunctionType(req, classType);
  auto declaredFunc = std::make_unique<Value>(req->getName(), funcTypePtr);
  declaredFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
  declaredFunc->setDeclarationKind(ValueDeclarationKind::METHOD);
  declaredFunc->setExported(false);
  declaredFunc->setDeclarationSpan(req->getNameSpan());
  declaredFunc->setDeclarationFilePath(mainFilePath);
  insertScope->insertSymbol(std::move(declaredFunc));
  Value* funcSymbol = insertScope->lookupFunction(req->getName(), lookupArgs);
  if (funcSymbol == nullptr) {
    return;
  }
  req->setResolvedSymbol(funcSymbol);
  SymbolTable* child = insertScope->createChildBlock("trait_default");
  funcSymbol->setBodyScope(child);
  SymbolTable* savedScope = scope;
  scope = child;
  auto selfSymbol = std::make_unique<Value>("self", selfPtr);
  selfSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  scope->insertSymbol(std::move(selfSymbol));
  const size_t paramOffset = 1U;
  for (size_t i = 0; i < req->getParameters().size(); ++i) {
    Parameter* param = req->getParameters()[i];
    auto paramSymbol = std::make_unique<Value>(param->name, lookupArgs[paramOffset + i]);
    paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    paramSymbol->setDeclarationKind(ValueDeclarationKind::PARAMETER);
    paramSymbol->setDeclarationSpan(param->nameSpan);
    paramSymbol->setDeclarationFilePath(mainFilePath);
    param->setResolvedSymbol(paramSymbol.get());
    scope->insertSymbol(std::move(paramSymbol));
  }
  scope = savedScope;
}

auto Typechecker::mergeTraitImplTypeArgsIntoCurrentGenericEnv(const Class* classNode,
                                                              size_t traitClauseIndex,
                                                              const TraitDecl* trait) -> void {
  const auto& argLists = classNode->getImplTraitTypeArgs();
  if (traitClauseIndex >= argLists.size()) {
    throw TypeCheckError(classNode->getNameSpan(), "Internal: impl trait clause index");
  }
  const auto& typeArgExprs = argLists[traitClauseIndex];
  llvm::SMRange errSpan = classNode->getImplTraitSpans()[traitClauseIndex];
  const auto& traitGenParams = trait->getGenericParamDecls();
  if (traitGenParams.size() != typeArgExprs.size()) {
    if (!traitGenParams.empty() && typeArgExprs.empty()) {
      throw TypeCheckError(errSpan, "Trait {} requires {} type argument(s), e.g. `impl {}<...>`",
                           trait->getIdentifier(), traitGenParams.size(), trait->getIdentifier());
    }
    if (traitGenParams.empty() && !typeArgExprs.empty()) {
      throw TypeCheckError(errSpan, "Trait {} is not generic; remove type arguments in `impl`",
                           trait->getIdentifier());
    }
    throw TypeCheckError(errSpan, "Trait {} requires {} type argument(s), got {}",
                         trait->getIdentifier(), traitGenParams.size(), typeArgExprs.size());
  }
  std::unordered_map<std::string, Type*> merged = currentGenericTypes;
  for (size_t j = 0; j < traitGenParams.size(); ++j) {
    typeArgExprs[j]->accept(*this);
    merged[traitGenParams[j].name] = result->getType();
  }
  currentGenericTypes = std::move(merged);
}

auto Typechecker::typecheckTraitDefaultBodies(const Class* classNode, Type* classType,
                                              SymbolTable* methodInsertScope) -> void {
  std::unordered_set<std::string> explicitNames;
  for (FuncDecl* f : classNode->getMethods()) {
    explicitNames.insert(f->getName());
  }
  SymbolTable* savedListScope = scope;
  Type* savedClass = currentClassType;
  currentClassType = classType;
  const auto& implNames = classNode->getImplTraitNames();
  if (implNames.size() != classNode->getImplTraitTypeArgs().size()) {
    throw TypeCheckError(classNode->getNameSpan(), "Internal: impl trait metadata mismatch");
  }
  for (size_t ti = 0; ti < implNames.size(); ++ti) {
    const std::string& traitName = implNames[ti];
    auto trIt = traitRegistry.find(traitName);
    if (trIt == traitRegistry.end()) {
      continue;
    }
    const TraitDecl* trait = trIt->second;
    auto savedTraitGenerics = currentGenericTypes;
    mergeTraitImplTypeArgsIntoCurrentGenericEnv(classNode, ti, trait);
    for (FuncDecl* req : trait->getRequirements()) {
      if (req->getBody() == nullptr) {
        continue;
      }
      if (explicitNames.contains(req->getName())) {
        continue;
      }
      Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
      std::vector<Type*> lookupArgs = {selfPtr};
      for (Parameter* p : req->getParameters()) {
        if (p->type != nullptr) {
          p->type->accept(*this);
          Type* pt = result->getType();
          if (pt->is(BaseType::TY_CLASS)) {
            pt = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, pt));
          }
          lookupArgs.push_back(pt);
        }
      }
      Value* funcSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs);
      if (funcSym == nullptr || funcSym->getBodyScope() == nullptr) {
        continue;
      }
      currentFunction = funcSym;
      scope = funcSym->getBodyScope();
      inTopLevel = false;
      auto savedTraitBounds = currentGenericParamTraitBounds;
      currentGenericParamTraitBounds.clear();
      req->getBody()->accept(*this);
      currentGenericParamTraitBounds = std::move(savedTraitBounds);
      Type* funcReturnType = funcSym->getType()->getReturnType();
      if (funcReturnType != nullptr && !funcReturnType->is(BaseType::TY_VOID) &&
          !blockAlwaysReturns(req->getBody())) {
        throw TypeCheckError(req->getSpan(),
                             "Non-void trait default may reach end without returning");
      }
      scope = savedListScope;
      currentFunction = nullptr;
      inTopLevel = true;
    }
    currentGenericTypes = std::move(savedTraitGenerics);
  }
  currentClassType = savedClass;
}

auto Typechecker::checkTraitImplementation(const Class* classNode, Type* classType,
                                           SymbolTable* methodInsertScope) -> void {
  const auto& implNames = classNode->getImplTraitNames();
  if (implNames.size() != classNode->getImplTraitTypeArgs().size()) {
    throw TypeCheckError(classNode->getNameSpan(), "Internal: impl trait metadata mismatch");
  }
  for (size_t ti = 0; ti < implNames.size(); ++ti) {
    const std::string& traitName = implNames[ti];
    auto trIt = traitRegistry.find(traitName);
    if (trIt == traitRegistry.end()) {
      throw TypeCheckError(classNode->getNameSpan(), "Unknown trait '{}'", traitName);
    }
    const TraitDecl* trait = trIt->second;
    auto savedTraitGenerics = currentGenericTypes;
    mergeTraitImplTypeArgsIntoCurrentGenericEnv(classNode, ti, trait);
    for (FuncDecl* req : trait->getRequirements()) {
      Type* expected = buildMethodFunctionType(req, classType);
      Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
      std::vector<Type*> lookupArgs = {selfPtr};
      for (Parameter* p : req->getParameters()) {
        if (p->type != nullptr) {
          p->type->accept(*this);
          Type* pt = result->getType();
          if (pt->is(BaseType::TY_CLASS)) {
            pt = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, pt));
          }
          lookupArgs.push_back(pt);
        }
      }
      Value* methodSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs);
      if (methodSym == nullptr && req->getBody() != nullptr) {
        registerTraitDefaultMethodSymbol(methodInsertScope, classType, req);
        methodSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs);
      }
      if (methodSym == nullptr) {
        throw TypeCheckError(classNode->getNameSpan(),
                             "Class {} does not implement trait method '{}' required by {}",
                             classNode->getIdentifier(), req->getName(), traitName);
      }
      if (!methodSym->getType()->isEqual(expected)) {
        throw TypeCheckError(
            req->getNameSpan(),
            "Method '{}' has incompatible type for trait {}: expected {}, found {}", req->getName(),
            traitName, expected->toString(), methodSym->getType()->toString());
      }
    }
    currentGenericTypes = std::move(savedTraitGenerics);
  }
}

auto Typechecker::verifyGenericTraitBounds(Value* callee,
                                           const std::unordered_map<std::string, Type*>& subs,
                                           llvm::SMRange span) -> void {
  if (callee == nullptr || callee->getType() == nullptr ||
      !callee->getType()->is(BaseType::TY_FUNCTION)) {
    return;
  }
  Type* ft = callee->getType();
  const auto& names = ft->getGenericParams();
  const auto& boundsList = ft->getGenericParamTraitBounds();
  if (boundsList.empty()) {
    return;
  }
  for (size_t i = 0; i < names.size() && i < boundsList.size(); ++i) {
    for (const std::string& traitName : boundsList[i]) {
      auto it = subs.find(names[i]);
      if (it == subs.end() || it->second == nullptr) {
        continue;
      }
      Type* concrete = it->second;
      if (concrete->is(BaseType::TY_PTR) && concrete->getElementType() != nullptr &&
          concrete->getElementType()->is(BaseType::TY_CLASS)) {
        concrete = concrete->getElementType();
      }
      if (!concrete->is(BaseType::TY_CLASS)) {
        throw TypeCheckError(span, "Generic parameter {} must be a class type to satisfy trait {}",
                             names[i], traitName);
      }
      if (!classDeclaresTrait(concrete, traitName)) {
        throw TypeCheckError(span, "Type {} does not declare impl {}", concrete->toString(),
                             traitName);
      }
    }
  }
}

auto Typechecker::classDeclaresTrait(Type* classTy, const std::string& traitName) -> bool {
  if (classTy == nullptr) {
    return false;
  }
  if (auto sp = specializedTypeToTemplate.find(classTy); sp != specializedTypeToTemplate.end()) {
    classTy = sp->second;
  }
  for (const auto& n : classTy->getImplTraitNames()) {
    if (n == traitName) {
      return true;
    }
  }
  return false;
}

} // namespace lesma
