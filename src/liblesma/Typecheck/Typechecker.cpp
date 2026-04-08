#include "Typechecker.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
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
#include "liblesma/Driver/AnalysisDiagnostic.h"
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

[[nodiscard]] auto isControlFlowTerminator(const Statement* stmt) -> bool {
  return stmt != nullptr && (dynamic_cast<const Return*>(stmt) != nullptr ||
                             dynamic_cast<const Break*>(stmt) != nullptr ||
                             dynamic_cast<const Continue*>(stmt) != nullptr);
}

/** Compares override soundness for a vtable slot: same trailing parameters and return type as the
 *  inherited callable; receiver (field 0) may differ structurally (subclass `self`). */
[[nodiscard]] auto virtualOverrideSignaturesMatch(Type* derivedFn, Type* baseFn) -> bool {
  if (derivedFn == nullptr || baseFn == nullptr) {
    return false;
  }
  if (!derivedFn->is(BaseType::TY_FUNCTION) || !baseFn->is(BaseType::TY_FUNCTION)) {
    return false;
  }
  if (!derivedFn->functionGenericSignatureEqual(baseFn)) {
    return false;
  }
  auto df = derivedFn->getFields();
  auto bf = baseFn->getFields();
  if (df.size() != bf.size()) {
    return false;
  }
  for (size_t i = 1; i < df.size(); ++i) {
    Type* dt = df[i]->type;
    Type* bt = bf[i]->type;
    if (dt == nullptr || bt == nullptr) {
      if (dt != bt) {
        return false;
      }
      continue;
    }
    if (!dt->isEqual(bt)) {
      return false;
    }
  }
  Type* dr = derivedFn->getReturnType();
  Type* br = baseFn->getReturnType();
  if (dr == nullptr && br == nullptr) {
    return true;
  }
  if (dr == nullptr || br == nullptr) {
    return false;
  }
  return dr->isEqual(br);
}

[[nodiscard]] auto typecheckingImportPathsInProgress() -> std::unordered_set<std::string>& {
  thread_local std::unordered_set<std::string> paths;
  return paths;
}

struct TypecheckImportActiveGuard {
  std::string key;

  explicit TypecheckImportActiveGuard(std::string normalizedPath) : key(std::move(normalizedPath)) {
    if (!typecheckingImportPathsInProgress().insert(key).second) {
      throw TypeCheckError(llvm::SMRange(), "Circular import detected: {}", key);
    }
  }

  ~TypecheckImportActiveGuard() { typecheckingImportPathsInProgress().erase(key); }

  TypecheckImportActiveGuard(TypecheckImportActiveGuard const&) = delete;
  auto operator=(TypecheckImportActiveGuard const&) -> TypecheckImportActiveGuard& = delete;
  TypecheckImportActiveGuard(TypecheckImportActiveGuard&&) = delete;
  auto operator=(TypecheckImportActiveGuard&&) -> TypecheckImportActiveGuard& = delete;
};

} // namespace

auto Typechecker::traitExistentialBaseName(const std::string& displayName) -> std::string {
  if (const auto pos = displayName.find('<'); pos != std::string::npos) {
    return displayName.substr(0U, pos);
  }
  return displayName;
}

auto Typechecker::methodLookupSignatureKey(const std::string& name,
                                           const std::vector<Type*>& lookupArgs) -> std::string {
  std::string key;
  key.reserve(name.size() + (lookupArgs.size() * 24U));
  key.append(name);
  key.push_back('\0');
  for (Type* t : lookupArgs) {
    key.append(t != nullptr ? t->toString() : std::string("null"));
    key.push_back('\0');
  }
  return key;
}

auto Typechecker::vtableMethodKey(const Value* methodSymbol) -> std::string {
  if (methodSymbol == nullptr) {
    return {};
  }
  std::string key = methodSymbol->getName();
  Type* methodType = methodSymbol->getType();
  if (methodType == nullptr || !methodType->is(BaseType::TY_FUNCTION)) {
    return key;
  }
  auto fields = methodType->getFields();
  size_t start = fields.empty() ? 0U : 1U;
  for (size_t i = start; i < fields.size(); ++i) {
    Field* field = fields[i];
    key += "|" + (field != nullptr && field->type != nullptr ? field->type->toString() : "?");
  }
  return key;
}

auto Typechecker::findInheritedVirtualMethodSymbol(SymbolTable* moduleScope, Type* subclassTy,
                                                   std::string const& name, Value* derivedSym)
    -> Value* {
  if (moduleScope == nullptr || subclassTy == nullptr || derivedSym == nullptr) {
    return nullptr;
  }
  Type* derivedFnTy = derivedSym->getType();
  if (derivedFnTy == nullptr || !derivedFnTy->is(BaseType::TY_FUNCTION)) {
    return nullptr;
  }
  std::vector<Field*> const fields = derivedFnTy->getFields();
  if (fields.empty()) {
    return nullptr;
  }
  std::vector<Type*> tailParams;
  tailParams.reserve(fields.size() > 1U ? fields.size() - 1U : 0U);
  for (size_t i = 1; i < fields.size(); ++i) {
    tailParams.push_back(fields[i]->type);
  }
  for (Type* walk = subclassTy->getClassSuperclass(); walk != nullptr;
       walk = walk->getClassSuperclass()) {
    Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, walk));
    std::vector<Type*> lookupTypes;
    lookupTypes.reserve(1U + tailParams.size());
    lookupTypes.push_back(selfPtr);
    lookupTypes.insert(lookupTypes.end(), tailParams.begin(), tailParams.end());
    if (Value* hit =
            moduleScope->lookupFunction(name, lookupTypes, FunctionLookupKind::OVERLOAD_IDENTITY)) {
      return hit;
    }
  }
  return nullptr;
}

auto Typechecker::classLexicalScopeMatchesForPrivate(Type* contextClass, Type* declaredIn) -> bool {
  if (contextClass == nullptr || declaredIn == nullptr) {
    return false;
  }
  if (contextClass->isEqual(declaredIn)) {
    return true;
  }
  Type* ctxTpl = contextClass;
  if (auto it = specializedTypeToTemplate.find(contextClass);
      it != specializedTypeToTemplate.end()) {
    ctxTpl = it->second;
  }
  Type* declTpl = declaredIn;
  if (auto it = specializedTypeToTemplate.find(declaredIn); it != specializedTypeToTemplate.end()) {
    declTpl = it->second;
  }
  return ctxTpl->isEqual(declTpl);
}

auto Typechecker::enforcePrivateMemberReadable(llvm::SMRange span, Value* member) -> void {
  if (member == nullptr || !member->isPrivateMember()) {
    return;
  }
  Type* declaredIn = member->getMemberDeclaredInClass();
  if (declaredIn == nullptr) {
    return;
  }
  if (!classLexicalScopeMatchesForPrivate(currentClassType, declaredIn)) {
    throw TypeCheckError(span, "Cannot access private member '{}' from this context",
                         member->getName());
  }
}

void Typechecker::mergeImportedAnalysisTypeCachesInto(
    std::vector<std::unique_ptr<Type>>& dest, const std::shared_ptr<ImportedModuleAnalysis>& mod) {
  if (mod == nullptr) {
    return;
  }
  for (auto& t : mod->typeCache) {
    dest.push_back(std::move(t));
  }
  mod->typeCache.clear();
  if (mod->rootScope != nullptr) {
    mod->rootScope->releaseOwnedTypesInto(dest);
  }
  for (auto& [path, nested] : mod->importedModules) {
    (void) path;
    mergeImportedAnalysisTypeCachesInto(dest, nested);
  }
}

auto Typechecker::isTypeSymbolForCustomTypeName(Value const* sym) -> bool {
  if (sym == nullptr) {
    return false;
  }
  return sym->getCategory() == ValueCategory::TYPE_SYMBOL;
}

auto Typechecker::pathLeadsToEndWithoutReturn(const std::vector<Statement*>& statements,
                                              size_t index) -> bool {
  if (index >= statements.size()) {
    return true; // fell off the end
  }
  std::function<bool(const Expression*)> expressionFallsThrough = [&](const Expression* expr) {
    if (expr == nullptr) {
      return true;
    }
    if (expressionCallsStdlibBaseLesExit(const_cast<Expression*>(expr))) {
      return false;
    }
    if (auto const* blockExpr = dynamic_cast<const BlockExpr*>(expr)) {
      bool bodyFallsThrough = blockExpr->getBody() == nullptr ||
                              pathLeadsToEndWithoutReturn(blockExpr->getBody()->getChildren(), 0);
      if (!bodyFallsThrough) {
        return false;
      }
      return expressionFallsThrough(blockExpr->getTailExpr());
    }
    if (auto const* matchExpr = dynamic_cast<const MatchExpr*>(expr)) {
      for (const MatchArm& arm : matchExpr->getArms()) {
        if (expressionFallsThrough(arm.body.get())) {
          return true;
        }
      }
      return false;
    }
    return true;
  };
  Statement* s = statements[index];
  if (dynamic_cast<Return*>(s) != nullptr) {
    return false; // this path returns
  }
  if (auto* exprStmt = dynamic_cast<ExpressionStatement*>(s)) {
    if (!expressionFallsThrough(exprStmt->getExpression())) {
      return false; // noreturn (stdlib extern exit in base.les)
    }
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
  if (!declarationPass && warningDiagnostics != nullptr && expected != nullptr &&
      result != nullptr) {
    Type* got = result->getType();
    if (got != nullptr && isAssignableTo(got, expected) &&
        isLossyImplicitConversion(got, expected)) {
      emitWarning(node->getSpan(),
                  fmt::format("Implicit conversion from {} to {} may lose precision",
                              got->toString(), expected->toString()));
    }
  }
}

auto Typechecker::currentExpectedType() const -> Type* {
  if (expectedTypes.empty()) {
    return nullptr;
  }
  return expectedTypes.back();
}

auto Typechecker::currentFunctionRootScope() const -> SymbolTable* {
  if (functionScopeStack.empty()) {
    return nullptr;
  }
  return functionScopeStack.back();
}

auto Typechecker::getOrCreateLambdaCaptureShadow(Value* outerSym, const std::string& name,
                                                 llvm::SMRange span) -> Value* {
  if (currentFunction == nullptr || currentFunction->getBodyScope() == nullptr) {
    throw TypeCheckError(span, "Internal error: lambda capture outside lambda");
  }
  SymbolTable* body = currentFunction->getBodyScope();
  if (Value* shallow = body->lookupShallow(name); shallow != nullptr) {
    if (shallow->getClosureSlotOuter() != nullptr) {
      return shallow;
    }
    throw TypeCheckError(span, "Lambda capture '{}' conflicts with a parameter or local", name);
  }
  currentFunction->pushClosureCaptureOuterIfNew(outerSym);
  auto shadow = std::make_unique<Value>(name, outerSym->getType());
  shadow->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  shadow->setDeclarationKind(ValueDeclarationKind::VARIABLE);
  shadow->setClosureSlotOuter(outerSym);
  shadow->setDeclarationSpan(span);
  shadow->setDeclarationFilePath(mainFilePath);
  Value* raw = shadow.get();
  body->insertSymbol(std::move(shadow));
  return raw;
}

auto Typechecker::resolveMethodWithTraitEnv(Type* baseType, const std::string& methodName,
                                            const std::vector<Type*>& argTypes, llvm::SMRange span)
    -> ResolvedMethodCallInfo {
  if (baseType == nullptr) {
    return {};
  }
  Type* base = baseType;
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (base->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    const std::string traitKey = traitExistentialBaseName(base->getDisplayName());
    auto trIt = traitMethodSignatures.find(traitKey);
    if (trIt == traitMethodSignatures.end()) {
      return {};
    }
    auto methIt = trIt->second.find(methodName);
    if (methIt == trIt->second.end() || methIt->second.empty()) {
      return {};
    }
    Type* selfType = base->is(BaseType::TY_PTR)
                         ? base
                         : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, base));
    std::vector<Type*> methodArgTypes = {selfType};
    for (Type* argType : argTypes) {
      methodArgTypes.push_back(typeAsPtrIfClassForOverload(argType));
    }
    Type* matched = selectBestFunctionTypeMatch(methIt->second, methodArgTypes);
    if (matched == nullptr) {
      return {};
    }
    Type* ret = matched->getReturnType();
    if (auto envIt = specializedTraitExistentialEnv.find(base);
        envIt != specializedTraitExistentialEnv.end() && ret != nullptr) {
      ret = substituteInType(ret, envIt->second);
    }
    return {ret, nullptr, {}};
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    return {};
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
    methodArgTypes.push_back(typeAsPtrIfClassForOverload(argType));
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
    return {};
  }

  enforcePrivateMemberReadable(span, method);

  auto* methodType = method->getType();
  auto fields = methodType->getFields();
  std::unordered_map<std::string, Type*> inferredFromArgs;
  for (size_t i = 0; i < fields.size() && i < methodArgTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, methodArgTypes[i], inferredFromArgs, span);
  }
  mergeInferredGenericBindings(methodTypeEnv, inferredFromArgs, span);
  Type* retType = methodType->getReturnType();
  if (!methodTypeEnv.empty() && retType != nullptr) {
    retType = substituteInType(retType, methodTypeEnv);
  }

  std::unordered_map<std::string, Type*> traitBoundSubs = methodTypeEnv;

  if (method->getDeclarationKind() == ValueDeclarationKind::METHOD ||
      method->getDeclarationKind() == ValueDeclarationKind::FUNCTION) {
    method->setUsed(true);
  }
  if (importedMethod && retType != nullptr) {
    retType = materializeImportedType(retType);
  }
  return {retType, method, std::move(traitBoundSubs)};
}

auto Typechecker::resolveMethodReturnType(Type* baseType, const std::string& methodName,
                                          const std::vector<Type*>& argTypes, llvm::SMRange span)
    -> Type* {
  return resolveMethodWithTraitEnv(baseType, methodName, argTypes, span).returnType;
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
  return functionName == "__buffer_new" || functionName == "__buffer_len" ||
         functionName == "__buffer_push" || functionName == "__buffer_pop" ||
         functionName == "__buffer_clear" || functionName == "__buffer_copy" ||
         functionName == "__buffer_get" || functionName == "__buffer_set" ||
         functionName == "__cstr_byte_at" || functionName == "__cstr_byte_set" ||
         functionName == "__str_concat" || functionName == "__str_slice" ||
         functionName == "__cstr_index_of" || functionName == "__cstr_offset";
}

auto Typechecker::visitListIntrinsicCall(const FuncCall* node, const std::vector<Type*>& argTypes)
    -> bool {
  if (!isListIntrinsicName(node->getName())) {
    return false;
  }
  if (!isStdlibSourcePath(mainFilePath)) {
    throw TypeCheckError(node->getSpan(),
                         "Compiler intrinsic `{}` is only available in the standard library",
                         node->getName());
  }
  if (node->getName() == "__cstr_byte_at") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_at expects exactly two arguments");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING)) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_at first argument must be cstr");
    }
    if (argTypes[1] == nullptr || !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_at second argument must be int");
    }
    auto intRet = std::make_unique<Type>(BaseType::TY_INT);
    intRet->setIntWidth(8);
    intRet->setSigned(false);
    result = std::make_unique<Value>(cacheType(std::move(intRet)));
    return true;
  }
  if (node->getName() == "__cstr_byte_set") {
    if (argTypes.size() != 3U) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_set expects cstr, index, and value");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING)) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_set first argument must be cstr");
    }
    if (argTypes[1] == nullptr || !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_set index must be int");
    }
    if (argTypes[2] == nullptr || !argTypes[2]->is(BaseType::TY_INT) ||
        argTypes[2]->getIntWidth() != 8) {
      throw TypeCheckError(node->getSpan(), "__cstr_byte_set value must be int8 or uint8");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (node->getName() == "__str_concat") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "__str_concat expects two cstr arguments");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING) || argTypes[1] == nullptr ||
        !argTypes[1]->is(BaseType::TY_STRING)) {
      throw TypeCheckError(node->getSpan(), "__str_concat requires cstr arguments");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    return true;
  }
  if (node->getName() == "__str_slice") {
    if (argTypes.size() != 3U) {
      throw TypeCheckError(node->getSpan(), "__str_slice expects cstr, start, and length");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING) || argTypes[1] == nullptr ||
        !argTypes[1]->is(BaseType::TY_INT) || argTypes[2] == nullptr ||
        !argTypes[2]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "__str_slice requires (cstr, int, int)");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    return true;
  }
  if (node->getName() == "__cstr_index_of") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "__cstr_index_of expects haystack and needle cstr");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING) || argTypes[1] == nullptr ||
        !argTypes[1]->is(BaseType::TY_STRING)) {
      throw TypeCheckError(node->getSpan(), "__cstr_index_of requires cstr arguments");
    }
    auto intRet = std::make_unique<Type>(BaseType::TY_INT);
    intRet->setIntWidth(64);
    result = std::make_unique<Value>(cacheType(std::move(intRet)));
    return true;
  }
  if (node->getName() == "__cstr_offset") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "__cstr_offset expects cstr and byte offset");
    }
    if (argTypes[0] == nullptr || !argTypes[0]->is(BaseType::TY_STRING) || argTypes[1] == nullptr ||
        !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "__cstr_offset requires (cstr, int)");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    return true;
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
  if (node->getName() == "__buffer_len") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    auto intLen = std::make_unique<Type>(BaseType::TY_INT);
    intLen->setIntWidth(64);
    result = std::make_unique<Value>(cacheType(std::move(intLen)));
    return true;
  }
  if (node->getName() == "__buffer_copy") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(listType);
    return true;
  }
  if (node->getName() == "__buffer_clear") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (node->getName() == "__buffer_pop") {
    if (argTypes.size() != 1U) {
      throw TypeCheckError(node->getSpan(), "{} expects exactly one argument", node->getName());
    }
    result = std::make_unique<Value>(listType->getElementType());
    return true;
  }
  if (node->getName() == "__buffer_push") {
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
  if (node->getName() == "__buffer_get") {
    if (argTypes.size() != 2U) {
      throw TypeCheckError(node->getSpan(), "{} expects buffer and index", node->getName());
    }
    if (argTypes[1] == nullptr || !argTypes[1]->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getSpan(), "{} index must be int", node->getName());
    }
    result = std::make_unique<Value>(listType->getElementType());
    return true;
  }
  if (node->getName() == "__buffer_set") {
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
  // Raw __buffer<T> (e.g. dict.keys) is TY_ARRAY; stdlib exposes length via list.len(self), not on
  // the buffer type. Match codegen's callListMethodByName buffer path.
  if (call->getName() == "len" && call->getArguments().empty()) {
    auto intLen = std::make_unique<Type>(BaseType::TY_INT);
    intLen->setIntWidth(64);
    result = std::make_unique<Value>(cacheType(std::move(intLen)));
    return true;
  }
  Type* const elemType = listType->getElementType();
  if (call->getName() == "copy" && call->getArguments().empty()) {
    result = std::make_unique<Value>(listType);
    return true;
  }
  if (call->getName() == "clear" && call->getArguments().empty()) {
    if (!isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value",
                           call->getName());
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (call->getName() == "pop") {
    if (!call->getArguments().empty()) {
      throw TypeCheckError(call->getSpan(), "pop expects no arguments");
    }
    if (!isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value",
                           call->getName());
    }
    if (elemType == nullptr) {
      return false;
    }
    if (isNullableType(elemType)) {
      throw TypeCheckError(call->getSpan(),
                           "pop() does not support nullable list element type {} because nil "
                           "marks an empty list",
                           elemType->toString());
    }
    std::vector<Type*> members = {elemType, cacheType(std::make_unique<Type>(BaseType::TY_NULL))};
    auto [canonical, displayName] = TypeUtils::canonicalizeUnionMembers(std::move(members));
    if (canonical.size() == 1U) {
      result = std::make_unique<Value>(canonical.front());
    } else {
      auto out = std::make_unique<Type>(BaseType::TY_UNION);
      out->setDisplayName(displayName);
      out->setUnionMembers(std::move(canonical));
      result = std::make_unique<Value>(cacheType(std::move(out)));
    }
    return true;
  }
  if (call->getName() == "push") {
    if (call->getArguments().size() != 1U) {
      throw TypeCheckError(call->getSpan(), "push expects one argument");
    }
    if (!isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value",
                           call->getName());
    }
    call->getArguments()[0]->accept(*this);
    Type* argType = result->getType();
    if (argType != nullptr && argType->is(BaseType::TY_CLASS)) {
      argType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, argType));
    }
    if (elemType == nullptr || argType == nullptr || !isAssignableTo(argType, elemType)) {
      throw TypeCheckError(call->getSpan(),
                           "Cannot push value of type {} into buffer element type {}",
                           argType != nullptr ? argType->toString() : "(unknown)",
                           elemType != nullptr ? elemType->toString() : "(unknown)");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
  }
  if (call->getName() == std::string{OperatorUtils::SUBSCRIPT_GET_NAME}) {
    if (call->getArguments().size() != 1U) {
      throw TypeCheckError(call->getSpan(), "operator [] expects one argument");
    }
    call->getArguments()[0]->accept(*this);
    Type* idxType = result->getType();
    if (idxType == nullptr || !idxType->is(BaseType::TY_INT)) {
      throw TypeCheckError(call->getSpan(), "operator [] index must be int");
    }
    if (elemType == nullptr) {
      return false;
    }
    result = std::make_unique<Value>(elemType);
    return true;
  }
  if (call->getName() == std::string{OperatorUtils::SUBSCRIPT_SET_NAME}) {
    if (call->getArguments().size() != 2U) {
      throw TypeCheckError(call->getSpan(), "operator []= expects two arguments");
    }
    if (!isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value",
                           call->getName());
    }
    call->getArguments()[0]->accept(*this);
    Type* idxType = result->getType();
    if (idxType == nullptr || !idxType->is(BaseType::TY_INT)) {
      throw TypeCheckError(call->getSpan(), "operator []= index must be int");
    }
    call->getArguments()[1]->accept(*this);
    Type* valType = result->getType();
    if (valType != nullptr && valType->is(BaseType::TY_CLASS)) {
      valType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, valType));
    }
    if (elemType == nullptr || valType == nullptr || !isAssignableTo(valType, elemType)) {
      throw TypeCheckError(call->getSpan(), "Cannot assign type {} into buffer element type {}",
                           valType != nullptr ? valType->toString() : "(unknown)",
                           elemType != nullptr ? elemType->toString() : "(unknown)");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return true;
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
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  SymbolTable* importedScope = getOrTypecheckImport(listModulePath.string());
  Value* callee =
      importedScope != nullptr ? importedScope->lookupFunction(call->getName(), argTypes) : nullptr;
  if (callee == nullptr || !callee->getType()->is(BaseType::TY_FUNCTION)) {
    return false;
  }

  call->setResolvedSymbol(callee);
  markValueRead(callee);
  auto* funcType = callee->getType();
  auto fields = funcType->getFields();
  if (!call->getExplicitTypeArgs().empty()) {
    std::vector<Type*> explicitTypes;
    collectExplicitTypesFromCallByVisit(call, explicitTypes);
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
      if (expected != nullptr && (!isAssignableTo(argTypes[i], expected) ||
                                  isLossyImplicitConversion(argTypes[i], expected))) {
        throw TypeCheckError(call->getSpan(),
                             "Argument type {} does not match explicit parameter type {}",
                             argTypes[i]->toString(), expected->toString());
      }
    }
    Type* retType = substituteInType(funcType->getReturnType(), explicitSubst);
    Type* finalType =
        retType != nullptr ? retType : cacheType(std::make_unique<Type>(BaseType::TY_VOID));
    result = std::make_unique<Value>(materializeForCallSite(finalType, importedScope));
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
  result = std::make_unique<Value>(materializeForCallSite(finalType, importedScope));
  return true;
}

auto Typechecker::cacheType(std::unique_ptr<Type> type) -> Type* {
  typeCache.push_back(std::move(type));
  return typeCache.back().get();
}

auto Typechecker::typeAsPtrIfClassForOverload(Type* t) -> Type* {
  if (t != nullptr && t->is(BaseType::TY_CLASS)) {
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
  }
  return t;
}

auto Typechecker::overloadArgTypesFromCall(const FuncCall* fc) -> std::vector<Type*> {
  std::vector<Type*> argTypes;
  if (fc == nullptr) {
    return argTypes;
  }
  argTypes.reserve(fc->getArguments().size());
  for (Expression* arg : fc->getArguments()) {
    arg->accept(*this);
    argTypes.push_back(typeAsPtrIfClassForOverload(result->getType()));
  }
  return argTypes;
}

void Typechecker::collectExplicitTypesFromCallByVisit(const FuncCall* call,
                                                      std::vector<Type*>& out) {
  out.clear();
  for (TypeExpr* texpr : call->getExplicitTypeArgs()) {
    texpr->accept(*this);
    out.push_back(result->getType());
  }
}

