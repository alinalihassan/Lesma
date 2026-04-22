#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/Support/Casting.h>

#include "Codegen.h"
#include <nameof.hpp>

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;
using namespace llvm;

auto Codegen::traitExistentialBaseName(const std::string& displayName) const -> std::string {
  if (const auto pos = displayName.find('<'); pos != std::string::npos) {
    return displayName.substr(0U, pos);
  }
  return displayName;
}

auto Codegen::unionDiscriminantMinBits(std::size_t memberCount) -> unsigned {
  unsigned bits = 0;
  for (std::size_t x = memberCount - 1; x != 0; x >>= 1) {
    ++bits;
  }
  return std::max(1U, bits);
}

auto Codegen::roundUnionTagToSupportedBitWidth(unsigned minBits) -> unsigned {
  if (minBits <= 8) {
    return 8;
  }
  if (minBits <= 16) {
    return 16;
  }
  if (minBits <= 32) {
    return 32;
  }
  if (minBits <= 64) {
    return 64;
  }
  return 0;
}

auto Codegen::visit(const TypeExpr* node) -> void {
  // For primitive types, cache them so they survive beyond result's lifetime
  // This is needed because setReturnType and similar store raw Type* pointers
  if (node->getType() == TokenType::INT_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    type->setIntWidth(64);
    type->setSigned(true);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT8_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt8Ty()));
    type->setIntWidth(8);
    type->setSigned(true);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT16_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt16Ty()));
    type->setIntWidth(16);
    type->setSigned(true);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT32_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt32Ty()));
    type->setIntWidth(32);
    type->setSigned(true);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::UINT_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    type->setIntWidth(64);
    type->setSigned(false);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::UINT8_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt8Ty()));
    type->setIntWidth(8);
    type->setSigned(false);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::UINT16_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt16Ty()));
    type->setIntWidth(16);
    type->setSigned(false);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::UINT32_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt32Ty()));
    type->setIntWidth(32);
    type->setSigned(false);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT32_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT32, builder->getFloatTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::BOOL_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::STRING_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::VOID_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::NIL) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_NULL, builder->getPtrTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::PTR_TYPE) {
    node->getElementType()->accept(*this);
    // Function type is already a pointer at LLVM level; `*func(...)` is optional
    // sugar for the same nominal type, so do not add another pointer layer.
    if (!result->getType()->is(BaseType::TY_FUNCTION)) {
      auto* type = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), result->getType()));
      result = std::make_unique<Value>(type);
    }
  } else if (node->getType() == TokenType::FUNC_TYPE) {
    node->getReturnType()->accept(*this);
    auto retType = std::move(result);
    lesma::Type* loweredReturnType = retType->getType();
    if (node->isAsyncFunctionType()) {
      loweredReturnType = getOrCreateAsyncTaskType(loweredReturnType);
    }
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<lesma::Type*> paramTypes;
    std::vector<llvm::Type*> paramLLVMTypes;
    for (auto* paramType : node->getParams()) {
      paramType->accept(*this);
      paramLLVMTypes.push_back(result->getType()->getLlvmType());
      paramTypes.push_back(result->getType());
      fields.push_back(std::make_unique<Field>(result->getName(), result->getType()));
    }

    // With opaque pointers, function pointer types are just `ptr`
    // The actual function signature is tracked in Lesma's Type system via
    // fields
    auto funcType =
        std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
    funcType->setReturnType(loweredReturnType);
    result = std::make_unique<Value>(cacheType(std::move(funcType)));
  } else if (node->getType() == TokenType::TUPLE_TYPE) {
    std::vector<std::unique_ptr<Field>> fields;
    std::string displayName = "tuple<";
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      Type* elemTy = result->getType();
      if (!fields.empty()) {
        displayName += ", ";
      }
      displayName += elemTy->toString();
      fields.push_back(std::make_unique<Field>("_" + std::to_string(fields.size()), elemTy));
    }
    displayName += ">";
    auto tup = std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(fields));
    tup->setDisplayName(displayName);
    Type* cached = cacheType(std::move(tup));
    getOrCreateLlvmType(cached);
    result = std::make_unique<Value>(cached);
  } else if (node->getType() == TokenType::UNION_TYPE) {
    std::vector<Type*> members;
    members.reserve(node->getParams().size());
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      members.push_back(result->getType());
    }
    auto [unique, displayName] = TypeUtils::canonicalizeUnionMembers(std::move(members));
    if (unique.size() == 1U) {
      Type* cached = unique[0];
      getOrCreateLlvmType(cached);
      result = std::make_unique<Value>(cached);
    } else {
      auto u = std::make_unique<Type>(BaseType::TY_UNION);
      u->setUnionMembers(std::move(unique));
      u->setDisplayName(displayName);
      u->setDeclarationSpan(node->getSpan());
      Type* cached = cacheType(std::move(u));
      getOrCreateLlvmType(cached);
      result = std::make_unique<Value>(cached);
    }
  } else if (node->getType() == TokenType::ANY_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_ANY));
    getOrCreateLlvmType(type);
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::CUSTOM_TYPE) {
    const std::string lookupName = node->getLookupName();
    auto git = currentGenericTypes.find(lookupName);
    if (git != currentGenericTypes.end()) {
      result = std::make_unique<Value>(git->second);
      result->setCategory(ValueCategory::TYPE_SYMBOL);
      return;
    }
    std::vector<lesma::Type*> explicitTypeArgs;
    for (auto* typeArg : node->getTypeArgs()) {
      typeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
    }
    if (Value* resolvedSym = node->getResolvedSymbol();
        resolvedSym != nullptr && resolvedSym->getDeclarationKind() == ValueDeclarationKind::TYPE &&
        resolvedSym->getType() != nullptr && explicitTypeArgs.empty()) {
      getOrCreateLlvmType(resolvedSym->getType());
      result = std::make_unique<Value>(*resolvedSym);
      result->setType(resolvedSym->getType());
      return;
    }
    if (lookupName == "__buffer") {
      if (explicitTypeArgs.size() != 1U) {
        throw CodegenError(node->getSpan(), "__buffer<T> expects exactly one type argument");
      }
      auto* type =
          cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, explicitTypeArgs.front()));
      type->setDisplayName(node->getName());
      getOrCreateLlvmType(type);
      result = std::make_unique<Value>(type);
      return;
    }
    auto* typ = scope->lookupType(lookupName);
    auto* sym = scope->lookupStruct(lookupName);
    if (typ == nullptr && sym == nullptr) {
      throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());
    }
    if (typ != nullptr && typ->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
      getOrCreateLlvmType(typ);
      Value* traitSym = scope->lookup(lookupName);
      if (traitSym != nullptr) {
        result = std::make_unique<Value>(*traitSym);
      } else {
        result = std::make_unique<Value>(typ);
      }
      return;
    }
    if (sym == nullptr) {
      throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());
    }
    if (!explicitTypeArgs.empty()) {
      const Class* templateClass = nullptr;
      const Enum* templateEnum = nullptr;
      if (auto gitClass = genericClasses.find(lookupName); gitClass != genericClasses.end()) {
        templateClass = gitClass->second;
      } else if (sym != nullptr && sym->getGenericClassTemplate() != nullptr) {
        templateClass = static_cast<const Class*>(sym->getGenericClassTemplate());
      } else if (auto gitEnum = genericEnums.find(lookupName); gitEnum != genericEnums.end()) {
        templateEnum = gitEnum->second;
      }
      if (templateClass == nullptr && templateEnum == nullptr) {
        throw CodegenError(node->getSpan(), "Type {} is not a generic nominal type",
                           node->getName());
      }
      if (templateClass != nullptr) {
        sym = specializeClass(templateClass, {}, explicitTypeArgs);
        typ = sym->getType();
      } else {
        Type* templateType = sym->getType();
        const auto& genericParamNames = templateType->getGenericParams();
        if (genericParamNames.size() != explicitTypeArgs.size()) {
          throw CodegenError(node->getSpan(), "Generic type {} expects {} type arguments, got {}",
                             node->getName(), genericParamNames.size(), explicitTypeArgs.size());
        }
        std::unordered_map<std::string, Type*> env;
        for (size_t i = 0; i < genericParamNames.size(); ++i) {
          env[genericParamNames[i]] = explicitTypeArgs[i];
        }
        const std::string registryKey =
            TypeUtils::makeSpecializedClassKey(templateType, genericParamNames, env);
        auto specIt = specializedClassTypesByKey.find(registryKey);
        if (specIt == specializedClassTypesByKey.end()) {
          auto specialized =
              std::make_unique<Type>(BaseType::TY_ENUM, nullptr, std::vector<std::unique_ptr<Field>>{});
          specialized->setDisplayName(node->getName());
          specialized->setGenericParams(genericParamNames);
          specialized->setDeclarationSpan(templateType->getDeclarationSpan());
          specialized->setDeclarationFilePath(templateType->getDeclarationFilePath());
          typ = cacheType(std::move(specialized));
          specializedClassTypeEnvs[typ] = env;
          specializedClassTemplateOf[typ] = templateType;
          specializedClassTypesByKey[registryKey] = typ;

          std::vector<std::unique_ptr<Field>> newFields;
          for (Field* field : templateType->getFields()) {
            Type* subst = substituteTypeForSpecializationEnv(field->type, env);
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
          typ->replaceFields(std::move(newFields));

          std::vector<std::unique_ptr<EnumVariant>> newVariants;
          for (EnumVariant* variant : templateType->getEnumVariants()) {
            if (variant == nullptr) {
              continue;
            }
            std::vector<Type*> payloadTypes;
            payloadTypes.reserve(variant->payloadTypes.size());
            for (Type* payload : variant->payloadTypes) {
              payloadTypes.push_back(substituteTypeForSpecializationEnv(payload, env));
            }
            auto newVariant = std::make_unique<EnumVariant>(variant->name, std::move(payloadTypes));
            newVariant->setDeclarationSpan(variant->getDeclarationSpan());
            newVariant->setDeclarationFilePath(variant->getDeclarationFilePath());
            newVariants.push_back(std::move(newVariant));
          }
          typ->replaceEnumVariants(std::move(newVariants));
          specIt = specializedClassTypesByKey.find(registryKey);
        }
        if (specIt == specializedClassTypesByKey.end()) {
          throw CodegenError(node->getSpan(), "Missing specialized enum type for {}", node->getName());
        }
        typ = specIt->second;
        if (isTypeFullyConcrete(typ)) {
          emitEnumMonomorph(typ, templateEnum);
        }
      }
    }
    if (typ->getLlvmType() == nullptr) {
      getOrCreateLlvmType(typ);
    }

    result = std::make_unique<Value>(*sym);
    result->setType(typ);
    result->setCategory(ValueCategory::TYPE_SYMBOL);
  } else {
    throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
  }
}

