#include "Codegen.h"

#include <algorithm>
#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include "liblesma/Backend/CodegenError.h"
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
      std::string{codegen::runtime::kCalloc},
      llvm::FunctionType::get(builder->getPtrTy(),
                              {builder->getInt64Ty(), builder->getInt64Ty()}, false));
  return builder->CreateCall(callocFn, {count, size}, name);
}

auto Codegen::emitMalloc(llvm::Value* size, const llvm::Twine& name) -> llvm::Value* {
  auto mallocFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kMalloc},
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty()}, false));
  return builder->CreateCall(mallocFn, {size}, name);
}

auto Codegen::emitRealloc(llvm::Value* ptr, llvm::Value* size, const llvm::Twine& name)
    -> llvm::Value* {
  auto reallocFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kRealloc},
      llvm::FunctionType::get(builder->getPtrTy(),
                              {builder->getPtrTy(), builder->getInt64Ty()}, false));
  return builder->CreateCall(reallocFn, {ptr, size}, name);
}

auto Codegen::emitFree(llvm::Value* ptr) -> void {
  auto freeFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kFree},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
  builder->CreateCall(freeFn, {ptr});
}

auto Codegen::emitExit(int code) -> void {
  auto exitFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kExit},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt64Ty()}, false));
  builder->CreateCall(exitFn, {builder->getInt64(code)});
}

auto Codegen::emitArcAllocClass(llvm::Value* totalSize, const llvm::Twine& name) -> llvm::Value* {
  llvm::FunctionCallee fn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kLesmaArcAlloc},
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty()}, false));
  return builder->CreateCall(fn, {totalSize}, name);
}

void Codegen::emitArcRetain(llvm::Value* classPtr) {
  llvm::FunctionCallee fn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kLesmaArcRetain},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
  builder->CreateCall(fn, {classPtr});
}

void Codegen::emitArcRelease(llvm::Value* classPtr, lesma::Type* classHeapType) {
  if (classPtr == nullptr || classHeapType == nullptr || !classHeapType->is(BaseType::TY_CLASS)) {
    return;
  }
  llvm::Function* fin = getOrEmitClassFinalize(classHeapType);
  llvm::FunctionCallee callee = theModule->getOrInsertFunction(
      std::string{codegen::runtime::kLesmaArcRelease},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy(), builder->getPtrTy()},
                              false));
  llvm::Value* finPtr = builder->CreateBitCast(fin, builder->getPtrTy());
  builder->CreateCall(callee, {classPtr, finPtr});
}

void Codegen::clearArcTracking() { arcTrackedLocals.clear(); }

void Codegen::emitArcReleaseAllTrackedLocals() {
  for (lesma::Value* sym : arcTrackedLocals) {
    if (sym == nullptr || sym->getLlvmValue() == nullptr) {
      continue;
    }
    lesma::Type* t = sym->getType();
    if (t == nullptr || !t->is(BaseType::TY_PTR) || t->getElementType() == nullptr ||
        !t->getElementType()->is(BaseType::TY_CLASS)) {
      continue;
    }
    llvm::Value* slot = sym->getLlvmValue();
    llvm::Value* p = builder->CreateLoad(builder->getPtrTy(), slot, "arc.leave");
    emitArcRelease(p, t->getElementType());
  }
}

void Codegen::emitArcReleaseAllTrackedLocalsExcept(lesma::Value* exceptSym) {
  for (lesma::Value* sym : arcTrackedLocals) {
    if (sym == nullptr || sym == exceptSym || sym->getLlvmValue() == nullptr) {
      continue;
    }
    lesma::Type* t = sym->getType();
    if (t == nullptr || !t->is(BaseType::TY_PTR) || t->getElementType() == nullptr ||
        !t->getElementType()->is(BaseType::TY_CLASS)) {
      continue;
    }
    llvm::Value* slot = sym->getLlvmValue();
    llvm::Value* p = builder->CreateLoad(builder->getPtrTy(), slot, "arc.leave");
    emitArcRelease(p, t->getElementType());
  }
}