auto Typechecker::lookupFunctionInScopeThenImportedModuleCaches(
    const std::string& name, const std::vector<Type*>& methodArgTypes) -> Value* {
  Value* method = scope->lookupFunction(name, methodArgTypes);
  if (method != nullptr) {
    return method;
  }
  for (auto& [_, cachedModule] : importedModuleCache) {
    if (cachedModule == nullptr || cachedModule->rootScope == nullptr) {
      continue;
    }
    method = cachedModule->rootScope->lookupFunction(name, methodArgTypes);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

void Typechecker::registerEnumSyntheticMembers(const Enum* node, Type* enumTypePtr,
                                               SymbolTable* outerScope) {
  if (node == nullptr || enumTypePtr == nullptr || outerScope == nullptr) {
    return;
  }
  for (EnumVariant* variant : enumTypePtr->getEnumVariants()) {
    if (variant == nullptr) {
      continue;
    }

    std::vector<std::unique_ptr<Field>> ctorFields;
    std::vector<Type*> ctorParamTypes;
    for (size_t i = 0; i < variant->payloadTypes.size(); ++i) {
      Type* payloadType = variant->payloadTypes[i];
      Type* paramType = typeAsPtrIfClassForOverload(payloadType);
      ctorParamTypes.push_back(paramType);
      ctorFields.push_back(std::make_unique<Field>("arg" + std::to_string(i), paramType));
    }
    auto ctorType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(ctorFields));
    ctorType->setReturnType(enumTypePtr);
    Type* ctorTypePtr = cacheType(std::move(ctorType));
    Value* existingCtor = outerScope->lookupFunction(variant->name, ctorParamTypes,
                                                     FunctionLookupKind::OVERLOAD_IDENTITY);
    if (existingCtor != nullptr) {
      throw TypeCheckError(variant->getDeclarationSpan(),
                           "Enum variant constructor '{}' conflicts with an existing symbol",
                           variant->name);
    }
    auto ctorSym = std::make_unique<Value>(variant->name, ctorTypePtr);
    ctorSym->setCategory(ValueCategory::CALLABLE_SYMBOL);
    ctorSym->setDeclarationKind(ValueDeclarationKind::METHOD);
    ctorSym->setExported(node->isExported());
    ctorSym->setDeclarationSpan(variant->getDeclarationSpan());
    ctorSym->setDeclarationFilePath(mainFilePath);
    ctorSym->setStaticMethod(true);
    outerScope->insertSymbol(std::move(ctorSym));
  }
}

auto Typechecker::tryLookupFunctionViaDotImportLiterals(const DotOp* node, const std::string& name,
                                                        const std::vector<Type*>& methodArgTypes)
    -> Value* {
  auto* leftLit = dynamic_cast<Literal*>(node->getLeft());
  if (leftLit == nullptr || leftLit->getType() != TokenType::IDENTIFIER) {
    return nullptr;
  }
  if (auto srcIt = importedNameToSource.find(leftLit->getValue());
      srcIt != importedNameToSource.end()) {
    if (SymbolTable* imp = getOrTypecheckImport(srcIt->second.first)) {
      return imp->lookupFunction(name, methodArgTypes);
    }
    return nullptr;
  }
  if (auto ap = importAliasToPath.find(leftLit->getValue()); ap != importAliasToPath.end()) {
    if (SymbolTable* imp = getOrTypecheckImport(ap->second)) {
      return imp->lookupFunction(name, methodArgTypes);
    }
  }
  return nullptr;
}

void Typechecker::inferClassTemplateParamsForStaticMethodCallOnTemplate(
    llvm::SMRange span, Type* receiverForLookup, Type* methodType,
    const std::vector<Type*>& methodArgTypes,
    std::unordered_map<std::string, Type*>& traitBoundSubs) {
  const auto& classGenParams = getDeclaredGenericParams(receiverForLookup);
  if (classGenParams.empty()) {
    return;
  }
  std::unordered_map<std::string, Type*> inferredFromArgs;
  std::vector<Field*> const& mtFields = methodType->getFields();
  for (size_t i = 0; i < methodArgTypes.size() && i < mtFields.size(); ++i) {
    inferGenericBindings(mtFields[i]->type, methodArgTypes[i], inferredFromArgs, span);
  }
  for (const std::string& gn : classGenParams) {
    auto inferredIt = inferredFromArgs.find(gn);
    auto existingIt = traitBoundSubs.find(gn);
    if (existingIt != traitBoundSubs.end()) {
      if (inferredIt != inferredFromArgs.end() &&
          !existingIt->second->isEqual(inferredIt->second)) {
        throw TypeCheckError(span, "Conflicting types for class type parameter `{}`: {} vs {}", gn,
                             existingIt->second->toString(), inferredIt->second->toString());
      }
      continue;
    }
    if (inferredIt == inferredFromArgs.end()) {
      Type* ctxClass = nullptr;
      if (Type* exp = currentExpectedType(); exp != nullptr) {
        Type* shape = exp;
        if (shape->is(BaseType::TY_PTR) && shape->getElementType() != nullptr &&
            shape->getElementType()->is(BaseType::TY_CLASS)) {
          shape = shape->getElementType();
        }
        if (shape->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
          if (auto tit = specializedTypeToTemplate.find(shape);
              tit != specializedTypeToTemplate.end() && tit->second->isEqual(receiverForLookup)) {
            ctxClass = shape;
          }
        }
      }
      if (ctxClass != nullptr) {
        if (auto eit = specializedTypeEnv.find(ctxClass); eit != specializedTypeEnv.end()) {
          auto cit = eit->second.find(gn);
          if (cit != eit->second.end()) {
            traitBoundSubs[gn] = cit->second;
            continue;
          }
        }
      }
      throw TypeCheckError(
          span,
          "Cannot infer type parameter `{}` from this static method call; use "
          "arguments that determine `{}`, a contextual type (e.g. variable annotation), or "
          "call through a specialized type",
          gn, gn);
    }
    traitBoundSubs[gn] = inferredIt->second;
  }
}

auto Typechecker::isStdListClassType(Type* type) const -> bool {
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
}

auto Typechecker::getStdListElementType(Type* type) const -> Type* {
  if (type == nullptr) {
    return nullptr;
  }
  Type* cls = type;
  if (cls->is(BaseType::TY_PTR) && cls->getElementType() != nullptr) {
    cls = cls->getElementType();
  }
  if (!isStdListClassType(type)) {
    return nullptr;
  }
  if (auto it = specializedTypeEnv.find(cls); it != specializedTypeEnv.end()) {
    auto envIt = it->second.find("T");
    if (envIt != it->second.end()) {
      return envIt->second;
    }
  }
  return nullptr;
}

auto Typechecker::lookupSuperDispatchMethodInScopeThenImports(
    SymbolTable* insertScope, Type* superTy, const std::string& methodName,
    const std::vector<Type*>& argTypes, const std::unordered_map<std::string, Type*>* superSeed)
    -> Value* {
  auto receiverMatches = [this, superTy](Type* recvCls) {
    return superMethodReceiverMatchesFormal(recvCls, superTy);
  };
  Value* method = insertScope->lookupSuperClassMethod(methodName, argTypes, receiverMatches,
                                                      superTy, superSeed);
  if (method == nullptr) {
    method = insertScope->lookupFunction(methodName, argTypes, FunctionLookupKind::VALUE,
                                         currentClassType);
  }
  if (method != nullptr) {
    return method;
  }
  for (auto& [_, cachedModule] : importedModuleCache) {
    if (cachedModule == nullptr || cachedModule->rootScope == nullptr) {
      continue;
    }
    SymbolTable* root = cachedModule->rootScope.get();
    method =
        root->lookupSuperClassMethod(methodName, argTypes, receiverMatches, superTy, superSeed);
    if (method == nullptr) {
      method =
          root->lookupFunction(methodName, argTypes, FunctionLookupKind::VALUE, currentClassType);
    }
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

void Typechecker::finalizeResolvedMethodCallTyping(
    FuncCall* fc, Value* method, std::unordered_map<std::string, Type*>& methodTypeEnv,
    const std::vector<Type*>& methodArgTypes, llvm::SMRange span,
    const std::function<void(std::unordered_map<std::string, Type*>&)>& afterExplicitBindings) {
  Type* methodType = method->getType();
  typecheckExplicitResolvedMethodTypeArgsIfPresent(fc, methodType, methodTypeEnv, methodArgTypes,
                                                   span);
  if (static_cast<bool>(afterExplicitBindings)) {
    afterExplicitBindings(methodTypeEnv);
  }
  mergeMethodGenericParamsFromArgumentsWhenNoExplicitTypeArgs(fc, methodType, methodArgTypes,
                                                              methodTypeEnv, span);
  verifyGenericTraitBounds(method, methodTypeEnv, span);
  Type* retType = methodType->getReturnType();
  if (!methodTypeEnv.empty() && retType != nullptr) {
    retType = substituteInType(retType, methodTypeEnv);
  }
  if (!methodTypeEnv.empty()) {
    fc->setGenericBindingEnv(methodTypeEnv);
  }
  result = std::make_unique<Value>(retType);
}

void Typechecker::handleDotOpTyImportReceiver(const DotOp* node) {
  if (auto* leftLit = dynamic_cast<Literal*>(node->getLeft());
      leftLit != nullptr && leftLit->getType() == TokenType::IDENTIFIER) {
    if (Value* modSym = scope->lookup(leftLit->getValue())) {
      markValueRead(modSym);
    }
  }
  if (auto* idLit = dynamic_cast<Literal*>(node->getRight());
      idLit != nullptr && idLit->getType() == TokenType::IDENTIFIER) {
    std::string const alias = result->getName();
    auto pathIt = importAliasToPath.find(alias);
    if (pathIt != importAliasToPath.end()) {
      SymbolTable* importScope = getOrTypecheckImport(pathIt->second);
      if (importScope != nullptr) {
        Value* member = importScope->lookup(idLit->getValue());
        if (member != nullptr && member->getDeclarationKind() == ValueDeclarationKind::VARIABLE &&
            member->isExported()) {
          markImportNameStubUsedForQualifiedAccess(pathIt->second, idLit->getValue());
          idLit->setResolvedSymbol(member);
          Type* vt = materializeImportedType(member->getType());
          auto out = std::make_unique<Value>(*member);
          out->setType(vt);
          result = std::move(out);
          return;
        }
      }
    }
  }
  if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
    std::string alias = result->getName();
    auto pathIt = importAliasToPath.find(alias);
    if (pathIt != importAliasToPath.end()) {
      SymbolTable* importScope = getOrTypecheckImport(pathIt->second);
      if (importScope != nullptr) {
        std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
        Value* func = importScope->lookupFunction(fc->getName(), argTypes);
        if (func == nullptr) {
          Value* sym = importScope->lookup(fc->getName());
          if (sym != nullptr && sym->getType()->is(BaseType::TY_FUNCTION)) {
            func = sym;
          } else if (sym != nullptr && sym->getType()->is(BaseType::TY_CLASS)) {
            fc->setResolvedSymbol(sym);
            Type* classType = materializeImportedType(sym->getType());
            if (!fc->getExplicitTypeArgs().empty()) {
              finishGenericClassCallWithExplicitTypeArgs(
                  fc, classType, argTypes, importScope, false,
                  [&] { markImportNameStubUsedForQualifiedAccess(pathIt->second, fc->getName()); });
              return;
            }
            if (tryFinishGenericClassCallWithInferredTypeArgs(
                    fc, classType, argTypes, importScope, false, [&] {
                      markImportNameStubUsedForQualifiedAccess(pathIt->second, fc->getName());
                    })) {
              return;
            }
            markImportNameStubUsedForQualifiedAccess(pathIt->second, fc->getName());
            result = std::make_unique<Value>(materializeImportedType(classType));
            return;
          } else if (sym != nullptr && sym->getType()->is(BaseType::TY_ENUM)) {
            markImportNameStubUsedForQualifiedAccess(pathIt->second, fc->getName());
            result = std::make_unique<Value>(materializeImportedType(sym->getType()));
            return;
          }
        }
        if (func != nullptr) {
          markImportNameStubUsedForQualifiedAccess(pathIt->second, fc->getName());
          fc->setResolvedSymbol(func);
          Type* retType = func->getType()->getReturnType();
          result = std::make_unique<Value>(
              retType != nullptr ? materializeImportedType(retType)
                                 : cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
          return;
        }
      }
    }
    visitCallArgumentsIgnoringResult(fc);
  }
  result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
}

auto Typechecker::maybeRetypeCallArgsForStringLiteralOverload(const FuncCall* node,
                                                              std::vector<Type*>& argTypes)
    -> bool {
  bool hasStringLit = false;
  for (Expression* arg : node->getArguments()) {
    if (auto* lit = dynamic_cast<Literal*>(arg);
        lit != nullptr && lit->getType() == TokenType::STRING) {
      hasStringLit = true;
      break;
    }
  }
  if (!hasStringLit) {
    return false;
  }
  argTypes.clear();
  for (Expression* arg : node->getArguments()) {
    if (auto* lit = dynamic_cast<Literal*>(arg);
        lit != nullptr && lit->getType() == TokenType::STRING) {
      Type* cstrT = cacheType(std::make_unique<Type>(BaseType::TY_STRING));
      visitExprWithExpectedType(lit, cstrT);
      lit->setResolvedStrClassType(nullptr);
    } else {
      arg->accept(*this);
    }
    argTypes.push_back(typeAsPtrIfClassForOverload(result->getType()));
  }
  return true;
}

auto Typechecker::materializeForCallSite(Type* t, SymbolTable* importedScope) -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  return importedScope != nullptr ? materializeImportedType(t) : t;
}

void Typechecker::visitCallArgumentsIgnoringResult(const FuncCall* fc) {
  if (fc == nullptr) {
    return;
  }
  for (Expression* arg : fc->getArguments()) {
    arg->accept(*this);
  }
}

void Typechecker::tryPeelDotReceiverFromNamedImportStub(const DotOp* node, Type*& base,
                                                        bool& dotLeftDenotesTypeName) {
  auto* leftLit = dynamic_cast<Literal*>(node->getLeft());
  if (leftLit == nullptr || leftLit->getType() != TokenType::IDENTIFIER ||
      result->getCategory() != ValueCategory::MODULE_SYMBOL || base == nullptr ||
      !base->is(BaseType::TY_IMPORT)) {
    return;
  }
  if (auto srcIt = importedNameToSource.find(leftLit->getValue());
      srcIt != importedNameToSource.end()) {
    if (SymbolTable* imp = getOrTypecheckImport(srcIt->second.first)) {
      if (Value* exported = imp->lookup(srcIt->second.second);
          exported != nullptr && exported->getType() != nullptr &&
          exported->getType()->isOneOf(
              {BaseType::TY_CLASS, BaseType::TY_ENUM, BaseType::TY_TRAIT_EXISTENTIAL})) {
        base = materializeImportedType(exported->getType());
        dotLeftDenotesTypeName = true;
      }
    }
  } else if (auto pathIt = importAliasToPath.find(leftLit->getValue());
             pathIt != importAliasToPath.end()) {
    if (SymbolTable* imp = getOrTypecheckImport(pathIt->second)) {
      if (auto* rightId = dynamic_cast<Literal*>(node->getRight());
          rightId != nullptr && rightId->getType() == TokenType::IDENTIFIER) {
        if (Value* exported = imp->lookup(rightId->getValue());
            exported != nullptr && exported->getType() != nullptr &&
            exported->getType()->isOneOf(
                {BaseType::TY_CLASS, BaseType::TY_ENUM, BaseType::TY_TRAIT_EXISTENTIAL})) {
          base = materializeImportedType(exported->getType());
          dotLeftDenotesTypeName = true;
        }
      }
    }
  }
}

void Typechecker::typecheckDotUnionMethodCall(Type* unionTy, const DotOp* node, FuncCall* fc) {
  if (!fc->getExplicitTypeArgs().empty()) {
    throw TypeCheckError(node->getSpan(),
                         "Explicit type arguments are not supported on union method calls");
  }
  std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
  Type* commonRet = nullptr;
  Value* resolvedSymbol = nullptr;
  for (Type* mem : unionTy->getUnionMembers()) {
    if (mem == nullptr || !mem->is(BaseType::TY_CLASS)) {
      throw TypeCheckError(node->getSpan(),
                           "Calling a method on a union requires every variant to be a class "
                           "type");
    }
    ResolvedMethodCallInfo resolved =
        resolveMethodWithTraitEnv(mem, fc->getName(), argTypes, node->getSpan());
    if (resolved.returnType == nullptr) {
      throw TypeCheckError(node->getSpan(), "Method '{}' is not available on all union members",
                           fc->getName());
    }
    verifyGenericTraitBounds(resolved.method, resolved.traitBoundSubs, node->getSpan());
    if (commonRet == nullptr) {
      commonRet = resolved.returnType;
    } else if (!commonRet->isEqual(resolved.returnType)) {
      throw TypeCheckError(node->getSpan(),
                           "Method '{}' has incompatible return types across union members",
                           fc->getName());
    }
    resolvedSymbol = resolved.method;
  }
  fc->setResolvedSymbol(resolvedSymbol);
  result = std::make_unique<Value>(commonRet);
}

void Typechecker::finishGenericClassCallWithExplicitTypeArgs(
    const FuncCall* callSite, Type* classType, const std::vector<Type*>& argTypes,
    SymbolTable* ctorLookupScope, bool markConstructorSymbolRead,
    const std::function<void()>& afterSpecialize) {
  const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
  std::vector<Type*> explicitTypes;
  collectExplicitTypesFromCallByVisit(callSite, explicitTypes);
  if (explicitTypes.size() != genericParamNames.size()) {
    throw TypeCheckError(callSite->getSpan(),
                         "Explicit type argument count {} does not match "
                         "generic class parameter count {}",
                         explicitTypes.size(), genericParamNames.size());
  }
  std::unordered_map<std::string, Type*> env;
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    env[genericParamNames[i]] = explicitTypes[i];
  }
  Value* constructorForMark = nullptr;
  if (!argTypes.empty()) {
    Type* ptrToClass = typeAsPtrIfClassForOverload(classType);
    std::vector<Type*> constructorParamTypes = {ptrToClass};
    constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(), argTypes.end());
    Value* constructor =
        lookupConstructorForAllocatedClass(ctorLookupScope, constructorParamTypes, classType);
    if (constructor == nullptr) {
      throw TypeCheckError(callSite->getSpan(),
                           "Constructor not found for {} with given type arguments",
                           callSite->getName());
    }
    enforcePrivateMemberReadable(callSite->getSpan(), constructor);
    constructorForMark = constructor;
    auto ctorParams = constructor->getType()->getFields();
    for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
      Type* expected = substituteInType(ctorParams[i]->type, env);
      if (expected != nullptr && (!isAssignableTo(argTypes[i - 1], expected) ||
                                  isLossyImplicitConversion(argTypes[i - 1], expected))) {
        throw TypeCheckError(callSite->getSpan(),
                             "Argument type {} does not match explicit parameter type {}",
                             argTypes[i - 1]->toString(), expected->toString());
      }
    }
  }
  Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
  if (markConstructorSymbolRead && constructorForMark != nullptr) {
    markValueRead(constructorForMark);
  }
  if (afterSpecialize) {
    afterSpecialize();
  }
  result = std::make_unique<Value>(specialized);
  callSite->setAllocatedClassMonomorph(specialized);
}

auto Typechecker::tryFinishGenericClassCallWithInferredTypeArgs(
    const FuncCall* callSite, Type* classType, const std::vector<Type*>& argTypes,
    SymbolTable* ctorLookupScope, bool markConstructorSymbolRead,
    const std::function<void()>& afterSuccess) -> bool {
  if (argTypes.empty()) {
    return false;
  }
  const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
  Type* ptrToClass = typeAsPtrIfClassForOverload(classType);
  std::vector<Type*> constructorParamTypes = {ptrToClass};
  constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(), argTypes.end());
  Value* constructor =
      lookupConstructorForAllocatedClass(ctorLookupScope, constructorParamTypes, classType);
  if (constructor == nullptr) {
    return false;
  }
  enforcePrivateMemberReadable(callSite->getSpan(), constructor);
  std::unordered_map<std::string, Type*> env;
  auto* funcType = constructor->getType();
  auto ctorParams = funcType->getFields();
  for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
    inferGenericBindings(ctorParams[i]->type, argTypes[i - 1], env, callSite->getSpan());
  }
  Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
  if (markConstructorSymbolRead) {
    markValueRead(constructor);
  }
  if (afterSuccess) {
    afterSuccess();
  }
  result = std::make_unique<Value>(specialized);
  callSite->setAllocatedClassMonomorph(specialized);
  return true;
}

void Typechecker::typecheckExplicitResolvedMethodTypeArgsIfPresent(
    const FuncCall* fc, Type* methodType, std::unordered_map<std::string, Type*>& methodTypeEnv,
    const std::vector<Type*>& methodArgTypes, llvm::SMRange span) {
  if (fc->getExplicitTypeArgs().empty()) {
    return;
  }
  std::vector<Type*> explicitTypes;
  std::vector<TypeExpr*> const tas = fc->getExplicitTypeArgs();
  explicitTypes.reserve(tas.size());
  for (TypeExpr* texpr : tas) {
    explicitTypes.push_back(resolveType(texpr));
  }
  const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(methodType);
  if (explicitTypes.size() != genericParamNames.size()) {
    throw TypeCheckError(span,
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
    if (expected != nullptr && (!isAssignableTo(methodArgTypes[i], expected) ||
                                isLossyImplicitConversion(methodArgTypes[i], expected))) {
      throw TypeCheckError(span,
                           "Argument type {} does not match explicit "
                           "parameter type {}",
                           methodArgTypes[i]->toString(), expected->toString());
    }
  }
}

void Typechecker::mergeMethodGenericParamsFromArgumentsWhenNoExplicitTypeArgs(
    const FuncCall* fc, Type* methodType, const std::vector<Type*>& methodArgTypes,
    std::unordered_map<std::string, Type*>& traitBoundSubs, llvm::SMRange span) {
  if (!fc->getExplicitTypeArgs().empty()) {
    return;
  }
  const auto& methodGenericNames = methodType->getGenericParams();
  if (methodGenericNames.empty()) {
    return;
  }
  std::unordered_map<std::string, Type*> extraInferred;
  auto fields = methodType->getFields();
  for (size_t i = 0; i < fields.size() && i < methodArgTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, methodArgTypes[i], extraInferred, span);
  }
  mergeInferredGenericBindings(traitBoundSubs, extraInferred, span, &methodGenericNames);
}

void Typechecker::mergeInferredGenericBindings(
    std::unordered_map<std::string, Type*>& targetBindings,
    const std::unordered_map<std::string, Type*>& inferredBindings, llvm::SMRange span,
    const std::vector<std::string>* allowedGenericNames) {
  for (const auto& kv : inferredBindings) {
    if (allowedGenericNames != nullptr &&
        std::find(allowedGenericNames->begin(), allowedGenericNames->end(), kv.first) ==
            allowedGenericNames->end()) {
      continue;
    }
    auto existingIt = targetBindings.find(kv.first);
    if (existingIt == targetBindings.end()) {
      targetBindings[kv.first] = kv.second;
      continue;
    }
    if (existingIt->second == nullptr || kv.second == nullptr) {
      if (existingIt->second == kv.second) {
        continue;
      }
      throw TypeCheckError(span, "Conflicting inferred types for generic parameter {}: {} and {}",
                           kv.first,
                           existingIt->second != nullptr ? existingIt->second->toString() : "void",
                           kv.second != nullptr ? kv.second->toString() : "void");
    }
    if (existingIt->second->isEqual(kv.second) ||
        (isAssignableTo(kv.second, existingIt->second) &&
         !isLossyImplicitConversion(kv.second, existingIt->second))) {
      continue;
    }
    throw TypeCheckError(span, "Conflicting inferred types for generic parameter {}: {} and {}",
                         kv.first, existingIt->second->toString(), kv.second->toString());
  }
}

auto Typechecker::traitRequirementParamLookupTypes(Type* selfPtr, const FuncDecl* req)
    -> std::vector<Type*> {
  std::vector<Type*> lookupArgs = {selfPtr};
  if (req == nullptr) {
    return lookupArgs;
  }
  for (Parameter* p : req->getParameters()) {
    if (p->type != nullptr) {
      p->type->accept(*this);
      lookupArgs.push_back(typeAsPtrIfClassForOverload(result->getType()));
    }
  }
  return lookupArgs;
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
    copy->setBuiltinStringClass(type->isBuiltinStringClass());
    copy->setGenericParams(type->getGenericParams());
    copy->setImplTraitNames(std::vector<std::string>(type->getImplTraitNames()));
    copy->setDeclarationSpan(type->getDeclarationSpan());
    copy->setDeclarationFilePath(type->getDeclarationFilePath());
    for (Field* field : type->getFields()) {
      copy->addField(cloneImportedField(field));
    }
    for (EnumVariant* variant : type->getEnumVariants()) {
      if (variant == nullptr) {
        continue;
      }
      std::vector<Type*> payloadTypes;
      payloadTypes.reserve(variant->payloadTypes.size());
      for (Type* payload : variant->payloadTypes) {
        payloadTypes.push_back(materializeImportedType(payload));
      }
      auto clonedVariant = std::make_unique<EnumVariant>(variant->name, std::move(payloadTypes));
      clonedVariant->setDeclarationSpan(variant->getDeclarationSpan());
      clonedVariant->setDeclarationFilePath(variant->getDeclarationFilePath());
      copy->addEnumVariant(std::move(clonedVariant));
    }
    if (type->is(BaseType::TY_CLASS)) {
      for (Field* field : type->getStaticFields()) {
        copy->addStaticField(cloneImportedField(field));
      }
      copy->setClassSuperclass(materializeImportedType(type->getClassSuperclass()));
      copy->setClassVtableMethodOrder(std::vector<std::string>(type->getClassVtableMethodOrder()));
      copy->setClassHasDerivedClass(type->getClassHasDerivedClass());
    }
    if (auto tmplIt = specializedTypeToTemplate.find(type);
        tmplIt != specializedTypeToTemplate.end()) {
      std::unordered_map<std::string, Type*> envCopy;
      if (auto envIt = specializedTypeEnv.find(type); envIt != specializedTypeEnv.end()) {
        for (const auto& [name, envType] : envIt->second) {
          envCopy[name] = materializeImportedType(envType);
        }
      }
      registerSpecializedClassType(copy, materializeImportedType(tmplIt->second),
                                   std::move(envCopy));
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
      fields.push_back(cloneImportedField(field));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(materializeImportedType(type->getReturnType()));
    funcType->setGenericParams(type->getGenericParams());
    funcType->setGenericParamTraitBounds(
        std::vector<std::vector<std::string>>(type->getGenericParamTraitBounds()));
    funcType->setVarArgs(type->isVarArgs());
    Type* copy = cacheType(std::move(funcType));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_UNION)) {
    std::vector<Type*> members;
    members.reserve(type->getUnionMembers().size());
    for (Type* m : type->getUnionMembers()) {
      members.push_back(materializeImportedType(m));
    }
    auto [canonical, displayName] = TypeUtils::canonicalizeUnionMembers(std::move(members));
    if (canonical.size() == 1U) {
      Type* single = canonical.front();
      importedTypeCopies[type] = single;
      return single;
    }
    auto u = std::make_unique<Type>(BaseType::TY_UNION);
    u->setUnionMembers(std::move(canonical));
    u->setDisplayName(displayName);
    u->setDeclarationSpan(type->getDeclarationSpan());
    Type* copy = cacheType(std::move(u));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_TUPLE)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : type->getFields()) {
      fields.push_back(cloneImportedField(field));
    }
    auto tupleType = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
    tupleType->setDisplayName(type->getDisplayName());
    Type* copy = cacheType(std::move(tupleType));
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
    u->setSigned(type->isSigned());
    Type* copy = cacheType(std::move(u));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    auto u = std::make_unique<Type>(BaseType::TY_TRAIT_EXISTENTIAL, nullptr);
    u->setDisplayName(type->getDisplayName());
    Type* copy = cacheType(std::move(u));
    importedTypeCopies[type] = copy;
    if (auto envIt = specializedTraitExistentialEnv.find(type);
        envIt != specializedTraitExistentialEnv.end()) {
      std::unordered_map<std::string, Type*> envCopy;
      for (const auto& [name, envType] : envIt->second) {
        envCopy[name] = materializeImportedType(envType);
      }
      specializedTraitExistentialEnv[copy] = std::move(envCopy);
    }
    return copy;
  }
  if (type->is(BaseType::TY_FLOAT)) {
    Type* copy = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_FLOAT32)) {
    Type* copy = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT32));
    importedTypeCopies[type] = copy;
    return copy;
  }
  Type* copy = cacheType(std::make_unique<Type>(type->getBaseType()));
  importedTypeCopies[type] = copy;
  return copy;
}

auto Typechecker::cloneImportedField(Field* field) -> std::unique_ptr<Field> {
  auto fieldCopy = std::make_unique<Field>(field->name, materializeImportedType(field->type));
  fieldCopy->setDeclarationSpan(field->getDeclarationSpan());
  fieldCopy->setDeclarationFilePath(field->getDeclarationFilePath());
  if (Value* declarationSymbol = field->getDeclarationSymbol()) {
    auto symbolCopy =
        std::make_unique<Value>(declarationSymbol->getName(), materializeImportedType(field->type));
    symbolCopy->setCategory(declarationSymbol->getCategory());
    symbolCopy->setDeclarationKind(declarationSymbol->getDeclarationKind());
    symbolCopy->setDeclarationSpan(declarationSymbol->getDeclarationSpan());
    symbolCopy->setDeclarationFilePath(declarationSymbol->getDeclarationFilePath());
    symbolCopy->setMutable(declarationSymbol->getMutability());
    symbolCopy->setPrivateMember(declarationSymbol->isPrivateMember());
    if (Type* declCls = declarationSymbol->getMemberDeclaredInClass(); declCls != nullptr) {
      symbolCopy->setMemberDeclaredInClass(materializeImportedType(declCls));
    }
    fieldCopy->setDeclarationSymbol(std::move(symbolCopy));
  }
  return fieldCopy;
}

auto Typechecker::tryResolveNonCustomTypeExpr(const TypeExpr* node) -> Type* {
  const TokenType tt = node->getType();
  switch (tt) {
  case TokenType::INT_TYPE:
  case TokenType::INT8_TYPE:
  case TokenType::INT16_TYPE:
  case TokenType::INT32_TYPE:
  case TokenType::UINT_TYPE:
  case TokenType::UINT8_TYPE:
  case TokenType::UINT16_TYPE:
  case TokenType::UINT32_TYPE: {
    unsigned width = 64;
    bool isSignedInt = true;
    switch (tt) {
    case TokenType::INT_TYPE:
      width = 64;
      isSignedInt = true;
      break;
    case TokenType::INT8_TYPE:
      width = 8;
      isSignedInt = true;
      break;
    case TokenType::INT16_TYPE:
      width = 16;
      isSignedInt = true;
      break;
    case TokenType::INT32_TYPE:
      width = 32;
      isSignedInt = true;
      break;
    case TokenType::UINT_TYPE:
      width = 64;
      isSignedInt = false;
      break;
    case TokenType::UINT8_TYPE:
      width = 8;
      isSignedInt = false;
      break;
    case TokenType::UINT16_TYPE:
      width = 16;
      isSignedInt = false;
      break;
    case TokenType::UINT32_TYPE:
      width = 32;
      isSignedInt = false;
      break;
    default:
      break;
    }
    auto u = std::make_unique<Type>(BaseType::TY_INT);
    u->setIntWidth(static_cast<std::uint16_t>(width));
    u->setSigned(isSignedInt);
    return cacheType(std::move(u));
  }
  case TokenType::FLOAT_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_FLOAT));
  case TokenType::FLOAT32_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_FLOAT32));
  case TokenType::BOOL_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  case TokenType::STRING_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_STRING));
  case TokenType::ANY_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_ANY));
  case TokenType::VOID_TYPE:
    return cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  case TokenType::NIL:
    return cacheType(std::make_unique<Type>(BaseType::TY_NULL));
  case TokenType::PTR_TYPE: {
    node->getElementType()->accept(*this);
    Type* elem = result->getType();
    if (elem->is(BaseType::TY_FUNCTION)) {
      return elem;
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  case TokenType::FUNC_TYPE: {
    node->getReturnType()->accept(*this);
    Type* retType = wrapReturnTypeIfNominal(result->getType());
    std::vector<std::unique_ptr<Field>> fields;
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      fields.push_back(std::make_unique<Field>(result->getName(), result->getType()));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(retType);
    return cacheType(std::move(funcType));
  }
  case TokenType::TUPLE_TYPE: {
    std::vector<std::unique_ptr<Field>> fields;
    std::string displayName = "tuple<";
    for (size_t i = 0; i < node->getParams().size(); ++i) {
      TypeExpr* param = node->getParams()[i];
      param->accept(*this);
      Type* elemTy = result->getType();
      if (i > 0) {
        displayName += ", ";
      }
      displayName += elemTy->toString();
      fields.push_back(std::make_unique<Field>("_" + std::to_string(i), elemTy));
    }
    displayName += ">";
    auto tup = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
    tup->setDisplayName(displayName);
    return cacheType(std::move(tup));
  }
  case TokenType::UNION_TYPE: {
    std::vector<Type*> flat;
    for (TypeExpr* arm : node->getParams()) {
      Type* t = resolveType(arm);
      if (t->is(BaseType::TY_UNION)) {
        for (Type* inner : t->getUnionMembers()) {
          flat.push_back(inner);
        }
      } else {
        flat.push_back(t);
      }
    }
    std::vector<Type*> unique;
    for (Type* t : flat) {
      bool dup = false;
      for (Type* u : unique) {
        if (t->isEqual(u)) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        unique.push_back(t);
      }
    }
    for (Type* t : unique) {
      if (t->is(BaseType::TY_ARRAY)) {
        if (!isStdlibSourcePath(mainFilePath)) {
          throw TypeCheckError(node->getSpan(),
                               "`__buffer<…>` is not allowed as a union member outside the "
                               "standard library (use `list<…>` or another class type instead)",
                               t->toString());
        }
      } else if (t->is(BaseType::TY_GENERIC)) {
        if (!currentGenericTypes.contains(t->getGenericName())) {
          throw TypeCheckError(
              node->getSpan(),
              "Union member `{}` is not a class or primitive; only type parameters of the "
              "enclosing generic declaration may appear here",
              t->toString());
        }
      } else if (!isSupportedUnionMemberType(t)) {
        throw TypeCheckError(
            node->getSpan(),
            "Union member type `{}` is not supported (allowed: int, float, float32, bool, enum, "
            "and class types — e.g. str, list<U>, your own classes, or enums)",
            t->toString());
      }
    }
    auto [canonicalMembers, displayName] = TypeUtils::canonicalizeUnionMembers(std::move(unique));
    auto u = std::make_unique<Type>(BaseType::TY_UNION);
    u->setUnionMembers(std::move(canonicalMembers));
    u->setDisplayName(displayName);
    u->setDeclarationSpan(node->getSpan());
    return cacheType(std::move(u));
  }
  case TokenType::CUSTOM_TYPE:
    return nullptr;
  default:
    throw TypeCheckError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
  }
}

