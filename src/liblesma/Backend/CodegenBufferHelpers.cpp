#include <algorithm>
#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>

#include "Codegen.h"

#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

using namespace lesma;
using namespace llvm;

auto Codegen::getOrCreateListStructType(lesma::Type* listType) -> llvm::StructType* {
  std::string typeName = "lesma.list";
  if (listType != nullptr && listType->getElementType() != nullptr) {
    getOrCreateLlvmType(listType->getElementType());
    typeName += "." + MangleUtils::getTypeMangledName({}, listType->getElementType());
  }
  auto existing = listStructTypes.find(typeName);
  if (existing != listStructTypes.end()) {
    return existing->second;
  }
  auto* structType = llvm::StructType::create(
      theModule->getContext(), {builder->getPtrTy(), builder->getInt64Ty(), builder->getInt64Ty()},
      typeName, false);
  listStructTypes[typeName] = structType;
  return structType;
}

auto Codegen::getListStoredElementType(lesma::Type* listType) -> llvm::Type* {
  auto* elementType = listType->getElementType();
  if (elementType != nullptr && elementType->is(BaseType::TY_CLASS)) {
    return builder->getPtrTy();
  }
  return getOrCreateLlvmType(elementType);
}

auto Codegen::getListStoredElementValue(llvm::SMRange span, lesma::Value* value,
                                        lesma::Type* elementType) -> llvm::Value* {
  if (elementType != nullptr && elementType->is(BaseType::TY_CLASS)) {
    return value->getLlvmValue();
  }
  auto castVal = cast(span, value, elementType);
  return castVal->getLlvmValue();
}

auto Codegen::emitCalloc(llvm::Value* count, llvm::Value* size, const llvm::Twine& name)
    -> llvm::Value* {
  auto callocFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::CALLOC},
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty(), builder->getInt64Ty()},
                              false));
  return builder->CreateCall(callocFn, {count, size}, name);
}

auto Codegen::emitMalloc(llvm::Value* size, const llvm::Twine& name) -> llvm::Value* {
  auto mallocFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::MALLOC},
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty()}, false));
  return builder->CreateCall(mallocFn, {size}, name);
}

auto Codegen::emitRealloc(llvm::Value* ptr, llvm::Value* size, const llvm::Twine& name)
    -> llvm::Value* {
  auto reallocFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::REALLOC},
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getPtrTy(), builder->getInt64Ty()},
                              false));
  return builder->CreateCall(reallocFn, {ptr, size}, name);
}

auto Codegen::emitFree(llvm::Value* ptr) -> void {
  auto freeFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::FREE},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
  builder->CreateCall(freeFn, {ptr});
}

auto Codegen::emitExit(int code) -> void {
  auto exitFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::EXIT},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt64Ty()}, false));
  builder->CreateCall(exitFn, {builder->getInt64(code)});
}

auto Codegen::emitListLength(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* lenPtr = builder->CreateStructGEP(structType, listHandle, 1, "list.len.ptr");
  return builder->CreateLoad(builder->getInt64Ty(), lenPtr, "list.len");
}

auto Codegen::emitListCapacity(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* capPtr = builder->CreateStructGEP(structType, listHandle, 2, "list.cap.ptr");
  return builder->CreateLoad(builder->getInt64Ty(), capPtr, "list.cap");
}

auto Codegen::emitListDataPtr(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* dataPtrPtr = builder->CreateStructGEP(structType, listHandle, 0, "list.data.ptr.ptr");
  return builder->CreateLoad(builder->getPtrTy(), dataPtrPtr, "list.data.ptr");
}

auto Codegen::emitStoreListDataPtr(lesma::Type* listType, llvm::Value* listHandle,
                                   llvm::Value* dataValue) -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 0, "list.data.slot");
  builder->CreateStore(dataValue, slot);
}

auto Codegen::emitStoreListLength(lesma::Type* listType, llvm::Value* listHandle,
                                  llvm::Value* length) -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 1, "list.len.slot");
  builder->CreateStore(length, slot);
}

auto Codegen::emitStoreListCapacity(lesma::Type* listType, llvm::Value* listHandle,
                                    llvm::Value* capacity) -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 2, "list.cap.slot");
  builder->CreateStore(capacity, slot);
}

auto Codegen::emitListBoundsCheck(llvm::SMRange span, lesma::Type* listType,
                                  llvm::Value* listHandle, llvm::Value* index) -> void {
  (void) span;
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* okBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.bounds.ok", parentFunction);
  auto* failBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.bounds.fail", parentFunction);
  auto* length = emitListLength(listType, listHandle);
  auto* nonNegative = builder->CreateICmpSGE(index, builder->getInt64(0));
  auto* inRange = builder->CreateICmpSLT(index, length);
  builder->CreateCondBr(builder->CreateLogicalAnd(nonNegative, inRange), okBlock, failBlock);

  builder->SetInsertPoint(failBlock);
  emitExit(1);
  builder->CreateUnreachable();

  builder->SetInsertPoint(okBlock);
}