auto Codegen::pushGenericTypeFallback(const std::unordered_map<std::string, lesma::Type*>* env)
    -> void {
  if (env != nullptr && !env->empty()) {
    genericTypeFallbackStack.push_back(env);
  }
}

auto Codegen::popGenericTypeFallback() -> void {
  if (!genericTypeFallbackStack.empty()) {
    genericTypeFallbackStack.pop_back();
  }
}

auto Codegen::lookupGenericTypeFallback(const std::string& name) const -> lesma::Type* {
  for (auto it = genericTypeFallbackStack.rbegin(); it != genericTypeFallbackStack.rend(); ++it) {
    if (*it == nullptr) {
      continue;
    }
    auto found = (*it)->find(name);
    if (found != (*it)->end()) {
      return found->second;
    }
  }
  return nullptr;
}

auto Codegen::getOrCreateLlvmType(lesma::Type* type) -> llvm::Type* {
  if (type->getLlvmType() != nullptr) {
    if (type->is(BaseType::TY_ENUM)) {
      if (auto* existingStruct = llvm::dyn_cast<llvm::StructType>(type->getLlvmType());
          existingStruct != nullptr && existingStruct->isOpaque()) {
        auto variants = type->getEnumVariants();
        if (variants.empty()) {
          existingStruct->setBody({builder->getInt8Ty()});
        } else {
          const llvm::DataLayout& dl = theModule->getDataLayout();
          unsigned maxAlloc = 0U;
          unsigned maxAbiAlign = 1U;
          for (unsigned idx = 0; idx < variants.size(); ++idx) {
            EnumVariant* variant = variants[idx];
            if (variant == nullptr) {
              continue;
            }
            Type* aggregatePayloadType = getEnumVariantAggregatePayloadType(type, idx);
            if (aggregatePayloadType == nullptr) {
              continue;
            }
            llvm::Type* const lt = getStoredAggregateFieldLlvmType(aggregatePayloadType);
            maxAlloc =
                std::max(maxAlloc, static_cast<unsigned>(dl.getTypeAllocSize(lt).getFixedValue()));
            maxAbiAlign =
                std::max(maxAbiAlign, static_cast<unsigned>(dl.getABITypeAlign(lt).value()));
          }
          unsigned const payloadBytes = llvm::alignTo(maxAlloc, maxAbiAlign);
          unsigned const numI64 = std::max(1U, (payloadBytes + 7U) / 8U);
          llvm::Type* payloadTy = llvm::ArrayType::get(builder->getInt64Ty(), numI64);
          unsigned const minTagBits = unionDiscriminantMinBits(variants.size());
          unsigned const tagBitWidth = roundUnionTagToSupportedBitWidth(minTagBits);
          llvm::Type* tagTy = builder->getIntNTy(tagBitWidth);
          existingStruct->setBody({tagTy, payloadTy});
        }
      }
    }
    return type->getLlvmType();
  }
  switch (type->getBaseType()) {
  case BaseType::TY_GENERIC: {
    auto it = currentGenericTypes.find(type->getGenericName());
    if (it == currentGenericTypes.end()) {
      if (lesma::Type* fb = lookupGenericTypeFallback(type->getGenericName()); fb != nullptr) {
        return getOrCreateLlvmType(fb);
      }
      throw CodegenError({}, "Unknown generic type {}", type->getGenericName());
    }
    return getOrCreateLlvmType(it->second);
  }
  case BaseType::TY_INT: {
    const unsigned w = type->getIntWidth();
    llvm::Type* mapped = builder->getInt64Ty();
    if (w == 8) {
      mapped = builder->getInt8Ty();
    } else if (w == 16) {
      mapped = builder->getInt16Ty();
    } else if (w == 32) {
      mapped = builder->getInt32Ty();
    }
    type->setLlvmType(mapped);
    break;
  }
  case BaseType::TY_FLOAT:
    type->setLlvmType(builder->getDoubleTy());
    break;
  case BaseType::TY_FLOAT32:
    type->setLlvmType(builder->getFloatTy());
    break;
  case BaseType::TY_BOOL:
    type->setLlvmType(builder->getInt1Ty());
    break;
  case BaseType::TY_STRING:
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_ANY: {
    llvm::StructType* st = llvm::StructType::getTypeByName(theModule->getContext(), "lesma.any");
    if (st == nullptr) {
      st = llvm::StructType::create(theModule->getContext(),
                                    {builder->getPtrTy(), builder->getPtrTy()}, "lesma.any");
    }
    type->setLlvmType(st);
    break;
  }
  case BaseType::TY_VOID:
    type->setLlvmType(builder->getVoidTy());
    break;
  case BaseType::TY_NULL:
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_PTR:
    if (type->getElementType() != nullptr) {
      getOrCreateLlvmType(type->getElementType());
    }
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_ARRAY: {
    if (type->getElementType() != nullptr) {
      getOrCreateLlvmType(type->getElementType());
    }
    getOrCreateListStructType(type);
    type->setLlvmType(builder->getPtrTy());
    break;
  }
  case BaseType::TY_FUNCTION:
    getOrCreateLlvmType(type->getReturnType());
    for (auto* f : type->getFields()) {
      getOrCreateLlvmType(f->type);
    }
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_TRAIT_EXISTENTIAL: {
    llvm::Type* st =
        llvm::StructType::get(theModule->getContext(), {builder->getPtrTy(), builder->getPtrTy()});
    type->setLlvmType(st);
    break;
  }
  case BaseType::TY_TUPLE: {
    std::string const& name = type->getDisplayName();
    llvm::StructType* st = nullptr;
    if (!name.empty()) {
      st = llvm::StructType::getTypeByName(theModule->getContext(), name);
    }
    std::vector<llvm::Type*> elementTypes;
    for (auto* f : type->getFields()) {
      elementTypes.push_back(getStoredAggregateFieldLlvmType(f->type));
    }
    if (elementTypes.empty()) {
      elementTypes.push_back(builder->getInt8Ty());
    }
    if (st == nullptr) {
      if (!name.empty()) {
        st = llvm::StructType::create(theModule->getContext(), elementTypes, name);
      } else {
        st = llvm::StructType::create(theModule->getContext(), elementTypes);
      }
    }
    type->setLlvmType(st);
    break;
  }
  case BaseType::TY_UNION: {
    llvm::SMRange const unionDeclSpan = type->getDeclarationSpan();
    const std::vector<Type*>& mem = type->getUnionMembers();
    if (mem.empty()) {
      throw CodegenError(unionDeclSpan, "Internal error: union type has no members");
    }
    for (Type* m : mem) {
      getOrCreateLlvmType(m);
    }
    const llvm::DataLayout& dl = theModule->getDataLayout();
    unsigned maxAlloc = 0;
    unsigned maxAbiAlign = 1;
    for (Type* m : mem) {
      llvm::Type* const lt = getStoredAggregateFieldLlvmType(m);
      maxAlloc = std::max(maxAlloc, static_cast<unsigned>(dl.getTypeAllocSize(lt).getFixedValue()));
      maxAbiAlign = std::max(maxAbiAlign, static_cast<unsigned>(dl.getABITypeAlign(lt).value()));
    }
    unsigned const payloadBytes = llvm::alignTo(maxAlloc, maxAbiAlign);
    unsigned const numI64 = std::max(1U, (payloadBytes + 7U) / 8U);
    llvm::Type* const payloadTy = llvm::ArrayType::get(builder->getInt64Ty(), numI64);
    unsigned const minTagBits = unionDiscriminantMinBits(mem.size());
    unsigned const tagBitWidth = roundUnionTagToSupportedBitWidth(minTagBits);
    if (tagBitWidth == 0) {
      throw CodegenError(
          unionDeclSpan,
          "Union has too many members for the discriminant (tag width would exceed 64 bits)");
    }
    llvm::Type* const tagTy = builder->getIntNTy(tagBitWidth);
    llvm::StructType* const st = llvm::StructType::get(theModule->getContext(), {tagTy, payloadTy});
    type->setLlvmType(st);
    break;
  }
  case BaseType::TY_CLASS: {
    // Create opaque struct first to break recursion (e.g. class with field
    // *Self). Leading slot: vtable pointer (single inheritance, dynamic dispatch).
    llvm::StructType* st = nullptr;
    if (!type->getDisplayName().empty()) {
      st = llvm::StructType::create(theModule->getContext(), type->getDisplayName());
    } else {
      st = llvm::StructType::create(theModule->getContext());
    }
    type->setLlvmType(st);
    std::vector<llvm::Type*> elementTypes;
    elementTypes.push_back(builder->getPtrTy());
    for (auto* f : type->getFields()) {
      elementTypes.push_back(getStoredAggregateFieldLlvmType(f->type));
    }
    if (elementTypes.size() == 1U) {
      elementTypes.push_back(builder->getInt8Ty());
    }
    st->setBody(elementTypes);
    break;
  }
  case BaseType::TY_ENUM: {
    llvm::StructType* st = nullptr;
    if (!type->getDisplayName().empty()) {
      st = llvm::StructType::create(theModule->getContext(), type->getDisplayName());
    } else {
      st = llvm::StructType::create(theModule->getContext());
    }
    type->setLlvmType(st);
    auto variants = type->getEnumVariants();
    if (variants.empty()) {
      st->setBody({builder->getInt8Ty()});
      break;
    }
    const llvm::DataLayout& dl = theModule->getDataLayout();
    unsigned maxAlloc = 0U;
    unsigned maxAbiAlign = 1U;
    for (unsigned idx = 0; idx < variants.size(); ++idx) {
      EnumVariant* variant = variants[idx];
      if (variant == nullptr) {
        continue;
      }
      Type* aggregatePayloadType = getEnumVariantAggregatePayloadType(type, idx);
      if (aggregatePayloadType == nullptr) {
        continue;
      }
      llvm::Type* const lt = getStoredAggregateFieldLlvmType(aggregatePayloadType);
      maxAlloc = std::max(maxAlloc, static_cast<unsigned>(dl.getTypeAllocSize(lt).getFixedValue()));
      maxAbiAlign = std::max(maxAbiAlign, static_cast<unsigned>(dl.getABITypeAlign(lt).value()));
    }
    unsigned const payloadBytes = llvm::alignTo(maxAlloc, maxAbiAlign);
    unsigned const numI64 = std::max(1U, (payloadBytes + 7U) / 8U);
    llvm::Type* const payloadTy = llvm::ArrayType::get(builder->getInt64Ty(), numI64);
    unsigned const minTagBits = unionDiscriminantMinBits(variants.size());
    unsigned const tagBitWidth = roundUnionTagToSupportedBitWidth(minTagBits);
    if (tagBitWidth == 0U) {
      throw CodegenError(type->getDeclarationSpan(),
                         "Enum has too many variants for the discriminant");
    }
    llvm::Type* const tagTy = builder->getIntNTy(tagBitWidth);
    st->setBody({tagTy, payloadTy});
    break;
  }
  default:
    type->setLlvmType(builder->getPtrTy());
    break;
  }
  return type->getLlvmType();
}