auto Typechecker::resolveCustomTypeExpr(const TypeExpr* node) -> Type* {
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
                         "to the generic parameter list (e.g. func foo<T>(x: T) -> T).",
                         node->getName());
  }
  if (sym == nullptr) {
    sym = scope->lookup(lookupName);
  }
  if (sym != nullptr && !isTypeSymbolForCustomTypeName(sym)) {
    sym = nullptr;
  }
  if (typ == nullptr && sym == nullptr) {
    throw TypeCheckError(node->getSpan(),
                         "Type '{}' not found. If you meant a generic type parameter, add it "
                         "to the generic parameter list (e.g. func foo<T>(x: T) -> T).",
                         node->getName());
  }
  node->setResolvedSymbol(sym);
  Type* resolvedType = nullptr;
  if (typ != nullptr) {
    resolvedType = typ;
  } else if (sym != nullptr) {
    resolvedType = sym->getType();
  }
  if (resolvedFromImport) {
    resolvedType = materializeImportedType(resolvedType);
  }
  if (explicitTypeArgs.empty()) {
    return resolvedType;
  }
  if (resolvedType != nullptr && resolvedType->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    auto trIt = traitRegistry.find(lookupName);
    if (trIt == traitRegistry.end()) {
      throw TypeCheckError(node->getSpan(), "Unknown trait '{}'", lookupName);
    }
    const auto& traitGen = trIt->second->getGenericParamDecls();
    if (traitGen.size() != explicitTypeArgs.size()) {
      throw TypeCheckError(node->getSpan(), "Trait {} expects {} type argument(s), got {}",
                           lookupName, traitGen.size(), explicitTypeArgs.size());
    }
    std::vector<std::string> genericParamNames;
    genericParamNames.reserve(traitGen.size());
    for (const auto& p : traitGen) {
      genericParamNames.push_back(p.name);
    }
    return getOrCreateSpecializedTraitExistentialType(resolvedType, lookupName, genericParamNames,
                                                      explicitTypeArgs);
  }
  if (resolvedType == nullptr ||
      !resolvedType->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    throw TypeCheckError(node->getSpan(), "Type {} is not a generic nominal type", node->getName());
  }
  Type* classTemplate = resolvedType;
  auto templateIt = specializedTypeToTemplate.find(classTemplate);
  if (templateIt != specializedTypeToTemplate.end()) {
    classTemplate = templateIt->second;
  }
  const auto& genericParamNames = classTemplate->getGenericParams();
  if (genericParamNames.size() != explicitTypeArgs.size()) {
    throw TypeCheckError(node->getSpan(), "Generic type {} expects {} type arguments, got {}",
                         node->getName(), genericParamNames.size(), explicitTypeArgs.size());
  }
  std::unordered_map<std::string, Type*> env;
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    env[genericParamNames[i]] = explicitTypeArgs[i];
  }
  return getOrCreateSpecializedClassType(classTemplate, genericParamNames, env);
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
    funcType->setGenericParamTraitBounds(
        std::vector<std::vector<std::string>>(t->getGenericParamTraitBounds()));
    funcType->setVarArgs(t->isVarArgs());
    return cacheType(std::move(funcType));
  }
  if (t->is(BaseType::TY_ENUM)) {
    if (auto tmplIt = specializedTypeToTemplate.find(t); tmplIt != specializedTypeToTemplate.end()) {
      t = tmplIt->second;
    }
    if (t->getGenericParams().empty()) {
      return t;
    }
    return getOrCreateSpecializedClassType(t, t->getGenericParams(), env);
  }
  if (t->is(BaseType::TY_TUPLE)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : t->getFields()) {
      fields.push_back(std::make_unique<Field>(field->name, substituteInType(field->type, env)));
    }
    auto tupleType = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
    tupleType->setDisplayName(t->getDisplayName());
    return cacheType(std::move(tupleType));
  }
  if (t->is(BaseType::TY_UNION)) {
    std::vector<Type*> arms;
    arms.reserve(t->getUnionMembers().size());
    for (Type* m : t->getUnionMembers()) {
      arms.push_back(substituteInType(m, env));
    }
    auto [unique, dn] = TypeUtils::canonicalizeUnionMembers(std::move(arms));
    // Match Codegen::substituteTypeForSpecializationEnv: a single arm is the arm type itself, not a
    // tagged singleton union (avoids reintroducing union layout after specialization).
    if (unique.size() == 1U) {
      return unique.front();
    }
    auto u = std::make_unique<Type>(BaseType::TY_UNION);
    u->setDisplayName(dn);
    u->setUnionMembers(std::move(unique));
    u->setDeclarationSpan(t->getDeclarationSpan());
    return cacheType(std::move(u));
  }
  if (t->is(BaseType::TY_CLASS)) {
    Type* classTemplate = t;
    if (auto tmplIt = specializedTypeToTemplate.find(t);
        tmplIt != specializedTypeToTemplate.end()) {
      classTemplate = tmplIt->second;
    }
    const auto& genericParamNames = classTemplate->getGenericParams();
    if (genericParamNames.empty()) {
      return t;
    }
    std::unordered_map<std::string, Type*> classEnv;
    if (auto specTmpl = specializedTypeToTemplate.find(t);
        specTmpl != specializedTypeToTemplate.end()) {
      if (auto envIt = specializedTypeEnv.find(t); envIt != specializedTypeEnv.end()) {
        for (const auto& name : genericParamNames) {
          auto b = envIt->second.find(name);
          if (b != envIt->second.end()) {
            classEnv[name] = substituteInType(b->second, env);
          }
        }
      }
    }
    for (const auto& name : genericParamNames) {
      if (auto it = env.find(name); it != env.end()) {
        classEnv[name] = it->second;
      }
    }
    bool allBound = true;
    for (const auto& name : genericParamNames) {
      if (!classEnv.contains(name)) {
        allBound = false;
        break;
      }
    }
    if (allBound) {
      return getOrCreateSpecializedClassType(classTemplate, genericParamNames, classEnv);
    }
  }
  return t;
}

auto Typechecker::typeUsesClassTypeParameter(
    Type* t, const std::unordered_set<std::string>& classParamNames) const -> bool {
  std::unordered_set<Type*> visitedNominalClasses;
  return typeUsesClassTypeParameter(t, classParamNames, visitedNominalClasses);
}

auto Typechecker::typeUsesClassTypeParameter(Type* t,
                                             const std::unordered_set<std::string>& classParamNames,
                                             std::unordered_set<Type*>& visitedNominalClasses) const
    -> bool {
  if (t == nullptr) {
    return false;
  }
  if (t->is(BaseType::TY_GENERIC)) {
    return classParamNames.contains(t->getGenericName());
  }
  if (t->is(BaseType::TY_PTR) || t->is(BaseType::TY_ARRAY)) {
    return typeUsesClassTypeParameter(t->getElementType(), classParamNames, visitedNominalClasses);
  }
  if (t->is(BaseType::TY_FUNCTION)) {
    for (Field* field : t->getFields()) {
      if (typeUsesClassTypeParameter(field->type, classParamNames, visitedNominalClasses)) {
        return true;
      }
    }
    return typeUsesClassTypeParameter(t->getReturnType(), classParamNames, visitedNominalClasses);
  }
  if (t->is(BaseType::TY_TUPLE)) {
    for (Field* field : t->getFields()) {
      if (typeUsesClassTypeParameter(field->type, classParamNames, visitedNominalClasses)) {
        return true;
      }
    }
    return false;
  }
  if (t->is(BaseType::TY_UNION)) {
    for (Type* m : t->getUnionMembers()) {
      if (typeUsesClassTypeParameter(m, classParamNames, visitedNominalClasses)) {
        return true;
      }
    }
    return false;
  }
  if (t->is(BaseType::TY_CLASS)) {
    auto envIt = specializedTypeEnv.find(t);
    if (envIt == specializedTypeEnv.end()) {
      // Unspecialized generic class template (declaration type) is not in specializedTypeEnv;
      // treat it as using type parameters for static-field / similar restrictions.
      Type* classTemplate = t;
      if (auto tmplIt = specializedTypeToTemplate.find(t);
          tmplIt != specializedTypeToTemplate.end()) {
        classTemplate = tmplIt->second;
      }
      return !classTemplate->getGenericParams().empty();
    }
    if (visitedNominalClasses.contains(t)) {
      return false;
    }
    visitedNominalClasses.insert(t);
    for (const auto& kv : envIt->second) {
      if (typeUsesClassTypeParameter(kv.second, classParamNames, visitedNominalClasses)) {
        visitedNominalClasses.erase(t);
        return true;
      }
    }
    visitedNominalClasses.erase(t);
    return false;
  }
  if (t->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    if (auto it = specializedTraitExistentialEnv.find(t);
        it != specializedTraitExistentialEnv.end()) {
      for (const auto& kv : it->second) {
        if (typeUsesClassTypeParameter(kv.second, classParamNames, visitedNominalClasses)) {
          return true;
        }
      }
    }
    return false;
  }
  return false;
}

auto Typechecker::inferGenericBindings(Type* pattern, Type* actual,
                                       std::unordered_map<std::string, Type*>& bindings,
                                       llvm::SMRange span) -> void {
  if (pattern == nullptr || actual == nullptr) {
    return;
  }
  if (pattern->is(BaseType::TY_GENERIC)) {
    const std::string genericName = pattern->getGenericName();
    // Class-typed arguments use pointer types at the call/overload boundary (`*U`) while generic
    // bindings and class specializations use the nominal class type `U`. Bind the parameter to
    // the pointee so inference matches `specializedTypeEnv` and method signatures written as `K`.
    Type* bindingActual = actual;
    Type* const pointee =
        actual != nullptr && actual->is(BaseType::TY_PTR) ? actual->getElementType() : nullptr;
    if (pointee != nullptr && pointee->isNominal()) {
      bindingActual = pointee;
    }
    auto it = bindings.find(genericName);
    if (it == bindings.end()) {
      bindings[genericName] = bindingActual;
      return;
    }
    if (!bindingActual->isEqual(it->second)) {
      throw TypeCheckError(span, "Conflicting inferred types for generic parameter {}: {} and {}",
                           genericName, it->second->toString(), bindingActual->toString());
    }
    return;
  }
  if (pattern->is(BaseType::TY_UNION) && actual->is(BaseType::TY_UNION)) {
    const auto& pmem = pattern->getUnionMembers();
    const auto& amem = actual->getUnionMembers();
    if (pmem.size() != amem.size()) {
      return;
    }
    std::vector<bool> used(amem.size(), false);
    std::unordered_map<std::string, Type*> const emptyAcc;
    auto tryInfer = [&](auto&& self, size_t fi,
                        const std::unordered_map<std::string, Type*>& acc) -> bool {
      if (fi == pmem.size()) {
        for (const auto& kv : acc) {
          auto it = bindings.find(kv.first);
          if (it == bindings.end()) {
            bindings[kv.first] = kv.second;
          } else if (!it->second->isEqual(kv.second)) {
            throw TypeCheckError(span,
                                 "Conflicting inferred types for generic parameter {}: {} and {}",
                                 kv.first, it->second->toString(), kv.second->toString());
          }
        }
        return true;
      }
      for (size_t aj = 0; aj < amem.size(); ++aj) {
        if (used[aj]) {
          continue;
        }
        if (!pmem[fi]->isEqual(amem[aj])) {
          continue;
        }
        auto probe = acc;
        try {
          inferGenericBindings(pmem[fi], amem[aj], probe, span);
        } catch (const TypeCheckError&) {
          continue;
        }
        used[aj] = true;
        if (self(self, fi + 1, probe)) {
          return true;
        }
        used[aj] = false;
      }
      return false;
    };
    if (!tryInfer(tryInfer, 0, emptyAcc)) {
      return;
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
    if (!pattern->functionGenericSignatureEqual(actual)) {
      return;
    }
    auto patternFields = pattern->getFields();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < patternFields.size() && i < actualFields.size(); ++i) {
      inferGenericBindings(patternFields[i]->type, actualFields[i]->type, bindings, span);
    }
    inferGenericBindings(pattern->getReturnType(), actual->getReturnType(), bindings, span);
    return;
  }
  if (pattern->is(BaseType::TY_TUPLE)) {
    auto patternFields = pattern->getFields();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < patternFields.size() && i < actualFields.size(); ++i) {
      inferGenericBindings(patternFields[i]->type, actualFields[i]->type, bindings, span);
    }
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

auto Typechecker::superMethodReceiverMatchesFormal(Type* formalReceiverClass, Type* staticSuperType)
    -> bool {
  if (formalReceiverClass == nullptr || staticSuperType == nullptr) {
    return false;
  }
  if (formalReceiverClass->isEqual(staticSuperType)) {
    return true;
  }
  if (auto specIt = specializedTypeToTemplate.find(staticSuperType);
      specIt != specializedTypeToTemplate.end() && formalReceiverClass->isEqual(specIt->second)) {
    return true;
  }
  return false;
}

auto Typechecker::lookupConstructorForAllocatedClass(SymbolTable* tab,
                                                     const std::vector<Type*>& ctorParamTypes,
                                                     Type* classType) -> Value* {
  if (tab == nullptr) {
    return nullptr;
  }
  if (classType != nullptr && getDeclaredGenericParams(classType).empty()) {
    if (Value* exact =
            tab->lookupFunction("new", ctorParamTypes, FunctionLookupKind::OVERLOAD_IDENTITY);
        exact != nullptr) {
      return exact;
    }
  }
  return tab->lookupFunction("new", ctorParamTypes);
}

void Typechecker::registerSpecializedClassType(Type* specialized, Type* classTemplate,
                                               std::unordered_map<std::string, Type*> env) {
  if (specialized == nullptr || classTemplate == nullptr) {
    return;
  }
  const auto& genericParamNames = classTemplate->getGenericParams();
  specializedTypeEnv[specialized] = std::move(env);
  specializedTypeToTemplate[specialized] = classTemplate;
  specialized->setGenericParams(genericParamNames);
  specialized->setImplTraitNames(classTemplate->getImplTraitNames());
  specialized->setDeclarationSpan(classTemplate->getDeclarationSpan());
  specialized->setDeclarationFilePath(classTemplate->getDeclarationFilePath());
  if (classTemplate->is(BaseType::TY_CLASS)) {
    specialized->setClassVtableMethodOrder(
        std::vector<std::string>(classTemplate->getClassVtableMethodOrder()));
    specialized->setClassHasDerivedClass(classTemplate->getClassHasDerivedClass());
  }
  specialized->setDisplayName(makeSpecializedDisplayName(classTemplate, genericParamNames,
                                                         specializedTypeEnv[specialized]));
  specializedClassTypes[TypeUtils::makeSpecializedClassKey(
      classTemplate, genericParamNames, specializedTypeEnv[specialized])] = specialized;
}

auto Typechecker::getOrCreateSpecializedClassType(Type* classTemplate,
                                                  const std::vector<std::string>& genericParamNames,
                                                  const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  if (genericParamNames.empty()) {
    return classTemplate;
  }

  std::string keyStr = TypeUtils::makeSpecializedClassKey(classTemplate, genericParamNames, env);
  auto it = specializedClassTypes.find(keyStr);
  if (it != specializedClassTypes.end()) {
    return it->second;
  }
  const bool templateIncomplete = classTemplateBeingDeclared != nullptr &&
                                  classTemplate == classTemplateBeingDeclared &&
                                  classTemplate->getFields().size() < classFieldCountExpected;
  if (templateIncomplete) {
    auto specialized = std::make_unique<Type>(classTemplate->getBaseType(), nullptr,
                                              std::vector<std::unique_ptr<Field>>{});
    Type* ptr = cacheType(std::move(specialized));
    registerSpecializedClassType(ptr, classTemplate, std::unordered_map<std::string, Type*>(env));
    if (classTemplate->getClassSuperclass() != nullptr) {
      ptr->setClassSuperclass(substituteInType(classTemplate->getClassSuperclass(), env));
    }
    return ptr;
  }
  // Cache an empty specialization before substituting fields so recursive references (e.g. *Node<T>
  // to Node<T>) hit specializedClassTypes and do not recurse infinitely in substituteInType.
  auto specializedShell = std::make_unique<Type>(classTemplate->getBaseType(), nullptr,
                                                 std::vector<std::unique_ptr<Field>>{});
  Type* ptr = cacheType(std::move(specializedShell));
  registerSpecializedClassType(ptr, classTemplate, std::unordered_map<std::string, Type*>(env));
  if (classTemplate->getClassSuperclass() != nullptr) {
    ptr->setClassSuperclass(substituteInType(classTemplate->getClassSuperclass(), env));
  }

  std::vector<std::unique_ptr<Field>> newFields;
  for (Field* f : classTemplate->getFields()) {
    Type* subst = substituteInType(f->type, env);
    auto nf = std::make_unique<Field>(f->name, subst);
    nf->setDeclarationSpan(f->getDeclarationSpan());
    nf->setDeclarationFilePath(f->getDeclarationFilePath());
    if (Value* ds = f->getDeclarationSymbol()) {
      auto symCopy = std::make_unique<Value>(*ds);
      symCopy->setType(subst);
      nf->setDeclarationSymbol(std::move(symCopy));
    }
    newFields.push_back(std::move(nf));
  }
  ptr->replaceFields(std::move(newFields));
  if (classTemplate->is(BaseType::TY_ENUM)) {
    std::vector<std::unique_ptr<EnumVariant>> newVariants;
    for (EnumVariant* variant : classTemplate->getEnumVariants()) {
      if (variant == nullptr) {
        continue;
      }
      std::vector<Type*> payloadTypes;
      payloadTypes.reserve(variant->payloadTypes.size());
      for (Type* payload : variant->payloadTypes) {
        payloadTypes.push_back(substituteInType(payload, env));
      }
      auto nv = std::make_unique<EnumVariant>(variant->name, std::move(payloadTypes));
      nv->setDeclarationSpan(variant->getDeclarationSpan());
      nv->setDeclarationFilePath(variant->getDeclarationFilePath());
      newVariants.push_back(std::move(nv));
    }
    ptr->replaceEnumVariants(std::move(newVariants));
  }
  {
    std::vector<std::unique_ptr<Field>> newStaticFields;
    for (Field* f : classTemplate->getStaticFields()) {
      Type* subst = substituteInType(f->type, env);
      auto nf = std::make_unique<Field>(f->name, subst);
      nf->setDeclarationSpan(f->getDeclarationSpan());
      nf->setDeclarationFilePath(f->getDeclarationFilePath());
      if (Value* ds = f->getDeclarationSymbol()) {
        auto symCopy = std::make_unique<Value>(*ds);
        symCopy->setType(subst);
        nf->setDeclarationSymbol(std::move(symCopy));
      }
      newStaticFields.push_back(std::move(nf));
    }
    ptr->replaceStaticFields(std::move(newStaticFields));
  }
  return ptr;
}

void Typechecker::finalizeSpecializedTypesForTemplate(Type* classTemplate) {
  if (classTemplate == nullptr || classTemplate->getGenericParams().empty()) {
    return;
  }
  const auto& genericParamNames = classTemplate->getGenericParams();
  for (const auto& [specPtr, tmplPtr] : specializedTypeToTemplate) {
    if (tmplPtr != classTemplate) {
      continue;
    }
    auto envIt = specializedTypeEnv.find(specPtr);
    if (envIt == specializedTypeEnv.end()) {
      continue;
    }
    const auto& env = envIt->second;
    std::vector<std::unique_ptr<Field>> newFields;
    for (Field* f : classTemplate->getFields()) {
      Type* subst = substituteInType(f->type, env);
      auto nf = std::make_unique<Field>(f->name, subst);
      nf->setDeclarationSpan(f->getDeclarationSpan());
      nf->setDeclarationFilePath(f->getDeclarationFilePath());
      if (Value* ds = f->getDeclarationSymbol()) {
        auto symCopy = std::make_unique<Value>(*ds);
        symCopy->setType(subst);
        nf->setDeclarationSymbol(std::move(symCopy));
      }
      newFields.push_back(std::move(nf));
    }
    specPtr->replaceFields(std::move(newFields));
    if (classTemplate->is(BaseType::TY_ENUM)) {
      std::vector<std::unique_ptr<EnumVariant>> newVariants;
      for (EnumVariant* variant : classTemplate->getEnumVariants()) {
        if (variant == nullptr) {
          continue;
        }
        std::vector<Type*> payloadTypes;
        payloadTypes.reserve(variant->payloadTypes.size());
        for (Type* payload : variant->payloadTypes) {
          payloadTypes.push_back(substituteInType(payload, env));
        }
        auto nv = std::make_unique<EnumVariant>(variant->name, std::move(payloadTypes));
        nv->setDeclarationSpan(variant->getDeclarationSpan());
        nv->setDeclarationFilePath(variant->getDeclarationFilePath());
        newVariants.push_back(std::move(nv));
      }
      specPtr->replaceEnumVariants(std::move(newVariants));
    }
    {
      std::vector<std::unique_ptr<Field>> newStaticFields;
      for (Field* f : classTemplate->getStaticFields()) {
        Type* subst = substituteInType(f->type, env);
        auto nf = std::make_unique<Field>(f->name, subst);
        nf->setDeclarationSpan(f->getDeclarationSpan());
        nf->setDeclarationFilePath(f->getDeclarationFilePath());
        if (Value* ds = f->getDeclarationSymbol()) {
          auto symCopy = std::make_unique<Value>(*ds);
          symCopy->setType(subst);
          nf->setDeclarationSymbol(std::move(symCopy));
        }
        newStaticFields.push_back(std::move(nf));
      }
      specPtr->replaceStaticFields(std::move(newStaticFields));
    }
    specPtr->setDisplayName(makeSpecializedDisplayName(classTemplate, genericParamNames, env));
    if (classTemplate->is(BaseType::TY_CLASS) && classTemplate->getClassSuperclass() != nullptr) {
      specPtr->setClassSuperclass(substituteInType(classTemplate->getClassSuperclass(), env));
    } else if (classTemplate->is(BaseType::TY_CLASS)) {
      specPtr->setClassSuperclass(nullptr);
    }
    if (classTemplate->is(BaseType::TY_CLASS)) {
      specPtr->setClassVtableMethodOrder(
          std::vector<std::string>(classTemplate->getClassVtableMethodOrder()));
      specPtr->setClassHasDerivedClass(classTemplate->getClassHasDerivedClass());
    }
  }
}

auto Typechecker::getOrCreateSpecializedTraitExistentialType(
    Type* traitTemplate, const std::string& lookupName,
    const std::vector<std::string>& genericParamNames, const std::vector<Type*>& explicitTypeArgs)
    -> Type* {
  if (traitTemplate == nullptr || genericParamNames.size() != explicitTypeArgs.size()) {
    return traitTemplate;
  }
  std::ostringstream key;
  key << lookupName;
  for (Type* t : explicitTypeArgs) {
    key << "|" << (t != nullptr ? t->toString() : "?");
  }
  const std::string keyStr = key.str();
  if (auto it = specializedTraitExistentialTypes.find(keyStr);
      it != specializedTraitExistentialTypes.end()) {
    return it->second;
  }
  std::unordered_map<std::string, Type*> env;
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    env[genericParamNames[i]] = explicitTypeArgs[i];
  }
  auto u = std::make_unique<Type>(BaseType::TY_TRAIT_EXISTENTIAL, nullptr);
  u->setDisplayName(makeSpecializedDisplayName(traitTemplate, genericParamNames, env));
  Type* ptr = cacheType(std::move(u));
  specializedTraitExistentialTypes[keyStr] = ptr;
  specializedTraitExistentialEnv[ptr] = std::move(env);
  return ptr;
}

auto Typechecker::getExtendedType(Type* left, Type* right) -> Type* {
  if (left->isEqual(right)) {
    return left;
  }
  if (left->is(BaseType::TY_NULL) || right->is(BaseType::TY_NULL)) {
    return nullptr;
  }
  if (left->is(BaseType::TY_VOID)) {
    return right;
  }
  if (right->is(BaseType::TY_VOID)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    if (left->getIntWidth() > right->getIntWidth()) {
      return left;
    }
    if (right->getIntWidth() > left->getIntWidth()) {
      return right;
    }
    if (!left->isSigned()) {
      return left;
    }
    if (!right->isSigned()) {
      return right;
    }
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT32)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT32)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_FLOAT32)) {
    return left;
  }
  return nullptr;
}

auto Typechecker::getOptionalPayloadType(Type* type) -> Type* {
  auto payloadMembers = TypeUtils::computeOptionalPayloadMembers(type);
  if (!payloadMembers.has_value()) {
    return nullptr;
  }
  auto& canonical = payloadMembers->members;
  auto& displayName = payloadMembers->displayName;
  if (canonical.size() == 1U) {
    return canonical.front();
  }
  auto payload = std::make_unique<Type>(BaseType::TY_UNION);
  payload->setDisplayName(displayName);
  payload->setUnionMembers(std::move(canonical));
  return cacheType(std::move(payload));
}

auto Typechecker::isNullableType(Type* type) -> bool {
  return (type != nullptr && type->is(BaseType::TY_NULL)) ||
         getOptionalPayloadType(type) != nullptr;
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
    if (!unified->is(BaseType::TY_INT) && !unified->isFloatingPoint()) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        return overloadedType;
      }
      throw TypeCheckError(span, "Arithmetic operator requires numeric types");
    }
    return unified;
  case TokenType::AMPERSAND:
  case TokenType::PIPE:
  case TokenType::XOR:
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
    if (!unified->is(BaseType::TY_INT)) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        return overloadedType;
      }
      throw TypeCheckError(span, "Bitwise operator requires integer types");
    }
    return unified;
  case TokenType::SHIFT_LEFT:
  case TokenType::SHIFT_RIGHT:
    if (hasGeneric) {
      return leftTy != nullptr ? leftTy : rightTy;
    }
    if (leftTy == nullptr || rightTy == nullptr || !leftTy->is(BaseType::TY_INT) ||
        !rightTy->is(BaseType::TY_INT)) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        return overloadedType;
      }
      throw TypeCheckError(span, "Shift operator requires integer types");
    }
    return leftTy;
  case TokenType::EQUAL_EQUAL:
  case TokenType::BANG_EQUAL:
  case TokenType::GREATER:
  case TokenType::GREATER_EQUAL:
  case TokenType::LESS:
  case TokenType::LESS_EQUAL:
    if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
      return overloadedType;
    }
    if ((op == TokenType::EQUAL_EQUAL || op == TokenType::BANG_EQUAL) && leftTy != nullptr &&
        rightTy != nullptr &&
        ((leftTy->is(BaseType::TY_PTR) && rightTy->is(BaseType::TY_INT)) ||
         (leftTy->is(BaseType::TY_INT) && rightTy->is(BaseType::TY_PTR)))) {
      return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
    }
    if (leftTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) ||
        rightTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS})) {
      if (leftTy != rightTy) {
        throw TypeCheckError(span, "Comparison requires operands of the same enum or class type");
      }
    }
    if ((op == TokenType::EQUAL_EQUAL || op == TokenType::BANG_EQUAL) &&
        (isAssignableTo(leftTy, rightTy) || isAssignableTo(rightTy, leftTy))) {
      return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
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
  case TokenType::NULL_COALESCE: {
    Type* payloadType = getOptionalPayloadType(leftTy);
    if (payloadType == nullptr) {
      throw TypeCheckError(span, "Nil-coalescing requires an optional left operand, got {}",
                           leftTy != nullptr ? leftTy->toString() : "unknown");
    }
    if (rightTy == nullptr || !isAssignableTo(rightTy, payloadType) ||
        isLossyImplicitConversion(rightTy, payloadType)) {
      throw TypeCheckError(
          span, "Nil-coalescing default type {} does not match optional payload {}",
          rightTy != nullptr ? rightTy->toString() : "unknown", payloadType->toString());
    }
    return payloadType;
  }
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
  if (from->is(BaseType::TY_NULL)) {
    if (to->is(BaseType::TY_NULL)) {
      return true;
    }
    if (to->is(BaseType::TY_UNION)) {
      return std::ranges::any_of(to->getUnionMembers(), [](Type* m) -> bool {
        return m != nullptr && m->is(BaseType::TY_NULL);
      });
    }
    return false;
  }
  if (from->is(BaseType::TY_GENERIC) || to->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (to->is(BaseType::TY_ANY)) {
    return !from->is(BaseType::TY_VOID) && !isNullableType(from);
  }
  if (from->isEqual(to)) {
    return true;
  }
  if (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr && to->is(BaseType::TY_PTR) &&
      to->getElementType() != nullptr && from->getElementType()->is(BaseType::TY_CLASS) &&
      to->getElementType()->is(BaseType::TY_CLASS)) {
    Type* fromCls = from->getElementType();
    Type* toCls = to->getElementType();
    if (fromCls->isEqual(toCls)) {
      return true;
    }
    for (Type* t = fromCls; t != nullptr; t = t->getClassSuperclass()) {
      if (t->isEqual(toCls)) {
        return true;
      }
    }
  }
  if (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr &&
      from->getElementType()->is(BaseType::TY_CLASS) && to->is(BaseType::TY_CLASS)) {
    return from->getElementType()->isEqual(to);
  }
  if (to->is(BaseType::TY_PTR) && to->getElementType() != nullptr &&
      to->getElementType()->is(BaseType::TY_CLASS) && from->is(BaseType::TY_CLASS)) {
    return from->isEqual(to->getElementType());
  }
  if (to->is(BaseType::TY_PTR) && to->getElementType() != nullptr &&
      to->getElementType()->is(BaseType::TY_TRAIT_EXISTENTIAL) && from->is(BaseType::TY_PTR) &&
      from->getElementType() != nullptr && from->getElementType()->is(BaseType::TY_CLASS)) {
    return classDeclaresTrait(from->getElementType(),
                              traitExistentialBaseName(to->getElementType()->getDisplayName()));
  }
  // Void is only assignable to Void
  if (from->is(BaseType::TY_VOID)) {
    return to->is(BaseType::TY_VOID);
  }
  if (to->is(BaseType::TY_INT)) {
    return from->is(BaseType::TY_INT) || from->isFloatingPoint();
  }
  if (to->is(BaseType::TY_FLOAT)) {
    return from->is(BaseType::TY_INT) || from->isFloatingPoint();
  }
  if (to->is(BaseType::TY_FLOAT32)) {
    return from->is(BaseType::TY_INT) || from->is(BaseType::TY_FLOAT32);
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
      return classDeclaresTrait(cls, traitExistentialBaseName(want));
    }
    if (from->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
      return from->getDisplayName() == want;
    }
    return false;
  }
  if (from->is(BaseType::TY_TUPLE) && to->is(BaseType::TY_TUPLE)) {
    std::vector<Field*> const fromFields = from->getFields();
    std::vector<Field*> const toFields = to->getFields();
    if (fromFields.size() != toFields.size()) {
      return false;
    }
    for (size_t i = 0; i < fromFields.size(); ++i) {
      if (toFields[i]->type == nullptr) {
        continue;
      }
      if (fromFields[i]->type == nullptr ||
          !isAssignableTo(fromFields[i]->type, toFields[i]->type)) {
        return false;
      }
    }
    return true;
  }
  if (to->is(BaseType::TY_UNION)) {
    if (from->is(BaseType::TY_UNION)) {
      for (Type* fm : from->getUnionMembers()) {
        bool memberOk = false;
        for (Type* tm : to->getUnionMembers()) {
          if (isAssignableTo(fm, tm)) {
            memberOk = true;
            break;
          }
        }
        if (!memberOk) {
          return false;
        }
      }
      return true;
    }
    return std::ranges::any_of(to->getUnionMembers(),
                               [this, from](Type* m) -> bool { return isAssignableTo(from, m); });
  }
  if (from->is(BaseType::TY_ANY)) {
    return to->is(BaseType::TY_ANY);
  }
  return false;
}

