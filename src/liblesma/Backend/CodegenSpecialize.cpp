#include <algorithm>
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

auto Codegen::findGenericClassAstForTemplateType(Type* classTemplateTy) const -> const Class* {
  if (classTemplateTy == nullptr) {
    return nullptr;
  }
  for (const auto& [id, ast] : genericClasses) {
    lesma::Value* sym = rootScope->lookupStruct(id);
    if (sym != nullptr && sym->getType() != nullptr && sym->getType()->isEqual(classTemplateTy)) {
      return ast;
    }
  }
  return nullptr;
}

auto Codegen::isTypeFullyConcrete(Type* t) const -> bool {
  std::unordered_set<Type*> active;
  auto isTypeFullyConcreteImpl = [&](auto&& self, Type* cur) -> bool {
    if (cur == nullptr) {
      return false;
    }
    if (cur->is(BaseType::TY_GENERIC)) {
      return false;
    }
    if (!active.insert(cur).second) {
      return true;
    }

    bool isConcrete = true;
    switch (cur->getBaseType()) {
    case BaseType::TY_INVALID:
      isConcrete = true;
      break;
    case BaseType::TY_PTR:
    case BaseType::TY_ARRAY:
      isConcrete = self(self, cur->getElementType());
      break;
    case BaseType::TY_FUNCTION:
      for (Field* field : cur->getFields()) {
        if (!self(self, field->type)) {
          isConcrete = false;
          break;
        }
      }
      if (isConcrete) {
        isConcrete = self(self, cur->getReturnType());
      }
      break;
    case BaseType::TY_TUPLE:
      for (Field* field : cur->getFields()) {
        if (!self(self, field->type)) {
          isConcrete = false;
          break;
        }
      }
      break;
    case BaseType::TY_ENUM:
      if (auto envIt = specializedClassTypeEnvs.find(cur);
          envIt != specializedClassTypeEnvs.end()) {
        for (const auto& [name, boundType] : envIt->second) {
          (void) name;
          if (!self(self, boundType)) {
            isConcrete = false;
            break;
          }
        }
      } else if (!cur->getGenericParams().empty()) {
        isConcrete = false;
      }
      if (isConcrete) {
        for (EnumVariant* variant : cur->getEnumVariants()) {
          if (variant == nullptr) {
            continue;
          }
          for (Type* payload : variant->payloadTypes) {
            if (!self(self, payload)) {
              isConcrete = false;
              break;
            }
          }
          if (!isConcrete) {
            break;
          }
        }
      }
      break;
    case BaseType::TY_UNION:
      for (Type* m : cur->getUnionMembers()) {
        if (!self(self, m)) {
          isConcrete = false;
          break;
        }
      }
      break;
    case BaseType::TY_CLASS:
      if (cur->isBuiltinTask()) {
        isConcrete = self(self, cur->getTaskPayloadType());
        break;
      }
      if (auto envIt = specializedClassTypeEnvs.find(cur);
          envIt != specializedClassTypeEnvs.end()) {
        for (const auto& [name, boundType] : envIt->second) {
          (void) name;
          if (!self(self, boundType)) {
            isConcrete = false;
            break;
          }
        }
      } else if (!cur->getGenericParams().empty()) {
        isConcrete = false;
      }
      if (isConcrete && cur->getClassSuperclass() != nullptr) {
        isConcrete = self(self, cur->getClassSuperclass());
      }
      if (isConcrete) {
        for (Field* field : cur->getFields()) {
          if (!self(self, field->type)) {
            isConcrete = false;
            break;
          }
        }
      }
      break;
    default:
      break;
    }

    active.erase(cur);
    return isConcrete;
  };
  return isTypeFullyConcreteImpl(isTypeFullyConcreteImpl, t);
}

auto Codegen::typeWithSingletonUnionsCollapsed(Type* t) -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  while (t->is(BaseType::TY_UNION) && t->getUnionMembers().size() == 1U) {
    Type* inner = t->getUnionMembers()[0];
    if (inner == nullptr) {
      break;
    }
    t = inner;
  }
  if (t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
    Type* inner = typeWithSingletonUnionsCollapsed(t->getElementType());
    if (inner != t->getElementType()) {
      return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, inner));
    }
  } else if (t->is(BaseType::TY_ARRAY) && t->getElementType() != nullptr) {
    Type* inner = typeWithSingletonUnionsCollapsed(t->getElementType());
    if (inner != t->getElementType()) {
      auto* arr = cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, inner));
      arr->setDisplayName(t->getDisplayName());
      return arr;
    }
  }
  return t;
}