auto Codegen::emitListElementPointer(llvm::SMRange span, lesma::Type* listType,
                                     llvm::Value* listHandle, llvm::Value* index) -> llvm::Value* {
  emitListBoundsCheck(span, listType, listHandle, index);
  auto* dataPtr = emitListDataPtr(listType, listHandle);
  return builder->CreateGEP(getListStoredElementType(listType), dataPtr, index, "list.elem.ptr");
}

auto Codegen::emitListEnsureCapacity(lesma::Type* listType, llvm::Value* listHandle,
                                     llvm::Value* minCapacity) -> void {
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* currentBlock = builder->GetInsertBlock();
  auto* growBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.grow", parentFunction);
  auto* doneBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.done", parentFunction);
  auto* capacity = emitListCapacity(listType, listHandle);
  auto* currentData = emitListDataPtr(listType, listHandle);
  builder->CreateCondBr(builder->CreateICmpSGE(capacity, minCapacity), doneBlock, growBlock);

  builder->SetInsertPoint(growBlock);
  auto* doubledCap = builder->CreateMul(capacity, builder->getInt64(2), "list.cap.doubled");
  auto* baseCap = builder->CreateSelect(builder->CreateICmpEQ(capacity, builder->getInt64(0)),
                                        builder->getInt64(1), doubledCap);
  auto* newCap = builder->CreateSelect(builder->CreateICmpSGE(baseCap, minCapacity), baseCap,
                                       minCapacity, "list.cap.new");
  auto* elementSize = builder->getInt64(theModule->getDataLayout()
                                            .getTypeAllocSize(getListStoredElementType(listType))
                                            .getFixedValue());
  auto* newBytes = builder->CreateMul(newCap, elementSize, "list.grow.bytes");
  auto* allocBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.alloc", parentFunction);
  auto* reallocBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.realloc", parentFunction);
  builder->CreateCondBr(
      builder->CreateICmpEQ(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      allocBlock, reallocBlock);

  builder->SetInsertPoint(allocBlock);
  auto* allocatedData = emitMalloc(newBytes, "list.grow.alloc");
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(reallocBlock);
  auto* reallocatedData = emitRealloc(currentData, newBytes, "list.grow.realloc");
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
  auto* newData = builder->CreatePHI(builder->getPtrTy(), 3, "list.grow.result");
  auto* finalCapacity = builder->CreatePHI(builder->getInt64Ty(), 3, "list.grow.capacity");
  newData->addIncoming(currentData, currentBlock);
  newData->addIncoming(allocatedData, allocBlock);
  newData->addIncoming(reallocatedData, reallocBlock);
  finalCapacity->addIncoming(capacity, currentBlock);
  finalCapacity->addIncoming(newCap, allocBlock);
  finalCapacity->addIncoming(newCap, reallocBlock);
  emitStoreListDataPtr(listType, listHandle, newData);
  emitStoreListCapacity(listType, listHandle, finalCapacity);
}

auto Codegen::emitListDeepCopy(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* newHandle = emitMalloc(headerSize, "list.copy.header");
  auto* length = emitListLength(listType, listHandle);
  auto* capacity = emitListCapacity(listType, listHandle);
  emitStoreListLength(listType, newHandle, length);
  emitStoreListCapacity(listType, newHandle, capacity);
  auto* elementType = listType->getElementType();
  auto* elementLlvmType = getListStoredElementType(listType);
  auto* elementSize = builder->getInt64(
      theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue());
  auto* hasCapacity = builder->CreateICmpSGT(capacity, builder->getInt64(0));
  auto* dataSlot = builder->CreateAlloca(builder->getPtrTy(), nullptr, "list.copy.data.slot");
  builder->CreateStore(llvm::ConstantPointerNull::get(builder->getPtrTy()), dataSlot);

  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* copyElementsBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.copy.elements", parentFunction);
  auto* doneBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.copy.done", parentFunction);
  builder->CreateCondBr(hasCapacity, copyElementsBlock, doneBlock);

  builder->SetInsertPoint(copyElementsBlock);
  auto* newBytes = builder->CreateMul(capacity, elementSize, "list.copy.bytes");
  auto* initBytes = builder->CreateMul(length, elementSize, "list.copy.initbytes");
  auto* newData = emitMalloc(newBytes, "list.copy.data");
  builder->CreateStore(newData, dataSlot);
  auto isNestedListLike = [elementType]() -> bool {
    if (elementType == nullptr || !elementType->is(BaseType::TY_CLASS)) {
      return false;
    }
    auto fields = elementType->getFields();
    return !fields.empty() && fields.front()->type != nullptr &&
           fields.front()->type->is(BaseType::TY_ARRAY);
  };
  if (elementType->is(BaseType::TY_ARRAY) || isNestedListLike()) {
    auto* indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "list.copy.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
    auto* loopCond =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.cond", parentFunction);
    auto* loopBody =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.body", parentFunction);
    auto* loopInc =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.inc", parentFunction);
    builder->CreateBr(loopCond);

    builder->SetInsertPoint(loopCond);
    auto* index = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    builder->CreateCondBr(builder->CreateICmpSLT(index, length), loopBody, doneBlock);

    builder->SetInsertPoint(loopBody);
    auto* oldElementPtr = emitListElementPointer({}, listType, listHandle, index);
    auto* oldElement = builder->CreateLoad(elementLlvmType, oldElementPtr);
    llvm::Value* copiedElement = nullptr;
    if (elementType->is(BaseType::TY_ARRAY)) {
      copiedElement = emitListDeepCopy(elementType, oldElement);
    } else {
      lesma::Value oldValue("", elementType, oldElement);
      auto copiedValue = callMethodByName({}, &oldValue, "copy");
      copiedElement = copiedValue->getLlvmValue();
    }
    auto* newElementPtr = builder->CreateGEP(elementLlvmType, newData, index, "list.copy.elem.ptr");
    builder->CreateStore(copiedElement, newElementPtr);
    builder->CreateBr(loopInc);

    builder->SetInsertPoint(loopInc);
    auto* nextIndex = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                         builder->getInt64(1));
    builder->CreateStore(nextIndex, indexPtr);
    builder->CreateBr(loopCond);
  } else {
    auto memcpyFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::MEMCPY},
        llvm::FunctionType::get(builder->getPtrTy(),
                                {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()},
                                false));
    builder->CreateCall(memcpyFn, {newData, emitListDataPtr(listType, listHandle), initBytes});
    builder->CreateBr(doneBlock);
  }

  builder->SetInsertPoint(doneBlock);
  emitStoreListDataPtr(listType, newHandle, builder->CreateLoad(builder->getPtrTy(), dataSlot));
  return newHandle;
}