auto Typechecker::functionTypesMatchForTraitImpl(Type* actualFn, Type* expectedFn) -> bool {
  if (actualFn == nullptr || expectedFn == nullptr || !actualFn->is(BaseType::TY_FUNCTION) ||
      !expectedFn->is(BaseType::TY_FUNCTION)) {
    return false;
  }
  if (!actualFn->functionGenericSignatureEqual(expectedFn)) {
    return false;
  }
  auto af = actualFn->getFields();
  auto ef = expectedFn->getFields();
  if (af.size() != ef.size()) {
    return false;
  }
  for (size_t i = 0; i < af.size(); ++i) {
    if (af[i]->type == nullptr || ef[i]->type == nullptr || !af[i]->type->isEqual(ef[i]->type)) {
      return false;
    }
  }
  Type* ar = actualFn->getReturnType();
  Type* er = expectedFn->getReturnType();
  if (ar == nullptr && er == nullptr) {
    return true;
  }
  if (ar == nullptr || er == nullptr) {
    return false;
  }
  if (er->is(BaseType::TY_PTR) && er->getElementType() != nullptr &&
      er->getElementType()->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return isAssignableTo(ar, er);
  }
  return ar->isEqual(er);
}

auto Typechecker::wrapReturnTypeIfNominal(Type* returnType) -> Type* {
  if (returnType == nullptr) {
    return nullptr;
  }
  if (returnType->is(BaseType::TY_PTR)) {
    return returnType;
  }
  if (TypeUtils::passesByPointerInAbi(returnType)) {
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, returnType));
  }
  return returnType;
}

auto Typechecker::resolveType(const TypeExpr* node) -> Type* {
  if (Type* nonCustom = tryResolveNonCustomTypeExpr(node)) {
    return nonCustom;
  }
  return resolveCustomTypeExpr(node);
}

Typechecker::Typechecker()
    : rootScope(std::make_unique<SymbolTable>(nullptr)), scope(rootScope.get()) {}

Typechecker::Typechecker(std::string mainFilePath, GetExportsFn getExports,
                         std::vector<AnalysisDiagnostic>* warningDiagnosticsOut,
                         std::shared_ptr<llvm::SourceMgr> diagnosticUnitSourceMgrIn,
                         unsigned diagnosticUnitBufferIdIn)
    : rootScope(std::make_unique<SymbolTable>(nullptr)), scope(rootScope.get()),
      mainFilePath(std::move(mainFilePath)), getExports(std::move(getExports)),
      warningDiagnostics(warningDiagnosticsOut),
      diagnosticUnitSourceMgr(std::move(diagnosticUnitSourceMgrIn)),
      diagnosticUnitBufferId(diagnosticUnitBufferIdIn) {}

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
  importType->setDeclarationFilePath(basePath.string());
  for (auto* sym : baseScope->getSymbols()) {
    if (!sym->isExported()) {
      continue;
    }
    const std::string& name = sym->getName();
    if (importedNameToSource.contains(name)) {
      continue;
    }
    // Nominal types need a TYPE_SYMBOL so `file.write` and `str` resolve as types; keep the
    // TY_IMPORT stub too so importedNameToSource / FuncCall logic still opens the defining scope.
    if (sym->getType()->isOneOf(
            {BaseType::TY_CLASS, BaseType::TY_ENUM, BaseType::TY_TRAIT_EXISTENTIAL})) {
      auto typeSym = std::make_unique<Value>(name, sym->getType());
      typeSym->setCategory(ValueCategory::TYPE_SYMBOL);
      typeSym->setDeclarationKind(sym->getDeclarationKind());
      typeSym->setExported(sym->isExported());
      typeSym->setGenericClassTemplate(sym->getGenericClassTemplate());
      typeSym->setDeclarationSpan(sym->getDeclarationSpan());
      typeSym->setDeclarationFilePath(sym->getDeclarationFilePath());
      scope->insertTypeRef(name, sym->getType());
      scope->insertSymbol(std::move(typeSym));
    }
    if (sym->getDeclarationKind() == ValueDeclarationKind::VARIABLE) {
      auto varSym = std::make_unique<Value>(name, sym->getType());
      varSym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      varSym->setDeclarationKind(ValueDeclarationKind::VARIABLE);
      varSym->setMutable(sym->getMutability());
      varSym->setDeclarationSpan(sym->getDeclarationSpan());
      varSym->setDeclarationFilePath(sym->getDeclarationFilePath());
      varSym->setExported(sym->isExported());
      scope->insertSymbol(std::move(varSym));
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

void Typechecker::insertImportedVariableAlias(const std::string& resolvedPath,
                                              const std::string& exportedName,
                                              const std::string& localName) {
  SymbolTable* importRoot = getOrTypecheckImport(resolvedPath);
  if (importRoot == nullptr) {
    return;
  }
  Value* vs = importRoot->lookup(exportedName);
  if (vs == nullptr || vs->getDeclarationKind() != ValueDeclarationKind::VARIABLE ||
      !vs->isExported()) {
    return;
  }
  auto clone = std::make_unique<Value>(localName, vs->getType());
  clone->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  clone->setDeclarationKind(ValueDeclarationKind::VARIABLE);
  clone->setMutable(vs->getMutability());
  clone->setDeclarationSpan(vs->getDeclarationSpan());
  clone->setDeclarationFilePath(vs->getDeclarationFilePath());
  clone->setExported(false);
  scope->insertSymbol(std::move(clone));
}

void Typechecker::validateParameterDefaultOrdering(llvm::SMRange span,
                                                   const std::vector<Parameter*>& params) {
  bool seenDefault = false;
  for (Parameter* param : params) {
    if (param->defaultVal != nullptr) {
      seenDefault = true;
    } else if (seenDefault) {
      throw TypeCheckError(span, "Parameters without default values must appear before parameters "
                                 "with default values");
    }
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
  const std::string normPath = normalizeResolvedFilesystemPath(absolutePath);
  auto it = importedModuleCache.find(normPath);
  if (it != importedModuleCache.end()) {
    return it->second != nullptr ? it->second->rootScope.get() : nullptr;
  }
  TypecheckImportActiveGuard const activeGuard(normPath);
  auto buffer = llvm::MemoryBuffer::getFile(normPath);
  if (!buffer) {
    return nullptr;
  }
  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  unsigned const bufferId = srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  auto lexer = std::make_unique<Lexer>(srcMgr);
  lexer->scanAll();
  auto parser = std::make_unique<Parser>(lexer->getTokens(), nullptr, srcMgr, bufferId, normPath);
  parser->parse();
  Compound* ast = parser->getAst();
  if (ast == nullptr) {
    return nullptr;
  }
  Typechecker sub(normPath, getExports, warningDiagnostics, srcMgr, bufferId);
  sub.run(ast);
  auto imported = std::make_shared<ImportedModuleAnalysis>();
  imported->sourceMgr = std::move(srcMgr);
  imported->mainBufferId = bufferId;
  imported->mainFilePath = normPath;
  imported->parser = std::move(parser);
  imported->typeCache = sub.takeTypeCache();
  imported->rootScope = sub.takeRootScope();
  {
    auto subEnv = sub.takeSpecializedTypeEnv();
    auto subTemplateOf = sub.takeSpecializedTypeToTemplate();
    auto subClassTypes = sub.takeSpecializedClassTypes();
    for (auto& [ty, env] : subEnv) {
      if (ty->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
        specializedTraitExistentialEnv[ty] = std::move(env);
      } else {
        specializedTypeEnv[ty] = std::move(env);
      }
    }
    for (const auto& [specPtr, tmplPtr] : subTemplateOf) {
      specializedTypeToTemplate[specPtr] = tmplPtr;
    }
    for (const auto& kv : subClassTypes) {
      specializedClassTypes[kv.first] = kv.second;
    }
  }
  imported->index =
      buildAnalysisIndex(imported->parser != nullptr ? imported->parser->getAst() : nullptr,
                         imported->sourceMgr.get(), imported->mainBufferId,
                         imported->mainFilePath);
  imported->importAliasToPath = sub.takeImportAliasToPath();
  imported->importedNameToSource = sub.takeImportedNameToSource();
  imported->importedModules = sub.takeImportedModules();
  SymbolTable* scopePtr = imported->rootScope.get();
  importedModuleCache[normPath] = std::move(imported);
  return scopePtr;
}

auto Typechecker::run(const Compound* ast) -> void {
  const auto currentPath =
      mainFilePath.empty()
          ? std::filesystem::path()
          : std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal();
  const auto basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  std::error_code ec;
  const bool mainIsStdlibEntry =
      !mainFilePath.empty() && std::filesystem::equivalent(currentPath, basePath, ec);
  if (!mainIsStdlibEntry || mainFilePath.empty()) {
    if (mainFilePath.empty() || !std::filesystem::equivalent(currentPath, basePath, ec)) {
      loadImplicitStdModule("base.les");
    }
    if (mainFilePath.empty() || !std::filesystem::equivalent(currentPath, basePath, ec)) {
      registerTraitsFromImportedModule(basePath.string());
    }
  }
  classesPendingUnusedMemberDiagnosis.clear();
  declarationPass = true;
  ast->accept(*this);
  declarationPass = false;
  ast->accept(*this);
  if (warningDiagnostics != nullptr && rootScope != nullptr) {
    for (const Class* classNode : classesPendingUnusedMemberDiagnosis) {
      diagnoseUnusedNonExportedClassMembers(classNode);
    }
    classesPendingUnusedMemberDiagnosis.clear();
    checkUnusedBindingsInScope(rootScope.get());
  }
}

auto Typechecker::takeRootScope() -> std::unique_ptr<SymbolTable> {
  scope = nullptr;
  return std::move(rootScope);
}

auto Typechecker::takeTypeCache() -> std::vector<std::unique_ptr<Type>> {
  for (auto& [path, mod] : importedModuleCache) {
    (void) path;
    mergeImportedAnalysisTypeCachesInto(typeCache, mod);
  }
  if (rootScope != nullptr) {
    rootScope->releaseOwnedTypesInto(typeCache);
  }
  return std::move(typeCache);
}

auto Typechecker::takeSpecializedTypeEnv()
    -> std::unordered_map<Type*, std::unordered_map<std::string, Type*>> {
  auto result = std::move(specializedTypeEnv);
  for (auto& [ty, env] : specializedTraitExistentialEnv) {
    result.insert_or_assign(ty, std::move(env));
  }
  specializedTraitExistentialEnv.clear();
  return result;
}

auto Typechecker::takeSpecializedTypeToTemplate() -> std::unordered_map<Type*, Type*> {
  return std::move(specializedTypeToTemplate);
}

auto Typechecker::takeSpecializedClassTypes() -> std::unordered_map<std::string, Type*> {
  return std::move(specializedClassTypes);
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
  bool precededByTerminator = false;
  for (Statement* elem : node->getChildren()) {
    if (precededByTerminator) {
      emitWarning(elem->getSpan(), "Unreachable code");
    }
    try {
      elem->accept(*this);
    } catch (const TypeCheckError& err) {
      recoverFromTypeError(err);
    }
    precededByTerminator = isControlFlowTerminator(elem);
  }
}

void Typechecker::appendSemanticDiagnostic(llvm::SMRange span, std::string message,
                                           AnalysisDiagnosticSeverity severity) {
  if (warningDiagnostics == nullptr) {
    return;
  }
  warningDiagnostics->push_back(AnalysisDiagnostic{.message = std::move(message),
                                                   .span = span,
                                                   .severity = severity,
                                                   .spanSourceMgr = diagnosticUnitSourceMgr,
                                                   .spanBufferId = diagnosticUnitBufferId,
                                                   .spanDisplayPath = mainFilePath});
}

void Typechecker::emitWarning(llvm::SMRange span, std::string message) {
  if (warningDiagnostics == nullptr || !span.isValid()) {
    return;
  }
  if (!mainFilePath.empty() && isStdlibSourcePath(mainFilePath)) {
    return;
  }
  appendSemanticDiagnostic(span, std::move(message), AnalysisDiagnosticSeverity::Warning);
}

void Typechecker::recoverFromTypeError(const TypeCheckError& err) {
  if (warningDiagnostics == nullptr) {
    throw err;
  }
  if (!mainFilePath.empty() && isStdlibSourcePath(mainFilePath)) {
    throw TypeCheckError(err.getSpan(), std::string(err.what()));
  }
  llvm::SMRange const span = err.getSpan().isValid() ? err.getSpan() : llvm::SMRange();
  std::string const message = std::string(err.what());
  for (const AnalysisDiagnostic& existing : *warningDiagnostics) {
    if (existing.severity == AnalysisDiagnosticSeverity::Error && existing.message == message &&
        existing.span.Start.getPointer() == span.Start.getPointer() &&
        existing.span.End.getPointer() == span.End.getPointer() &&
        existing.spanSourceMgr.get() == diagnosticUnitSourceMgr.get() &&
        existing.spanBufferId == diagnosticUnitBufferId) {
      return;
    }
  }
  appendSemanticDiagnostic(span, message, AnalysisDiagnosticSeverity::Error);
}

void Typechecker::markValueRead(Value* sym) {
  if (sym == nullptr || declarationPass) {
    return;
  }
  const std::string& n = sym->getName();
  if (!n.empty() && n[0] == '_') {
    return;
  }
  if (n == "self") {
    return;
  }
  const auto kind = sym->getDeclarationKind();
  if (kind == ValueDeclarationKind::VARIABLE || kind == ValueDeclarationKind::PARAMETER ||
      kind == ValueDeclarationKind::PROPERTY) {
    sym->setUsed(true);
    return;
  }
  if (kind == ValueDeclarationKind::METHOD || kind == ValueDeclarationKind::FUNCTION) {
    sym->setUsed(true);
    return;
  }
  if (sym->getCategory() == ValueCategory::MODULE_SYMBOL && sym->getType() != nullptr &&
      sym->getType()->is(BaseType::TY_IMPORT)) {
    sym->setUsed(true);
  }
}

void Typechecker::markImportNameStubUsedForQualifiedAccess(const std::string& modulePath,
                                                           const std::string& exportedName) {
  if (declarationPass) {
    return;
  }
  for (const auto& [localName, src] : importedNameToSource) {
    if (src.first != modulePath || src.second != exportedName) {
      continue;
    }
    Value* stub = scope->lookupImportModuleSymbol(localName);
    if (stub != nullptr) {
      markValueRead(stub);
    }
    return;
  }
}

void Typechecker::checkUnusedBindingsInScope(SymbolTable* blockScope) {
  if (warningDiagnostics == nullptr || blockScope == nullptr || declarationPass) {
    return;
  }
  // Declaration + definition passes each insert a symbol for the same VarDecl, so the multimap
  // can hold duplicate names; emit at most one unused diagnostic per declaration span per scope
  // (same span => same VarDecl), while still warning for distinct bindings that share a name.
  std::unordered_set<std::string> variableNameReadInScope;
  for (Value* u : blockScope->getSymbols()) {
    if (u->getDeclarationKind() == ValueDeclarationKind::VARIABLE && u->isUsed()) {
      variableNameReadInScope.insert(u->getName());
    }
  }
  struct UnusedVarDeclSpanHash {
    auto operator()(llvm::SMRange s) const noexcept -> std::size_t {
      const void* a = s.Start.getPointer();
      const void* b = s.End.getPointer();
      const std::size_t ha = std::hash<const void*>{}(a);
      const std::size_t hb = std::hash<const void*>{}(b);
      return ha ^ (hb + 0x9e3779b9U + (ha << 6U) + (ha >> 2U));
    }
  };
  struct UnusedVarDeclSpanEq {
    auto operator()(llvm::SMRange lhs, llvm::SMRange rhs) const noexcept -> bool {
      return lhs.Start.getPointer() == rhs.Start.getPointer() &&
             lhs.End.getPointer() == rhs.End.getPointer();
    }
  };
  std::unordered_set<llvm::SMRange, UnusedVarDeclSpanHash, UnusedVarDeclSpanEq>
      warnedUnusedVariableDecl;
  std::unordered_set<std::string> warnedUnusedParameter;
  std::unordered_set<std::string> warnedUnusedImport;
  for (Value* v : blockScope->getSymbols()) {
    const std::string& n = v->getName();
    if (!n.empty() && n[0] == '_') {
      continue;
    }
    if (n == "self") {
      continue;
    }
    if (v->isExported()) {
      continue;
    }
    if (v->getDeclarationKind() == ValueDeclarationKind::VARIABLE && !v->isUsed()) {
      if (variableNameReadInScope.contains(n)) {
        continue;
      }
      llvm::SMRange const declSpan = v->getDeclarationSpan();
      if (warnedUnusedVariableDecl.insert(declSpan).second) {
        emitWarning(declSpan, "Unused variable '" + n + "'");
      }
      continue;
    }
    if (v->getDeclarationKind() == ValueDeclarationKind::PARAMETER && !v->isUsed()) {
      if (warnedUnusedParameter.insert(n).second) {
        emitWarning(v->getDeclarationSpan(), "Unused parameter '" + n + "'");
      }
      continue;
    }
    if (v->getCategory() == ValueCategory::MODULE_SYMBOL && v->getType() != nullptr &&
        v->getType()->is(BaseType::TY_IMPORT) && !v->isUsed()) {
      if (warnedUnusedImport.insert(n).second) {
        emitWarning(v->getDeclarationSpan(), "Unused import '" + n + "'");
      }
    }
  }
}

void Typechecker::warnShadowingFromEnclosing(const std::string& name, llvm::SMRange span) {
  if (warningDiagnostics == nullptr || declarationPass || scope == nullptr ||
      scope->getParent() == nullptr || !span.isValid()) {
    return;
  }
  Value* outer = scope->getParent()->lookup(name);
  if (outer == nullptr) {
    return;
  }
  if (outer->getCategory() == ValueCategory::MODULE_SYMBOL && outer->getType() != nullptr &&
      outer->getType()->is(BaseType::TY_IMPORT)) {
    emitWarning(span, "Declaration of '" + name + "' shadows an import");
    return;
  }
  emitWarning(span, "Declaration of '" + name + "' shadows an outer binding");
}

auto Typechecker::tryGetLiteralBool(const Expression* e, bool& outValue) -> bool {
  const auto* lit = dynamic_cast<const Literal*>(e);
  if (lit == nullptr) {
    return false;
  }
  switch (lit->getType()) {
  case TokenType::TRUE_:
    outValue = true;
    return true;
  case TokenType::FALSE_:
    outValue = false;
    return true;
  case TokenType::BOOL: {
    const std::string& v = lit->getValue();
    if (v == "true") {
      outValue = true;
      return true;
    }
    if (v == "false") {
      outValue = false;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

void Typechecker::warnIfTrivialBoolCondition(const Expression* cond) {
  if (warningDiagnostics == nullptr || declarationPass || cond == nullptr) {
    return;
  }
  if (dynamic_cast<const Else*>(cond) != nullptr) {
    return;
  }
  bool v = false;
  if (!tryGetLiteralBool(cond, v)) {
    return;
  }
  emitWarning(cond->getSpan(), v ? "Condition is always true" : "Condition is always false");
}

void Typechecker::warnIfEmptyCompoundBody(const Compound* block, const char* context) {
  if (warningDiagnostics == nullptr || declarationPass || block == nullptr) {
    return;
  }
  if (!block->getChildren().empty()) {
    return;
  }
  emitWarning(block->getSpan(), std::string("Empty ") + context);
}

auto Typechecker::isLossyImplicitConversion(Type* from, Type* to) -> bool {
  if (from == nullptr || to == nullptr) {
    return false;
  }
  if (from->is(BaseType::TY_GENERIC) || to->is(BaseType::TY_GENERIC)) {
    return false;
  }
  if (from->isEqual(to)) {
    return false;
  }
  if (to->is(BaseType::TY_FLOAT) || to->is(BaseType::TY_FLOAT32)) {
    return from->is(BaseType::TY_INT) ||
           (to->is(BaseType::TY_FLOAT32) && from->is(BaseType::TY_FLOAT));
  }
  if (to->is(BaseType::TY_INT)) {
    return from->isFloatingPoint();
  }
  return false;
}

void Typechecker::warnIfLossyConversion(llvm::SMRange span, Type* from, Type* to) {
  if (warningDiagnostics == nullptr || declarationPass || from == nullptr || to == nullptr ||
      !span.isValid()) {
    return;
  }
  if (!isAssignableTo(from, to) || !isLossyImplicitConversion(from, to)) {
    return;
  }
  emitWarning(span, fmt::format("Implicit conversion from {} to {} may lose precision",
                                from->toString(), to->toString()));
}

void Typechecker::diagnoseUnusedNonExportedClassMembers(const Class* classNode) {
  if (warningDiagnostics == nullptr || declarationPass || classNode == nullptr) {
    return;
  }
  for (VarDecl* field : classNode->getFields()) {
    if (field->isExported()) {
      continue;
    }
    Value* vs = field->getResolvedSymbol();
    if (vs != nullptr && !vs->isUsed()) {
      emitWarning(vs->getDeclarationSpan(),
                  "Unused non-exported class field '" + vs->getName() + "'");
    }
  }
  for (FuncDecl* method : classNode->getMethods()) {
    if (method->isExported() || classNode->isExported()) {
      continue;
    }
    if (method->getName() == "new" && classNode->getBaseType() != nullptr) {
      // Calls may bind to specialized `new` overloads; the template symbol stays unmarked.
      continue;
    }
    Value* vs = method->getResolvedSymbol();
    if (vs != nullptr && !vs->isUsed()) {
      emitWarning(method->getNameSpan(),
                  "Unused non-exported class method '" + method->getName() + "'");
    }
  }
}

auto Typechecker::visit(const VarDecl* node) -> void {
  std::vector<Literal*> const names = node->getVarLiterals();
  if (node->isExported() && names.size() > 1U) {
    throw TypeCheckError(node->getSpan(), "Cannot export destructuring declarations");
  }
  if (names.size() > 1U) {
    Type* declTupleType = nullptr;
    if (node->getType() != nullptr) {
      node->getType()->accept(*this);
      declTupleType = result->getType();
      if (declTupleType == nullptr || !declTupleType->is(BaseType::TY_TUPLE)) {
        throw TypeCheckError(
            node->getSpan(),
            "Destructuring declaration requires a tuple type or tuple initializer");
      }
      std::vector<Field*> const fields = declTupleType->getFields();
      if (fields.size() != names.size()) {
        throw TypeCheckError(node->getSpan(), "Tuple type has {} elements but {} names were given",
                             fields.size(), names.size());
      }
    }
    if (node->getValue() == nullptr) {
      throw TypeCheckError(node->getSpan(), "Destructuring declaration requires an initializer");
    }
    visitExprWithExpectedType(node->getValue(), declTupleType);
    Type* initType = result->getType();
    if (initType == nullptr || !initType->is(BaseType::TY_TUPLE)) {
      throw TypeCheckError(node->getSpan(),
                           "Destructuring requires a tuple on the right-hand side, got {}",
                           initType != nullptr ? initType->toString() : "unknown");
    }
    if (declTupleType != nullptr && !initType->isEqual(declTupleType)) {
      throw TypeCheckError(node->getSpan(),
                           "Initializer type {} does not match declared tuple type {}",
                           initType->toString(), declTupleType->toString());
    }
    std::vector<Field*> const rhsFields = initType->getFields();
    if (rhsFields.size() != names.size()) {
      throw TypeCheckError(node->getSpan(), "Tuple has {} elements but {} names were given",
                           rhsFields.size(), names.size());
    }
    std::vector<Value*> resolved;
    resolved.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      Type* elemTy = rhsFields[i]->type;
      auto symbol = std::make_unique<Value>(names[i]->getValue(), elemTy, SymbolState::INITIALIZED);
      symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      symbol->setDeclarationKind(ValueDeclarationKind::VARIABLE);
      symbol->setMutable(node->getMutability());
      symbol->setExported(node->isExported());
      symbol->setDeclarationSpan(names[i]->getSpan());
      symbol->setDeclarationFilePath(mainFilePath);
      resolved.push_back(symbol.get());
      warnShadowingFromEnclosing(names[i]->getValue(), names[i]->getSpan());
      scope->insertSymbol(std::move(symbol));
    }
    node->setResolvedSymbols(std::move(resolved));
    return;
  }

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
  symbol->setExported(node->isExported());
  symbol->setDeclarationSpan(node->getIdentifier()->getSpan());
  symbol->setDeclarationFilePath(mainFilePath);
  if (declType != nullptr && declType->is(BaseType::TY_FUNCTION)) {
    symbol->setStoresFuncValuePair(true);
  }
  if (auto* lam = dynamic_cast<LambdaExpr*>(node->getValue())) {
    symbol->setOriginLambdaExpr(lam);
  }
  node->setResolvedSymbol(symbol.get());
  warnShadowingFromEnclosing(node->getIdentifier()->getValue(), node->getIdentifier()->getSpan());
  scope->insertSymbol(std::move(symbol));
}

auto Typechecker::isSupportedUnionMemberType(Type* t) -> bool {
  if (t == nullptr) {
    return false;
  }
  return t->is(BaseType::TY_INT) || t->isFloatingPoint() || t->is(BaseType::TY_BOOL) ||
         t->is(BaseType::TY_CLASS) || t->is(BaseType::TY_ENUM) || t->is(BaseType::TY_ANY) ||
         t->is(BaseType::TY_NULL);
}

auto Typechecker::lookupUnionNarrowedType(Value* sym) const -> Type* {
  if (sym == nullptr) {
    return nullptr;
  }
  const UnionNarrowingStableKey key = unionNarrowingStableKeyForSymbol(sym);
  if (!key.declarationSpan.isValid() && key.fallbackAnchor == nullptr) {
    return nullptr;
  }
  for (auto it = unionNarrowingStack.rbegin(); it != unionNarrowingStack.rend(); ++it) {
    auto j = it->find(key);
    if (j != it->end()) {
      return j->second;
    }
  }
  return nullptr;
}

auto Typechecker::lookupUnionNarrowedType(const Expression* expr) const -> Type* {
  auto key = tryGetUnionNarrowingKey(expr);
  if (!key.has_value() || (!key->declarationSpan.isValid() && key->fallbackAnchor == nullptr)) {
    return nullptr;
  }
  for (auto it = unionNarrowingStack.rbegin(); it != unionNarrowingStack.rend(); ++it) {
    if (auto jt = it->find(*key); jt != it->end()) {
      return jt->second;
    }
  }
  return nullptr;
}

void Typechecker::invalidateUnionNarrowingForSymbol(Value* sym) {
  if (sym == nullptr) {
    return;
  }
  const UnionNarrowingStableKey rootKey = unionNarrowingStableRootKeyForSymbol(sym);
  if (!rootKey.declarationSpan.isValid() && rootKey.fallbackAnchor == nullptr) {
    return;
  }
  for (auto& frame : unionNarrowingStack) {
    for (auto it = frame.begin(); it != frame.end();) {
      if (unionNarrowingSameRoot(it->first, rootKey)) {
        it = frame.erase(it);
      } else {
        ++it;
      }
    }
  }
}

auto Typechecker::isStableSubscriptNarrowingIndex(const Expression* expr) const -> bool {
  auto const* lit = dynamic_cast<const Literal*>(expr);
  if (lit == nullptr) {
    return false;
  }
  return lit->getType() != TokenType::IDENTIFIER;
}

auto Typechecker::rootStorageSymbolForAssignmentLhs(Expression* lhs) -> Value* {
  if (lhs == nullptr) {
    return nullptr;
  }
  if (auto const* lit = dynamic_cast<Literal*>(lhs)) {
    if (lit->getType() != TokenType::IDENTIFIER) {
      return nullptr;
    }
    if (Value* rs = lit->getResolvedSymbol()) {
      return rs;
    }
    return scope->lookup(lit->getValue());
  }
  if (auto* dot = dynamic_cast<DotOp*>(lhs)) {
    return rootStorageSymbolForAssignmentLhs(dot->getLeft());
  }
  if (auto* sub = dynamic_cast<SubscriptOp*>(lhs)) {
    return rootStorageSymbolForAssignmentLhs(sub->getLeft());
  }
  return nullptr;
}

auto Typechecker::assignmentStorageTypeForDotLhs(const DotOp* lhs, Type* fallbackType) -> Type* {
  if (lhs == nullptr) {
    return fallbackType;
  }
  auto* rightLit = dynamic_cast<Literal*>(lhs->getRight());
  if (rightLit == nullptr || rightLit->getType() != TokenType::IDENTIFIER) {
    return fallbackType;
  }
  if (Value* resolved = rightLit->getResolvedSymbol()) {
    return resolved->getType();
  }

  lhs->getLeft()->accept(*this);
  bool dotLeftDenotesTypeName =
      result != nullptr && result->getCategory() == ValueCategory::TYPE_SYMBOL;
  Type* base = result != nullptr ? result->getType() : nullptr;
  tryPeelDotReceiverFromNamedImportStub(lhs, base, dotLeftDenotesTypeName);
  if (base == nullptr) {
    return fallbackType;
  }
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    return fallbackType;
  }

  Field* staticPart = nullptr;
  if (base->is(BaseType::TY_CLASS) && dotLeftDenotesTypeName) {
    staticPart = TypeUtils::findStaticFieldInClass(base, rightLit->getValue());
  }
  Field* field =
      staticPart != nullptr ? staticPart : TypeUtils::findFieldInFields(base, rightLit->getValue());
  return field != nullptr && field->type != nullptr ? field->type : fallbackType;
}

namespace {

[[nodiscard]] auto normalizeUnionNarrowingIntegerLiteral(const std::string& value) -> std::string {
  try {
    return fmt::format("{}", std::stoll(value));
  } catch (...) {
    return value;
  }
}

[[nodiscard]] auto normalizeUnionNarrowingFloatLiteral(const std::string& value) -> std::string {
  try {
    return fmt::format("{:.17g}", std::stod(value));
  } catch (...) {
    return value;
  }
}

[[nodiscard]] auto appendUnionNarrowingLiteralKey(std::string& out, const Literal* lit) -> bool {
  if (lit == nullptr) {
    return false;
  }
  switch (lit->getType()) {
  case TokenType::IDENTIFIER:
    out += "$" + lit->getValue();
    return true;
  case TokenType::INTEGER:
    out += "#i:" + normalizeUnionNarrowingIntegerLiteral(lit->getValue());
    return true;
  case TokenType::DOUBLE:
    out += "#d:" + normalizeUnionNarrowingFloatLiteral(lit->getValue());
    return true;
  case TokenType::STRING:
    out += fmt::format("#s:{}:{}", lit->getValue().size(), lit->getValue());
    return true;
  case TokenType::BOOL:
  case TokenType::TRUE_:
    out += "#b:true";
    return true;
  case TokenType::FALSE_:
    out += "#b:false";
    return true;
  case TokenType::NIL:
    out += "#n:nil";
    return true;
  default:
    return false;
  }
}

} // namespace

auto Typechecker::appendUnionNarrowingExprKey(std::string& out, const Expression* expr) const
    -> bool {
  if (expr == nullptr) {
    return false;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    return appendUnionNarrowingLiteralKey(out, lit);
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    auto const* right = dynamic_cast<const Literal*>(dot->getRight());
    if (right == nullptr || right->getType() != TokenType::IDENTIFIER ||
        !appendUnionNarrowingExprKey(out, dot->getLeft())) {
      return false;
    }
    out += "." + right->getValue();
    return true;
  }
  if (auto const* sub = dynamic_cast<const SubscriptOp*>(expr)) {
    if (!appendUnionNarrowingExprKey(out, sub->getLeft())) {
      return false;
    }
    std::string indexKey;
    if (!appendUnionNarrowingExprKey(indexKey, sub->getIndex())) {
      return false;
    }
    out += "[" + indexKey + "]";
    return true;
  }
  return false;
}

auto Typechecker::tryGetUnionNarrowingKey(const Expression* expr) const
    -> std::optional<UnionNarrowingStableKey> {
  if (expr == nullptr) {
    return std::nullopt;
  }
  if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
    if (lit->getType() != TokenType::IDENTIFIER) {
      return std::nullopt;
    }
    Value* root = lit->getResolvedSymbol();
    if (root == nullptr) {
      root = scope->lookup(lit->getValue());
    }
    if (root == nullptr) {
      return std::nullopt;
    }
    return unionNarrowingStableKeyForSymbol(root);
  }
  if (auto const* dot = dynamic_cast<const DotOp*>(expr)) {
    auto key = tryGetUnionNarrowingKey(dot->getLeft());
    auto const* right = dynamic_cast<const Literal*>(dot->getRight());
    if (!key.has_value() || right == nullptr || right->getType() != TokenType::IDENTIFIER) {
      return std::nullopt;
    }
    key->accessPath += "." + right->getValue();
    return key;
  }
  if (auto const* sub = dynamic_cast<const SubscriptOp*>(expr)) {
    auto key = tryGetUnionNarrowingKey(sub->getLeft());
    if (!key.has_value()) {
      return std::nullopt;
    }
    if (!isStableSubscriptNarrowingIndex(sub->getIndex())) {
      return std::nullopt;
    }
    std::string indexKey;
    if (!appendUnionNarrowingExprKey(indexKey, sub->getIndex())) {
      return std::nullopt;
    }
    key->accessPath += "[" + indexKey + "]";
    return key;
  }
  return std::nullopt;
}

auto Typechecker::tryGetIsOpUnionNarrowingKey(const IsOp* is) const
    -> std::optional<UnionNarrowingStableKey> {
  if (is == nullptr) {
    return std::nullopt;
  }
  return tryGetUnionNarrowingKey(is->getLeft());
}

auto Typechecker::narrowUnionByExcludingMembers(Type* unionTy, const std::vector<Type*>& toExclude)
    -> Type* {
  if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
    return nullptr;
  }
  std::vector<Type*> remainder = unionTy->getUnionMembers();
  unsigned removed = 0U;
  for (Type* ex : toExclude) {
    if (ex == nullptr) {
      continue;
    }
    auto it =
        std::ranges::find_if(remainder, [ex](Type* m) { return m != nullptr && m->isEqual(ex); });
    if (it != remainder.end()) {
      remainder.erase(it);
      ++removed;
    }
  }
  if (removed == 0U || remainder.empty()) {
    return nullptr;
  }
  auto [canonical, displayName] = TypeUtils::canonicalizeUnionMembers(std::move(remainder));
  if (canonical.size() == 1U) {
    return canonical.front();
  }
  if (canonical.empty()) {
    return nullptr;
  }
  auto u = std::make_unique<Type>(BaseType::TY_UNION);
  u->setDisplayName(displayName);
  u->setUnionMembers(std::move(canonical));
  u->setDeclarationSpan(unionTy->getDeclarationSpan());
  return cacheType(std::move(u));
}

auto Typechecker::rhsTypeIsUnionMember(Type* unionTy, Type* rhs) -> bool {
  if (rhs == nullptr || unionTy == nullptr) {
    return false;
  }
  return std::ranges::any_of(unionTy->getUnionMembers(),
                             [rhs](Type* m) -> bool { return m->isEqual(rhs); });
}

void Typechecker::appendExcludedTypesFromPriorIsArms(const If* node, unsigned blockIndex,
                                                     const UnionNarrowingStableKey& key,
                                                     Type* unionTy, std::vector<Type*>& excluded) {
  for (unsigned i = 0; i < blockIndex; ++i) {
    const auto* isPrev = dynamic_cast<const IsOp*>(node->getConds()[i]);
    if (isPrev == nullptr || isPrev->getOperator() != TokenType::IS) {
      continue;
    }
    auto prevKey = tryGetIsOpUnionNarrowingKey(isPrev);
    if (!prevKey.has_value() || *prevKey != key) {
      continue;
    }
    Type* rhsPrev = nullptr;
    try {
      rhsPrev = resolveType(isPrev->getRight());
    } catch (const TypeCheckError&) {
      continue;
    }
    if (rhsTypeIsUnionMember(unionTy, rhsPrev)) {
      excluded.push_back(rhsPrev);
    }
  }
}

auto Typechecker::fillUnionNarrowingForIfBlock(
    const If* node, unsigned blockIndex,
    std::unordered_map<UnionNarrowingStableKey, Type*, UnionNarrowingStableKeyHash,
                       UnionNarrowingStableKeyEq>& out) -> void {
  if (blockIndex >= node->getBlocks().size()) {
    return;
  }
  Expression* cond = node->getConds()[blockIndex];
  if (dynamic_cast<const Else*>(cond) != nullptr) {
    if (blockIndex == 0) {
      return;
    }
    const auto* is0 = dynamic_cast<const IsOp*>(node->getConds()[0]);
    if (is0 == nullptr) {
      return;
    }
    auto key = tryGetIsOpUnionNarrowingKey(is0);
    if (!key.has_value()) {
      return;
    }
    is0->getLeft()->accept(*this);
    Type* unionTy = result != nullptr ? result->getType() : nullptr;
    if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
      return;
    }
    std::vector<Type*> excluded;
    appendExcludedTypesFromPriorIsArms(node, blockIndex, *key, unionTy, excluded);
    if (!excluded.empty()) {
      Type* narrowed = narrowUnionByExcludingMembers(unionTy, excluded);
      if (narrowed != nullptr) {
        if (key->declarationSpan.isValid() || key->fallbackAnchor != nullptr) {
          out[*key] = narrowed;
        }
      }
      return;
    }
    Type* rhsTy = nullptr;
    try {
      rhsTy = resolveType(is0->getRight());
    } catch (const TypeCheckError&) {
      return;
    }
    if (is0->getOperator() == TokenType::IS_NOT && rhsTypeIsUnionMember(unionTy, rhsTy)) {
      if (key->declarationSpan.isValid() || key->fallbackAnchor != nullptr) {
        out[*key] = rhsTy;
      }
    }
    return;
  }
  const auto* is = dynamic_cast<const IsOp*>(cond);
  if (is == nullptr) {
    return;
  }
  auto key = tryGetIsOpUnionNarrowingKey(is);
  if (!key.has_value()) {
    return;
  }
  is->getLeft()->accept(*this);
  Type* unionTy = result != nullptr ? result->getType() : nullptr;
  if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
    return;
  }
  Type* rhsTy = nullptr;
  try {
    rhsTy = resolveType(is->getRight());
  } catch (const TypeCheckError&) {
    return;
  }
  if (is->getOperator() == TokenType::IS) {
    if (rhsTypeIsUnionMember(unionTy, rhsTy)) {
      if (key->declarationSpan.isValid() || key->fallbackAnchor != nullptr) {
        out[*key] = rhsTy;
      }
    }
  } else if (is->getOperator() == TokenType::IS_NOT) {
    std::vector<Type*> excluded;
    appendExcludedTypesFromPriorIsArms(node, blockIndex, *key, unionTy, excluded);
    if (rhsTypeIsUnionMember(unionTy, rhsTy)) {
      excluded.push_back(rhsTy);
    }
    if (!excluded.empty()) {
      Type* narrowed = narrowUnionByExcludingMembers(unionTy, excluded);
      if (narrowed != nullptr) {
        if (key->declarationSpan.isValid() || key->fallbackAnchor != nullptr) {
          out[*key] = narrowed;
        }
      }
    }
  }
}

auto Typechecker::visit(const If* node) -> void {
  for (Expression* cond : node->getConds()) {
    try {
      cond->accept(*this);
      if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
          !result->getType()->is(BaseType::TY_GENERIC)) {
        throw TypeCheckError(cond->getSpan(), "Condition must be Bool, got {}",
                             result->getType()->toString());
      }
      warnIfTrivialBoolCondition(cond);
    } catch (const TypeCheckError& err) {
      recoverFromTypeError(err);
    }
  }
  std::vector<Compound*> const blocks = node->getBlocks();
  for (unsigned bi = 0; bi < blocks.size(); ++bi) {
    std::unordered_map<UnionNarrowingStableKey, Type*, UnionNarrowingStableKeyHash,
                       UnionNarrowingStableKeyEq>
        narrowMap;
    fillUnionNarrowingForIfBlock(node, bi, narrowMap);
    bool const pushedNarrowing = !narrowMap.empty();
    if (pushedNarrowing) {
      unionNarrowingStack.push_back(std::move(narrowMap));
    }
    try {
      warnIfEmptyCompoundBody(blocks[bi], "if/else branch");
      blocks[bi]->accept(*this);
    } catch (const TypeCheckError& err) {
      recoverFromTypeError(err);
    }
    if (pushedNarrowing) {
      unionNarrowingStack.pop_back();
    }
  }
}