auto Codegen::classUserFieldLlvmIndex(unsigned userFieldIndex) -> unsigned {
  return userFieldIndex + 1U;
}

void Codegen::emitArcRetainOutgoingCallArgs(const std::vector<Field*>& fields,
                                            const std::vector<llvm::Value*>& llvmVals,
                                            size_t skipFirst) {
  for (size_t i = skipFirst; i < llvmVals.size() && i < fields.size(); ++i) {
    Field* f = fields[i];
    if (f == nullptr) {
      continue;
    }
    lesma::Type* ft = f->type;
    if (ft != nullptr && ft->is(BaseType::TY_PTR) && ft->getElementType() != nullptr &&
        ft->getElementType()->is(BaseType::TY_CLASS)) {
      emitArcRetain(llvmVals[i]);
    }
  }
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

void Codegen::emitListReleaseClassElementsIfNeeded(lesma::Type* listType,
                                                   llvm::Value* listHandle) {
  lesma::Type* el = listType != nullptr ? listType->getElementType() : nullptr;
  if (el == nullptr || !el->is(BaseType::TY_CLASS)) {
    return;
  }
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* length = emitListLength(listType, listHandle);
  auto* dataPtr = emitListDataPtr(listType, listHandle);
  auto* idxPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "list.rel.i");
  builder->CreateStore(builder->getInt64(0), idxPtr);
  auto* loopCond =
      llvm::BasicBlock::Create(theModule->getContext(), "list.rel.cond", parentFunction);
  auto* loopBody =
      llvm::BasicBlock::Create(theModule->getContext(), "list.rel.body", parentFunction);
  auto* loopInc = llvm::BasicBlock::Create(theModule->getContext(), "list.rel.inc", parentFunction);
  auto* loopEnd = llvm::BasicBlock::Create(theModule->getContext(), "list.rel.end", parentFunction);
  builder->CreateBr(loopCond);

  builder->SetInsertPoint(loopCond);
  auto* idx = builder->CreateLoad(builder->getInt64Ty(), idxPtr);
  builder->CreateCondBr(builder->CreateICmpSLT(idx, length), loopBody, loopEnd);

  builder->SetInsertPoint(loopBody);
  auto* elemTy = getListStoredElementType(listType);
  auto* elemPtr = builder->CreateGEP(elemTy, dataPtr, idx, "list.rel.elem");
  auto* p = builder->CreateLoad(elemTy, elemPtr);
  emitArcRelease(p, el);
  builder->CreateBr(loopInc);

  builder->SetInsertPoint(loopInc);
  auto* next = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), idxPtr),
                                  builder->getInt64(1));
  builder->CreateStore(next, idxPtr);
  builder->CreateBr(loopCond);

  builder->SetInsertPoint(loopEnd);
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
  } else if (elementType != nullptr && elementType->is(BaseType::TY_CLASS)) {
    auto* indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "list.copy.class.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
    auto* loopCond =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.class.cond", parentFunction);
    auto* loopBody =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.class.body", parentFunction);
    auto* loopInc =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.class.inc", parentFunction);
    builder->CreateBr(loopCond);

    builder->SetInsertPoint(loopCond);
    auto* index = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    builder->CreateCondBr(builder->CreateICmpSLT(index, length), loopBody, doneBlock);

    builder->SetInsertPoint(loopBody);
    auto* oldElementPtr = emitListElementPointer({}, listType, listHandle, index);
    auto* oldPtr = builder->CreateLoad(elementLlvmType, oldElementPtr);
    emitArcRetain(oldPtr);
    auto* newElementPtr = builder->CreateGEP(elementLlvmType, newData, index, "list.copy.class.ptr");
    builder->CreateStore(oldPtr, newElementPtr);
    builder->CreateBr(loopInc);

    builder->SetInsertPoint(loopInc);
    auto* nextIndex = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                         builder->getInt64(1));
    builder->CreateStore(nextIndex, indexPtr);
    builder->CreateBr(loopCond);
  } else {
    auto memcpyFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::kMemcpy},
        llvm::FunctionType::get(
            builder->getPtrTy(),
            {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()}, false));
    builder->CreateCall(memcpyFn, {newData, emitListDataPtr(listType, listHandle), initBytes});
    builder->CreateBr(doneBlock);
  }

  builder->SetInsertPoint(doneBlock);
  emitStoreListDataPtr(listType, newHandle, builder->CreateLoad(builder->getPtrTy(), dataSlot));
  return newHandle;
}

