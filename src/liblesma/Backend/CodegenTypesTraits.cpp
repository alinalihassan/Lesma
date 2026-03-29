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

namespace {

// `Iterator<int>` -> `Iterator` for trait registry / witness metadata keyed by trait identifier.
auto traitExistentialBaseName(const std::string& displayName) -> std::string {
  if (const auto pos = displayName.find('<'); pos != std::string::npos) {
    return displayName.substr(0U, pos);
  }
  return displayName;
}

} // namespace

using namespace lesma;
using namespace llvm;

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
    funcType->setReturnType(retType->getType());
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
  } else if (node->getType() == TokenType::CUSTOM_TYPE) {
    const std::string lookupName = node->getLookupName();
    auto git = currentGenericTypes.find(lookupName);
    if (git != currentGenericTypes.end()) {
      result = std::make_unique<Value>(git->second);
      return;
    }
    std::vector<lesma::Type*> explicitTypeArgs;
    for (auto* typeArg : node->getTypeArgs()) {
      typeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
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
      if (auto gitClass = genericClasses.find(lookupName); gitClass != genericClasses.end()) {
        templateClass = gitClass->second;
      } else if (sym != nullptr && sym->getGenericClassTemplate() != nullptr) {
        templateClass = static_cast<const Class*>(sym->getGenericClassTemplate());
      }
      if (templateClass == nullptr) {
        throw CodegenError(node->getSpan(), "Type {} is not a generic class", node->getName());
      }
      sym = specializeClass(templateClass, {}, explicitTypeArgs);
      typ = sym->getType();
    }
    if (sym->getType()->getLlvmType() == nullptr) {
      getOrCreateLlvmType(sym->getType());
    }

    result = std::make_unique<Value>(*sym);
  } else {
    throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
  }
}

auto Codegen::getOrCreateLlvmType(lesma::Type* type) -> llvm::Type* {
  if (type->getLlvmType() != nullptr) {
    return type->getLlvmType();
  }
  switch (type->getBaseType()) {
  case BaseType::TY_GENERIC: {
    auto it = currentGenericTypes.find(type->getGenericName());
    if (it == currentGenericTypes.end()) {
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
  case BaseType::TY_VOID:
    type->setLlvmType(builder->getVoidTy());
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
    std::vector<llvm::Type*> elementTypes;
    for (auto* f : type->getFields()) {
      elementTypes.push_back(getOrCreateLlvmType(f->type));
    }
    if (elementTypes.empty()) {
      elementTypes.push_back(builder->getInt8Ty());
    }
    st->setBody(elementTypes);
    break;
  }
  default:
    type->setLlvmType(builder->getPtrTy());
    break;
  }
  return type->getLlvmType();
}

auto Codegen::getStoredAggregateFieldLlvmType(lesma::Type* fieldType) -> llvm::Type* {
  if (fieldType == nullptr) {
    return nullptr;
  }
  getOrCreateLlvmType(fieldType);
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
  importedState.specializedClassTypesByKey = specializedClassTypesByKey;
  importedState.specializedClassTypeEnvs = specializedClassTypeEnvs;
  importedState.specializedClassTemplateOf = specializedClassTemplateOf;
  importedState.specializationEnvs = specializationEnvs;
  importedState.codegenClassAstByDisplayName = codegenClassAstByDisplayName;
  importedState.traitRequirementMethodOrder = traitRequirementMethodOrder;
  importedState.traitDeclByName = traitDeclByName;
  return importedState;
}

auto Codegen::mergeImportedSpecializationState(ImportedSpecializationState const& imported) -> void {
  for (const auto& entry : imported.genericClasses) {
    genericClasses.insert(entry);
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
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_')) {
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
  const std::string baseTraitName = traitExistentialBaseName(traitName);
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
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_')) {
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
  const std::string baseTraitName = traitExistentialBaseName(receiverDisplay);
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
  if (const auto* envPtr = specializedClassEnvFor(receiverType); envPtr != nullptr) {
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