auto Codegen::getOrCreateUnionTagLlvmType(lesma::Type* unionTy) -> llvm::Type* {
  if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
    llvm::SMRange const span = unionTy != nullptr ? unionTy->getDeclarationSpan() : llvm::SMRange{};
    throw CodegenError(span, "Internal error: getOrCreateUnionTagLlvmType expects a union type");
  }
  getOrCreateLlvmType(unionTy);
  auto* st = llvm::cast<llvm::StructType>(unionTy->getLlvmType());
  return st->getElementType(0U);
}

auto Codegen::getOrCreateEnumTagLlvmType(lesma::Type* enumTy) -> llvm::Type* {
  if (enumTy == nullptr || !enumTy->is(BaseType::TY_ENUM)) {
    llvm::SMRange const span = enumTy != nullptr ? enumTy->getDeclarationSpan() : llvm::SMRange{};
    throw CodegenError(span, "Internal error: getOrCreateEnumTagLlvmType expects an enum type");
  }
  getOrCreateLlvmType(enumTy);
  auto* st = llvm::cast<llvm::StructType>(enumTy->getLlvmType());
  return st->getElementType(0U);
}

auto Codegen::getEnumPayloadLlvmType(lesma::Type* enumTy) -> llvm::Type* {
  if (enumTy == nullptr || !enumTy->is(BaseType::TY_ENUM)) {
    llvm::SMRange const span = enumTy != nullptr ? enumTy->getDeclarationSpan() : llvm::SMRange{};
    throw CodegenError(span, "Internal error: getEnumPayloadLlvmType expects an enum type");
  }
  getOrCreateLlvmType(enumTy);
  auto* st = llvm::cast<llvm::StructType>(enumTy->getLlvmType());
  if (st->getNumElements() <= 1U) {
    return nullptr;
  }
  return st->getElementType(1U);
}

