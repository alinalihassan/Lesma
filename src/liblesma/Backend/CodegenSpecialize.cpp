#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>

#include "Codegen.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;
using namespace llvm;

auto Codegen::bindGenericsFromTypePair(const TypeExpr* declared, lesma::Type* actual,
                                       const std::unordered_set<std::string>& genericNameSet,
                                       std::unordered_map<std::string, lesma::Type*>& env) -> void {
  if (declared == nullptr || actual == nullptr) {
    return;
  }
  if (declared->getType() == TokenType::CUSTOM_TYPE) {
    const std::string name = declared->getLookupName();
    const auto declTypeArgs = declared->getTypeArgs();
    lesma::Type* classActual = actual;
    if (classActual->is(BaseType::TY_PTR) && classActual->getElementType() != nullptr &&
        classActual->getElementType()->is(BaseType::TY_CLASS)) {
      classActual = classActual->getElementType();
    }
    if (!declTypeArgs.empty() && classActual != nullptr && classActual->is(BaseType::TY_CLASS)) {
      const std::string& display = classActual->getDisplayName();
      std::string actualBase = display;
      const auto anglePos = display.find('<');
      if (anglePos != std::string::npos) {
        actualBase = display.substr(0, anglePos);
      }
      if (name == actualBase) {
        if (auto envIt = specializedClassTypeEnvs.find(classActual);
            envIt != specializedClassTypeEnvs.end()) {
          const Class* templateClass = nullptr;
          if (auto git = genericClasses.find(name); git != genericClasses.end()) {
            templateClass = git->second;
          } else if (scope != nullptr) {
            if (auto* sym = scope->lookupStruct(name);
                sym != nullptr && sym->getGenericClassTemplate() != nullptr) {
              templateClass = static_cast<const Class*>(sym->getGenericClassTemplate());
            }
          }
          if (templateClass != nullptr) {
            const std::vector<std::string> templateGenericParams =
                templateClass->getGenericParams();
            if (templateGenericParams.size() == declTypeArgs.size()) {
              for (size_t i = 0; i < declTypeArgs.size(); ++i) {
                if (auto concreteIt = envIt->second.find(templateGenericParams[i]);
                    concreteIt != envIt->second.end() && concreteIt->second != nullptr) {
                  bindGenericsFromTypePair(declTypeArgs[i], concreteIt->second, genericNameSet,
                                           env);
                }
              }
              return;
            }
          }
        }
      }
    }
    if (genericNameSet.contains(name) && !env.contains(name)) {
      env[name] = actual;
      return;
    }
    if (name == "__buffer" && actual->is(BaseType::TY_ARRAY) &&
        actual->getElementType() != nullptr) {
      if (declTypeArgs.size() == 1U) {
        bindGenericsFromTypePair(declTypeArgs.front(), actual->getElementType(), genericNameSet,
                                 env);
      }
    }
    return;
  }
  if (declared->getType() == TokenType::PTR_TYPE && actual->is(BaseType::TY_PTR) &&
      declared->getElementType() != nullptr && actual->getElementType() != nullptr) {
    bindGenericsFromTypePair(declared->getElementType(), actual->getElementType(), genericNameSet,
                             env);
    return;
  }
  if (declared->getType() == TokenType::FUNC_TYPE && actual->is(BaseType::TY_FUNCTION)) {
    if (declared->getReturnType() != nullptr && actual->getReturnType() != nullptr) {
      bindGenericsFromTypePair(declared->getReturnType(), actual->getReturnType(), genericNameSet,
                               env);
    }
    auto declParams = declared->getParams();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < declParams.size() && i < actualFields.size(); ++i) {
      bindGenericsFromTypePair(declParams[i], actualFields[i]->type, genericNameSet, env);
    }
  }
}

auto Codegen::wrapNominalReturnAsPointer(Type* t) -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  if (t->is(BaseType::TY_PTR)) {
    return t;
  }
  if (TypeUtils::passesByPointerInAbi(t)) {
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
  }
  return t;
}