auto Typechecker::visit(const While* node) -> void {
  try {
    node->getCond()->accept(*this);
    if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
        !result->getType()->is(BaseType::TY_GENERIC)) {
      throw TypeCheckError(node->getCond()->getSpan(), "Condition must be Bool, got {}",
                           result->getType()->toString());
    }
    warnIfTrivialBoolCondition(node->getCond());
  } catch (const TypeCheckError& err) {
    recoverFromTypeError(err);
  }
  try {
    warnIfEmptyCompoundBody(node->getBlock(), "while body");
    node->getBlock()->accept(*this);
  } catch (const TypeCheckError& err) {
    recoverFromTypeError(err);
  }
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
  auto getBufferIterableArrayFieldType = [this, peelPtr](Type* type) -> Type* {
    Type* t = peelPtr(type);
    if (t == nullptr || !t->is(BaseType::TY_CLASS) || !classDeclaresTrait(t, "Iterable")) {
      return nullptr;
    }
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
  } else if (Type* bufferType = getBufferIterableArrayFieldType(iterableType);
             bufferType != nullptr && bufferType->getElementType() != nullptr) {
    loopVarType = bufferType->getElementType();
  } else {
    auto loopItemTypeForNext = [this](Type* nextType) -> Type* {
      if (nextType == nullptr || !nextType->is(BaseType::TY_UNION)) {
        return nextType;
      }
      std::vector<Type*> nonNullMembers;
      bool sawNull = false;
      for (Type* member : nextType->getUnionMembers()) {
        if (member != nullptr && member->is(BaseType::TY_NULL)) {
          sawNull = true;
          continue;
        }
        if (member != nullptr) {
          nonNullMembers.push_back(member);
        }
      }
      if (!sawNull || nonNullMembers.empty()) {
        return nextType;
      }
      auto [canonical, displayName] =
          TypeUtils::canonicalizeUnionMembers(std::move(nonNullMembers));
      if (canonical.size() == 1U) {
        return canonical.front();
      }
      auto out = std::make_unique<Type>(BaseType::TY_UNION);
      out->setDisplayName(displayName);
      out->setUnionMembers(std::move(canonical));
      return cacheType(std::move(out));
    };
    Type* iteratorType = resolveMethodReturnType(iterableType, "iter", {}, node->getSpan());
    if (iteratorType == nullptr) {
      throw TypeCheckError(
          node->getIterable()->getSpan(),
          "For-in requires an array type, Iterable with __buffer-backed first field, "
          "or iter() returning Iterator (next), got {}",
          iterableType != nullptr ? iterableType->toString() : "unknown");
    }
    Type* nextType = resolveMethodReturnType(iteratorType, "next", {}, node->getSpan());
    if (nextType == nullptr) {
      throw TypeCheckError(node->getIterable()->getSpan(), "For-in iterator must define next()");
    }
    if (getOptionalPayloadType(nextType) == nullptr) {
      throw TypeCheckError(node->getIterable()->getSpan(),
                           "For-in iterator next() must return an optional item type");
    }
    Type* payloadType = getOptionalPayloadType(nextType);
    if (isNullableType(payloadType)) {
      throw TypeCheckError(node->getIterable()->getSpan(),
                           "For-in iterator next() payload type {} cannot be nullable because nil "
                           "marks the end of iteration",
                           payloadType->toString());
    }
    loopVarType = loopItemTypeForNext(nextType);
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

  warnIfEmptyCompoundBody(node->getBlock(), "for-in body");
  node->getBlock()->accept(*this);
  checkUnusedBindingsInScope(child);
  scope = savedScope;
}

auto Typechecker::registerTraitsFromImportedModule(const std::string& absolutePath) -> void {
  if (absolutePath.empty()) {
    return;
  }
  const std::string normPath = normalizeResolvedFilesystemPath(absolutePath);
  (void) getOrTypecheckImport(normPath);
  auto it = importedModuleCache.find(normPath);
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
        traitMethodSignatures[tr->getIdentifier()].clear();
        auto savedImpTraitG = currentGenericTypes;
        for (const auto& p : tr->getGenericParamDecls()) {
          currentGenericTypes[p.name] = cacheType(std::make_unique<Type>(p.name));
        }
        Type* traitTy = scope->lookupType(tr->getIdentifier());
        for (FuncDecl* req : tr->getRequirements()) {
          if (traitTy != nullptr) {
            traitMethodSignatures[tr->getIdentifier()][req->getName()].push_back(
                buildMethodFunctionType(req, traitTy));
          }
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
  if (node->isStd()) {
    importType->setDeclarationFilePath(
        std::filesystem::absolute(std::filesystem::path(getStdDir()) / node->getFilePath())
            .lexically_normal()
            .string());
  } else {
    importType->setDeclarationFilePath(
        std::filesystem::absolute(std::filesystem::path(resolvedPath)).lexically_normal().string());
  }
  if (!node->isStd()) {
    registerTraitsFromImportedModule(resolvedPath);
  }
  auto addImportSymbol = [this, &importType](const std::string& name, llvm::SMRange declSpan) {
    if (!name.empty()) {
      auto symbol = std::make_unique<Value>(name, importType);
      symbol->setCategory(ValueCategory::MODULE_SYMBOL);
      symbol->setDeclarationKind(ValueDeclarationKind::NAMESPACE);
      symbol->setDeclarationSpan(declSpan.isValid() ? declSpan : llvm::SMRange());
      symbol->setDeclarationFilePath(mainFilePath);
      scope->insertSymbol(std::move(symbol));
    }
  };
  if (node->getImportAll() && node->getImportScope() && getExports) {
    ExportDiscoveryResult const discovery =
        getExports(node->getFilePath(), node->isStd(), mainFilePath);
    if (!discovery.failureMessage.empty()) {
      std::string const msg = fmt::format("Import * failed: {}", discovery.failureMessage);
      if (warningDiagnostics == nullptr) {
        throw TypeCheckError(node->getSpan(), msg);
      }
      recoverFromTypeError(TypeCheckError(node->getSpan(), msg));
      return;
    }
    for (const std::string& name : discovery.names) {
      if (importedNameToSource.contains(name)) {
        throw TypeCheckError(node->getSpan(),
                             "Import * conflicts: `{}` is already imported from another module",
                             name);
      }
      addImportSymbol(name, node->getSpan());
      importedNameToSource[name] = std::make_pair(resolvedPath, name);
      insertImportedVariableAlias(resolvedPath, name, name);
    }
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName,
                    node->getAliasSpan().isValid() ? node->getAliasSpan() : node->getSpan());
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  if (node->getImportedNames().empty()) {
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName,
                    node->getAliasSpan().isValid() ? node->getAliasSpan() : node->getSpan());
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  for (const ImportedNameBinding& binding : node->getImportedNames()) {
    const std::string& name = binding.name;
    const std::string& alias = binding.alias;
    const std::string localName = alias.empty() ? name : alias;
    llvm::SMRange const localSpan =
        !alias.empty() && binding.aliasSpan.isValid()
            ? binding.aliasSpan
            : (binding.nameSpan.isValid() ? binding.nameSpan : node->getSpan());
    addImportSymbol(localName, localSpan);
    importedNameToSource[localName] = std::make_pair(resolvedPath, name);
    insertImportedVariableAlias(resolvedPath, name, localName);
  }
}

auto Typechecker::visit(const Enum* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* outerScope = scope;
  SymbolTable* enumGenericScope = outerScope->createChildBlock("enum_generics");
  scope = enumGenericScope;
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(enumGenericScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);

  Type* enumTypePtr = outerScope->lookupType(node->getIdentifier());
  if (declarationPass) {
    if (enumTypePtr != nullptr) {
      throw TypeCheckError(node->getNameSpan(), "Duplicate enum definition: {}",
                           node->getIdentifier());
    }
    auto stub =
        std::make_unique<Type>(BaseType::TY_ENUM, nullptr, std::vector<std::unique_ptr<Field>>{});
    stub->setDisplayName(node->getIdentifier() + makeGenericDisplaySuffix(node->getGenericParams()));
    stub->setDeclarationSpan(node->getNameSpan());
    stub->setDeclarationFilePath(mainFilePath);
    stub->setGenericParams(node->getGenericParams());
    enumTypePtr = stub.get();
    outerScope->insertType(node->getIdentifier(), std::move(stub));

    auto enumSymbol = std::make_unique<Value>(node->getIdentifier(), enumTypePtr);
    enumSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    enumSymbol->setDeclarationKind(ValueDeclarationKind::ENUM);
    enumSymbol->setExported(node->isExported());
    enumSymbol->setDeclarationSpan(node->getNameSpan());
    enumSymbol->setDeclarationFilePath(mainFilePath);
    outerScope->insertSymbol(std::move(enumSymbol));
    node->setResolvedSymbol(outerScope->lookupStruct(node->getIdentifier()));

    Type* savedTemplateBeingDeclared = classTemplateBeingDeclared;
    size_t savedFieldCountExpected = classFieldCountExpected;
    classTemplateBeingDeclared = enumTypePtr;
    classFieldCountExpected = node->getValueDecls().size();
    for (const EnumValueDecl& value : node->getValueDecls()) {
      if (TypeUtils::findIndexInEnumVariants(enumTypePtr, value.name) >= 0) {
        throw TypeCheckError(value.span, "Duplicate enum variant '{}'", value.name);
      }
      std::vector<Type*> payloadTypes;
      payloadTypes.reserve(value.payloadTypes.size());
      for (TypeExpr* payloadTypeExpr : value.getPayloadTypes()) {
        Type* payloadType = resolveType(payloadTypeExpr);
        payloadTypes.push_back(payloadType);
      }
      auto field = std::make_unique<Field>(value.name, enumTypePtr);
      field->setDeclarationSpan(value.span);
      field->setDeclarationFilePath(mainFilePath);
      auto memberSymbol = std::make_unique<Value>(value.name, enumTypePtr);
      memberSymbol->setCategory(ValueCategory::DIRECT_VALUE);
      memberSymbol->setDeclarationKind(ValueDeclarationKind::ENUM_MEMBER);
      memberSymbol->setDeclarationSpan(value.span);
      memberSymbol->setDeclarationFilePath(mainFilePath);
      field->setDeclarationSymbol(std::move(memberSymbol));
      enumTypePtr->addField(std::move(field));

      auto variant = std::make_unique<EnumVariant>(value.name, std::move(payloadTypes));
      variant->setDeclarationSpan(value.span);
      variant->setDeclarationFilePath(mainFilePath);
      enumTypePtr->addEnumVariant(std::move(variant));
    }
    finalizeSpecializedTypesForTemplate(enumTypePtr);
    registerEnumSyntheticMembers(node, enumTypePtr, outerScope);
    classTemplateBeingDeclared = savedTemplateBeingDeclared;
    classFieldCountExpected = savedFieldCountExpected;
  } else if (enumTypePtr == nullptr) {
    throw TypeCheckError(node->getNameSpan(), "Enum not found: {}", node->getIdentifier());
  }

  enumTypePtr->setGenericParams(node->getGenericParams());
  SymbolTable* savedMethodInsertScope = currentMethodInsertScope;
  Type* savedEnumType = currentEnumType;
  bool const savedNominalExported = currentClassExported;
  currentMethodInsertScope = outerScope;
  currentEnumType = enumTypePtr;
  currentClassExported = node->isExported();
  for (FuncDecl* func : node->getMethods()) {
    SymbolTable* methodScope = scope->createChildBlock("enum_method");
    scope = methodScope;
    if (!func->getIsStatic()) {
      auto selfSym = std::make_unique<Value>("self", enumTypePtr);
      selfSym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      selfSym->setDeclarationKind(ValueDeclarationKind::PARAMETER);
      selfSym->setDeclarationSpan(func->getNameSpan());
      selfSym->setDeclarationFilePath(mainFilePath);
      scope->insertSymbol(std::move(selfSym));
    }
    func->accept(*this);
    scope = scope->getParent();
  }
  currentClassExported = savedNominalExported;
  currentEnumType = savedEnumType;
  currentMethodInsertScope = savedMethodInsertScope;
  scope = outerScope;
  currentGenericTypes = std::move(savedGenerics);
}

[[nodiscard]] auto Typechecker::cloneFieldForInheritance(Field* source) -> std::unique_ptr<Field> {
  std::unique_ptr<Field> nf;
  if (source->defaultValue != nullptr) {
    nf = std::make_unique<Field>(source->name, source->type,
                                 std::make_unique<Value>(*source->defaultValue));
  } else {
    nf = std::make_unique<Field>(source->name, source->type);
  }
  nf->setDeclarationSpan(source->getDeclarationSpan());
  nf->setDeclarationFilePath(source->getDeclarationFilePath());
  if (Value* declarationSymbol = source->getDeclarationSymbol()) {
    nf->setDeclarationSymbol(std::make_unique<Value>(*declarationSymbol));
  }
  return nf;
}

[[nodiscard]] auto Typechecker::findVarDeclWithName(const std::vector<VarDecl*>& fields,
                                                    const std::string& name) -> VarDecl* {
  for (VarDecl* v : fields) {
    if (v != nullptr && v->getIdentifier()->getValue() == name) {
      return v;
    }
  }
  return nullptr;
}

void Typechecker::registerSynthesizedClassConstructor(const Class* node, Type* classTypePtr,
                                                      SymbolTable* outerScope) {
  Type* selfPtrType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classTypePtr));
  std::vector<std::unique_ptr<Field>> paramFields;
  paramFields.push_back(std::make_unique<Field>("self", selfPtrType));
  std::vector<Type*> lookupTypes = {selfPtrType};
  for (Field* tf : classTypePtr->getFields()) {
    if (tf->defaultValue != nullptr) {
      continue;
    }
    if (VarDecl* vd = Typechecker::findVarDeclWithName(node->getFields(), tf->name);
        vd != nullptr && vd->getValue() != nullptr) {
      continue;
    }
    Type* storageTy = tf->type;
    Type* paramType = typeAsPtrIfClassForOverload(storageTy);
    if (paramType == nullptr) {
      throw TypeCheckError(node->getNameSpan(),
                           "Synthesized constructor cannot infer type for field '{}'", tf->name);
    }
    lookupTypes.push_back(paramType);
    paramFields.push_back(std::make_unique<Field>(tf->name, paramType));
  }
  Type* voidRet = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(voidRet);
  Type* funcTypePtr = cacheType(std::move(funcType));

  if (outerScope->lookupFunction("new", lookupTypes, FunctionLookupKind::OVERLOAD_IDENTITY) !=
      nullptr) {
    throw TypeCheckError(node->getNameSpan(), "Constructor 'new' already registered for class {}",
                         node->getIdentifier());
  }
  auto ctorSym = std::make_unique<Value>("new", funcTypePtr);
  ctorSym->setCategory(ValueCategory::CALLABLE_SYMBOL);
  ctorSym->setDeclarationKind(ValueDeclarationKind::METHOD);
  ctorSym->setExported(node->isExported());
  ctorSym->setDeclarationSpan(node->getNameSpan());
  ctorSym->setDeclarationFilePath(mainFilePath);
  outerScope->insertSymbol(std::move(ctorSym));
  Value* ctorFunc =
      outerScope->lookupFunction("new", lookupTypes, FunctionLookupKind::OVERLOAD_IDENTITY);
  if (ctorFunc == nullptr) {
    throw TypeCheckError(node->getNameSpan(), "Internal error registering synthesized constructor");
  }
  SymbolTable* body = outerScope->createChildBlock("function");
  ctorFunc->setBodyScope(body);
  auto selfParam = std::make_unique<Value>("self", selfPtrType);
  selfParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  selfParam->setDeclarationKind(ValueDeclarationKind::PARAMETER);
  selfParam->setDeclarationSpan(node->getNameSpan());
  selfParam->setDeclarationFilePath(mainFilePath);
  body->insertSymbol(std::move(selfParam));
  for (size_t i = 1; i < lookupTypes.size(); ++i) {
    std::string const pname = funcTypePtr->getFields()[i]->name;
    auto ps = std::make_unique<Value>(pname, lookupTypes[i]);
    ps->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    ps->setDeclarationKind(ValueDeclarationKind::PARAMETER);
    ps->setDeclarationSpan(node->getNameSpan());
    ps->setDeclarationFilePath(mainFilePath);
    body->insertSymbol(std::move(ps));
  }
}