namespace {
[[nodiscard]] auto stripSpacesCopy(std::string s) -> std::string {
  s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
  return s;
}
[[nodiscard]] auto classDisplayNamesMatch(const std::string& a, const std::string& b) -> bool {
  if (a == b) {
    return true;
  }
  if (a.size() < 6 || b.size() < 6 || a.compare(0, 5, "list<") != 0 || b.compare(0, 5, "list<") != 0) {
    return false;
  }
  return stripSpacesCopy(a) == stripSpacesCopy(b);
}
} // namespace

auto Codegen::tryEnsureStdlibListClassSpecialized(lesma::Type* classTy) -> void {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return;
  }
  const std::string& dn = classTy->getDisplayName();
  if (dn.size() < 6 || dn.compare(0, 5, "list<") != 0 || dn.back() != '>') {
    return;
  }
  auto git = genericClasses.find("list");
  if (git == genericClasses.end() || git->second == nullptr) {
    return;
  }
  lesma::Type* typeArg = nullptr;
  if (auto envIt = specializedClassTypeEnvs.find(classTy); envIt != specializedClassTypeEnvs.end()) {
    if (auto tIt = envIt->second.find("T"); tIt != envIt->second.end()) {
      typeArg = tIt->second;
    }
  }
  if (typeArg == nullptr) {
    for (const auto& [ty, env] : specializedClassTypeEnvs) {
      if (ty != nullptr && classDisplayNamesMatch(ty->getDisplayName(), dn)) {
        if (auto tIt = env.find("T"); tIt != env.end()) {
          typeArg = tIt->second;
          break;
        }
      }
    }
  }
  if (typeArg == nullptr) {
    auto fields = classTy->getFields();
    if (!fields.empty() && fields[0]->type != nullptr && fields[0]->type->is(BaseType::TY_ARRAY) &&
        fields[0]->type->getElementType() != nullptr) {
      typeArg = fields[0]->type->getElementType();
    }
  }
  if (typeArg == nullptr) {
    return;
  }
  specializeClass(git->second, {}, {typeArg});
}