auto Codegen::emitClassMonomorph(Type* specialized, const Class* templateAst) -> lesma::Value* {
  if (specialized == nullptr || templateAst == nullptr) {
    throw CodegenError({}, "Internal error: emitClassMonomorph requires specialized class input");
  }
  if (auto it = specializedClassSymbolsByType.find(specialized);
      it != specializedClassSymbolsByType.end()) {
    return it->second;
  }

  auto envIt = specializedClassTypeEnvs.find(specialized);
  if (envIt == specializedClassTypeEnvs.end()) {
    throw CodegenError(templateAst->getSpan(), "Internal error: missing specialization env for {}",
                       specialized->getDisplayName());
  }

  for (auto& nameAndTy : envIt->second) {
    nameAndTy.second = typeWithSingletonUnionsCollapsed(nameAndTy.second);
  }
  for (Field* field : specialized->getFields()) {
    if (field->type != nullptr) {
      field->type = typeWithSingletonUnionsCollapsed(field->type);
    }
  }

  const auto& env = envIt->second;
  const auto& genericNames = templateAst->getGenericParams();

  auto saved = currentGenericTypes;
  auto* savedSelfSymbol = selfSymbol;
  currentGenericTypes = env;

  for (const auto& [name, ty] : env) {
    (void) name;
    if (ty != nullptr) {
      getOrCreateLlvmType(ty);
    }
  }

  std::string concreteName = (alias.empty() ? "" : alias + "_") + templateAst->getIdentifier();
  for (const auto& name : genericNames) {
    concreteName += "_" + MangleUtils::getTypeMangledName(templateAst->getSpan(), env.at(name));
  }

  auto* structType = llvm::StructType::create(theModule->getContext(), concreteName);
  specialized->setLlvmType(structType);
  scope->insertTypeRef(concreteName, specialized);

  auto structSymbol = std::make_unique<Value>(concreteName, specialized);
  structSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  structSymbol->setExported(templateAst->isExported());
  auto* structSymbolPtr = structSymbol.get();
  scope->insertSymbol(std::move(structSymbol));
  specializedClassSymbolsByType[specialized] = structSymbolPtr;
  codegenClassAstByType[specialized] = templateAst;
  if (!specialized->getDisplayName().empty()) {
    codegenClassAstByDisplayName.insert({specialized->getDisplayName(), templateAst});
  }

  std::string specializationKey =
      (alias.empty() ? "" : "&" + alias + "=>") + templateAst->getIdentifier();
  for (const auto& name : genericNames) {
    specializationKey +=
        "|" + MangleUtils::getTypeMangledName(templateAst->getSpan(), env.at(name));
  }
  specializedClasses[specializationKey] = structSymbolPtr;

  std::vector<llvm::Type*> elementLLVMTypes;
  elementLLVMTypes.push_back(builder->getPtrTy());
  for (Field* field : specialized->getFields()) {
    elementLLVMTypes.push_back(getStoredAggregateFieldLlvmType(field->type));
  }
  if (elementLLVMTypes.size() == 1U) {
    elementLLVMTypes.push_back(builder->getInt8Ty());
  }
  structType->setBody(elementLLVMTypes, false);

  if (specialized->getClassVtableMethodOrder().empty()) {
    if (auto* tmplSym = scope->lookupStruct(templateAst->getIdentifier());
        tmplSym != nullptr && tmplSym->getType() != nullptr) {
      specialized->setClassVtableMethodOrder(tmplSym->getType()->getClassVtableMethodOrder());
      specialized->setClassHasDerivedClass(tmplSym->getType()->getClassHasDerivedClass());
    }
  }

  if (Type* superTy = specialized->getClassSuperclass();
      superTy != nullptr && superTy->is(BaseType::TY_CLASS)) {
    Type* superTemplate = superTy;
    if (auto it = specializedClassTemplateOf.find(superTy);
        it != specializedClassTemplateOf.end()) {
      superTemplate = it->second;
    }
    if (const Class* superAst = findGenericClassAstForTemplateType(superTemplate);
        superAst != nullptr) {
      emitClassMonomorph(superTy, superAst);
    }
  }

  auto* selfType =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), specialized));
  methodSelfSymbols.push_back(std::make_unique<Value>(concreteName, selfType));
  methodSelfSymbols.back()->setExported(templateAst->isExported());
  selfSymbol = methodSelfSymbols.back().get();

  bool hasConstructor = false;
  for (auto* method : templateAst->getMethods()) {
    method->accept(*this);
    if (method->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = buildClassMethodParamTypesForLookup(method);
      auto* constructor =
          scope->lookupFunction("new", constructorParams, FunctionLookupKind::OVERLOAD_IDENTITY);
      structSymbolPtr->setConstructor(constructor);
    }
  }
  selfSymbol = nullptr;

  if (!hasConstructor) {
    selfSymbol = structSymbolPtr;
    lesma::Value* synthCtor =
        declareSynthesizedClassConstructor(templateAst, specialized, structSymbolPtr);
    structSymbolPtr->setConstructor(synthCtor);
    selfSymbol = nullptr;
  }

  getOrEmitClassVtableGlobal(specialized, templateAst);
  selfSymbol = savedSelfSymbol;
  currentGenericTypes = std::move(saved);
  return structSymbolPtr;
}

auto Codegen::substituteTypeForSpecializationEnv(Type* t,
                                                 const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  std::set<Type const*> active;
  return substituteTypeForSpecializationEnv(t, env, active);
}

auto Codegen::tryReuseActiveSpecializedNominalType(
    Type* t, const std::unordered_map<std::string, Type*>& env, std::set<Type const*>& active)
    -> Type* {
  if (t == nullptr || !active.contains(t) || !t->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    return nullptr;
  }
  Type* nominalTemplate = t;
  if (auto tmplIt = specializedClassTemplateOf.find(t); tmplIt != specializedClassTemplateOf.end()) {
    nominalTemplate = tmplIt->second;
  }
  const auto& genericParamNames = nominalTemplate->getGenericParams();
  if (genericParamNames.empty()) {
    return nullptr;
  }
  std::unordered_map<std::string, Type*> nominalEnv;
  if (auto specTmpl = specializedClassTemplateOf.find(t);
      specTmpl != specializedClassTemplateOf.end()) {
    if (auto envIt = specializedClassTypeEnvs.find(t); envIt != specializedClassTypeEnvs.end()) {
      for (const auto& name : genericParamNames) {
        auto boundIt = envIt->second.find(name);
        if (boundIt != envIt->second.end()) {
          nominalEnv[name] = substituteTypeForSpecializationEnv(boundIt->second, env, active);
        }
      }
    }
  }
  for (const auto& name : genericParamNames) {
    if (auto it = env.find(name); it != env.end()) {
      nominalEnv[name] = it->second;
    }
  }
  for (const auto& name : genericParamNames) {
    if (!nominalEnv.contains(name)) {
      return nullptr;
    }
  }
  auto registryKey = TypeUtils::makeSpecializedClassKey(nominalTemplate, genericParamNames, nominalEnv);
  if (auto it = specializedClassTypesByKey.find(registryKey); it != specializedClassTypesByKey.end()) {
    return it->second;
  }
  return nullptr;
}