void Typechecker::mergeClassVtableOrder(const Class* node, Type* classTy,
                                        SymbolTable* methodLookupScope) {
  std::vector<std::string> order;
  std::unordered_map<std::string, size_t> slotIndex;
  if (Type* sup = classTy->getClassSuperclass()) {
    Type* supForVtable = sup;
    if (auto it = specializedTypeToTemplate.find(sup); it != specializedTypeToTemplate.end()) {
      supForVtable = it->second;
    }
    order = supForVtable->getClassVtableMethodOrder();
    for (size_t i = 0; i < order.size(); ++i) {
      slotIndex[order[i]] = i;
    }
  }
  for (FuncDecl* m : node->getMethods()) {
    if (m->getName() == "new" || m->getIsStatic()) {
      continue;
    }
    std::string methodKey = vtableMethodKey(m->getResolvedSymbol());
    if (methodKey.empty()) {
      methodKey = m->getName();
    }
    if (slotIndex.contains(methodKey)) {
      if (!m->getDeclaresOverload()) {
        throw TypeCheckError(m->getNameSpan(),
                             "Method overrides an inherited method; add the `overload` keyword "
                             "before `func`");
      }
      Value* derivedSym = m->getResolvedSymbol();
      if (derivedSym != nullptr && methodLookupScope != nullptr) {
        Value* baseSym =
            findInheritedVirtualMethodSymbol(methodLookupScope, classTy, m->getName(), derivedSym);
        if (baseSym != nullptr) {
          Type* derivedTy = derivedSym->getType();
          Type* baseTy = baseSym->getType();
          if (!virtualOverrideSignaturesMatch(derivedTy, baseTy)) {
            throw TypeCheckError(m->getNameSpan(),
                                 "Method override must match inherited signature (parameters after "
                                 "`self` and return type); inherited {}, overriding {}",
                                 baseTy != nullptr ? baseTy->toString() : std::string("?"),
                                 derivedTy != nullptr ? derivedTy->toString() : std::string("?"));
          }
        }
      }
      continue;
    }
    slotIndex[methodKey] = order.size();
    order.push_back(std::move(methodKey));
  }
  classTy->setClassVtableMethodOrder(std::move(order));
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
    if (classTypePtr != nullptr) {
      throw TypeCheckError(node->getNameSpan(), "Duplicate class definition: {}",
                           node->getIdentifier());
    }
    auto stub =
        std::make_unique<Type>(BaseType::TY_CLASS, nullptr, std::vector<std::unique_ptr<Field>>{});
    stub->setDisplayName(node->getIdentifier() +
                         makeGenericDisplaySuffix(node->getGenericParams()));
    stub->setDeclarationSpan(node->getNameSpan());
    stub->setDeclarationFilePath(mainFilePath);
    const auto stdBasePath =
        std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les")
            .lexically_normal();
    const auto declPath =
        std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal();
    std::error_code builtinStrEc;
    stub->setBuiltinStringClass(node->getIdentifier() == "str" &&
                                std::filesystem::equivalent(declPath, stdBasePath, builtinStrEc));
    stub->setGenericParams(node->getGenericParams());
    stub->setImplTraitNames(node->getImplTraitNames());
    classTypePtr = stub.get();
    outerScope->insertType(node->getIdentifier(), std::move(stub));
    auto classSymbol = std::make_unique<Value>(node->getIdentifier(), classTypePtr);
    classSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    classSymbol->setDeclarationKind(ValueDeclarationKind::CLASS);
    classSymbol->setExported(node->isExported());
    classSymbol->setDeclarationSpan(node->getNameSpan());
    classSymbol->setDeclarationFilePath(mainFilePath);
    outerScope->insertSymbol(std::move(classSymbol));
    node->setResolvedSymbol(outerScope->lookupStruct(node->getIdentifier()));

    if (node->getBaseType() != nullptr) {
      node->getBaseType()->accept(*this);
      Type* superType = result->getType();
      if (superType == nullptr || !superType->is(BaseType::TY_CLASS)) {
        throw TypeCheckError(node->getBaseTypeSpan(), "Superclass must be a class type");
      }
      std::unordered_set<Type*> seen;
      for (Type* t = superType; t != nullptr; t = t->getClassSuperclass()) {
        if (!seen.insert(t).second) {
          throw TypeCheckError(node->getBaseTypeSpan(), "Cyclic inheritance in superclass chain");
        }
        if (t->isEqual(classTypePtr)) {
          throw TypeCheckError(node->getBaseTypeSpan(), "Cyclic inheritance");
        }
        if (auto it = specializedTypeToTemplate.find(t);
            it != specializedTypeToTemplate.end() && it->second->isEqual(classTypePtr)) {
          throw TypeCheckError(node->getBaseTypeSpan(), "Cyclic inheritance");
        }
      }
      classTypePtr->setClassSuperclass(superType);
      superType->setClassHasDerivedClass(true);
      for (Field* bf : superType->getFields()) {
        classTypePtr->addField(Typechecker::cloneFieldForInheritance(bf));
      }
    }

    classTemplateBeingDeclared = classTypePtr;
    classFieldCountExpected = classTypePtr->getFields().size();
    for (VarDecl* f : node->getFields()) {
      if (!f->getIsStatic()) {
        classFieldCountExpected++;
      }
    }

    for (VarDecl* field : node->getFields()) {
      std::string const fname = field->getIdentifier()->getValue();
      for (Field* existing : classTypePtr->getFields()) {
        if (existing->name == fname) {
          throw TypeCheckError(
              field->getSpan(),
              "Field '{}' conflicts with a superclass field or duplicate declaration", fname);
        }
      }
      for (Field* existing : classTypePtr->getStaticFields()) {
        if (existing->name == fname) {
          throw TypeCheckError(field->getSpan(), "Duplicate static field '{}'", fname);
        }
      }
      Type* fieldType = nullptr;
      Type* initType = nullptr;
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
        fieldType = result->getType();
      }
      if (field->getValue() != nullptr) {
        field->getValue()->accept(*this);
        initType = result->getType();
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
      if (!node->getGenericParamDecls().empty() && field->getIsStatic()) {
        std::unordered_set<std::string> classParamNames;
        classParamNames.reserve(node->getGenericParamDecls().size());
        for (const auto& p : node->getGenericParamDecls()) {
          classParamNames.insert(p.name);
        }
        if (typeUsesClassTypeParameter(fieldType, classParamNames)) {
          throw TypeCheckError(field->getSpan(),
                               "Static field type cannot use the class's generic parameters");
        }
        if (initType != nullptr && typeUsesClassTypeParameter(initType, classParamNames)) {
          throw TypeCheckError(
              field->getSpan(),
              "Static field initializer type cannot use the class's generic parameters");
        }
      }
      auto fieldEntry = std::make_unique<Field>(field->getIdentifier()->getValue(), fieldType);
      fieldEntry->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldEntry->setDeclarationFilePath(mainFilePath);
      auto fieldSymbol = std::make_unique<Value>(field->getIdentifier()->getValue(), fieldType,
                                                 SymbolState::INITIALIZED);
      fieldSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      fieldSymbol->setDeclarationKind(ValueDeclarationKind::PROPERTY);
      fieldSymbol->setMutable(field->getMutability());
      fieldSymbol->setPrivateMember(field->getIsPrivate());
      fieldSymbol->setMemberDeclaredInClass(classTypePtr);
      fieldSymbol->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldSymbol->setDeclarationFilePath(mainFilePath);
      if (fieldType != nullptr && fieldType->is(BaseType::TY_FUNCTION)) {
        fieldSymbol->setStoresFuncValuePair(true);
      }
      field->setResolvedSymbol(fieldSymbol.get());
      fieldEntry->setDeclarationSymbol(std::move(fieldSymbol));
      if (field->getIsStatic()) {
        classTypePtr->addStaticField(std::move(fieldEntry));
      } else {
        classTypePtr->addField(std::move(fieldEntry));
      }
    }

    finalizeSpecializedTypesForTemplate(classTypePtr);
    classTemplateBeingDeclared = nullptr;
    classFieldCountExpected = 0;
  } else {
    if (classTypePtr == nullptr) {
      throw TypeCheckError(node->getNameSpan(), "Class not found: {}", node->getIdentifier());
    }
  }

  classTypePtr->setGenericParams(node->getGenericParams());
  classTypePtr->setImplTraitNames(node->getImplTraitNames());
  auto* selfPtrType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classTypePtr));
  SymbolTable* savedMethodInsertScope = currentMethodInsertScope;
  bool const savedClassExportedFlag = currentClassExported;
  currentClassExported = node->isExported();
  for (FuncDecl* func : node->getMethods()) {
    currentClassType = classTypePtr;
    currentMethodInsertScope = outerScope;
    SymbolTable* methodScope = scope->createChildBlock("method");
    scope = methodScope;
    if (!func->getIsStatic()) {
      auto selfSym = std::make_unique<Value>("self", selfPtrType);
      selfSym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(selfSym));
    }
    func->accept(*this);
    scope = scope->getParent();
    currentClassType = nullptr;
  }
  currentClassExported = savedClassExportedFlag;
  currentMethodInsertScope = savedMethodInsertScope;

  if (declarationPass) {
    mergeClassVtableOrder(node, classTypePtr, outerScope);
    bool hasExplicitNew = false;
    for (FuncDecl* m : node->getMethods()) {
      if (m->getName() == "new") {
        hasExplicitNew = true;
        break;
      }
    }
    if (classTypePtr->getClassSuperclass() != nullptr && !hasExplicitNew) {
      throw TypeCheckError(node->getNameSpan(),
                           "Class with a superclass must define `func new` (call super.new to "
                           "initialize the base)");
    }
    if (!hasExplicitNew) {
      registerSynthesizedClassConstructor(node, classTypePtr, outerScope);
    }
  }

  if (!declarationPass) {
    classesPendingUnusedMemberDiagnosis.push_back(node);
  }

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
  if (node->getIsStatic() && node->getName() == "new") {
    throw TypeCheckError(node->getNameSpan(), "Constructor `new` cannot be declared `static`");
  }
  node->getReturnType()->accept(*this);
  Type* returnType = wrapReturnTypeIfNominal(result->getType());
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  if (currentClassType != nullptr && !node->getIsStatic()) {
    Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, currentClassType));
    paramFields.push_back(std::make_unique<Field>("self", selfPtr));
    paramTypes.push_back(selfPtr);
  } else if (currentEnumType != nullptr && !node->getIsStatic()) {
    paramFields.push_back(std::make_unique<Field>("self", currentEnumType));
    paramTypes.push_back(currentEnumType);
  }
  validateParameterDefaultOrdering(node->getSpan(), node->getParameters());
  for (Parameter* param : node->getParameters()) {
    Type* nominalParamType = nullptr;
    if (param->type != nullptr) {
      param->type->accept(*this);
      nominalParamType = result->getType();
    }
    if (param->defaultVal != nullptr) {
      if (nominalParamType != nullptr) {
        visitExprWithExpectedType(param->defaultVal.get(), nominalParamType);
      } else {
        param->defaultVal->accept(*this);
        nominalParamType = result->getType();
      }
    } else if (nominalParamType == nullptr) {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type and no default value",
                           param->name);
    }
    Type* paramType = typeAsPtrIfClassForOverload(nominalParamType);
    if (param->defaultVal != nullptr && !isAssignableTo(result->getType(), paramType)) {
      throw TypeCheckError(param->defaultVal->getSpan(),
                           "Default value type {} is not assignable to parameter type {}",
                           result->getType()->toString(), paramType->toString());
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
  Value* funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes,
                                                  FunctionLookupKind::OVERLOAD_IDENTITY);
  bool const effectiveFuncExported =
      (currentClassType != nullptr || currentEnumType != nullptr) ? currentClassExported
                                                                  : node->isExported();

  if (declarationPass) {
    if (funcSymbol == nullptr) {
      auto declaredFunc = std::make_unique<Value>(node->getName(), funcTypePtr);
      declaredFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
      declaredFunc->setDeclarationKind((currentClassType != nullptr || currentEnumType != nullptr)
                                           ? ValueDeclarationKind::METHOD
                                           : ValueDeclarationKind::FUNCTION);
      declaredFunc->setExported(effectiveFuncExported);
      if (currentClassType != nullptr) {
        declaredFunc->setPrivateMember(node->getIsPrivate());
        declaredFunc->setMemberDeclaredInClass(currentClassType);
      }
      declaredFunc->setDeclarationSpan(node->getNameSpan());
      declaredFunc->setDeclarationFilePath(mainFilePath);
      declaredFunc->setStaticMethod(node->getIsStatic());
      insertScope->insertSymbol(std::move(declaredFunc));
      funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes,
                                               FunctionLookupKind::OVERLOAD_IDENTITY);
      // Set resolvedSymbol immediately after we get the symbol for this exact overload
      if (funcSymbol != nullptr) {
        node->setResolvedSymbol(funcSymbol);
      }
    } else {
      funcSymbol->setType(funcTypePtr);
      funcSymbol->setDeclarationKind((currentClassType != nullptr || currentEnumType != nullptr)
                                         ? ValueDeclarationKind::METHOD
                                         : ValueDeclarationKind::FUNCTION);
      funcSymbol->setExported(effectiveFuncExported);
      if (currentClassType != nullptr) {
        funcSymbol->setPrivateMember(node->getIsPrivate());
        funcSymbol->setMemberDeclaredInClass(currentClassType);
      }
      funcSymbol->setStaticMethod(node->getIsStatic());
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
      const size_t paramOffset =
          (currentClassType != nullptr || currentEnumType != nullptr) && !node->getIsStatic() ? 1U
                                                                                              : 0U;
      for (size_t i = 0; i < node->getParameters().size(); ++i) {
        Parameter* param = node->getParameters()[i];
        auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[paramOffset + i]);
        paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
        paramSymbol->setDeclarationKind(ValueDeclarationKind::PARAMETER);
        paramSymbol->setDeclarationSpan(param->nameSpan);
        paramSymbol->setDeclarationFilePath(mainFilePath);
        if (paramTypes[paramOffset + i] != nullptr &&
            paramTypes[paramOffset + i]->is(BaseType::TY_FUNCTION)) {
          paramSymbol->setStoresFuncValuePair(true);
        }
        param->setResolvedSymbol(paramSymbol.get());
        warnShadowingFromEnclosing(param->name, param->nameSpan);
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
    functionScopeStack.push_back(scope);
    inTopLevel = false;
    auto savedTraitBounds = currentGenericParamTraitBounds;
    currentGenericParamTraitBounds.clear();
    for (const auto& p : node->getGenericParamDecls()) {
      currentGenericParamTraitBounds[p.name] = p.traitBounds;
    }
    if (node->getBody() != nullptr) {
      node->getBody()->accept(*this);
    }
    checkUnusedBindingsInScope(scope);
    currentGenericParamTraitBounds = std::move(savedTraitBounds);
    Type* funcReturnType = currentFunction->getType()->getReturnType();
    if (node->getBody() != nullptr && funcReturnType != nullptr &&
        !funcReturnType->is(BaseType::TY_VOID) && !blockAlwaysReturns(node->getBody())) {
      throw TypeCheckError(node->getSpan(), "Non-void function may reach end without returning");
    }
    scope = scope->getParent();
    currentFunction = nullptr;
    functionScopeStack.pop_back();
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
  Type* returnType = wrapReturnTypeIfNominal(result->getType());
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  validateParameterDefaultOrdering(node->getSpan(), node->getParameters());
  for (Parameter* param : node->getParameters()) {
    Type* nominalParamType = nullptr;
    if (param->type != nullptr) {
      param->type->accept(*this);
      nominalParamType = result->getType();
    }
    if (param->defaultVal != nullptr) {
      if (nominalParamType != nullptr) {
        Type* expectedForDefault = nominalParamType;
        // Match codegen: extern params use pointer-to-class; defaults must check against that type
        // so overload resolution agrees with Codegen::visit(ExternFuncDecl).
        if (expectedForDefault->is(BaseType::TY_CLASS)) {
          expectedForDefault =
              cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, expectedForDefault));
        }
        visitExprWithExpectedType(param->defaultVal.get(), expectedForDefault);
      } else {
        param->defaultVal->accept(*this);
        nominalParamType = result->getType();
      }
    } else if (nominalParamType == nullptr) {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type and no default value",
                           param->name);
    }
    Type* paramType = typeAsPtrIfClassForOverload(nominalParamType);
    if (param->defaultVal != nullptr && !isAssignableTo(result->getType(), paramType)) {
      throw TypeCheckError(param->defaultVal->getSpan(),
                           "Default value type {} is not assignable to parameter type {}",
                           result->getType()->toString(), paramType->toString());
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
  case TokenType::AMPERSAND_EQUAL:
    return TokenType::AMPERSAND;
  case TokenType::PIPE_EQUAL:
    return TokenType::PIPE;
  case TokenType::XOR_EQUAL:
    return TokenType::XOR;
  case TokenType::SHIFT_LEFT_EQUAL:
    return TokenType::SHIFT_LEFT;
  case TokenType::SHIFT_RIGHT_EQUAL:
    return TokenType::SHIFT_RIGHT;
  default:
    return std::nullopt;
  }
}

auto Typechecker::visit(const Assignment* node) -> void {
  TokenType assignOp = node->getOperator();
  std::optional<TokenType> binaryOp = compoundToBinaryOp(assignOp);
  auto validateNullCoalesceAssignment = [this, node](Type* targetType, Type* rhsType,
                                                     std::string_view targetKind) {
    Type* payloadType = getOptionalPayloadType(targetType);
    if (payloadType == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Null-coalescing assignment requires {} to have an "
                           "optional type",
                           targetKind);
    }
    if (rhsType == nullptr || !isAssignableTo(rhsType, payloadType) ||
        isLossyImplicitConversion(rhsType, payloadType)) {
      throw TypeCheckError(node->getSpan(),
                           "Null-coalescing assignment value type {} is not assignable to {} "
                           "optional payload type {}",
                           rhsType != nullptr ? rhsType->toString() : "void", targetKind,
                           payloadType->toString());
    }
  };

  if (auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide())) {
    Value* sym = scope->lookup(lit->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Variable not found: {}", lit->getValue());
    }
    if (!sym->getMutability()) {
      throw TypeCheckError(node->getSpan(), "Cannot assign to immutable variable {}",
                           lit->getValue());
    }
    if (binaryOp.has_value()) {
      markValueRead(sym);
    }
    Type* lhsType = sym->getType();
    Type* rhsExpectedType = lhsType;
    if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
      rhsExpectedType = getOptionalPayloadType(lhsType);
      if (rhsExpectedType == nullptr) {
        throw TypeCheckError(node->getSpan(),
                             "Null-coalescing assignment requires variable {} to have an optional "
                             "type",
                             lit->getValue());
      }
    }
    visitExprWithExpectedType(node->getRightHandSide(), rhsExpectedType);
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
      if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
        validateNullCoalesceAssignment(lhsType, rhsType, "variable");
      } else if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      } else if (!isAssignableTo(rhsType, lhsType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to variable of type {}",
                             rhsType->toString(), lhsType->toString());
      }
    }
    invalidateUnionNarrowingForSymbol(sym);
    return;
  }
  if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr) {
    auto* dotLhs = static_cast<DotOp*>(node->getLeftHandSide());
    if (auto* dot = dynamic_cast<DotOp*>(node->getLeftHandSide())) {
      if (auto* leftLit = dynamic_cast<Literal*>(dot->getLeft())) {
        if (leftLit->getType() == TokenType::IDENTIFIER &&
            importAliasToPath.contains(leftLit->getValue())) {
          if (auto* rightLit = dynamic_cast<Literal*>(dot->getRight())) {
            if (rightLit->getType() == TokenType::IDENTIFIER) {
              SymbolTable* imp = getOrTypecheckImport(importAliasToPath[leftLit->getValue()]);
              if (imp != nullptr) {
                Value* globalSym = imp->lookup(rightLit->getValue());
                if (globalSym != nullptr &&
                    globalSym->getDeclarationKind() == ValueDeclarationKind::VARIABLE &&
                    globalSym->isExported() && !globalSym->getMutability()) {
                  throw TypeCheckError(node->getSpan(), "Cannot assign to immutable variable {}",
                                       rightLit->getValue());
                }
              }
            }
          }
        }
      }
    }
    node->getLeftHandSide()->accept(*this);
    Type* lhsType = result->getType();
    Type* targetType = assignmentStorageTypeForDotLhs(dotLhs, lhsType);
    Type* rhsExpectedType = targetType;
    if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
      rhsExpectedType = getOptionalPayloadType(targetType);
      if (rhsExpectedType == nullptr) {
        throw TypeCheckError(node->getSpan(),
                             "Null-coalescing assignment requires field target to have an "
                             "optional type");
      }
    }
    visitExprWithExpectedType(node->getRightHandSide(), rhsExpectedType);
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
      if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
        validateNullCoalesceAssignment(targetType, rhsType, "field");
      } else if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      } else if (targetType != nullptr && !isAssignableTo(rhsType, targetType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to field of type {}",
                             rhsType->toString(), targetType->toString());
      }
    }
    invalidateUnionNarrowingForSymbol(rootStorageSymbolForAssignmentLhs(node->getLeftHandSide()));
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
      Type* targetType = baseType->getElementType();
      Type* rhsExpectedType = targetType;
      if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
        rhsExpectedType = getOptionalPayloadType(targetType);
        if (rhsExpectedType == nullptr) {
          throw TypeCheckError(node->getSpan(),
                               "Null-coalescing assignment requires list element type to be "
                               "optional");
        }
      }
      visitExprWithExpectedType(node->getRightHandSide(), rhsExpectedType);
      Type* rhsType = result->getType();
      if (binaryOp.has_value()) {
        Type* resultType = typecheckBinaryOpResult(*binaryOp, targetType, rhsType, node->getSpan());
        if (targetType != nullptr && !isAssignableTo(resultType, targetType)) {
          throw TypeCheckError(
              node->getSpan(),
              "Compound assignment result type {} is not assignable to list element type {}",
              resultType->toString(), targetType->toString());
        }
      } else {
        if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
          validateNullCoalesceAssignment(targetType, rhsType, "list element");
        } else if (assignOp != TokenType::EQUAL) {
          throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                               NAMEOF_ENUM(assignOp));
        } else if (targetType != nullptr && !isAssignableTo(rhsType, targetType)) {
          throw TypeCheckError(node->getSpan(), "Cannot assign type {} to list element of type {}",
                               rhsType->toString(), targetType->toString());
        }
      }
      invalidateUnionNarrowingForSymbol(rootStorageSymbolForAssignmentLhs(node->getLeftHandSide()));
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
      if (assignOp == TokenType::NULL_COALESCE_EQUAL) {
        Type* lhsType = resolveMethodReturnType(
            baseType, std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexType}, node->getSpan());
        if (lhsType == nullptr) {
          throw TypeCheckError(node->getSpan(), "Operator [] not found for assignment target");
        }
        Type* payloadType = getOptionalPayloadType(lhsType);
        if (payloadType == nullptr) {
          throw TypeCheckError(node->getSpan(),
                               "Null-coalescing assignment requires subscript target to have an "
                               "optional type");
        }
        visitExprWithExpectedType(node->getRightHandSide(), payloadType);
        Type* rhsType = result->getType();
        validateNullCoalesceAssignment(lhsType, rhsType, "subscript target");
        auto hasSetter = [this, baseType, indexType, node](Type* valueType) -> bool {
          try {
            return resolveMethodReturnType(baseType, std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                           {indexType, valueType}, node->getSpan()) != nullptr;
          } catch (const TypeCheckError&) {
            return false;
          }
        };
        if (!hasSetter(rhsType) && !hasSetter(lhsType)) {
          throw TypeCheckError(node->getSpan(), "Operator []= not found for assignment target");
        }
      } else if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      } else {
        node->getRightHandSide()->accept(*this);
        Type* rhsType = result->getType();
        if (resolveMethodReturnType(baseType, std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                    {indexType, rhsType}, node->getSpan()) == nullptr) {
          throw TypeCheckError(node->getSpan(), "Operator []= not found for assignment target");
        }
      }
    }
    invalidateUnionNarrowingForSymbol(rootStorageSymbolForAssignmentLhs(node->getLeftHandSide()));
    return;
  }
  throw TypeCheckError(node->getSpan(), "Invalid assignment target");
}
auto Typechecker::visit(const Break* node) -> void {
  (void) node;
  // We don't track break targets in typecheck; Codegen will enforce.
}

auto Typechecker::visit(const Continue* node) -> void { (void) node; }

auto Typechecker::visit(const Pass* node) -> void { (void) node; }

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
  emitWarning(node->getSpan(), std::string("Unimplemented: ") + node->getMessage());
  throw TypeCheckError(node->getSpan(), "{}", node->getMessage());
}

auto Typechecker::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Typechecker::resolveFuncCallCalleeOrEarlyReturn(const FuncCall* node,
                                                     std::vector<Type*>& argTypes,
                                                     SymbolTable*& importedScope,
                                                     bool& funcCallResolvedViaImportedNameBinding,
                                                     Value*& callee) -> bool {
  if (callee != nullptr) {
    return false;
  }
  Value* importStubForCalleeName = nullptr;
  Value* sym = scope->lookup(node->getName());
  std::string importedName;
  if (auto importedIt = importedNameToSource.find(node->getName());
      importedIt != importedNameToSource.end()) {
    importedScope = getOrTypecheckImport(importedIt->second.first);
    importedName = importedIt->second.second;
  }
  if (sym == nullptr || sym->getType()->is(BaseType::TY_IMPORT)) {
    if (sym != nullptr && sym->getType()->is(BaseType::TY_IMPORT)) {
      importStubForCalleeName = sym;
    }
    if (importedScope != nullptr) {
      sym = importedScope->lookup(importedName);
      callee = importedScope->lookupFunction(importedName, argTypes);
      if (callee != nullptr) {
        funcCallResolvedViaImportedNameBinding = true;
      }
    }
  }
  auto markImportStubIfCalleeWasNamedImport = [&]() {
    if (importStubForCalleeName != nullptr) {
      markValueRead(importStubForCalleeName);
    }
  };
  if (callee == nullptr && sym != nullptr) {
    node->setResolvedSymbol(sym);
    if (sym->getType()->is(BaseType::TY_CLASS)) {
      Type* classType = materializeForCallSite(sym->getType(), importedScope);
      if (!node->getExplicitTypeArgs().empty()) {
        SymbolTable* ctorScope = importedScope != nullptr ? importedScope : scope;
        finishGenericClassCallWithExplicitTypeArgs(node, classType, argTypes, ctorScope, true,
                                                   [&] { markImportStubIfCalleeWasNamedImport(); });
        return true;
      }
      if (!node->getArguments().empty()) {
        SymbolTable* ctorScope = importedScope != nullptr ? importedScope : scope;
        if (tryFinishGenericClassCallWithInferredTypeArgs(
                node, classType, argTypes, ctorScope, true,
                [&] { markImportStubIfCalleeWasNamedImport(); })) {
          return true;
        }
      }
      if (node->getArguments().empty()) {
        Type* ptrToClass = typeAsPtrIfClassForOverload(classType);
        std::vector<Type*> ctorParamTypes = {ptrToClass};
        Value* constructor = importedScope != nullptr
                                 ? importedScope->lookupFunction(
                                       "new", ctorParamTypes, FunctionLookupKind::OVERLOAD_IDENTITY)
                                 : scope->lookupFunction("new", ctorParamTypes,
                                                         FunctionLookupKind::OVERLOAD_IDENTITY);
        if (constructor != nullptr) {
          enforcePrivateMemberReadable(node->getSpan(), constructor);
          markValueRead(constructor);
        }
        markImportStubIfCalleeWasNamedImport();
        result = std::make_unique<Value>(classType);
        return true;
      }
    }
    if (sym->getType()->is(BaseType::TY_ENUM)) {
      markImportStubIfCalleeWasNamedImport();
      result = std::make_unique<Value>(materializeForCallSite(sym->getType(), importedScope));
      return true;
    }
    if (sym->getType()->is(BaseType::TY_IMPORT)) {
      result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
      return true;
    }
  }
  if (callee == nullptr && maybeRetypeCallArgsForStringLiteralOverload(node, argTypes)) {
    callee = scope->lookupFunction(node->getName(), argTypes);
    if (callee == nullptr) {
      Value* classSym = scope->lookupStruct(node->getName());
      if (classSym == nullptr) {
        auto classImportedIt = importedNameToSource.find(node->getName());
        if (classImportedIt != importedNameToSource.end()) {
          SymbolTable* imp = getOrTypecheckImport(classImportedIt->second.first);
          if (imp != nullptr) {
            classSym = imp->lookupStruct(classImportedIt->second.second);
          }
        }
      }
      if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
        Type* classType = materializeForCallSite(classSym->getType(), importedScope);
        Type* ptrToClass = typeAsPtrIfClassForOverload(classType);
        std::vector<Type*> constructorParamTypes = {ptrToClass};
        constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(), argTypes.end());
        callee = lookupConstructorForAllocatedClass(scope, constructorParamTypes, classType);
        if (callee != nullptr) {
          enforcePrivateMemberReadable(node->getSpan(), callee);
        }
        if (callee == nullptr) {
          auto ctorImportedIt = importedNameToSource.find(node->getName());
          if (ctorImportedIt != importedNameToSource.end()) {
            SymbolTable* imp = getOrTypecheckImport(ctorImportedIt->second.first);
            if (imp != nullptr) {
              callee = lookupConstructorForAllocatedClass(imp, constructorParamTypes, classType);
              if (callee != nullptr) {
                funcCallResolvedViaImportedNameBinding = true;
                enforcePrivateMemberReadable(node->getSpan(), callee);
              }
            }
          }
        }
      }
    }
    if (callee == nullptr) {
      auto importedIt = importedNameToSource.find(node->getName());
      if (importedIt != importedNameToSource.end()) {
        importedScope = getOrTypecheckImport(importedIt->second.first);
        if (importedScope != nullptr) {
          callee = importedScope->lookupFunction(importedIt->second.second, argTypes);
          if (callee != nullptr) {
            funcCallResolvedViaImportedNameBinding = true;
          }
        }
      }
    }
  }
  if (callee == nullptr) {
    throw TypeCheckError(node->getSpan(), "Function not found: {}", node->getName());
  }
  return false;
}

auto Typechecker::visit(const FuncCall* node) -> void {
  node->clearGenericBindingEnv();
  std::vector<Type*> argTypes = overloadArgTypesFromCall(node);
  if (visitListIntrinsicCall(node, argTypes)) {
    return;
  }
  if (!node->getExplicitTypeArgs().empty()) {
    Value* classSym = scope->lookup(node->getName());
    if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
      node->setResolvedSymbol(classSym);
      Type* classType = classSym->getType();
      finishGenericClassCallWithExplicitTypeArgs(node, classType, argTypes, scope, true, {});
      return;
    }
  }

  SymbolTable* importedScope = nullptr;
  Value* callee = scope->lookupFunction(node->getName(), argTypes);
  bool funcCallResolvedViaImportedNameBinding = false;
  if (resolveFuncCallCalleeOrEarlyReturn(node, argTypes, importedScope,
                                         funcCallResolvedViaImportedNameBinding, callee)) {
    return;
  }
  if (!callee->getType()->is(BaseType::TY_FUNCTION)) {
    throw TypeCheckError(node->getSpan(), "Not a function: {}", node->getName());
  }
  node->setResolvedSymbol(callee);
  markValueRead(callee);
  if (funcCallResolvedViaImportedNameBinding) {
    if (Value* importStub = scope->lookup(node->getName());
        importStub != nullptr && importStub->getCategory() == ValueCategory::MODULE_SYMBOL &&
        importStub->getType() != nullptr && importStub->getType()->is(BaseType::TY_IMPORT)) {
      markValueRead(importStub);
    }
  }

  completeOrdinaryFuncCallTyping(node, callee, importedScope, argTypes);
}