auto Codegen::computeGenericFunctionBindingEnv(const FuncDecl* node,
                                               const std::vector<lesma::Type*>& paramTypes,
                                               const std::vector<std::string>& genericNames,
                                               const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unordered_map<std::string, lesma::Type*> {
  std::unordered_map<std::string, lesma::Type*> env = currentGenericTypes;
  if (!explicitTypeArgs.empty()) {
    if (explicitTypeArgs.size() != genericNames.size()) {
      throw CodegenError(
          node->getSpan(),
          "Explicit type argument count {} does not match generic parameter count {}",
          explicitTypeArgs.size(), genericNames.size());
    }
    for (size_t i = 0; i < genericNames.size(); ++i) {
      env[genericNames[i]] = explicitTypeArgs[i];
    }
  }
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  auto templateParams = node->getParameters();
  size_t offset = (selfSymbol != nullptr) ? 1U : 0U;
  for (size_t i = 0; i < templateParams.size() && (i + offset) < paramTypes.size(); ++i) {
    TypeExpr* declType = templateParams[i]->type.get();
    if (declType != nullptr) {
      bindGenericsFromTypePair(declType, paramTypes[i + offset], genericNameSet, env);
    }
  }
  return env;
}

auto Codegen::appendGenericBindingSuffix(llvm::SMRange span, std::string& base,
                                         const std::vector<std::string>& genericNames,
                                         const std::unordered_map<std::string, lesma::Type*>& env)
    -> void {
  for (const auto& gn : genericNames) {
    auto it = env.find(gn);
    if (it == env.end() || it->second == nullptr) {
      continue;
    }
    getOrCreateLlvmType(it->second);
    base += "|" + MangleUtils::getTypeMangledName(span, it->second);
  }
}

auto Codegen::specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                                 const std::vector<std::string>& genericNames,
                                 const std::vector<lesma::Type*>& explicitTypeArgs)
    -> lesma::Value* {
  auto saved = currentGenericTypes;
  auto env = computeGenericFunctionBindingEnv(node, paramTypes, genericNames, explicitTypeArgs);
  currentGenericTypes = env;

  for (auto* paramType : paramTypes) {
    if (paramType != nullptr) {
      getOrCreateLlvmType(paramType);
    }
  }
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }
  std::string key =
      getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
  appendGenericBindingSuffix(node->getSpan(), key, genericNames, env);
  if (auto it = specializedFunctions.find(key); it != specializedFunctions.end()) {
    currentGenericTypes = std::move(saved);
    return it->second;
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> concreteParamTypes;
  if (selfSymbol != nullptr) {
    fields.push_back(std::make_unique<Field>("self", selfSymbol->getType()));
    concreteParamTypes.push_back(selfSymbol->getType());
  }
  for (auto* param : node->getParameters()) {
    param->type->accept(*this);
    Type* paramT = result->getType();
    if (paramT != nullptr && paramT->is(BaseType::TY_CLASS)) {
      paramT = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramT));
    }
    fields.push_back(std::make_unique<Field>(param->name, paramT));
    concreteParamTypes.push_back(paramT);
  }
  node->getReturnType()->accept(*this);
  Type* returnType = wrapNominalReturnAsPointer(result->getType());
  std::vector<llvm::Type*> paramLLVMTypes;
  for (auto* t : concreteParamTypes) {
    getOrCreateLlvmType(t);
    paramLLVMTypes.push_back(t->getLlvmType());
  }
  getOrCreateLlvmType(returnType);
  auto funcType =
      std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  auto* typePtr = cacheType(std::move(funcType));
  auto mangledName =
      getMangledName(node->getSpan(), node->getName(), concreteParamTypes, selfSymbol != nullptr);
  appendGenericBindingSuffix(node->getSpan(), mangledName, genericNames, env);
  const bool specializationKeysMatch = (mangledName == key);
  auto func = std::make_unique<Value>(node->getName(), typePtr);
  func->setCategory(ValueCategory::CALLABLE_SYMBOL);
  func->setMangledName(mangledName);
  func->setExported(node->isExported());
  auto linkage = node->isExported() ? Function::ExternalLinkage : Function::PrivateLinkage;
  llvm::Type* llvmReturnType = nullptr;
  if (returnType->is(BaseType::TY_PTR) || returnType->is(BaseType::TY_CLASS)) {
    llvmReturnType = builder->getPtrTy();
  } else {
    llvmReturnType = returnType->getLlvmType();
  }
  auto* llvmFuncType = FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
  auto* llvmFunc = Function::Create(llvmFuncType, linkage, mangledName, *theModule);
  typePtr->setLlvmType(llvmFuncType);
  func->setLlvmValue(llvmFunc);
  auto* funcPtr = func.get();
  scope->insertSymbol(std::move(func));
  prototypes.emplace_back(funcPtr, node, selfSymbol);
  specializedFunctions.emplace(std::move(key), funcPtr);
  if (!specializationKeysMatch) {
    specializedFunctions[mangledName] = funcPtr;
  }
  specializationEnvs.emplace(funcPtr, currentGenericTypes);
  currentGenericTypes = std::move(saved);
  return funcPtr;
}