auto Codegen::getEnumVariantAggregatePayloadType(lesma::Type* enumTy, unsigned variantIndex)
    -> lesma::Type* {
  if (enumTy == nullptr || !enumTy->is(BaseType::TY_ENUM)) {
    llvm::SMRange const span = enumTy != nullptr ? enumTy->getDeclarationSpan() : llvm::SMRange{};
    throw CodegenError(span,
                       "Internal error: getEnumVariantAggregatePayloadType expects an enum type");
  }
  auto variants = enumTy->getEnumVariants();
  if (variantIndex >= variants.size() || variants[variantIndex] == nullptr) {
    throw CodegenError(enumTy->getDeclarationSpan(), "Invalid enum variant index");
  }
  EnumVariant* variant = variants[variantIndex];
  if (variant->payloadTypes.empty()) {
    return nullptr;
  }
  if (variant->payloadTypes.size() == 1U) {
    return variant->payloadTypes.front();
  }
  std::vector<std::unique_ptr<Field>> tupleFields;
  tupleFields.reserve(variant->payloadTypes.size());
  for (size_t i = 0; i < variant->payloadTypes.size(); ++i) {
    tupleFields.push_back(
        std::make_unique<Field>("_" + std::to_string(i), variant->payloadTypes[i]));
  }
  return cacheType(std::make_unique<Type>(BaseType::TY_TUPLE, nullptr, std::move(tupleFields)));
}