auto Codegen::substituteTypeForSpecializationEnv(Type* t,
                                                 const std::unordered_map<std::string, Type*>& env,
                                                 std::set<Type const*>& active) -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  if (isTypeFullyConcrete(t)) {
    return t;
  }
  if (Type* reused = tryReuseActiveSpecializedNominalType(t, env, active); reused != nullptr) {
    return reused;
  }
  if (!active.insert(t).second) {
    return t;
  }
  struct ActiveGuard {
    std::set<Type const*>* setPtr;
    Type const* key;
    ~ActiveGuard() { setPtr->erase(key); }
  } guard{.setPtr = &active, .key = t};
  if (t->is(BaseType::TY_INVALID)) {
    return t;
  }
  if (t->is(BaseType::TY_GENERIC)) {
    auto it = env.find(t->getGenericName());
    if (it != env.end()) {
      return it->second;
    }
    return t;
  }
  if (t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
    Type* elem = substituteTypeForSpecializationEnv(t->getElementType(), env, active);
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (t->is(BaseType::TY_ARRAY) && t->getElementType() != nullptr) {
    Type* elem = substituteTypeForSpecializationEnv(t->getElementType(), env, active);
    auto* arr = cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, elem));
    arr->setDisplayName(t->getDisplayName());
    return arr;
  }
  if (t->is(BaseType::TY_FUNCTION)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : t->getFields()) {
      fields.push_back(std::make_unique<Field>(
          field->name, substituteTypeForSpecializationEnv(field->type, env, active)));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(substituteTypeForSpecializationEnv(t->getReturnType(), env, active));
    funcType->setGenericParams(t->getGenericParams());
    funcType->setGenericParamTraitBounds(
        std::vector<std::vector<std::string>>(t->getGenericParamTraitBounds()));
    funcType->setVarArgs(t->isVarArgs());
    return cacheType(std::move(funcType));
  }
  if (t->is(BaseType::TY_TUPLE)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : t->getFields()) {
      fields.push_back(std::make_unique<Field>(
          field->name, substituteTypeForSpecializationEnv(field->type, env, active)));
    }
    auto tupleType = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
    tupleType->setDisplayName(t->getDisplayName());
    return cacheType(std::move(tupleType));
  }
  if (t->is(BaseType::TY_UNION)) {
    std::vector<Type*> arms;
    arms.reserve(t->getUnionMembers().size());
    for (Type* m : t->getUnionMembers()) {
      arms.push_back(substituteTypeForSpecializationEnv(m, env, active));
    }
    auto [members, dn] = TypeUtils::canonicalizeUnionMembers(std::move(arms));
    if (members.size() == 1U) {
      return members.front();
    }
    auto u = std::make_unique<Type>(BaseType::TY_UNION);
    u->setUnionMembers(std::move(members));
    u->setDisplayName(t->getDisplayName().empty() ? dn : t->getDisplayName());
    u->setDeclarationSpan(t->getDeclarationSpan());
    u->setDeclarationFilePath(t->getDeclarationFilePath());
    return cacheType(std::move(u));
  }
  if (t->isBuiltinTask()) {
    Type* payloadType = substituteTypeForSpecializationEnv(t->getTaskPayloadType(), env, active);
    auto taskType = std::make_unique<Type>(BaseType::TY_CLASS);
    std::string payloadName = payloadType != nullptr ? payloadType->toString() : "void";
    taskType->setDisplayName("Task<" + payloadName + ">");
    taskType->setBuiltinTask(true);
    taskType->setTaskPayloadType(payloadType);
    return cacheType(std::move(taskType));
  }
  if (t->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    Type* nominalTemplate = t;
    if (auto tmplIt = specializedClassTemplateOf.find(t);
        tmplIt != specializedClassTemplateOf.end()) {
      nominalTemplate = tmplIt->second;
    }
    const auto& genericParamNames = nominalTemplate->getGenericParams();
    if (genericParamNames.empty()) {
      return t;
    }
    std::unordered_map<std::string, Type*> nominalEnv;
    if (auto specTmpl = specializedClassTemplateOf.find(t);
        specTmpl != specializedClassTemplateOf.end()) {
      if (auto envIt = specializedClassTypeEnvs.find(t); envIt != specializedClassTypeEnvs.end()) {
        for (const auto& name : genericParamNames) {
          auto b = envIt->second.find(name);
          if (b != envIt->second.end()) {
            nominalEnv[name] = substituteTypeForSpecializationEnv(b->second, env, active);
          }
        }
      }
    }
    for (const auto& name : genericParamNames) {
      if (auto it = env.find(name); it != env.end()) {
        nominalEnv[name] = it->second;
      }
    }
    bool allBound = true;
    for (const auto& name : genericParamNames) {
      if (!nominalEnv.contains(name)) {
        allBound = false;
        break;
      }
    }
    if (allBound) {
      const std::string registryKey =
          TypeUtils::makeSpecializedClassKey(nominalTemplate, genericParamNames, nominalEnv);
      if (auto it = specializedClassTypesByKey.find(registryKey);
          it != specializedClassTypesByKey.end()) {
        return it->second;
      }
      if (nominalTemplate->is(BaseType::TY_ENUM)) {
        auto specialized = std::make_unique<Type>(BaseType::TY_ENUM, nullptr,
                                                  std::vector<std::unique_ptr<Field>>{});
        std::string displayName = nominalTemplate->getDisplayName();
        if (displayName.empty()) {
          displayName = "enum";
        } else {
          const auto angle = displayName.find('<');
          if (angle != std::string::npos) {
            displayName = displayName.substr(0, angle);
          }
        }
        displayName += "<";
        for (size_t i = 0; i < genericParamNames.size(); ++i) {
          if (i > 0U) {
            displayName += ", ";
          }
          displayName += nominalEnv[genericParamNames[i]]->toString();
        }
        displayName += ">";
        specialized->setDisplayName(displayName);
        specialized->setGenericParams(genericParamNames);
        specialized->setDeclarationSpan(nominalTemplate->getDeclarationSpan());
        specialized->setDeclarationFilePath(nominalTemplate->getDeclarationFilePath());
        Type* specializedPtr = cacheType(std::move(specialized));
        specializedClassTypeEnvs[specializedPtr] = nominalEnv;
        specializedClassTemplateOf[specializedPtr] = nominalTemplate;
        specializedClassTypesByKey[registryKey] = specializedPtr;

        std::vector<std::unique_ptr<Field>> newFields;
        for (Field* field : nominalTemplate->getFields()) {
          Type* subst = substituteTypeForSpecializationEnv(field->type, nominalEnv, active);
          auto newField = std::make_unique<Field>(field->name, subst);
          newField->setDeclarationSpan(field->getDeclarationSpan());
          newField->setDeclarationFilePath(field->getDeclarationFilePath());
          if (Value* ds = field->getDeclarationSymbol()) {
            auto symCopy = std::make_unique<Value>(*ds);
            symCopy->setType(subst);
            newField->setDeclarationSymbol(std::move(symCopy));
          }
          newFields.push_back(std::move(newField));
        }
        specializedPtr->replaceFields(std::move(newFields));

        std::vector<std::unique_ptr<EnumVariant>> newVariants;
        for (EnumVariant* variant : nominalTemplate->getEnumVariants()) {
          if (variant == nullptr) {
            continue;
          }
          std::vector<Type*> payloadTypes;
          payloadTypes.reserve(variant->payloadTypes.size());
          for (Type* payload : variant->payloadTypes) {
            payloadTypes.push_back(substituteTypeForSpecializationEnv(payload, nominalEnv, active));
          }
          auto newVariant = std::make_unique<EnumVariant>(variant->name, std::move(payloadTypes));
          newVariant->setDeclarationSpan(variant->getDeclarationSpan());
          newVariant->setDeclarationFilePath(variant->getDeclarationFilePath());
          newVariants.push_back(std::move(newVariant));
        }
        specializedPtr->replaceEnumVariants(std::move(newVariants));
        return specializedPtr;
      }
    }
  }
  return t;
}