auto Codegen::specializeClass(const Class* node,
                              const std::vector<lesma::Type*>& constructorArgTypes,
                              const std::vector<lesma::Type*>& explicitTypeArgs) -> lesma::Value* {
  auto genericNames = node->getGenericParams();

  const FuncDecl* constructorDecl = nullptr;
  for (auto* method : node->getMethods()) {
    if (method->getName() == "new") {
      constructorDecl = method;
      break;
    }
  }

  std::unordered_map<std::string, lesma::Type*> env;
  if (!explicitTypeArgs.empty()) {
    if (explicitTypeArgs.size() != genericNames.size()) {
      throw CodegenError(node->getSpan(),
                         "Explicit type argument count {} does not match generic class parameter "
                         "count {}",
                         explicitTypeArgs.size(), genericNames.size());
    }
    for (size_t i = 0; i < genericNames.size(); ++i) {
      env[genericNames[i]] = explicitTypeArgs[i];
    }
  }
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  if (constructorDecl != nullptr) {
    auto params = constructorDecl->getParameters();
    for (size_t i = 0; i < params.size() && i < constructorArgTypes.size(); ++i) {
      TypeExpr* declType = params[i]->type.get();
      if (declType != nullptr) {
        bindGenericsFromTypePair(declType, constructorArgTypes[i], genericNameSet, env);
      }
    }
  }

  for (const auto& gn : genericNames) {
    if (!env.contains(gn)) {
      throw CodegenError(node->getSpan(),
                         "Generic class {} requires type arguments for all parameters; "
                         "could not infer {} from constructor",
                         node->getIdentifier(), gn);
    }
  }

  std::string key = (alias.empty() ? "" : "&" + alias + "=>") + node->getIdentifier();
  for (const auto& gn : genericNames) {
    key += "|" + MangleUtils::getTypeMangledName(node->getSpan(), env[gn]);
  }
  if (auto it = specializedClasses.find(key); it != specializedClasses.end()) {
    return it->second;
  }

  auto saved = currentGenericTypes;
  auto* savedSelfSymbol = selfSymbol;
  currentGenericTypes = env;

  for (auto* constructorArgType : constructorArgTypes) {
    if (constructorArgType != nullptr) {
      getOrCreateLlvmType(constructorArgType);
    }
  }
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }

  std::string concreteName = (alias.empty() ? "" : alias + "_") + node->getIdentifier();
  for (const auto& gn : genericNames) {
    concreteName += "_" + MangleUtils::getTypeMangledName(node->getSpan(), env[gn]);
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<llvm::Type*> elementLLVMTypes;
  for (auto* field : node->getFields()) {
    if (field->getType() != nullptr) {
      field->getType()->accept(*this);
    } else {
      field->getValue()->accept(*this);
    }
    getOrCreateLlvmType(result->getType());
    elementLLVMTypes.push_back(result->getType()->getLlvmType());
    std::unique_ptr<Value> defaultVal;
    if (field->getValue() != nullptr) {
      defaultVal = std::move(result);
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
      } else {
        result = std::make_unique<Value>(*defaultVal);
      }
    }
    fields.push_back(std::make_unique<Field>(field->getIdentifier()->getValue(), result->getType(),
                                             std::move(defaultVal)));
  }

  auto* structType =
      llvm::StructType::create(theModule->getContext(), elementLLVMTypes, concreteName);
  auto type = std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
  std::string displayName = node->getIdentifier() + "<";
  for (size_t i = 0; i < genericNames.size(); ++i) {
    if (i > 0U) {
      displayName += ", ";
    }
    displayName += env[genericNames[i]]->toString();
  }
  displayName += ">";
  type->setDisplayName(displayName);
  type->setImplTraitNames(std::vector<std::string>(node->getImplTraitNames()));
  auto* typePtr = type.get();
  scope->insertType(concreteName, std::move(type));

  auto structSymbol = std::make_unique<Value>(concreteName, typePtr);
  structSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  structSymbol->setExported(node->isExported());
  auto* structSymbolPtr = structSymbol.get();
  scope->insertSymbol(std::move(structSymbol));
  specializedClasses.emplace(key, structSymbolPtr);
  specializedClassSymbolsByType[typePtr] = structSymbolPtr;
  specializedClassTypeEnvs[typePtr] = env;

  auto* selfType =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  methodSelfSymbols.push_back(std::make_unique<Value>(concreteName, selfType));
  methodSelfSymbols.back()->setExported(node->isExported());
  selfSymbol = methodSelfSymbols.back().get();

  auto hasConstructor = false;
  for (auto* method : node->getMethods()) {
    method->accept(*this);
    if (method->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = buildClassMethodParamTypesForLookup(method);
      auto* constructor = scope->lookupFunction("new", constructorParams);
      structSymbolPtr->setConstructor(constructor);
    }
  }
  selfSymbol = nullptr;

  if (!hasConstructor) {
    throw CodegenError(node->getSpan(), "Generic class {} has no constructors",
                       node->getIdentifier());
  }
  selfSymbol = savedSelfSymbol;
  currentGenericTypes = std::move(saved);
  return structSymbolPtr;
}