auto Codegen::getStoredAggregateFieldLlvmType(lesma::Type* fieldType) -> llvm::Type* {
  if (fieldType == nullptr) {
    return nullptr;
  }
  getOrCreateLlvmType(fieldType);
  if (fieldType->is(BaseType::TY_FUNCTION)) {
    return getFuncValuePairLlvmType();
  }
  if (fieldType->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return fieldType->getLlvmType();
  }
  if (TypeUtils::passesByPointerInAbi(fieldType)) {
    return builder->getPtrTy();
  }
  return fieldType->getLlvmType();
}

auto Codegen::loadStoredAggregateFieldValue(llvm::Value* slotPtr, lesma::Type* fieldType,
                                            const llvm::Twine& name) -> llvm::Value* {
  return builder->CreateLoad(getStoredAggregateFieldLlvmType(fieldType), slotPtr, name);
}

auto Codegen::collectTraitMetadataFromAst() -> void {
  Compound* ast = parser->getAst();
  if (ast == nullptr) {
    return;
  }
  for (Statement* stmt : ast->getChildren()) {
    if (auto* tr = dynamic_cast<TraitDecl*>(stmt)) {
      std::vector<std::string> order;
      for (FuncDecl* req : tr->getRequirements()) {
        order.push_back(req->getName());
      }
      traitRequirementMethodOrder[tr->getIdentifier()] = std::move(order);
      traitDeclByName[tr->getIdentifier()] = tr;
    }
  }
}