auto Codegen::lookupClassStructSymbol(lesma::Type* classTy) -> Value* {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return nullptr;
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
    // Typechecker may use a different Type* than the one codegen registered when specializing
    // generics; match the struct symbol by display name.
    for (const auto& [ty, sym] : specializedClassSymbolsByType) {
      if (ty != nullptr && classDisplayNamesMatch(ty->getDisplayName(), classTy->getDisplayName())) {
        return sym;
      }
    }
  }
  tryEnsureStdlibListClassSpecialized(classTy);
  if (!classTy->getDisplayName().empty()) {
    if (Value* v = scope->lookupStruct(classTy->getDisplayName())) {
      return v;
    }
    for (const auto& [ty, sym] : specializedClassSymbolsByType) {
      if (ty != nullptr && classDisplayNamesMatch(ty->getDisplayName(), classTy->getDisplayName())) {
        return sym;
      }
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

void Codegen::emitListFieldDestroyOnFinalize(lesma::Type* listType, llvm::Value* listHandle) {
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) || listType->getElementType() == nullptr) {
    return;
  }
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* skipBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.fin.skip", parentFunction);
  auto* runBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.fin.run", parentFunction);
  auto* joinBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.fin.join", parentFunction);
  builder->CreateCondBr(
      builder->CreateICmpEQ(listHandle, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      skipBlock, runBlock);
  builder->SetInsertPoint(skipBlock);
  builder->CreateBr(joinBlock);
  builder->SetInsertPoint(runBlock);
  auto* currentData = emitListDataPtr(listType, listHandle);
  auto* freeDataBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.fin.freedata", parentFunction);
  auto* afterData =
      llvm::BasicBlock::Create(theModule->getContext(), "list.fin.afterdata", parentFunction);
  builder->CreateCondBr(
      builder->CreateICmpNE(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      freeDataBlock, afterData);
  builder->SetInsertPoint(freeDataBlock);
  emitListReleaseClassElementsIfNeeded(listType, listHandle);
  emitFree(currentData);
  builder->CreateBr(afterData);
  builder->SetInsertPoint(afterData);
  emitFree(listHandle);
  builder->CreateBr(joinBlock);
  builder->SetInsertPoint(joinBlock);
}

auto Codegen::getOrEmitClassFinalize(lesma::Type* classType) -> llvm::Function* {
  if (classType == nullptr || !classType->is(BaseType::TY_CLASS)) {
    throw CodegenError(llvm::SMRange{}, "Internal: finalize requires a class type");
  }
  if (auto it = classFinalizeFnCache.find(classType); it != classFinalizeFnCache.end()) {
    return it->second;
  }
  getOrCreateLlvmType(classType);
  std::string finName =
      std::string{"__lesma_finalize."} + MangleUtils::getTypeMangledName({}, classType);
  llvm::Function* fn = theModule->getFunction(finName);
  if (fn != nullptr && !fn->empty()) {
    classFinalizeFnCache[classType] = fn;
    return fn;
  }
  if (fn == nullptr) {
    llvm::FunctionType* ft =
        llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
    fn = llvm::Function::Create(ft, llvm::Function::PrivateLinkage, finName, *theModule);
  }
  classFinalizeFnCache[classType] = fn;

  llvm::IRBuilderBase::InsertPoint ip = builder->saveIP();
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(theModule->getContext(), "fin.entry", fn);
  builder->SetInsertPoint(entry);

  llvm::Argument* rawArg = fn->getArg(0);
  rawArg->setName("p");
  lesma::Type* selfPtrLesma =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
  getOrCreateLlvmType(selfPtrLesma);
  llvm::Value* self = builder->CreateBitCast(rawArg, selfPtrLesma->getLlvmType());

  std::vector<lesma::Type*> dropParams = {selfPtrLesma};
  Value* dropSym = rootScope->lookupFunction("drop", dropParams);
  if (dropSym != nullptr && dropSym->getLlvmValue() != nullptr) {
    auto* dropFn = llvm::cast<llvm::Function>(dropSym->getLlvmValue());
    builder->CreateCall(dropFn, {self});
  }

  auto* st = llvm::cast<llvm::StructType>(getOrCreateLlvmType(classType));
  const auto& fields = classType->getFields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    Field* f = fields[i];
    if (f == nullptr || f->type == nullptr) {
      continue;
    }
    lesma::Type* ft = f->type;
    unsigned idx = classUserFieldLlvmIndex(i);
    llvm::Value* fieldPtr = builder->CreateStructGEP(st, self, idx, "field.ptr");
    if (ft->is(BaseType::TY_PTR) && ft->getElementType() != nullptr &&
        ft->getElementType()->is(BaseType::TY_CLASS)) {
      llvm::Value* child = builder->CreateLoad(builder->getPtrTy(), fieldPtr, "field.class");
      emitArcRelease(child, ft->getElementType());
    } else if (ft->is(BaseType::TY_ARRAY) && ft->getElementType() != nullptr) {
      llvm::Value* buf = builder->CreateLoad(ft->getLlvmType(), fieldPtr, "field.buf");
      emitListFieldDestroyOnFinalize(ft, buf);
    }
  }

  emitFree(rawArg);
  builder->CreateRetVoid();
  builder->restoreIP(ip);
  return fn;
}