void Typechecker::completeOrdinaryFuncCallTyping(const FuncCall* node, Value* callee,
                                                 SymbolTable* importedScope,
                                                 const std::vector<Type*>& argTypes) {
  auto* funcType = callee->getType();
  auto fields = funcType->getFields();

  if (!node->getExplicitTypeArgs().empty()) {
    std::vector<Type*> explicitTypes;
    collectExplicitTypesFromCallByVisit(node, explicitTypes);
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
      if (expected != nullptr && (!isAssignableTo(argTypes[i], expected) ||
                                  isLossyImplicitConversion(argTypes[i], expected))) {
        throw TypeCheckError(node->getSpan(),
                             "Argument type {} does not match explicit "
                             "parameter type {}",
                             argTypes[i]->toString(), expected->toString());
      }
    }
    if (callee->getName() == "new" && !fields.empty() && fields[0]->type->is(BaseType::TY_PTR)) {
      Type* monomorph = substituteInType(fields[0]->type->getElementType(), explicitSubst);
      Type* valueType = monomorph;
      if (importedScope != nullptr && monomorph != nullptr) {
        const bool alreadyRegisteredSpecialized =
            specializedTypeToTemplate.contains(monomorph) ||
            specializedTypeEnv.contains(monomorph) ||
            std::ranges::any_of(specializedClassTypes, [monomorph](const auto& entry) {
              return entry.second == monomorph;
            });
        if (!alreadyRegisteredSpecialized) {
          valueType = materializeImportedType(monomorph);
        }
      }
      result = std::make_unique<Value>(valueType);
      node->setAllocatedClassMonomorph(valueType);
    } else {
      Type* retType = substituteInType(funcType->getReturnType(), explicitSubst);
      result = std::make_unique<Value>(materializeForCallSite(retType, importedScope));
    }
    if (callee->getName() != "new" && !funcType->getGenericParams().empty()) {
      node->setGenericBindingEnv(explicitSubst);
    }
    verifyGenericTraitBounds(callee, explicitSubst, node->getSpan());
    return;
  }

  std::unordered_map<std::string, Type*> localGenericTypes;
  for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, argTypes[i], localGenericTypes, node->getSpan());
  }
  if (callee->getName() == "new" && !funcType->getFields().empty() &&
      funcType->getFields()[0]->type->is(BaseType::TY_PTR)) {
    Type* classType = funcType->getFields()[0]->type->getElementType();
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
    Type* specialized =
        getOrCreateSpecializedClassType(classType, genericParamNames, localGenericTypes);
    Type* valueType = specialized;
    result = std::make_unique<Value>(valueType);
    node->setAllocatedClassMonomorph(valueType);
  } else {
    Type* retType = substituteInType(funcType->getReturnType(), localGenericTypes);
    if (retType == nullptr) {
      retType = funcType->getReturnType();
    }
    result = std::make_unique<Value>(materializeForCallSite(retType, importedScope));
  }
  if (callee->getName() != "new" && !funcType->getGenericParams().empty()) {
    node->setGenericBindingEnv(localGenericTypes);
  }
  verifyGenericTraitBounds(callee, localGenericTypes, node->getSpan());
}

auto Typechecker::visit(const LambdaExpr* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* genericsScope = nullptr;
  SymbolTable* savedScopeBeforeGenerics = scope;
  if (!node->getGenericParamDecls().empty()) {
    genericsScope = scope->createChildBlock("lambda_generics");
    scope = genericsScope;
    for (const auto& param : node->getGenericParamDecls()) {
      auto* genericType = cacheType(std::make_unique<Type>(param.name));
      currentGenericTypes[param.name] = genericType;
    }
    insertGenericParamSymbols(genericsScope, node->getGenericParamDecls(), currentGenericTypes,
                              mainFilePath);
  }

  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  for (Parameter* param : node->getParameters()) {
    if (param->defaultVal != nullptr) {
      throw TypeCheckError(param->nameSpan, "Lambda parameters cannot have default values");
    }
    if (param->type == nullptr) {
      throw TypeCheckError(param->nameSpan, "Lambda parameter {} requires an explicit type",
                           param->name);
    }
    param->type->accept(*this);
    Type* paramType = typeAsPtrIfClassForOverload(result->getType());
    paramTypes.push_back(paramType);
    paramFields.push_back(std::make_unique<Field>(param->name, paramType));
  }

  Type* returnType = nullptr;
  if (node->getReturnType() != nullptr) {
    node->getReturnType()->accept(*this);
    returnType = wrapReturnTypeIfNominal(result->getType());
  }

  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  if (returnType != nullptr) {
    funcType->setReturnType(returnType);
  }
  Type* funcTypePtr = cacheType(std::move(funcType));
  funcTypePtr->setGenericParams(node->getGenericParams());
  if (!node->getGenericParamDecls().empty()) {
    std::vector<std::vector<std::string>> tb;
    tb.reserve(node->getGenericParamDecls().size());
    for (const auto& p : node->getGenericParamDecls()) {
      tb.push_back(p.traitBounds);
    }
    funcTypePtr->setGenericParamTraitBounds(std::move(tb));
  }

  auto lambdaName = fmt::format("__lambda_{}", lambdaCounter++);
  auto lambdaSymbol = std::make_unique<Value>(lambdaName, funcTypePtr);
  lambdaSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
  lambdaSymbol->setDeclarationKind(ValueDeclarationKind::FUNCTION);
  lambdaSymbol->setDeclarationSpan(node->getSpan());
  lambdaSymbol->setDeclarationFilePath(mainFilePath);
  SymbolTable* lambdaBodyScope = scope->createChildBlock("lambda");
  lambdaSymbol->setBodyScope(lambdaBodyScope);
  lambdaSymbol->clearClosureCaptureOuters();
  savedScopeBeforeGenerics->insertSymbol(std::move(lambdaSymbol));
  Value* lambdaSymbolPtr = savedScopeBeforeGenerics->lookup(lambdaName);
  if (lambdaSymbolPtr == nullptr) {
    throw TypeCheckError(node->getSpan(), "Internal error creating lambda symbol");
  }
  node->setResolvedSymbol(lambdaSymbolPtr);

  SymbolTable* savedScope = scope;
  scope = lambdaBodyScope;
  for (size_t i = 0; i < node->getParameters().size(); ++i) {
    Parameter* param = node->getParameters()[i];
    auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[i]);
    paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    paramSymbol->setDeclarationKind(ValueDeclarationKind::PARAMETER);
    paramSymbol->setDeclarationSpan(param->nameSpan);
    paramSymbol->setDeclarationFilePath(mainFilePath);
    if (paramTypes[i] != nullptr && paramTypes[i]->is(BaseType::TY_FUNCTION)) {
      paramSymbol->setStoresFuncValuePair(true);
    }
    param->setResolvedSymbol(paramSymbol.get());
    scope->insertSymbol(std::move(paramSymbol));
  }

  Value* savedFunction = currentFunction;
  bool const savedTopLevel = inTopLevel;
  currentFunction = lambdaSymbolPtr;
  functionScopeStack.push_back(scope);
  inTopLevel = false;
  auto savedTraitBounds = currentGenericParamTraitBounds;
  currentGenericParamTraitBounds.clear();
  for (const auto& p : node->getGenericParamDecls()) {
    currentGenericParamTraitBounds[p.name] = p.traitBounds;
  }
  if (node->isExpressionBody()) {
    if (returnType != nullptr) {
      visitExprWithExpectedType(node->getExpressionBody(), returnType);
    } else {
      node->getExpressionBody()->accept(*this);
      returnType = wrapReturnTypeIfNominal(result->getType());
      lambdaSymbolPtr->getType()->setReturnType(returnType);
    }
  } else if (!declarationPass && node->getBlockBody() != nullptr) {
    if (returnType == nullptr) {
      returnType = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
      lambdaSymbolPtr->getType()->setReturnType(returnType);
    }
    node->getBlockBody()->accept(*this);
    if (returnType != nullptr && !returnType->is(BaseType::TY_VOID) &&
        !blockAlwaysReturns(node->getBlockBody())) {
      throw TypeCheckError(node->getSpan(), "Non-void lambda may reach end without returning");
    }
  } else if (returnType == nullptr) {
    returnType = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
    lambdaSymbolPtr->getType()->setReturnType(returnType);
  }

  currentGenericParamTraitBounds = std::move(savedTraitBounds);
  functionScopeStack.pop_back();
  currentFunction = savedFunction;
  inTopLevel = savedTopLevel;
  scope = savedScope;
  if (genericsScope != nullptr) {
    scope = genericsScope->getParent();
    currentGenericTypes = std::move(savedGenerics);
  }
  if (!node->getGenericParamDecls().empty() &&
      !lambdaSymbolPtr->getClosureCaptureOuters().empty()) {
    throw TypeCheckError(node->getSpan(),
                         "Generic lambdas that capture outer variables are not supported yet");
  }
  result = std::make_unique<Value>(*lambdaSymbolPtr);
}

auto Typechecker::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  std::unique_ptr<Value> left = std::move(result);
  node->getRight()->accept(*this);
  std::unique_ptr<Value> right = std::move(result);
  Type* resultType = typecheckBinaryOpResult(node->getOperator(), left->getType(), right->getType(),
                                             node->getSpan());
  auto extractConstantShiftCount = [](const Expression* expr) -> std::optional<long long> {
    if (auto const* lit = dynamic_cast<const Literal*>(expr)) {
      if (lit->getType() == TokenType::INTEGER) {
        try {
          return std::stoll(lit->getValue());
        } catch (...) {
          throw TypeCheckError(expr->getSpan(), "Invalid shift count literal");
        }
      }
      return std::nullopt;
    }
    if (auto const* unary = dynamic_cast<const UnaryOp*>(expr)) {
      if (unary->getOperator() != TokenType::MINUS) {
        return std::nullopt;
      }
      auto const* lit = dynamic_cast<const Literal*>(unary->getExpression());
      if (lit == nullptr || lit->getType() != TokenType::INTEGER) {
        return std::nullopt;
      }
      try {
        return -std::stoll(lit->getValue());
      } catch (...) {
        throw TypeCheckError(expr->getSpan(), "Invalid shift count literal");
      }
    }
    return std::nullopt;
  };
  if ((node->getOperator() == TokenType::SLASH || node->getOperator() == TokenType::MOD) &&
      left->getType() != nullptr && right->getType() != nullptr &&
      !left->getType()->is(BaseType::TY_GENERIC) && !right->getType()->is(BaseType::TY_GENERIC)) {
    auto* zeroLit = dynamic_cast<Literal*>(node->getRight());
    if (zeroLit != nullptr && zeroLit->getType() == TokenType::INTEGER &&
        zeroLit->getValue() == "0") {
      Type* unified = getExtendedType(left->getType(), right->getType());
      if (unified != nullptr && unified->is(BaseType::TY_INT)) {
        throw TypeCheckError(node->getSpan(), "Division or remainder by zero");
      }
    }
  }
  if ((node->getOperator() == TokenType::SHIFT_LEFT ||
       node->getOperator() == TokenType::SHIFT_RIGHT) &&
      resultType != nullptr && resultType->is(BaseType::TY_INT) && left->getType() != nullptr &&
      right->getType() != nullptr && !left->getType()->is(BaseType::TY_GENERIC) &&
      !right->getType()->is(BaseType::TY_GENERIC)) {
    if (auto count = extractConstantShiftCount(node->getRight()); count.has_value()) {
      if (*count < 0 || static_cast<unsigned long long>(*count) >= resultType->getIntWidth()) {
        throw TypeCheckError(node->getSpan(), "Shift count {} is out of range for {}-bit integer",
                             *count, resultType->getIntWidth());
      }
    }
  }
  result = std::make_unique<Value>(resultType);
}

auto Typechecker::visit(const SubscriptOp* node) -> void {
  node->setLspFlowSensitiveType(nullptr);
  auto applyFlowNarrowing = [this, node]() {
    if (result != nullptr) {
      if (Type* narrowed = lookupUnionNarrowedType(node)) {
        result->setType(narrowed);
        node->setLspFlowSensitiveType(narrowed);
      }
    }
  };
  node->getLeft()->accept(*this);
  Type* baseType = result->getType();
  node->getIndex()->accept(*this);
  Type* indexType = result->getType();
  if (baseType != nullptr && baseType->is(BaseType::TY_TUPLE)) {
    auto* idxLit = dynamic_cast<Literal*>(node->getIndex());
    if (idxLit == nullptr || idxLit->getType() != TokenType::INTEGER) {
      throw TypeCheckError(node->getIndex()->getSpan(),
                           "Tuple index must be a non-negative integer literal");
    }
    long long idxVal = 0;
    try {
      idxVal = std::stoll(idxLit->getValue());
    } catch (...) {
      throw TypeCheckError(node->getIndex()->getSpan(), "Invalid tuple index literal");
    }
    if (idxVal < 0) {
      throw TypeCheckError(node->getIndex()->getSpan(), "Tuple index must be non-negative");
    }
    std::vector<Field*> const fields = baseType->getFields();
    auto const idx = static_cast<size_t>(idxVal);
    if (idx >= fields.size()) {
      throw TypeCheckError(node->getSpan(), "Invalid index on type {}", baseType->toString());
    }
    if (indexType == nullptr || !indexType->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getIndex()->getSpan(), "Tuple index must be int, got {}",
                           indexType != nullptr ? indexType->toString() : "unknown");
    }
    result = std::make_unique<Value>(fields[idx]->type);
    applyFlowNarrowing();
    return;
  }
  if (baseType != nullptr && baseType->is(BaseType::TY_ARRAY) &&
      baseType->getElementType() != nullptr) {
    if (indexType == nullptr || !indexType->is(BaseType::TY_INT)) {
      throw TypeCheckError(node->getIndex()->getSpan(), "List index must be int, got {}",
                           indexType != nullptr ? indexType->toString() : "unknown");
    }
    if (auto* idxLit = dynamic_cast<Literal*>(node->getIndex());
        idxLit != nullptr && idxLit->getType() == TokenType::INTEGER) {
      long long idxVal = 0;
      try {
        idxVal = std::stoll(idxLit->getValue());
      } catch (...) {
        throw TypeCheckError(node->getIndex()->getSpan(), "Invalid list index literal");
      }
      if (idxVal < 0) {
        throw TypeCheckError(node->getIndex()->getSpan(), "List index must be non-negative");
      }
    }
    result = std::make_unique<Value>(baseType->getElementType());
    applyFlowNarrowing();
    return;
  }
  Type* overloadType = resolveMethodReturnType(
      baseType, std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexType}, node->getSpan());
  if (overloadType == nullptr) {
    throw TypeCheckError(node->getSpan(), "Subscript requires an array type or operator [], got {}",
                         baseType != nullptr ? baseType->toString() : "unknown");
  }
  result = std::make_unique<Value>(overloadType);
  applyFlowNarrowing();
}

void Typechecker::typecheckDotOpSuperDispatch(const DotOp* node) {
  if (currentClassType == nullptr || currentClassType->getClassSuperclass() == nullptr) {
    throw TypeCheckError(node->getSpan(),
                         "`super` is only valid in a method of a class that has a superclass");
  }
  auto* fc = dynamic_cast<FuncCall*>(node->getRight());
  if (fc == nullptr) {
    throw TypeCheckError(node->getSpan(), "`super` must be used in a call: super.method(...)");
  }
  Type* superTy = currentClassType->getClassSuperclass();
  std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
  SymbolTable* insertScope =
      currentMethodInsertScope != nullptr ? currentMethodInsertScope : scope->getParent();
  if (insertScope == nullptr) {
    insertScope = scope;
  }
  const std::unordered_map<std::string, Type*>* superSeed = nullptr;
  if (auto envIt = specializedTypeEnv.find(superTy); envIt != specializedTypeEnv.end()) {
    superSeed = &envIt->second;
  }
  Value* method = lookupSuperDispatchMethodInScopeThenImports(insertScope, superTy, fc->getName(),
                                                              argTypes, superSeed);
  if (method == nullptr) {
    throw TypeCheckError(node->getSpan(), "Super method not found: {}", fc->getName());
  }
  fc->setResolvedSymbol(method);
  fc->setSuperDispatch(true);
  enforcePrivateMemberReadable(node->getSpan(), method);
  markValueRead(method);
  std::unordered_map<std::string, Type*> methodTypeEnv;
  if (auto specializedIt = specializedTypeEnv.find(superTy);
      specializedIt != specializedTypeEnv.end()) {
    methodTypeEnv = specializedIt->second;
  }
  finalizeResolvedMethodCallTyping(fc, method, methodTypeEnv, argTypes, node->getSpan());
}

void Typechecker::typecheckDotOpClassOrEnumMemberAccess(const DotOp* node, Type* base,
                                                        bool dotLeftDenotesTypeName) {
  if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
    if (isStdListClassType(base) && isMutatingListFunction(fc->getName()) &&
        !isMutableListReceiver(node->getLeft())) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call mutating list method {} on immutable value", fc->getName());
    }
    if (isStdListClassType(base) && fc->getName() == "pop") {
      if (Type* elemType = getStdListElementType(base);
          elemType != nullptr && isNullableType(elemType)) {
        throw TypeCheckError(node->getSpan(),
                             "pop() does not support nullable list element type {} because nil "
                             "marks an empty list",
                             elemType->toString());
      }
    }
    std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
    Type* receiverForLookup = base;
    auto templateIt = specializedTypeToTemplate.find(base);
    if (templateIt != specializedTypeToTemplate.end()) {
      receiverForLookup = templateIt->second;
    }
    std::vector<Type*> methodArgTypes;
    bool const typeNameReceiver =
        dotLeftDenotesTypeName &&
        receiverForLookup->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM});
    if (typeNameReceiver && fc->getName() != "new") {
      methodArgTypes = argTypes;
    } else {
      Type* selfType = receiverForLookup;
      if (receiverForLookup->is(BaseType::TY_CLASS)) {
        selfType = receiverForLookup->is(BaseType::TY_PTR)
                       ? receiverForLookup
                       : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr,
                                                          receiverForLookup));
      }
      methodArgTypes.push_back(selfType);
      methodArgTypes.insert(methodArgTypes.end(), argTypes.begin(), argTypes.end());
    }
    std::unordered_map<std::string, Type*> methodTypeEnv;
    auto specializedIt = specializedTypeEnv.find(base);
    if (specializedIt != specializedTypeEnv.end()) {
      methodTypeEnv = specializedIt->second;
    }
    Value* method = lookupFunctionInScopeThenImportedModuleCaches(fc->getName(), methodArgTypes);
    if (method == nullptr) {
      method = tryLookupFunctionViaDotImportLiterals(node, fc->getName(), methodArgTypes);
    }
    if (method == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found: {}", fc->getName());
    }
    if (typeNameReceiver && fc->getName() != "new" && !method->isStaticMethod()) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot call instance method `{}` on a type name; call it on a value, "
                           "or declare the method `static`",
                           fc->getName());
    }
    fc->setResolvedSymbol(method);
    enforcePrivateMemberReadable(node->getSpan(), method);
    markValueRead(method);
    Type* methodType = method->getType();
    finalizeResolvedMethodCallTyping(
        fc, method, methodTypeEnv, methodArgTypes, node->getSpan(),
        [&](std::unordered_map<std::string, Type*>& subs) {
          if (typeNameReceiver && fc->getName() != "new" && method->isStaticMethod() &&
              receiverForLookup->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
            inferClassTemplateParamsForStaticMethodCallOnTemplate(
                node->getSpan(), receiverForLookup, methodType, methodArgTypes, subs);
          }
        });
    return;
  }
  auto* rightLit = dynamic_cast<Literal*>(node->getRight());
  if (rightLit == nullptr || rightLit->getType() != TokenType::IDENTIFIER) {
    throw TypeCheckError(node->getSpan(), "Expected field name or method call after dot");
  }
  Field* staticPart = nullptr;
  if (base->is(BaseType::TY_CLASS) && dotLeftDenotesTypeName) {
    staticPart = TypeUtils::findStaticFieldInClass(base, rightLit->getValue());
  }
  if (base->is(BaseType::TY_ENUM) && dotLeftDenotesTypeName) {
    EnumVariant* variant = TypeUtils::findEnumVariant(base, rightLit->getValue());
    if (variant == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unknown enum variant: {}", rightLit->getValue());
    }
    if (!variant->payloadTypes.empty()) {
      throw TypeCheckError(node->getSpan(),
                           "Payload-bearing enum variant '{}' must be constructed with arguments",
                           rightLit->getValue());
    }
    if (Field* variantField = TypeUtils::findFieldInFields(base, rightLit->getValue());
        variantField != nullptr && variantField->getDeclarationSymbol() != nullptr) {
      rightLit->setResolvedSymbol(variantField->getDeclarationSymbol());
      markValueRead(variantField->getDeclarationSymbol());
    }
    Type* enumResultType = base;
    if (!base->getGenericParams().empty()) {
      if (Type* expected = currentExpectedType(); expected != nullptr) {
        Type* expectedShape = expected;
        if (expectedShape->is(BaseType::TY_PTR) && expectedShape->getElementType() != nullptr) {
          expectedShape = expectedShape->getElementType();
        }
        if (expectedShape->is(BaseType::TY_ENUM)) {
          if (auto templateIt = specializedTypeToTemplate.find(expectedShape);
              templateIt != specializedTypeToTemplate.end() && templateIt->second->isEqual(base)) {
            enumResultType = expectedShape;
          }
        }
      }
    }
    result = std::make_unique<Value>(enumResultType);
    return;
  }
  Field* field = staticPart;
  if (field == nullptr) {
    field = TypeUtils::findFieldInFields(base, rightLit->getValue());
  }
  if (field == nullptr || field->type == nullptr) {
    throw TypeCheckError(node->getSpan(), "Unknown field: {}", rightLit->getValue());
  }
  if (dotLeftDenotesTypeName && base->is(BaseType::TY_CLASS) && staticPart == nullptr) {
    throw TypeCheckError(node->getSpan(),
                         "Cannot access instance field `{}` on a type name; use a value, "
                         "or declare the field `static`",
                         rightLit->getValue());
  }
  if (field->getDeclarationSymbol() != nullptr) {
    rightLit->setResolvedSymbol(field->getDeclarationSymbol());
    enforcePrivateMemberReadable(node->getSpan(), field->getDeclarationSymbol());
    markValueRead(field->getDeclarationSymbol());
  }
  result = std::make_unique<Value>(field->type);
}

auto Typechecker::visit(const DotOp* node) -> void {
  node->setLspFlowSensitiveType(nullptr);
  if (dynamic_cast<const SuperExpr*>(node->getLeft()) != nullptr) {
    typecheckDotOpSuperDispatch(node);
    return;
  }

  auto applyFlowNarrowing = [this, node]() {
    node->setLspFlowSensitiveType(nullptr);
    if (result != nullptr) {
      if (Type* narrowed = lookupUnionNarrowedType(node)) {
        result->setType(narrowed);
        node->setLspFlowSensitiveType(narrowed);
      }
    }
  };

  node->getLeft()->accept(*this);
  bool dotLeftDenotesTypeName = result->getCategory() == ValueCategory::TYPE_SYMBOL;
  Type* base = result->getType();
  tryPeelDotReceiverFromNamedImportStub(node, base, dotLeftDenotesTypeName);
  if (base == nullptr) {
    throw TypeCheckError(node->getSpan(), "Dot operator on unknown type");
  }
  if (base->is(BaseType::TY_VOID)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      visitCallArgumentsIgnoringResult(fc);
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_IMPORT)) {
    handleDotOpTyImportReceiver(node);
    return;
  }
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (base->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
      Type* retType = resolveMethodReturnType(base, fc->getName(), argTypes, node->getSpan());
      if (retType == nullptr) {
        throw TypeCheckError(node->getSpan(), "Method '{}' not found on trait '{}'", fc->getName(),
                             base->getDisplayName());
      }
      fc->setResolvedSymbol(nullptr);
      result = std::make_unique<Value>(retType);
      applyFlowNarrowing();
      return;
    }
    throw TypeCheckError(node->getSpan(), "Expected method call after dot on trait value");
  }
  if (base->is(BaseType::TY_UNION)) {
    auto* fc = dynamic_cast<FuncCall*>(node->getRight());
    if (fc == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Dot on a union requires a method call (fields are not supported)");
    }
    typecheckDotUnionMethodCall(base, node, fc);
    applyFlowNarrowing();
    return;
  }
  if (base->is(BaseType::TY_ARRAY)) {
    if (auto* call = dynamic_cast<FuncCall*>(node->getRight())) {
      if (visitListMethodCall(base, node, call)) {
        applyFlowNarrowing();
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
        std::vector<Type*> argTypes = overloadArgTypesFromCall(fc);
        for (const std::string& traitName : bit->second) {
          auto trIt = traitMethodSignatures.find(traitName);
          if (trIt == traitMethodSignatures.end()) {
            continue;
          }
          auto mIt = trIt->second.find(fc->getName());
          if (mIt == trIt->second.end() || mIt->second.empty()) {
            continue;
          }
          // Receiver is generic `T`; formal self is ptr(trait). Match overloads on args only.
          if (Type* matched = selectBestFunctionTypeMatchTail(mIt->second, argTypes);
              matched != nullptr) {
            fc->setResolvedSymbol(nullptr);
            result = std::make_unique<Value>(matched->getReturnType());
            applyFlowNarrowing();
            return;
          }
        }
        throw TypeCheckError(node->getSpan(),
                             "Method '{}' not found on generic parameter '{}' via trait bounds",
                             fc->getName(), gname);
      }
    }
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    throw TypeCheckError(node->getSpan(), "Dot operator requires class or enum type, got {}",
                         base->toString());
  }
  typecheckDotOpClassOrEnumMemberAccess(node, base, dotLeftDenotesTypeName);
  applyFlowNarrowing();
}

auto Typechecker::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* from = result->getType();
  node->getType()->accept(*this);
  Type* to = result->getType();
  if (!isAssignableTo(from, to) && !(from != nullptr && from->is(BaseType::TY_ANY) &&
                                     to != nullptr && !to->is(BaseType::TY_VOID))) {
    throw TypeCheckError(node->getSpan(), "Cannot cast from {} to {}", from->toString(),
                         to->toString());
  }
  result = std::make_unique<Value>(to);
}

auto Typechecker::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  Type* lhsTy = result->getType();
  Type* rhsTy = resolveType(node->getRight());
  node->setResolvedRhsType(rhsTy);
  if (lhsTy != nullptr && lhsTy->is(BaseType::TY_ANY)) {
    if (rhsTy != nullptr && rhsTy->is(BaseType::TY_VOID)) {
      throw TypeCheckError(node->getSpan(), "`is` cannot test `any` against void");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    return;
  }
  if (lhsTy != nullptr && lhsTy->is(BaseType::TY_UNION)) {
    bool found = false;
    for (Type* m : lhsTy->getUnionMembers()) {
      if (m->isEqual(rhsTy)) {
        found = true;
        break;
      }
    }
    if (!found) {
      throw TypeCheckError(node->getSpan(), "`is` type {} is not a member of union {}",
                           rhsTy->toString(), lhsTy->toString());
    }
  }
  result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
}