auto Codegen::mergeImportedTraitMetadata(Codegen const& imported) -> void {
  mergeImportedTraitMetadata(imported.captureImportedSpecializationState());
}

auto Codegen::mergeImportedTraitMetadata(ImportedSpecializationState const& imported) -> void {
  for (const auto& entry : imported.traitDeclByName) {
    if (traitDeclByName.contains(entry.first)) {
      continue;
    }
    traitDeclByName[entry.first] = entry.second;
    auto ordIt = imported.traitRequirementMethodOrder.find(entry.first);
    if (ordIt != imported.traitRequirementMethodOrder.end()) {
      traitRequirementMethodOrder[entry.first] = ordIt->second;
    }
  }
}

auto Codegen::captureImportedSpecializationState() const -> ImportedSpecializationState {
  ImportedSpecializationState importedState;
  importedState.genericClasses = genericClasses;
  importedState.genericEnums = genericEnums;
  importedState.specializedClassTypesByKey = specializedClassTypesByKey;
  importedState.specializedClassTypeEnvs = specializedClassTypeEnvs;
  importedState.specializedClassTemplateOf = specializedClassTemplateOf;
  importedState.specializationEnvs = specializationEnvs;
  importedState.codegenClassAstByDisplayName = codegenClassAstByDisplayName;
  importedState.codegenEnumAstByDisplayName = codegenEnumAstByDisplayName;
  importedState.traitRequirementMethodOrder = traitRequirementMethodOrder;
  importedState.traitDeclByName = traitDeclByName;
  return importedState;
}

auto Codegen::mergeImportedSpecializationState(ImportedSpecializationState const& imported)
    -> void {
  for (const auto& entry : imported.genericClasses) {
    genericClasses.insert(entry);
  }
  for (const auto& entry : imported.genericEnums) {
    genericEnums.insert(entry);
  }
  for (const auto& entry : imported.specializedClassTypesByKey) {
    specializedClassTypesByKey.insert(entry);
  }
  for (const auto& entry : imported.specializedClassTypeEnvs) {
    specializedClassTypeEnvs.insert(entry);
  }
  for (const auto& entry : imported.specializedClassTemplateOf) {
    specializedClassTemplateOf.insert(entry);
  }
  for (const auto& entry : imported.specializationEnvs) {
    specializationEnvs.insert(entry);
  }
  for (const auto& entry : imported.codegenClassAstByDisplayName) {
    codegenClassAstByDisplayName.insert(entry);
  }
  for (const auto& entry : imported.codegenEnumAstByDisplayName) {
    codegenEnumAstByDisplayName.insert(entry);
  }
}

auto Codegen::mergeImportedSpecializationState(Codegen const& imported) -> void {
  mergeImportedSpecializationState(imported.captureImportedSpecializationState());
}

auto Codegen::findTraitRequirement(const TraitDecl* trait, const std::string& methodName) const
    -> const FuncDecl* {
  if (trait == nullptr) {
    return nullptr;
  }
  for (FuncDecl* req : trait->getRequirements()) {
    if (req->getName() == methodName) {
      return req;
    }
  }
  return nullptr;
}