auto Codegen::lookupClassStructSymbol(lesma::Type* classTy) -> Value* {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return nullptr;
  }
  if (auto it = specializedClassSymbolsByType.find(classTy); it != specializedClassSymbolsByType.end()) {
    return it->second;
  }
  if (auto envIt = specializedClassTypeEnvs.find(classTy); envIt != specializedClassTypeEnvs.end()) {
    bool isConcrete = true;
    for (const auto& [name, ty] : envIt->second) {
      (void) name;
      if (!isTypeFullyConcrete(ty)) {
        isConcrete = false;
        break;
      }
    }
    if (isConcrete) {
      Type* templateTy = classTy;
      if (auto it = specializedClassTemplateOf.find(classTy); it != specializedClassTemplateOf.end()) {
        templateTy = it->second;
      }
      if (const Class* templateAst = findGenericClassAstForTemplateType(templateTy);
          templateAst != nullptr) {
        return emitClassMonomorph(classTy, templateAst);
      }
    }
  }
  llvm::Type* lt = classTy->getLlvmType();
  if (lt != nullptr) {
    if (auto* st = llvm::dyn_cast<llvm::StructType>(lt)) {
      if (st->hasName()) {
        if (Value* v = scope->lookupStruct(st->getName().str())) {
          return v;
        }
      }
    }
  }
  if (!classTy->getDisplayName().empty()) {
    if (Value* v = scope->lookupStruct(classTy->getDisplayName())) {
      return v;
    }
  }
  lt = classTy->getLlvmType();
  if (lt != nullptr) {
    if (auto* st = llvm::dyn_cast<llvm::StructType>(lt)) {
      if (st->hasName()) {
        if (Value* v = scope->lookupStruct(st->getName().str())) {
          return v;
        }
      }
    }
  }
  return nullptr;
}

auto Codegen::specializedClassEnvFor(lesma::Type* classTy)
    -> const std::unordered_map<std::string, lesma::Type*>* {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return nullptr;
  }
  if (auto it = specializedClassTypeEnvs.find(classTy); it != specializedClassTypeEnvs.end()) {
    return &it->second;
  }
  if (Value* sym = lookupClassStructSymbol(classTy); sym != nullptr && sym->getType() != nullptr) {
    if (auto it = specializedClassTypeEnvs.find(sym->getType()); it != specializedClassTypeEnvs.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

auto Codegen::specializedTraitExistentialEnvFor(lesma::Type* existentialTy)
    -> const std::unordered_map<std::string, lesma::Type*>* {
  if (existentialTy == nullptr || !existentialTy->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return nullptr;
  }
  if (auto it = specializedClassTypeEnvs.find(existentialTy); it != specializedClassTypeEnvs.end()) {
    return &it->second;
  }
  return nullptr;
}

auto Codegen::lookupClassVtableGlobal(lesma::Type* classTy) -> llvm::GlobalVariable* {
  if (classTy == nullptr) {
    return nullptr;
  }
  if (auto it = classVtableGlobals.find(classTy); it != classVtableGlobals.end()) {
    return it->second;
  }
  if (Value* sym = lookupClassStructSymbol(classTy); sym != nullptr && sym->getType() != nullptr) {
    if (auto it = classVtableGlobals.find(sym->getType()); it != classVtableGlobals.end()) {
      return it->second;
    }
  }
  return nullptr;
}