auto Typechecker::visit(const MatchExpr* node) -> void {
  node->getScrutinee()->accept(*this);
  Type* scrutineeType = result->getType();
  if (scrutineeType == nullptr) {
    throw TypeCheckError(node->getSpan(), "match requires a scrutinee with a known type");
  }
  bool const enumScrutinee = scrutineeType->is(BaseType::TY_ENUM);
  std::string enumBaseName;
  if (enumScrutinee) {
    Type* templateEnumType = scrutineeType;
    if (auto it = specializedTypeToTemplate.find(scrutineeType); it != specializedTypeToTemplate.end()) {
      templateEnumType = it->second;
    }
    enumBaseName = templateEnumType->getDisplayName();
    if (enumBaseName.empty()) {
      enumBaseName = scrutineeType->getDisplayName();
    }
    if (size_t anglePos = enumBaseName.find('<'); anglePos != std::string::npos) {
      enumBaseName = enumBaseName.substr(0, anglePos);
    }
  }

  const auto& arms = node->getArms();
  if (arms.empty()) {
    throw TypeCheckError(node->getSpan(), "match requires at least one arm");
  }

  std::vector<bool> seenVariants(enumScrutinee ? scrutineeType->getEnumVariants().size() : 0U, false);
  bool catchAllSeen = false;
  bool usesEnumDispatch = enumScrutinee;
  Type* expectedType = currentExpectedType();
  Type* matchResultType = nullptr;
  std::function<bool(const Expression*)> expressionFallsThrough = [&](const Expression* expr) {
    if (expr == nullptr) {
      return true;
    }
    if (expressionCallsStdlibBaseLesExit(const_cast<Expression*>(expr))) {
      return false;
    }
    if (auto const* blockExpr = dynamic_cast<const BlockExpr*>(expr)) {
      bool bodyFallsThrough = blockExpr->getBody() == nullptr ||
                              pathLeadsToEndWithoutReturn(blockExpr->getBody()->getChildren(), 0);
      if (!bodyFallsThrough) {
        return false;
      }
      return expressionFallsThrough(blockExpr->getTailExpr());
    }
    if (auto const* matchExpr = dynamic_cast<const MatchExpr*>(expr)) {
      for (const MatchArm& nestedArm : matchExpr->getArms()) {
        if (expressionFallsThrough(nestedArm.body.get())) {
          return true;
        }
      }
      return false;
    }
    return true;
  };

  for (size_t armIndex = 0; armIndex < arms.size(); ++armIndex) {
    const MatchArm& arm = arms[armIndex];
    SymbolTable* savedScope = scope;
    SymbolTable* armScope = scope->createChildBlock("match_arm");
    scope = armScope;

    const MatchPattern& pattern = arm.pattern;
    if (catchAllSeen) {
      throw TypeCheckError(pattern.span, "No match arms are allowed after a catch-all arm");
    }

    if (pattern.kind == MatchPatternKind::WILDCARD || pattern.kind == MatchPatternKind::ELSE_) {
      if (!pattern.bindings.empty()) {
        throw TypeCheckError(pattern.span, "Wildcard match arm cannot bind payload names");
      }
      if (pattern.valueExpr != nullptr) {
        throw TypeCheckError(pattern.span, "Catch-all match arm cannot have a value pattern");
      }
      if (catchAllSeen) {
        throw TypeCheckError(pattern.span, "Duplicate catch-all match arm");
      }
      if (armIndex + 1U != arms.size()) {
        throw TypeCheckError(pattern.span, "Catch-all match arm must be the final arm");
      }
      catchAllSeen = true;
      usesEnumDispatch = usesEnumDispatch && enumScrutinee;
    } else if (pattern.kind == MatchPatternKind::VARIANT) {
      if (!enumScrutinee) {
        throw TypeCheckError(pattern.span, "Enum variant patterns require an enum scrutinee");
      }
      if (!pattern.enumName.empty() && pattern.enumName != enumBaseName) {
        throw TypeCheckError(pattern.span, "Match arm enum {} does not match scrutinee enum {}",
                             pattern.enumName, enumBaseName);
      }
      int variantIndex = TypeUtils::findIndexInEnumVariants(scrutineeType, pattern.variantName);
      if (variantIndex < 0) {
        throw TypeCheckError(pattern.span, "Unknown enum variant {} for {}",
                             pattern.variantName, scrutineeType->toString());
      }
      if (seenVariants[static_cast<size_t>(variantIndex)]) {
        throw TypeCheckError(pattern.span, "Duplicate match arm for variant {}",
                             pattern.variantName);
      }
      seenVariants[static_cast<size_t>(variantIndex)] = true;
      EnumVariant* variant = scrutineeType->getEnumVariants()[static_cast<size_t>(variantIndex)];
      if (variant == nullptr) {
        throw TypeCheckError(pattern.span, "Invalid enum variant {}", pattern.variantName);
      }
      if (pattern.bindings.size() != variant->payloadTypes.size()) {
        throw TypeCheckError(pattern.span,
                             "Match arm for variant {} expects {} payload binding(s), got {}",
                             pattern.variantName, variant->payloadTypes.size(),
                             pattern.bindings.size());
      }
      for (size_t i = 0; i < pattern.bindings.size(); ++i) {
        if (pattern.bindings[i] == "_") {
          continue;
        }
        auto bindingSym = std::make_unique<Value>(pattern.bindings[i], variant->payloadTypes[i]);
        bindingSym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
        bindingSym->setDeclarationKind(ValueDeclarationKind::VARIABLE);
        bindingSym->setDeclarationSpan(pattern.span);
        bindingSym->setDeclarationFilePath(mainFilePath);
        warnShadowingFromEnclosing(pattern.bindings[i], pattern.span);
        scope->insertSymbol(std::move(bindingSym));
      }
      pattern.resolvedEnumType = scrutineeType;
      pattern.resolvedVariantIndex = static_cast<unsigned>(variantIndex);
    } else {
      usesEnumDispatch = false;
      if (!pattern.bindings.empty()) {
        throw TypeCheckError(pattern.span, "Value match arms cannot bind payload names");
      }
      if (pattern.valueExpr == nullptr) {
        throw TypeCheckError(pattern.span, "Value match arm is missing its pattern expression");
      }
      visitExprWithExpectedType(pattern.valueExpr.get(), scrutineeType);
      Type* patternType = result->getType();
      if (patternType == nullptr) {
        throw TypeCheckError(pattern.span, "Match pattern value has unknown type");
      }
      (void) typecheckBinaryOpResult(TokenType::EQUAL_EQUAL, scrutineeType, patternType,
                                     pattern.valueExpr->getSpan());
      pattern.resolvedValueType = patternType;
    }

    visitExprWithExpectedType(arm.body.get(), expectedType);
    Type* armType = result->getType();
    scope = savedScope;
    bool armFallsThrough = expressionFallsThrough(arm.body.get());

    if (armType == nullptr && armFallsThrough) {
      throw TypeCheckError(arm.body->getSpan(), "Match arm has unknown result type");
    }
    if (!armFallsThrough) {
      continue;
    }
    if (expectedType != nullptr) {
      if (!isAssignableTo(armType, expectedType)) {
        throw TypeCheckError(arm.body->getSpan(),
                             "Match arm type {} is not assignable to expected type {}",
                             armType->toString(), expectedType->toString());
      }
      matchResultType = expectedType;
      continue;
    }
    if (matchResultType == nullptr) {
      matchResultType = armType;
      continue;
    }
    if (Type* unified = getExtendedType(matchResultType, armType); unified != nullptr) {
      matchResultType = unified;
      continue;
    }
    if (isAssignableTo(armType, matchResultType)) {
      continue;
    }
    if (isAssignableTo(matchResultType, armType)) {
      matchResultType = armType;
      continue;
    }
    throw TypeCheckError(arm.body->getSpan(), "Match arms must have a common type, got {} and {}",
                         matchResultType->toString(), armType->toString());
  }

  if (enumScrutinee && usesEnumDispatch && !catchAllSeen) {
    for (size_t i = 0; i < seenVariants.size(); ++i) {
      if (!seenVariants[i]) {
        EnumVariant* variant = scrutineeType->getEnumVariants()[i];
        throw TypeCheckError(node->getSpan(), "Non-exhaustive match; missing variant {}",
                             variant != nullptr ? variant->name : "<unknown>");
      }
    }
  } else if (!catchAllSeen) {
    throw TypeCheckError(node->getSpan(), "Non-exhaustive match requires an else arm");
  }

  if (matchResultType == nullptr) {
    matchResultType = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  }
  node->setUsesEnumDispatch(usesEnumDispatch);
  node->setResolvedType(matchResultType);
  result = std::make_unique<Value>(matchResultType);
}

auto Typechecker::visit(const BlockExpr* node) -> void {
  if (node->getBody() != nullptr) {
    node->getBody()->accept(*this);
  }
  bool const bodyFallsThrough =
      node->getBody() == nullptr || pathLeadsToEndWithoutReturn(node->getBody()->getChildren(), 0);
  if (bodyFallsThrough && node->getTailExpr() != nullptr) {
    visitExprWithExpectedType(node->getTailExpr(), currentExpectedType());
    node->setResolvedType(result != nullptr ? result->getType() : nullptr);
    return;
  }

  Type* blockType = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  node->setResolvedType(blockType);
  result = std::make_unique<Value>(blockType);
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
    if (operand == nullptr || (!operand->is(BaseType::TY_INT) && !operand->isFloatingPoint())) {
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
    if (operand->getElementType() == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Cannot dereference pointer without a known pointee type");
    }
    result = std::make_unique<Value>(operand->getElementType());
    break;
  case TokenType::TILDE:
    if (operand != nullptr && operand->is(BaseType::TY_GENERIC)) {
      result = std::make_unique<Value>(operand);
      break;
    }
    if (operand == nullptr || !operand->is(BaseType::TY_INT)) {
      if (Type* overloadedType = tryOverload(); overloadedType != nullptr) {
        result = std::make_unique<Value>(overloadedType);
        break;
      }
      throw TypeCheckError(node->getSpan(), "Bitwise not requires integer type");
    }
    result = std::make_unique<Value>(operand);
    break;
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported unary operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
  }
}

auto Typechecker::visit(const ListLiteral* node) -> void {
  Type* expectedType = currentExpectedType();
  auto getStdListType = [this, node](Type* elementType) -> Type* {
    const auto listPath = std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les")
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

auto Typechecker::visit(const DictLiteral* node) -> void {
  Type* expectedType = currentExpectedType();
  // Return contexts use wrapReturnTypeIfNominal(), so e.g. dict<K,V> may appear as ptr dict<K,V>.
  // Stdlib dict specialization maps live on the class type, not the pointer wrapper.
  Type* dictExpectedShape = expectedType;
  while (dictExpectedShape != nullptr && dictExpectedShape->is(BaseType::TY_PTR) &&
         dictExpectedShape->getElementType() != nullptr &&
         dictExpectedShape->getElementType()->is(BaseType::TY_CLASS)) {
    dictExpectedShape = dictExpectedShape->getElementType();
  }
  auto isStdDictClassType = [this](Type* type) -> bool {
    if (type == nullptr || !type->is(BaseType::TY_CLASS)) {
      return false;
    }
    Type* baseType = type;
    if (auto it = specializedTypeToTemplate.find(type); it != specializedTypeToTemplate.end()) {
      baseType = it->second;
    }
    const std::string& displayName = baseType->getDisplayName();
    return displayName == "dict<K, V>" || displayName == "dict";
  };
  auto getStdDictKeyType = [this, &isStdDictClassType](Type* type) -> Type* {
    if (!isStdDictClassType(type)) {
      return nullptr;
    }
    if (auto it = specializedTypeEnv.find(type); it != specializedTypeEnv.end()) {
      auto envIt = it->second.find("K");
      if (envIt != it->second.end()) {
        return envIt->second;
      }
    }
    return nullptr;
  };
  auto getStdDictValueType = [this, &isStdDictClassType](Type* type) -> Type* {
    if (!isStdDictClassType(type)) {
      return nullptr;
    }
    if (auto it = specializedTypeEnv.find(type); it != specializedTypeEnv.end()) {
      auto envIt = it->second.find("V");
      if (envIt != it->second.end()) {
        return envIt->second;
      }
    }
    return nullptr;
  };
  auto getStdDictType = [this, node](Type* keyType, Type* valueType) -> Type* {
    const auto dictPath = std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les")
                              .lexically_normal();
    SymbolTable* dictScope = getOrTypecheckImport(dictPath.string());
    if (dictScope == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unable to load stdlib dict class");
    }
    Value* dictSymbol = dictScope->lookupStruct("dict");
    Type* dictTemplate = dictSymbol != nullptr
                             ? materializeImportedType(dictSymbol->getType())
                             : materializeImportedType(dictScope->lookupType("dict"));
    if (dictTemplate == nullptr || !dictTemplate->is(BaseType::TY_CLASS)) {
      throw TypeCheckError(node->getSpan(), "Stdlib dict class not found");
    }
    std::unordered_map<std::string, Type*> env = {{"K", keyType}, {"V", valueType}};
    return getOrCreateSpecializedClassType(dictTemplate, dictTemplate->getGenericParams(), env);
  };

  Type* expectedKeyType = nullptr;
  Type* expectedValueType = nullptr;
  if (dictExpectedShape != nullptr) {
    expectedKeyType = getStdDictKeyType(dictExpectedShape);
    expectedValueType = getStdDictValueType(dictExpectedShape);
  }

  std::vector<Expression*> const keys = node->getKeys();
  std::vector<Expression*> const values = node->getValues();
  if (keys.empty()) {
    if (expectedType == nullptr || expectedKeyType == nullptr || expectedValueType == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Empty dict literal requires an explicit dict<K, V> context");
    }
    node->setResolvedType(expectedType);
    result = std::make_unique<Value>(expectedType);
    return;
  }

  if (keys.size() != values.size()) {
    throw TypeCheckError(node->getSpan(), "Dict literal key/value count mismatch");
  }

  Type* keyType = expectedKeyType;
  Type* valueType = expectedValueType;
  for (size_t i = 0; i < keys.size(); ++i) {
    visitExprWithExpectedType(keys[i], expectedKeyType);
    Type* keyT = result->getType();
    if (keyT == nullptr) {
      throw TypeCheckError(keys[i]->getSpan(), "Dict key has unknown type");
    }
    if (expectedKeyType != nullptr) {
      if (!isAssignableTo(keyT, expectedKeyType)) {
        throw TypeCheckError(keys[i]->getSpan(),
                             "Dict key type {} is not assignable to expected type {}",
                             keyT->toString(), expectedKeyType->toString());
      }
    } else if (keyType == nullptr) {
      keyType = keyT;
    } else {
      Type* unifiedKey = getExtendedType(keyType, keyT);
      if (unifiedKey != nullptr) {
        keyType = unifiedKey;
      } else if (!keyT->isEqual(keyType)) {
        throw TypeCheckError(keys[i]->getSpan(), "Dict keys must have a common type, got {} and {}",
                             keyType->toString(), keyT->toString());
      }
    }

    visitExprWithExpectedType(values[i], expectedValueType);
    Type* valT = result->getType();
    if (valT == nullptr) {
      throw TypeCheckError(values[i]->getSpan(), "Dict value has unknown type");
    }
    if (expectedValueType != nullptr) {
      if (!isAssignableTo(valT, expectedValueType)) {
        throw TypeCheckError(values[i]->getSpan(),
                             "Dict value type {} is not assignable to expected type {}",
                             valT->toString(), expectedValueType->toString());
      }
    } else if (valueType == nullptr) {
      valueType = valT;
    } else {
      Type* unifiedVal = getExtendedType(valueType, valT);
      if (unifiedVal != nullptr) {
        valueType = unifiedVal;
      } else if (!valT->isEqual(valueType)) {
        throw TypeCheckError(values[i]->getSpan(),
                             "Dict values must have a common type, got {} and {}",
                             valueType->toString(), valT->toString());
      }
    }
  }

  if (expectedType != nullptr && (getStdDictKeyType(dictExpectedShape) == nullptr ||
                                  getStdDictValueType(dictExpectedShape) == nullptr)) {
    throw TypeCheckError(node->getSpan(), "Dict literal is not compatible with expected type {}",
                         expectedType->toString());
  }

  Type* dictTy = expectedType;
  if (dictTy == nullptr) {
    dictTy = getStdDictType(keyType, valueType);
  }
  node->setResolvedType(dictTy);
  result = std::make_unique<Value>(dictTy);
}

auto Typechecker::visit(const TupleLiteral* node) -> void {
  Type* expectedType = currentExpectedType();
  std::vector<Expression*> const els = node->getElements();
  std::vector<Type*> elemTypes;
  elemTypes.reserve(els.size());
  for (size_t i = 0; i < els.size(); ++i) {
    Type* expectedElem = nullptr;
    if (expectedType != nullptr && expectedType->is(BaseType::TY_TUPLE)) {
      std::vector<Field*> const fs = expectedType->getFields();
      if (i < fs.size()) {
        expectedElem = fs[i]->type;
      }
    }
    visitExprWithExpectedType(els[i], expectedElem);
    Type* t = result->getType();
    if (t == nullptr) {
      throw TypeCheckError(els[i]->getSpan(), "Tuple element has unknown type");
    }
    elemTypes.push_back(t);
  }
  std::vector<std::unique_ptr<Field>> fields;
  std::string displayName = "tuple<";
  for (size_t i = 0; i < elemTypes.size(); ++i) {
    if (i > 0) {
      displayName += ", ";
    }
    displayName += elemTypes[i]->toString();
    fields.push_back(std::make_unique<Field>("_" + std::to_string(i), elemTypes[i]));
  }
  displayName += ">";
  auto tup = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
  tup->setDisplayName(displayName);
  Type* cached = cacheType(std::move(tup));
  if (expectedType != nullptr) {
    if (expectedType->is(BaseType::TY_TUPLE)) {
      std::vector<Field*> const expFields = expectedType->getFields();
      if (expFields.size() != elemTypes.size()) {
        throw TypeCheckError(node->getSpan(),
                             "Tuple literal type {} is not compatible with expected {}",
                             cached->toString(), expectedType->toString());
      }
      for (size_t i = 0; i < elemTypes.size(); ++i) {
        Type* toElem = expFields[i]->type;
        if (toElem == nullptr) {
          continue;
        }
        if (!isAssignableTo(elemTypes[i], toElem)) {
          throw TypeCheckError(els[i]->getSpan(), "Tuple element type {} is not assignable to {}",
                               elemTypes[i]->toString(), toElem->toString());
        }
      }
    } else if (!isAssignableTo(cached, expectedType)) {
      throw TypeCheckError(node->getSpan(),
                           "Tuple literal type {} is not compatible with expected {}",
                           cached->toString(), expectedType->toString());
    }
  }
  node->setResolvedType(cached);
  result = std::make_unique<Value>(cached);
}

auto Typechecker::isStdStrClassType(Type* type) const -> bool {
  if (type == nullptr) {
    return false;
  }
  Type* baseType = type;
  if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr) {
    baseType = type->getElementType();
  }
  if (auto it = specializedTypeToTemplate.find(baseType); it != specializedTypeToTemplate.end()) {
    baseType = it->second;
  }
  return baseType->isBuiltinStringClass();
}

auto Typechecker::getStdStrType(llvm::SMRange span) -> Type* {
  const auto basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  auto const mainNorm =
      mainFilePath.empty()
          ? std::filesystem::path()
          : std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal();
  std::error_code ec;
  if (!mainFilePath.empty() && std::filesystem::equivalent(mainNorm, basePath, ec)) {
    Value* strSym = scope->lookupStruct("str");
    if (strSym != nullptr && strSym->getType()->is(BaseType::TY_CLASS)) {
      return strSym->getType();
    }
    throw TypeCheckError(span, "Stdlib str class not found");
  }
  SymbolTable* baseScope = getOrTypecheckImport(basePath.string());
  if (baseScope == nullptr) {
    throw TypeCheckError(span, "Unable to load stdlib str class");
  }
  Value* strSymbol = baseScope->lookupStruct("str");
  Type* strTemplate = strSymbol != nullptr ? materializeImportedType(strSymbol->getType())
                                           : materializeImportedType(baseScope->lookupType("str"));
  if (strTemplate == nullptr || !strTemplate->is(BaseType::TY_CLASS)) {
    throw TypeCheckError(span, "Stdlib str class not found");
  }
  return strTemplate;
}

auto Typechecker::visit(const SuperExpr* node) -> void {
  throw TypeCheckError(node->getSpan(), "`super` must be used as super.method(...)");
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
  case TokenType::STRING: {
    Type* expected = currentExpectedType();
    if (expected != nullptr && expected->is(BaseType::TY_STRING)) {
      node->setResolvedStrClassType(nullptr);
      result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
      break;
    }
    Type* strClassFromExpected = nullptr;
    if (expected != nullptr) {
      if (isStdStrClassType(expected)) {
        strClassFromExpected =
            expected->is(BaseType::TY_PTR) && expected->getElementType() != nullptr
                ? expected->getElementType()
                : expected;
      }
    }
    if (strClassFromExpected != nullptr) {
      node->setResolvedStrClassType(strClassFromExpected);
      result = std::make_unique<Value>(strClassFromExpected);
      break;
    }
    Type* strClass = getStdStrType(node->getSpan());
    node->setResolvedStrClassType(strClass);
    result = std::make_unique<Value>(strClass);
    break;
  }
  case TokenType::BOOL:
  case TokenType::TRUE_:
  case TokenType::FALSE_:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::NIL:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_NULL)));
    break;
  case TokenType::IDENTIFIER: {
    Value* sym = scope->lookup(node->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unknown name: {}", node->getValue());
    }
    if (currentFunction != nullptr && currentFunction->getName().starts_with("__lambda_")) {
      SymbolTable* foundScope = nullptr;
      for (SymbolTable* s = scope; s != nullptr && foundScope == nullptr; s = s->getParent()) {
        for (Value* candidate : s->getSymbols()) {
          if (candidate == sym) {
            foundScope = s;
            break;
          }
        }
      }
      if (foundScope != nullptr &&
          (sym->getDeclarationKind() == ValueDeclarationKind::VARIABLE ||
           sym->getDeclarationKind() == ValueDeclarationKind::PARAMETER) &&
          foundScope->getParent() != nullptr) {
        bool declaredInsideLambda = false;
        for (SymbolTable* s = foundScope; s != nullptr; s = s->getParent()) {
          if (s == currentFunctionRootScope()) {
            declaredInsideLambda = true;
            break;
          }
        }
        if (!declaredInsideLambda) {
          Value* shadow = getOrCreateLambdaCaptureShadow(sym, node->getValue(), node->getSpan());
          markValueRead(sym);
          node->setResolvedSymbol(shadow);
          result = std::make_unique<Value>(*shadow);
          node->setLspFlowSensitiveType(nullptr);
          if (Type* n = lookupUnionNarrowedType(node)) {
            result->setType(n);
            node->setLspFlowSensitiveType(n);
          }
          break;
        }
      }
    }
    node->setResolvedSymbol(sym);
    markValueRead(sym);
    result = std::make_unique<Value>(*sym);
    node->setLspFlowSensitiveType(nullptr);
    if (Type* n = lookupUnionNarrowedType(node)) {
      result->setType(n);
      node->setLspFlowSensitiveType(n);
    }
    break;
  }
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported literal type: {}",
                         NAMEOF_ENUM(node->getType()));
  }
}

auto Typechecker::isAllowedStringInterpolationExprType(Type* t) const -> bool {
  if (t == nullptr) {
    return false;
  }
  if (t->is(BaseType::TY_STRING)) {
    return true;
  }
  if (t->is(BaseType::TY_INT)) {
    return true;
  }
  if (t->is(BaseType::TY_FLOAT) || t->is(BaseType::TY_FLOAT32)) {
    return true;
  }
  if (t->is(BaseType::TY_BOOL)) {
    return true;
  }
  return isStdStrClassType(t);
}

auto Typechecker::visit(const StringInterpolation* node) -> void {
  const auto& chunks = node->getChunks();
  const std::vector<Expression*> exprs = node->getExprs();
  if (chunks.size() != exprs.size() + 1U) {
    throw TypeCheckError(node->getSpan(),
                         "Malformed string interpolation: expected {} literal segment(s) for {} "
                         "embedded expression(s), got {} segment(s)",
                         exprs.size() + 1, exprs.size(), chunks.size());
  }
  node->clearInterpolatedExprTypes();
  for (Expression* e : exprs) {
    e->accept(*this);
    Type* t = result != nullptr ? result->getType() : nullptr;
    if (!isAllowedStringInterpolationExprType(t)) {
      throw TypeCheckError(e->getSpan(), "Unsupported type in string interpolation: {}",
                           t != nullptr ? t->toString() : "?");
    }
    node->pushInterpolatedExprType(t);
  }
  Type* expected = currentExpectedType();
  if (expected != nullptr && expected->is(BaseType::TY_STRING)) {
    node->setResolvedStrClassType(nullptr);
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    return;
  }
  Type* strClassFromExpected = nullptr;
  if (expected != nullptr) {
    if (isStdStrClassType(expected)) {
      strClassFromExpected = expected->is(BaseType::TY_PTR) && expected->getElementType() != nullptr
                                 ? expected->getElementType()
                                 : expected;
    }
  }
  if (strClassFromExpected != nullptr) {
    node->setResolvedStrClassType(strClassFromExpected);
    result = std::make_unique<Value>(strClassFromExpected);
    return;
  }
  Type* strClass = getStdStrType(node->getSpan());
  node->setResolvedStrClassType(strClass);
  result = std::make_unique<Value>(strClass);
}

auto Typechecker::visit(const TypeExpr* node) -> void {
  Type* type = resolveType(node);
  result = std::make_unique<Value>(type);
}

auto Typechecker::visit(const TraitDecl* node) -> void {
  if (declarationPass) {
    const std::string& traitName = node->getIdentifier();
    if (traitRegistry.contains(traitName)) {
      // registerTraitsFromImportedModule may have pre-registered stdlib traits from a cached parse;
      // compiling that module as main uses a fresh parse — same name, not a user duplicate.
      if (scope->lookupType(traitName) != nullptr) {
        throw TypeCheckError(node->getNameSpan(), "Duplicate trait '{}'", traitName);
      }
    }
    traitRegistry[traitName] = node;

    if (scope->lookupType(traitName) != nullptr) {
      return;
    }
    {
      auto traitType = std::make_unique<Type>(BaseType::TY_TRAIT_EXISTENTIAL, nullptr);
      traitType->setDisplayName(traitName);
      scope->insertType(traitName, std::move(traitType));
    }
    Type* traitTypePtr = scope->lookupType(traitName);
    auto traitSym = std::make_unique<Value>(traitName, traitTypePtr);
    traitSym->setCategory(ValueCategory::TYPE_SYMBOL);
    traitSym->setDeclarationKind(ValueDeclarationKind::TRAIT);
    traitSym->setExported(node->isExported());
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

  traitMethodSignatures[node->getIdentifier()].clear();
  Type* traitTy = scope->lookupType(node->getIdentifier());
  for (FuncDecl* req : node->getRequirements()) {
    if (req->getName() == "new") {
      throw TypeCheckError(req->getNameSpan(), "Traits cannot declare constructor 'new'");
    }
    if (traitTy != nullptr) {
      traitMethodSignatures[node->getIdentifier()][req->getName()].push_back(
          buildMethodFunctionType(req, traitTy));
    }
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
    Type* paramType = typeAsPtrIfClassForOverload(result->getType());
    paramFields.push_back(std::make_unique<Field>(param->name, paramType));
  }
  decl->getReturnType()->accept(*this);
  Type* returnType = wrapReturnTypeIfNominal(result->getType());
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  return cacheType(std::move(funcType));
}

auto Typechecker::registerTraitDefaultMethodSymbol(SymbolTable* insertScope, Type* classType,
                                                   FuncDecl* req) -> void {
  Type* selfPtr = typeAsPtrIfClassForOverload(classType);
  std::vector<Type*> lookupArgs = traitRequirementParamLookupTypes(selfPtr, req);
  if (insertScope->lookupFunction(req->getName(), lookupArgs,
                                  FunctionLookupKind::OVERLOAD_IDENTITY) != nullptr) {
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
  Value* funcSymbol = insertScope->lookupFunction(req->getName(), lookupArgs,
                                                  FunctionLookupKind::OVERLOAD_IDENTITY);
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
  std::unordered_set<std::string> explicitSignatureKeys;
  for (FuncDecl* f : classNode->getMethods()) {
    Value* sym = f->getResolvedSymbol();
    if (sym == nullptr || sym->getType() == nullptr || !sym->getType()->is(BaseType::TY_FUNCTION)) {
      continue;
    }
    std::vector<Type*> lookupArgs;
    for (Field* fld : sym->getType()->getFields()) {
      lookupArgs.push_back(fld->type);
    }
    explicitSignatureKeys.insert(methodLookupSignatureKey(f->getName(), lookupArgs));
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
      Type* selfPtr = typeAsPtrIfClassForOverload(classType);
      std::vector<Type*> lookupArgs = traitRequirementParamLookupTypes(selfPtr, req);
      if (explicitSignatureKeys.contains(methodLookupSignatureKey(req->getName(), lookupArgs))) {
        continue;
      }
      Value* funcSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs,
                                                         FunctionLookupKind::OVERLOAD_IDENTITY);
      if (funcSym == nullptr || funcSym->getBodyScope() == nullptr) {
        continue;
      }
      currentFunction = funcSym;
      scope = funcSym->getBodyScope();
      functionScopeStack.push_back(scope);
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
      functionScopeStack.pop_back();
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
      Type* selfPtr = typeAsPtrIfClassForOverload(classType);
      std::vector<Type*> lookupArgs = traitRequirementParamLookupTypes(selfPtr, req);
      Value* methodSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs,
                                                           FunctionLookupKind::OVERLOAD_IDENTITY);
      if (methodSym == nullptr && req->getBody() != nullptr) {
        registerTraitDefaultMethodSymbol(methodInsertScope, classType, req);
        methodSym = methodInsertScope->lookupFunction(req->getName(), lookupArgs,
                                                      FunctionLookupKind::OVERLOAD_IDENTITY);
      }
      if (methodSym == nullptr) {
        throw TypeCheckError(classNode->getNameSpan(),
                             "Class {} does not implement trait method '{}' required by {}",
                             classNode->getIdentifier(), req->getName(), traitName);
      }
      if (traitName == "Iterable" && req->getName() == "iter") {
        const auto& traitGenParams = trait->getGenericParamDecls();
        if (!traitGenParams.empty()) {
          Type* elemTy = nullptr;
          if (auto it = currentGenericTypes.find(traitGenParams[0].name);
              it != currentGenericTypes.end()) {
            elemTy = it->second;
          }
          Type* retTy = methodSym->getType()->getReturnType();
          Type* iterClass = retTy;
          if (iterClass != nullptr && iterClass->is(BaseType::TY_PTR)) {
            iterClass = iterClass->getElementType();
          }
          if (elemTy != nullptr && iterClass != nullptr && iterClass->is(BaseType::TY_CLASS)) {
            if (isNullableType(elemTy)) {
              throw TypeCheckError(classNode->getNameSpan(),
                                   "Iterable type parameter {} cannot be nullable because "
                                   "Iterator.next() uses nil as the end-of-iteration sentinel",
                                   elemTy->toString());
            }
            Type* iterPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, iterClass));
            Type* nextT = resolveMethodReturnType(iterPtr, "next", {}, classNode->getNameSpan());
            Type* yielded = getOptionalPayloadType(nextT);
            if (yielded == nullptr) {
              throw TypeCheckError(classNode->getNameSpan(),
                                   "Iterable: iter() must return an iterator whose next() returns "
                                   "an optional value, got {}",
                                   nextT != nullptr ? nextT->toString() : "void");
            }
            if (!yielded->isEqual(elemTy)) {
              throw TypeCheckError(classNode->getNameSpan(),
                                   "Iterable: iter() must return a type whose next() matches the "
                                   "Iterable type parameter (expected {}, got {})",
                                   elemTy->toString(), nextT->toString());
            }
          }
        }
      }
      if (!functionTypesMatchForTraitImpl(methodSym->getType(), expected)) {
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
  return std::ranges::any_of(classTy->getImplTraitNames(),
                             [traitName](const std::string& n) -> bool { return n == traitName; });
}

} // namespace lesma