auto Codegen::emitErasedThunkForTraitMethod(lesma::Type* classType, const std::string& traitName,
                                            const FuncDecl* req) -> llvm::Function* {
  std::string cacheKey = traitName + "|" + classType->getDisplayName() + "|" + req->getName();
  if (auto it = traitThunkCache.find(cacheKey); it != traitThunkCache.end()) {
    return it->second;
  }

  lesma::Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
  std::vector<lesma::Type*> lookupArgs = {selfPtr};
  for (Parameter* p : req->getParameters()) {
    if (p->type != nullptr) {
      p->type->accept(*this);
      lesma::Type* pt = result->getType();
      if (pt->is(BaseType::TY_CLASS)) {
        pt = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, pt));
      }
      lookupArgs.push_back(pt);
    }
  }
  Value* methodSym = scope->lookupFunction(req->getName(), lookupArgs);
  if (methodSym == nullptr) {
    for (const auto& importedScope : *importedScopes) {
      if (importedScope == nullptr) {
        continue;
      }
      methodSym = importedScope->lookupFunction(req->getName(), lookupArgs);
      if (methodSym != nullptr) {
        break;
      }
    }
  }
  if (methodSym == nullptr || methodSym->getLlvmValue() == nullptr) {
    throw CodegenError(req->getSpan(), "Trait thunk: method {} not found for class {}",
                       req->getName(), classType->getDisplayName());
  }
  auto* realFn = llvm::cast<llvm::Function>(methodSym->getLlvmValue());
  llvm::FunctionType* rft = realFn->getFunctionType();

  llvm::SmallVector<llvm::Type*, 8> tparams;
  tparams.push_back(builder->getPtrTy());
  for (unsigned i = 1; i < rft->getNumParams(); ++i) {
    tparams.push_back(rft->getParamType(i));
  }
  llvm::FunctionType* tft = llvm::FunctionType::get(rft->getReturnType(), tparams, false);

  std::string thunkName = "lesma.trait.thunk." + cacheKey;
  for (char& c : thunkName) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '_') {
      c = '_';
    }
  }
  llvm::Function* thunk = theModule->getFunction(thunkName);
  if (thunk == nullptr) {
    thunk = llvm::Function::Create(tft, llvm::Function::InternalLinkage, thunkName, *theModule);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(theModule->getContext(), "entry", thunk);
    llvm::IRBuilder<> tmpBuilder(bb);
    llvm::Argument* rawArg = thunk->getArg(0);
    llvm::Value* castSelf = tmpBuilder.CreateBitCast(rawArg, rft->getParamType(0));
    llvm::SmallVector<llvm::Value*, 8> callArgs;
    callArgs.push_back(castSelf);
    for (unsigned i = 1; i < rft->getNumParams(); ++i) {
      callArgs.push_back(thunk->getArg(i));
    }
    if (rft->getReturnType()->isVoidTy()) {
      tmpBuilder.CreateCall(rft, realFn, callArgs);
      tmpBuilder.CreateRetVoid();
    } else {
      llvm::Value* r = tmpBuilder.CreateCall(rft, realFn, callArgs);
      tmpBuilder.CreateRet(r);
    }
  }
  traitThunkCache[cacheKey] = thunk;
  return thunk;
}

auto Codegen::getOrEmitWitnessTable(lesma::Type* classType, const std::string& traitName)
    -> llvm::GlobalVariable* {
  std::string cacheKey = traitName + "|" + classType->getDisplayName();
  if (auto it = witnessGlobalCache.find(cacheKey); it != witnessGlobalCache.end()) {
    return it->second;
  }
  const std::string baseTraitName = this->traitExistentialBaseName(traitName);
  const TraitDecl* tr = traitDeclByName[baseTraitName];
  if (tr == nullptr) {
    throw CodegenError({}, "Codegen: unknown trait {}", traitName);
  }
  auto ordIt = traitRequirementMethodOrder.find(baseTraitName);
  if (ordIt == traitRequirementMethodOrder.end()) {
    throw CodegenError({}, "Codegen: trait {} has no requirement order", traitName);
  }
  std::vector<llvm::Constant*> constants;
  constants.reserve(ordIt->second.size());
  for (const std::string& mn : ordIt->second) {
    const FuncDecl* req = findTraitRequirement(tr, mn);
    if (req == nullptr) {
      throw CodegenError({}, "Codegen: trait {} missing requirement {}", traitName, mn);
    }
    llvm::Function* thunk = emitErasedThunkForTraitMethod(classType, traitName, req);
    constants.push_back(llvm::ConstantExpr::getBitCast(thunk, builder->getPtrTy()));
  }
  llvm::ArrayType* at = llvm::ArrayType::get(builder->getPtrTy(), constants.size());
  llvm::Constant* init = llvm::ConstantArray::get(at, constants);
  std::string gname = "lesma.witness." + cacheKey;
  for (char& c : gname) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '_') {
      c = '_';
    }
  }
  auto* gv = new llvm::GlobalVariable(*theModule, at, true, llvm::GlobalValue::PrivateLinkage, init,
                                      gname);
  witnessGlobalCache[cacheKey] = gv;
  return gv;
}