auto Codegen::bindGenericsFromTypePair(const TypeExpr* declared, lesma::Type* actual,
                                       const std::unordered_set<std::string>& genericNameSet,
                                       std::unordered_map<std::string, lesma::Type*>& env,
                                       bool* bindingConflict) -> void {
  if (declared == nullptr || actual == nullptr) {
    return;
  }
  switch (declared->getType()) {
  case TokenType::INT_TYPE:
    // Plain `int` / `int64`: Codegen::visit(TypeExpr*) lowers to 64-bit signed TY_INT only.
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 64U || !actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::INT8_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 8U || !actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::INT16_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 16U || !actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::INT32_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 32U || !actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::UINT_TYPE:
    // `uint` / `uint64`: 64-bit unsigned TY_INT in Codegen::visit(const TypeExpr*).
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 64U || actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::UINT8_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 8U || actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::UINT16_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 16U || actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::UINT32_TYPE:
    if (!actual->is(BaseType::TY_INT) || actual->getIntWidth() != 32U || actual->isSigned()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::BOOL_TYPE:
    if (!actual->is(BaseType::TY_BOOL)) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::FLOAT_TYPE:
    if (!actual->is(BaseType::TY_FLOAT)) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::FLOAT32_TYPE:
    if (!actual->is(BaseType::TY_FLOAT32)) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  case TokenType::STRING_TYPE:
    // `cstr` in type position is TY_STRING in Codegen::visit(const TypeExpr*), not the stdlib str
    // class (that path uses CUSTOM_TYPE + lookup name "str").
    if (!actual->is(BaseType::TY_STRING)) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
    return;
  default:
    break;
  }
  if (declared->getType() == TokenType::UNION_TYPE && actual->is(BaseType::TY_UNION)) {
    const auto declArms = declared->getParams();
    const auto& actualMem = actual->getUnionMembers();
    if (declArms.size() != actualMem.size()) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
      return;
    }
    std::unordered_map<std::string, lesma::Type*> genericPlaceholders;
    for (const auto& name : genericNameSet) {
      genericPlaceholders.emplace(name, cacheType(std::make_unique<lesma::Type>(name)));
    }
    std::vector<bool> used(actualMem.size(), false);
    auto tryBind = [&](auto&& self, size_t fi,
                       std::unordered_map<std::string, lesma::Type*> trial) -> bool {
      if (fi == declArms.size()) {
        env = std::move(trial);
        return true;
      }
      for (size_t aj = 0; aj < actualMem.size(); ++aj) {
        if (used[aj]) {
          continue;
        }
        // Match Typechecker::inferGenericBindings: only pair arms with pmem[fi]->isEqual(amem[aj])
        // before inferring. Lower the declared arm under trial + TY_GENERIC placeholders for
        // unbound params (same idea as resolveType with currentGenericTypes).
        auto savedGenerics = currentGenericTypes;
        auto savedResult = std::move(result);
        std::unordered_map<std::string, lesma::Type*> merged = savedGenerics;
        for (const auto& kv : trial) {
          merged[kv.first] = kv.second;
        }
        for (const auto& name : genericNameSet) {
          if (!merged.contains(name)) {
            merged[name] = genericPlaceholders.at(name);
          }
        }
        currentGenericTypes = std::move(merged);
        lesma::Type* declArmTy = nullptr;
        try {
          declArms[fi]->accept(*this);
          declArmTy = result != nullptr ? result->getType() : nullptr;
        } catch (const CodegenError&) {
          declArmTy = nullptr;
        }
        result = std::move(savedResult);
        currentGenericTypes = std::move(savedGenerics);
        if (declArmTy == nullptr) {
          continue;
        }
        // Typechecker::inferGenericBindings tests pmem[fi]->isEqual(amem[aj]) before recursing;
        // that is false for TY_GENERIC vs concrete, so viable pairings are discovered via infer,
        // not structural equality on the open arm. Require isEqual only when the declared arm is
        // already concrete so we do not pair e.g. `int` with `str` before bindGenericsFromTypePair
        // runs.
        if (!declArmTy->is(BaseType::TY_GENERIC) && !declArmTy->isEqual(actualMem[aj])) {
          continue;
        }
        auto probe = trial;
        bool c = false;
        bindGenericsFromTypePair(declArms[fi], actualMem[aj], genericNameSet, probe, &c);
        if (c) {
          continue;
        }
        used[aj] = true;
        if (self(self, fi + 1, std::move(probe))) {
          return true;
        }
        used[aj] = false;
      }
      return false;
    };
    if (!tryBind(tryBind, 0, env)) {
      if (bindingConflict != nullptr) {
        *bindingConflict = true;
      }
    }
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
        if (const auto* envMap = specializedClassEnvFor(classActual); envMap != nullptr) {
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
                if (auto concreteIt = envMap->find(templateGenericParams[i]);
                    concreteIt != envMap->end() && concreteIt->second != nullptr) {
                  bindGenericsFromTypePair(declTypeArgs[i], concreteIt->second, genericNameSet, env,
                                           bindingConflict);
                }
              }
              return;
            }
          }
        }
      }
    }
    if (genericNameSet.contains(name)) {
      if (env.contains(name)) {
        if (bindingConflict != nullptr && actual != nullptr && !env[name]->isEqual(actual)) {
          *bindingConflict = true;
        }
        return;
      }
      env[name] = actual;
      return;
    }
    if (name == "__buffer" && actual->is(BaseType::TY_ARRAY) &&
        actual->getElementType() != nullptr) {
      if (declTypeArgs.size() == 1U) {
        bindGenericsFromTypePair(declTypeArgs.front(), actual->getElementType(), genericNameSet,
                                 env, bindingConflict);
      }
    }
    return;
  }
  if (declared->getType() == TokenType::PTR_TYPE && actual->is(BaseType::TY_PTR) &&
      declared->getElementType() != nullptr && actual->getElementType() != nullptr) {
    bindGenericsFromTypePair(declared->getElementType(), actual->getElementType(), genericNameSet,
                             env, bindingConflict);
    return;
  }
  if (declared->getType() == TokenType::FUNC_TYPE && actual->is(BaseType::TY_FUNCTION)) {
    if (declared->getReturnType() != nullptr && actual->getReturnType() != nullptr) {
      bindGenericsFromTypePair(declared->getReturnType(), actual->getReturnType(), genericNameSet,
                               env, bindingConflict);
    }
    auto declParams = declared->getParams();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < declParams.size() && i < actualFields.size(); ++i) {
      bindGenericsFromTypePair(declParams[i], actualFields[i]->type, genericNameSet, env,
                               bindingConflict);
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
  size_t offset = (selfSymbol != nullptr && !node->getIsStatic()) ? 1U : 0U;
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

auto Codegen::specializeFunction(
    const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
    const std::vector<std::string>& genericNames, const std::vector<lesma::Type*>& explicitTypeArgs,
    const std::unordered_map<std::string, lesma::Type*>* bindingEnvHint) -> lesma::Value* {
  Value* templateSym = node->getResolvedSymbol();
  auto saved = currentGenericTypes;
  auto env =
      bindingEnvHint != nullptr
          ? *bindingEnvHint
          : computeGenericFunctionBindingEnv(node, paramTypes, genericNames, explicitTypeArgs);
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
  std::string key = getMangledName(node->getSpan(), node->getName(), paramTypes,
                                   selfSymbol != nullptr && !node->getIsStatic());
  appendGenericBindingSuffix(node->getSpan(), key, genericNames, env);
  if (auto it = specializedFunctions.find(key); it != specializedFunctions.end()) {
    currentGenericTypes = std::move(saved);
    return it->second;
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> concreteParamTypes;
  if (selfSymbol != nullptr && !node->getIsStatic()) {
    Type* recvType = selfSymbol->getType();
    if (recvType != nullptr && recvType->is(BaseType::TY_CLASS)) {
      recvType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, recvType));
    }
    fields.push_back(std::make_unique<Field>("self", recvType));
    concreteParamTypes.push_back(recvType);
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
  Type* returnType = nullptr;
  if (node->getIsAsync()) {
    returnType = substituteTypeForSpecializationEnv(templateSym->getType()->getReturnType(), env);
  } else {
    node->getReturnType()->accept(*this);
    returnType = wrapNominalReturnAsPointer(result->getType());
  }
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
  auto mangledName = getMangledName(node->getSpan(), node->getName(), concreteParamTypes,
                                    selfSymbol != nullptr && !node->getIsStatic());
  appendGenericBindingSuffix(node->getSpan(), mangledName, genericNames, env);
  const bool specializationKeysMatch = (mangledName == key);
  auto func = std::make_unique<Value>(node->getName(), typePtr);
  func->setCategory(ValueCategory::CALLABLE_SYMBOL);
  if (templateSym != nullptr) {
    func->setDeclarationKind(templateSym->getDeclarationKind());
    func->setBodyScope(templateSym->getBodyScope());
  } else {
    func->setDeclarationKind(selfSymbol != nullptr ? ValueDeclarationKind::METHOD
                                                   : ValueDeclarationKind::FUNCTION);
  }
  func->setStaticMethod(node->getIsStatic());
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
  if (node->getIsAsync()) {
    llvmFunc->addFnAttr(llvm::Attribute::PresplitCoroutine);
  }
  attachFunctionDebugInfo(llvmFunc, node->getName(), mangledName, node->getSpan(), linkage, false);
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

auto Codegen::computeGenericLambdaBindingEnv(const LambdaExpr* node,
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
  for (size_t i = 0; i < node->getParameters().size() && i < paramTypes.size(); ++i) {
    TypeExpr* declType = node->getParameters()[i]->type.get();
    if (declType != nullptr) {
      bindGenericsFromTypePair(declType, paramTypes[i], genericNameSet, env);
    }
  }
  return env;
}

auto Codegen::specializeLambda(const LambdaExpr* node, const std::vector<lesma::Type*>& paramTypes,
                               const std::vector<std::string>& genericNames,
                               const std::vector<lesma::Type*>& explicitTypeArgs,
                               const std::unordered_map<std::string, lesma::Type*>* bindingEnvHint)
    -> lesma::Value* {
  Value* templateSym = node->getResolvedSymbol();
  if (templateSym == nullptr) {
    throw CodegenError(node->getSpan(), "Generic lambda is missing its template symbol");
  }
  auto saved = currentGenericTypes;
  auto env = bindingEnvHint != nullptr
                 ? *bindingEnvHint
                 : computeGenericLambdaBindingEnv(node, paramTypes, genericNames, explicitTypeArgs);
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
  std::string key = templateSym->getName();
  appendGenericBindingSuffix(node->getSpan(), key, genericNames, env);
  if (auto it = specializedFunctions.find(key); it != specializedFunctions.end()) {
    currentGenericTypes = std::move(saved);
    return it->second;
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> concreteParamTypes;
  for (auto* param : node->getParameters()) {
    param->type->accept(*this);
    Type* paramT = result->getType();
    if (paramT != nullptr && paramT->is(BaseType::TY_CLASS)) {
      paramT = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramT));
    }
    fields.push_back(std::make_unique<Field>(param->name, paramT));
    concreteParamTypes.push_back(paramT);
  }
  Type* returnType = nullptr;
  if (node->getIsAsync()) {
    returnType = substituteTypeForSpecializationEnv(templateSym->getType()->getReturnType(), env);
  } else if (node->getReturnType() != nullptr) {
    node->getReturnType()->accept(*this);
    returnType = wrapNominalReturnAsPointer(result->getType());
  } else {
    returnType = substituteTypeForSpecializationEnv(templateSym->getType()->getReturnType(), env);
  }
  std::vector<llvm::Type*> paramLLVMTypes;
  for (auto* t : concreteParamTypes) {
    getOrCreateLlvmType(t);
    paramLLVMTypes.push_back(t->getLlvmType());
  }
  getOrCreateLlvmType(returnType);
  auto funcType =
      std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
  funcType->setReturnType(returnType);
  auto* typePtr = cacheType(std::move(funcType));
  std::string mangledName = templateSym->getName();
  appendGenericBindingSuffix(node->getSpan(), mangledName, genericNames, env);
  const bool specializationKeysMatch = (mangledName == key);
  auto func = std::make_unique<Value>(templateSym->getName(), typePtr);
  func->setCategory(ValueCategory::CALLABLE_SYMBOL);
  func->setDeclarationKind(ValueDeclarationKind::FUNCTION);
  func->setMangledName(mangledName);
  llvm::Type* llvmReturnType = nullptr;
  if (returnType->is(BaseType::TY_PTR) || returnType->is(BaseType::TY_CLASS)) {
    llvmReturnType = builder->getPtrTy();
  } else {
    llvmReturnType = returnType->getLlvmType();
  }
  auto* llvmFuncType = FunctionType::get(llvmReturnType, paramLLVMTypes, false);
  auto* llvmFunc =
      Function::Create(llvmFuncType, Function::PrivateLinkage, mangledName, *theModule);
  if (node->getIsAsync()) {
    llvmFunc->addFnAttr(llvm::Attribute::PresplitCoroutine);
  }
  typePtr->setLlvmType(llvmFuncType);
  func->setLlvmValue(llvmFunc);
  func->setBodyScope(templateSym->getBodyScope());
  auto* funcPtr = func.get();
  scope->insertSymbol(std::move(func));
  lambdaPrototypes.emplace_back(funcPtr, node);
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
                              const std::vector<lesma::Type*>& explicitTypeArgs,
                              const std::unordered_map<std::string, lesma::Type*>* prebuiltClassEnv)
    -> lesma::Value* {
  auto genericNames = node->getGenericParams();

  const FuncDecl* constructorDecl = nullptr;
  for (auto* method : node->getMethods()) {
    if (method->getName() == "new") {
      constructorDecl = method;
      break;
    }
  }

  std::unordered_map<std::string, lesma::Type*> env;
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());

  if (prebuiltClassEnv != nullptr) {
    env = *prebuiltClassEnv;
    for (const auto& gn : genericNames) {
      auto it = env.find(gn);
      if (it == env.end() || it->second == nullptr) {
        throw CodegenError(
            node->getSpan(),
            "Internal error: incomplete specialization env for class {} parameter {}",
            node->getIdentifier(), gn);
      }
    }
  } else if (!explicitTypeArgs.empty()) {
    if (explicitTypeArgs.size() != genericNames.size()) {
      throw CodegenError(node->getSpan(),
                         "Explicit type argument count {} does not match generic class parameter "
                         "count {}",
                         explicitTypeArgs.size(), genericNames.size());
    }
    for (size_t i = 0; i < genericNames.size(); ++i) {
      env[genericNames[i]] = explicitTypeArgs[i];
    }
    for (const auto& gn : genericNames) {
      if (!env.contains(gn)) {
        throw CodegenError(node->getSpan(),
                           "Generic class {} requires type arguments for all parameters; "
                           "could not infer {} from constructor",
                           node->getIdentifier(), gn);
      }
    }
  } else {
    const bool needsConstructorInference = explicitTypeArgs.empty();
    bool constructorEnvResolved = false;
    if (needsConstructorInference && constructorDecl != nullptr) {
      for (auto* method : node->getMethods()) {
        if (method->getName() != "new") {
          continue;
        }
        const auto& params = method->getParameters();
        if (params.size() != constructorArgTypes.size()) {
          continue;
        }
        std::unordered_map<std::string, lesma::Type*> trialEnv = env;
        bool bindingConflict = false;
        for (size_t i = 0; i < params.size(); ++i) {
          TypeExpr* declType = params[i]->type.get();
          if (declType != nullptr) {
            bindGenericsFromTypePair(declType, constructorArgTypes[i], genericNameSet, trialEnv,
                                     &bindingConflict);
          }
        }
        if (bindingConflict) {
          continue;
        }
        bool allGenericsBound = true;
        for (const auto& gn : genericNames) {
          if (!trialEnv.contains(gn)) {
            allGenericsBound = false;
            break;
          }
        }
        if (allGenericsBound) {
          env = std::move(trialEnv);
          constructorDecl = method;
          constructorEnvResolved = true;
          break;
        }
      }
    }
    if (!constructorEnvResolved && needsConstructorInference && constructorDecl != nullptr) {
      auto params = constructorDecl->getParameters();
      for (size_t i = 0; i < params.size() && i < constructorArgTypes.size(); ++i) {
        TypeExpr* declType = params[i]->type.get();
        if (declType != nullptr) {
          bindGenericsFromTypePair(declType, constructorArgTypes[i], genericNameSet, env);
        }
      }
    }
    if (!constructorEnvResolved && needsConstructorInference && constructorDecl == nullptr) {
      std::vector<VarDecl*> requiredFields;
      for (VarDecl* fd : node->getFields()) {
        if (fd->getIsStatic()) {
          continue;
        }
        if (fd->getValue() == nullptr) {
          requiredFields.push_back(fd);
        }
      }
      if (constructorArgTypes.size() != requiredFields.size()) {
        throw CodegenError(node->getSpan(),
                           "Generic class {}: expected {} constructor argument(s) for synthesized "
                           "constructor, got {}",
                           node->getIdentifier(), requiredFields.size(),
                           constructorArgTypes.size());
      }
      for (size_t i = 0; i < requiredFields.size(); ++i) {
        TypeExpr* declType = requiredFields[i]->getType();
        if (declType != nullptr) {
          bindGenericsFromTypePair(declType, constructorArgTypes[i], genericNameSet, env);
        }
      }
      constructorEnvResolved = true;
    }

    for (const auto& gn : genericNames) {
      if (!env.contains(gn)) {
        throw CodegenError(node->getSpan(),
                           "Generic class {} requires type arguments for all parameters; "
                           "could not infer {} from constructor",
                           node->getIdentifier(), gn);
      }
    }
  }

  auto envIsFullyConcrete =
      [this](const std::unordered_map<std::string, lesma::Type*>& bindings) -> bool {
    return std::ranges::all_of(
        bindings, [this](const auto& nameAndTy) { return isTypeFullyConcrete(nameAndTy.second); });
  };

  Value* templateSymbol = scope->lookupStruct(node->getIdentifier());
  Type* templateType = templateSymbol != nullptr ? templateSymbol->getType() : nullptr;
  std::string registryKey;
  if (templateType != nullptr && !genericNames.empty()) {
    registryKey = TypeUtils::makeSpecializedClassKey(templateType, genericNames, env);
    if (auto it = specializedClassTypesByKey.find(registryKey);
        it != specializedClassTypesByKey.end()) {
      if (envIsFullyConcrete(env)) {
        return emitClassMonomorph(it->second, node);
      }
      if (auto symIt = specializedClassSymbolsByType.find(it->second);
          symIt != specializedClassSymbolsByType.end()) {
        return symIt->second;
      }
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

  std::string displayName = node->getIdentifier() + "<";
  for (size_t i = 0; i < genericNames.size(); ++i) {
    if (i > 0U) {
      displayName += ", ";
    }
    displayName += env[genericNames[i]]->toString();
  }
  displayName += ">";

  // Opaque shell first so recursive fields (e.g. Node<T> next) hit specializedClasses and a
  // concrete Type with LLVM type before we finish lowering field types / struct body.
  auto* structType = llvm::StructType::create(theModule->getContext(), concreteName);
  auto shellType =
      std::make_unique<Type>(BaseType::TY_CLASS, structType, std::vector<std::unique_ptr<Field>>{});
  shellType->setDisplayName(displayName);
  shellType->setImplTraitNames(std::vector<std::string>(node->getImplTraitNames()));
  auto* typePtr = shellType.get();
  scope->insertType(concreteName, std::move(shellType));

  auto structSymbol = std::make_unique<Value>(concreteName, typePtr);
  structSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  structSymbol->setExported(node->isExported());
  auto* structSymbolPtr = structSymbol.get();
  scope->insertSymbol(std::move(structSymbol));
  specializedClasses.emplace(key, structSymbolPtr);
  specializedClassSymbolsByType[typePtr] = structSymbolPtr;
  specializedClassTypeEnvs[typePtr] = env;
  if (templateType != nullptr && !registryKey.empty()) {
    specializedClassTemplateOf[typePtr] = templateType;
    specializedClassTypesByKey[registryKey] = typePtr;
  }
  codegenClassAstByType[typePtr] = node;
  if (!typePtr->getDisplayName().empty()) {
    codegenClassAstByDisplayName.insert({typePtr->getDisplayName(), node});
  }

  std::vector<llvm::Type*> elementLLVMTypes;
  elementLLVMTypes.push_back(builder->getPtrTy());
  lesma::Value* tmplSymForFields = scope->lookupStruct(node->getIdentifier());
  Type* tmplTyForFields = tmplSymForFields != nullptr ? tmplSymForFields->getType() : nullptr;

  if (!node->getGenericParams().empty() && tmplTyForFields != nullptr) {
    for (Field* f : tmplTyForFields->getFields()) {
      Type* ft = substituteTypeForSpecializationEnv(f->type, env);
      elementLLVMTypes.push_back(getStoredAggregateFieldLlvmType(ft));
      std::unique_ptr<Value> defaultVal;
      if (f->defaultValue != nullptr) {
        defaultVal = std::make_unique<Value>(*f->defaultValue);
      }
      typePtr->addField(std::make_unique<Field>(f->name, ft, std::move(defaultVal)));
    }
  } else {
    for (auto* field : node->getFields()) {
      if (field->getIsStatic()) {
        continue;
      }
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
      } else {
        field->getValue()->accept(*this);
      }
      elementLLVMTypes.push_back(getStoredAggregateFieldLlvmType(result->getType()));
      std::unique_ptr<Value> defaultVal;
      if (field->getValue() != nullptr) {
        defaultVal = std::move(result);
        if (field->getType() != nullptr) {
          field->getType()->accept(*this);
        } else {
          result = std::make_unique<Value>(*defaultVal);
        }
      }
      typePtr->addField(std::make_unique<Field>(field->getIdentifier()->getValue(),
                                                result->getType(), std::move(defaultVal)));
    }
  }

  if (elementLLVMTypes.size() == 1U) {
    elementLLVMTypes.push_back(builder->getInt8Ty());
  }
  structType->setBody(elementLLVMTypes, /*isPacked=*/false);
  typePtr->setDisplayName(std::move(displayName));
  typePtr->setImplTraitNames(std::vector<std::string>(node->getImplTraitNames()));

  if (auto* tmplSym = scope->lookupStruct(node->getIdentifier());
      tmplSym != nullptr && tmplSym->getType() != nullptr) {
    Type* tmplTy = tmplSym->getType();
    Type* superResolved = nullptr;
    if (tmplTy->getClassSuperclass() != nullptr) {
      superResolved = substituteTypeForSpecializationEnv(tmplTy->getClassSuperclass(), env);
    }
    typePtr->setClassSuperclass(superResolved);
    typePtr->setClassVtableMethodOrder(tmplTy->getClassVtableMethodOrder());
    typePtr->setClassHasDerivedClass(tmplTy->getClassHasDerivedClass());
  }

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
      auto* constructor =
          scope->lookupFunction("new", constructorParams, FunctionLookupKind::OVERLOAD_IDENTITY);
      structSymbolPtr->setConstructor(constructor);
    }
  }
  selfSymbol = nullptr;

  if (!hasConstructor) {
    selfSymbol = structSymbolPtr;
    lesma::Value* synthCtor = declareSynthesizedClassConstructor(node, typePtr, structSymbolPtr);
    structSymbolPtr->setConstructor(synthCtor);
    selfSymbol = nullptr;
  }
  getOrEmitClassVtableGlobal(typePtr, node);
  selfSymbol = savedSelfSymbol;
  currentGenericTypes = std::move(saved);
  return structSymbolPtr;
}