auto Codegen::emitBoxClassToExistential(lesma::Type* existentialType,
                                        lesma::Type* classPtrLesmaType, llvm::Value* classPtrVal)
    -> llvm::Value* {
  if (!classPtrLesmaType->is(BaseType::TY_PTR) || classPtrLesmaType->getElementType() == nullptr ||
      !classPtrLesmaType->getElementType()->is(BaseType::TY_CLASS)) {
    throw CodegenError({}, "Box to existential requires class pointer");
  }
  lesma::Type* cls = classPtrLesmaType->getElementType();
  const std::string traitName = existentialType->getDisplayName();
  llvm::GlobalVariable* wit = getOrEmitWitnessTable(cls, traitName);
  getOrCreateLlvmType(existentialType);
  llvm::StructType* st = llvm::cast<llvm::StructType>(existentialType->getLlvmType());
  llvm::Value* payload = builder->CreateBitCast(classPtrVal, builder->getPtrTy());
  llvm::Value* wptr = builder->CreateBitCast(wit, builder->getPtrTy());
  llvm::Value* u = llvm::UndefValue::get(st);
  llvm::Value* s0 = builder->CreateInsertValue(u, payload, 0U);
  return builder->CreateInsertValue(s0, wptr, 1U);
}

auto Codegen::callExistentialMethod(llvm::SMRange span, lesma::Value* receiver,
                                    const std::string& methodName,
                                    const std::vector<lesma::Value*>& args,
                                    const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  (void) explicitTypeArgs;
  lesma::Type* receiverType = receiver->getType();
  llvm::Value* fatVal = receiver->getLlvmValue();
  if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr &&
      receiverType->getElementType()->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    fatVal = builder->CreateLoad(receiverType->getElementType()->getLlvmType(), fatVal);
    receiverType = receiverType->getElementType();
  }
  if (!receiverType->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    throw CodegenError(span, "Expected trait existential receiver for dynamic dispatch");
  }
  const std::string receiverDisplay = receiverType->getDisplayName();
  const std::string baseTraitName = this->traitExistentialBaseName(receiverDisplay);
  auto ordIt = traitRequirementMethodOrder.find(baseTraitName);
  if (ordIt == traitRequirementMethodOrder.end()) {
    throw CodegenError(span, "Trait {} has no codegen metadata", receiverDisplay);
  }
  const auto& order = ordIt->second;
  size_t idx = static_cast<size_t>(-1);
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == methodName) {
      idx = i;
      break;
    }
  }
  if (idx == static_cast<size_t>(-1)) {
    throw CodegenError(span, "Method {} not in trait {}", methodName, receiverDisplay);
  }
  const TraitDecl* tr = traitDeclByName[baseTraitName];
  const FuncDecl* req = findTraitRequirement(tr, methodName);
  if (req == nullptr) {
    throw CodegenError(span, "Trait {} missing requirement {}", receiverDisplay, methodName);
  }

  llvm::Value* payload = builder->CreateExtractValue(fatVal, 0U);
  llvm::Value* tablePtr = builder->CreateExtractValue(fatVal, 1U);
  llvm::Type* ptrTy = builder->getPtrTy();
  llvm::ArrayType* arrTy = llvm::ArrayType::get(ptrTy, order.size());
  llvm::Value* slot = builder->CreateInBoundsGEP(
      arrTy, tablePtr, {builder->getInt32(0), builder->getInt32(static_cast<unsigned>(idx))});
  llvm::Value* fnPtr = builder->CreateLoad(ptrTy, slot);

  llvm::SmallVector<llvm::Type*, 8> tparams;
  tparams.push_back(ptrTy);
  auto savedGenerics = currentGenericTypes;
  if (const auto* envPtr = specializedTraitExistentialEnvFor(receiverType); envPtr != nullptr) {
    currentGenericTypes = *envPtr;
  }
  lesma::Type* retLesma = nullptr;
  try {
    for (Parameter* p : req->getParameters()) {
      if (p->type != nullptr) {
        p->type->accept(*this);
        lesma::Type* pt = result->getType();
        getOrCreateLlvmType(pt);
        tparams.push_back(pt->getLlvmType());
      }
    }
    req->getReturnType()->accept(*this);
    retLesma = result->getType();
    getOrCreateLlvmType(retLesma);
  } catch (...) {
    currentGenericTypes = std::move(savedGenerics);
    throw;
  }
  currentGenericTypes = std::move(savedGenerics);
  llvm::Type* llvmRet = retLesma->getLlvmType();
  llvm::FunctionType* callTy = llvm::FunctionType::get(llvmRet, tparams, false);
  llvm::SmallVector<llvm::Value*, 8> callArgs;
  callArgs.push_back(payload);
  for (lesma::Value* a : args) {
    callArgs.push_back(a->getLlvmValue());
  }
  if (llvmRet->isVoidTy()) {
    builder->CreateCall(callTy, fnPtr, callArgs);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  llvm::Value* ret = builder->CreateCall(callTy, fnPtr, callArgs);
  return std::make_unique<Value>("", retLesma, ret);
}
