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
#include "liblesma/Symbol/TypeUtils.h"
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

auto Codegen::getOrCreateArcHeaderType() -> llvm::StructType* {
  if (arcHeaderLlvmType == nullptr) {
    arcHeaderLlvmType = llvm::StructType::create(theModule->getContext(),
                                                 {builder->getInt64Ty(), builder->getPtrTy()},
                                                 "lesma.arc.header", false);
  }
  return arcHeaderLlvmType;
}

auto Codegen::emitArcAlloc(llvm::Value* payloadSize, llvm::Function* destroyFn,
                           const llvm::Twine& name) -> llvm::Value* {
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize = builder->getInt64(
      theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* totalSize = builder->CreateAdd(payloadSize, headerSize, name + ".arc.bytes");
  auto* raw = emitMalloc(totalSize, name + ".arc.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           name + ".arc.header");
  builder->CreateStore(builder->getInt64(1), builder->CreateStructGEP(headerTy, headerPtr, 0U));
  llvm::Value* destroyValue =
      destroyFn != nullptr ? builder->CreateBitCast(destroyFn, builder->getPtrTy())
                           : llvm::ConstantPointerNull::get(builder->getPtrTy());
  builder->CreateStore(destroyValue, builder->CreateStructGEP(headerTy, headerPtr, 1U));
  auto* payload =
      builder->CreateInBoundsGEP(builder->getInt8Ty(), raw, headerSize, name + ".arc.payload");
  builder->CreateMemSet(payload, builder->getInt8(0), payloadSize, llvm::MaybeAlign(1U));
  return builder->CreateBitCast(payload, builder->getPtrTy(), name);
}

auto Codegen::emitArcFreePayload(llvm::Value* payloadPtr) -> void {
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize = builder->getInt64(
      theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* payloadRaw = builder->CreateBitCast(payloadPtr, builder->getPtrTy(), "arc.payload.raw");
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(), payloadRaw,
                                         builder->CreateNeg(headerSize), "arc.raw");
  emitFree(raw);
}

auto Codegen::emitArcRetain(llvm::Value* payloadPtr) -> void {
  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  auto* retainBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.retain", parentFn);
  auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.retain.done", parentFn);
  builder->CreateCondBr(
      builder->CreateICmpEQ(payloadPtr, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      doneBlock, retainBlock);

  builder->SetInsertPoint(retainBlock);
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize = builder->getInt64(
      theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(),
                                         builder->CreateBitCast(payloadPtr, builder->getPtrTy()),
                                         builder->CreateNeg(headerSize), "arc.retain.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           "arc.retain.header");
  auto* refSlot = builder->CreateStructGEP(headerTy, headerPtr, 0U);
  auto* refCount = builder->CreateLoad(builder->getInt64Ty(), refSlot, "arc.retain.count");
  builder->CreateStore(builder->CreateAdd(refCount, builder->getInt64(1)), refSlot);
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
}

auto Codegen::emitArcRelease(llvm::Value* payloadPtr) -> void {
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize = builder->getInt64(
      theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(),
                                         builder->CreateBitCast(payloadPtr, builder->getPtrTy()),
                                         builder->CreateNeg(headerSize), "arc.release.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           "arc.release.header");
  auto* refSlot = builder->CreateStructGEP(headerTy, headerPtr, 0U);
  auto* destroySlot = builder->CreateStructGEP(headerTy, headerPtr, 1U);
  auto* refCount = builder->CreateLoad(builder->getInt64Ty(), refSlot, "arc.release.count");
  auto* next = builder->CreateSub(refCount, builder->getInt64(1), "arc.release.next");
  builder->CreateStore(next, refSlot);

  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  auto* destroyBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.release.destroy",
                                                parentFn);
  auto* doneBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "arc.release.done", parentFn);
  builder->CreateCondBr(builder->CreateICmpEQ(next, builder->getInt64(0)), destroyBlock, doneBlock);

  builder->SetInsertPoint(destroyBlock);
  auto* destroyPtr = builder->CreateLoad(builder->getPtrTy(), destroySlot, "arc.release.destroyfn");
  auto* destroyTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  builder->CreateCall(destroyTy, destroyPtr, {payloadPtr});
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
}

auto Codegen::emitArcReleaseNullable(llvm::Value* payloadPtr) -> void {
  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  auto* releaseBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.release", parentFn);
  auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.release.done", parentFn);
  builder->CreateCondBr(
      builder->CreateICmpEQ(payloadPtr, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      doneBlock, releaseBlock);
  builder->SetInsertPoint(releaseBlock);
  emitArcRelease(payloadPtr);
  builder->CreateBr(doneBlock);
  builder->SetInsertPoint(doneBlock);
}

auto Codegen::emitRetainLoadedValue(lesma::Type* type, llvm::Value* value, bool storesFuncValuePair)
    -> void {
  if (type == nullptr || value == nullptr) {
    return;
  }
  if (TypeUtils::isArcReferenceType(type)) {
    emitArcRetain(value);
    return;
  }
  switch (type->getBaseType()) {
  case BaseType::TY_PTR:
    if (type->getElementType() != nullptr && type->getElementType()->is(BaseType::TY_CLASS)) {
      emitArcRetain(value);
    }
    return;
  case BaseType::TY_FUNCTION:
    if (!storesFuncValuePair || !value->getType()->isStructTy()) {
      return;
    }
    emitArcRetain(builder->CreateExtractValue(value, {1U}, "arc.fn.env"));
    return;
  case BaseType::TY_TRAIT_EXISTENTIAL:
    emitArcRetain(builder->CreateExtractValue(value, {0U}, "arc.existential.payload"));
    return;
  case BaseType::TY_TUPLE: {
    auto fields = type->getFields();
    for (unsigned i = 0; i < fields.size(); ++i) {
      if (!TypeUtils::containsArcManagedValue(fields[i]->type)) {
        continue;
      }
      emitRetainLoadedValue(fields[i]->type, builder->CreateExtractValue(value, {i}, "arc.tuple.v"),
                           false);
    }
    return;
  }
  case BaseType::TY_UNION: {
    bool hasArcMember = false;
    for (Type* member : type->getUnionMembers()) {
      hasArcMember = hasArcMember || TypeUtils::containsArcManagedValue(member);
    }
    if (!hasArcMember) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.union.retain.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.union.tag");
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "arc.union.retain.done", parentFn);
    llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
    for (unsigned idx = 0; idx < type->getUnionMembers().size(); ++idx) {
      Type* member = type->getUnionMembers()[idx];
      if (!TypeUtils::containsArcManagedValue(member)) {
        continue;
      }
      auto* matchBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.union.retain.match", parentFn);
      auto* nextBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.union.retain.next", parentFn);
      builder->SetInsertPoint(currentBlock);
      builder->CreateCondBr(
          builder->CreateICmpEQ(tagVal, llvm::ConstantInt::get(tagVal->getType(), idx)), matchBlock,
          nextBlock);
      builder->SetInsertPoint(matchBlock);
      emitRetainLoadedValue(member, emitUnionPayloadLoadFromSlot(slot, type, member), false);
      builder->CreateBr(mergeBlock);
      currentBlock = nextBlock;
    }
    builder->SetInsertPoint(currentBlock);
    builder->CreateBr(mergeBlock);
    builder->SetInsertPoint(mergeBlock);
    return;
  }
  case BaseType::TY_ANY: {
    llvm::Value* payloadPtr = builder->CreateExtractValue(value, {1U}, "arc.any.payload");
    emitArcRetain(payloadPtr);
    return;
  }
  default:
    return;
  }
}

auto Codegen::emitReleaseLoadedValue(lesma::Type* type, llvm::Value* value, bool storesFuncValuePair)
    -> void {
  if (type == nullptr || value == nullptr) {
    return;
  }
  if (TypeUtils::isArcReferenceType(type)) {
    emitArcReleaseNullable(value);
    return;
  }
  switch (type->getBaseType()) {
  case BaseType::TY_PTR:
    if (type->getElementType() != nullptr && type->getElementType()->is(BaseType::TY_CLASS)) {
      emitArcReleaseNullable(value);
    }
    return;
  case BaseType::TY_FUNCTION:
    if (!storesFuncValuePair || !value->getType()->isStructTy()) {
      return;
    }
    emitArcReleaseNullable(builder->CreateExtractValue(value, {1U}, "arc.fn.env"));
    return;
  case BaseType::TY_TRAIT_EXISTENTIAL:
    emitArcReleaseNullable(builder->CreateExtractValue(value, {0U}, "arc.existential.payload"));
    return;
  case BaseType::TY_TUPLE: {
    auto fields = type->getFields();
    for (unsigned i = 0; i < fields.size(); ++i) {
      if (!TypeUtils::containsArcManagedValue(fields[i]->type)) {
        continue;
      }
      emitReleaseLoadedValue(fields[i]->type, builder->CreateExtractValue(value, {i}, "arc.tuple.v"),
                             false);
    }
    return;
  }
  case BaseType::TY_UNION: {
    bool hasArcMember = false;
    for (Type* member : type->getUnionMembers()) {
      hasArcMember = hasArcMember || TypeUtils::containsArcManagedValue(member);
    }
    if (!hasArcMember) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.union.release.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.union.tag");
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "arc.union.release.done", parentFn);
    llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
    for (unsigned idx = 0; idx < type->getUnionMembers().size(); ++idx) {
      Type* member = type->getUnionMembers()[idx];
      if (!TypeUtils::containsArcManagedValue(member)) {
        continue;
      }
      auto* matchBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.union.release.match", parentFn);
      auto* nextBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.union.release.next", parentFn);
      builder->SetInsertPoint(currentBlock);
      builder->CreateCondBr(
          builder->CreateICmpEQ(tagVal, llvm::ConstantInt::get(tagVal->getType(), idx)), matchBlock,
          nextBlock);
      builder->SetInsertPoint(matchBlock);
      emitReleaseLoadedValue(member, emitUnionPayloadLoadFromSlot(slot, type, member), false);
      builder->CreateBr(mergeBlock);
      currentBlock = nextBlock;
    }
    builder->SetInsertPoint(currentBlock);
    builder->CreateBr(mergeBlock);
    builder->SetInsertPoint(mergeBlock);
    return;
  }
  case BaseType::TY_ANY: {
    llvm::Value* payloadPtr = builder->CreateExtractValue(value, {1U}, "arc.any.payload");
    emitArcReleaseNullable(payloadPtr);
    return;
  }
  default:
    return;
  }
}

auto Codegen::emitReleaseTrackedSlot(const ArcTrackedSlot& tracked) -> void {
  if (tracked.slot == nullptr || tracked.type == nullptr ||
      !TypeUtils::containsArcManagedValue(tracked.type)) {
    return;
  }
  llvm::Type* slotTy = tracked.type->is(BaseType::TY_FUNCTION) && tracked.storesFuncValuePair
                           ? static_cast<llvm::Type*>(getFuncValuePairLlvmType())
                           : getStoredAggregateFieldLlvmType(tracked.type);
  if (slotTy == nullptr) {
    return;
  }
  emitReleaseLoadedValue(tracked.type, builder->CreateLoad(slotTy, tracked.slot, "arc.slot.release"),
                         tracked.storesFuncValuePair);
}

auto Codegen::pushArcOwnedSlotFrame() -> void { arcOwnedSlotFrames.emplace_back(); }

auto Codegen::popArcOwnedSlotFrame(bool emitCleanup) -> void {
  if (arcOwnedSlotFrames.empty()) {
    return;
  }
  if (emitCleanup) {
    for (auto it = arcOwnedSlotFrames.back().rbegin(); it != arcOwnedSlotFrames.back().rend(); ++it) {
      emitReleaseTrackedSlot(*it);
    }
  }
  arcOwnedSlotFrames.pop_back();
}

auto Codegen::registerArcOwnedSlot(llvm::Value* slot, lesma::Type* type, bool storesFuncValuePair)
    -> void {
  if (slot == nullptr || type == nullptr || arcOwnedSlotFrames.empty() ||
      !TypeUtils::containsArcManagedValue(type)) {
    return;
  }
  auto& frame = arcOwnedSlotFrames.back();
  for (const ArcTrackedSlot& tracked : frame) {
    if (tracked.slot == slot) {
      return;
    }
  }
  frame.push_back(ArcTrackedSlot{slot, type, storesFuncValuePair});
}

auto Codegen::emitReleaseCurrentArcOwnedSlots() -> void {
  if (arcOwnedSlotFrames.empty()) {
    return;
  }
  for (auto it = arcOwnedSlotFrames.back().rbegin(); it != arcOwnedSlotFrames.back().rend(); ++it) {
    emitReleaseTrackedSlot(*it);
  }
}

auto Codegen::getOrCreateArcStorageRetainFunction(lesma::Type* type) -> llvm::Function* {
  if (type == nullptr) {
    return nullptr;
  }
  if (auto it = arcStorageRetainFns.find(type); it != arcStorageRetainFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_retain_storage_" + MangleUtils::getTypeMangledName({}, type);
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcStorageRetainFns[type] = fn;
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  llvm::Value* storage = fn->getArg(0U);
  llvm::Type* storageTy = type->is(BaseType::TY_FUNCTION)
                              ? static_cast<llvm::Type*>(getFuncValuePairLlvmType())
                              : getStoredAggregateFieldLlvmType(type);
  llvm::Value* typedStorage =
      builder->CreateBitCast(storage, llvm::PointerType::get(storageTy->getContext(), 0U));
  emitRetainLoadedValue(type, builder->CreateLoad(storageTy, typedStorage, "arc.storage.load"),
                        type->is(BaseType::TY_FUNCTION));
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcStorageReleaseFunction(lesma::Type* type) -> llvm::Function* {
  if (type == nullptr) {
    return nullptr;
  }
  if (auto it = arcStorageReleaseFns.find(type); it != arcStorageReleaseFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_release_storage_" + MangleUtils::getTypeMangledName({}, type);
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcStorageReleaseFns[type] = fn;
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  llvm::Value* storage = fn->getArg(0U);
  llvm::Type* storageTy = type->is(BaseType::TY_FUNCTION)
                              ? static_cast<llvm::Type*>(getFuncValuePairLlvmType())
                              : getStoredAggregateFieldLlvmType(type);
  llvm::Value* typedStorage =
      builder->CreateBitCast(storage, llvm::PointerType::get(storageTy->getContext(), 0U));
  emitReleaseLoadedValue(type, builder->CreateLoad(storageTy, typedStorage, "arc.storage.load"),
                         type->is(BaseType::TY_FUNCTION));
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcPayloadDestroyFunction(lesma::Type* type) -> llvm::Function* {
  if (type == nullptr) {
    return nullptr;
  }
  if (auto it = arcPayloadDestroyFns.find(type); it != arcPayloadDestroyFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_destroy_payload_" + MangleUtils::getTypeMangledName({}, type);
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcPayloadDestroyFns[type] = fn;
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto* releaseFn = getOrCreateArcStorageReleaseFunction(type);
  auto* releaseTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  builder->CreateCall(releaseTy, releaseFn, {fn->getArg(0U)});
  emitArcFreePayload(fn->getArg(0U));
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcDestroyFunction(lesma::Type* type) -> llvm::Function* {
  if (type == nullptr) {
    return nullptr;
  }
  if (auto it = arcDestroyFns.find(type); it != arcDestroyFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_destroy_" + MangleUtils::getTypeMangledName({}, type);
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcDestroyFns[type] = fn;

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  llvm::Value* payload = fn->getArg(0U);

  if (type->is(BaseType::TY_ARRAY)) {
    auto* listHandle = builder->CreateBitCast(payload, llvm::PointerType::get(builder->getContext(), 0U),
                                              "arc.list.handle");
    if (type->getElementType() != nullptr &&
        TypeUtils::containsArcManagedValue(type->getElementType())) {
      llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
      auto* loopCond =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.list.release.cond", parentFn);
      auto* loopBody =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.list.release.body", parentFn);
      auto* loopInc =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.list.release.inc", parentFn);
      auto* loopDone =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.list.release.done", parentFn);
      auto* indexPtr = createAllocaInEntry(parentFn, builder->getInt64Ty(), "arc.list.release.idx");
      builder->CreateStore(builder->getInt64(0), indexPtr);
      builder->CreateBr(loopCond);

      builder->SetInsertPoint(loopCond);
      auto* index = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
      auto* length = emitListLength(type, listHandle);
      builder->CreateCondBr(builder->CreateICmpSLT(index, length), loopBody, loopDone);

      builder->SetInsertPoint(loopBody);
      auto* elemPtr = emitListElementPointer({}, type, listHandle, index);
      auto* elemVal = builder->CreateLoad(getListStoredElementType(type), elemPtr, "arc.list.elem");
      emitReleaseLoadedValue(type->getElementType(), elemVal, false);
      builder->CreateBr(loopInc);

      builder->SetInsertPoint(loopInc);
      builder->CreateStore(builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                              builder->getInt64(1)),
                           indexPtr);
      builder->CreateBr(loopCond);
      builder->SetInsertPoint(loopDone);
    }

    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* freeBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "arc.list.data.free", parentFn);
    auto* doneBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "arc.list.data.done", parentFn);
    auto* data = emitListDataPtr(type, listHandle);
    builder->CreateCondBr(
        builder->CreateICmpEQ(data, llvm::ConstantPointerNull::get(builder->getPtrTy())), doneBlock,
        freeBlock);
    builder->SetInsertPoint(freeBlock);
    emitFree(data);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    emitArcFreePayload(listHandle);
    builder->CreateRetVoid();
    builder->restoreIP(savedIp);
    return fn;
  }

  auto* classTy = llvm::cast<llvm::StructType>(getOrCreateLlvmType(type));
  auto* object = builder->CreateBitCast(payload, llvm::PointerType::get(classTy->getContext(), 0U),
                                        "arc.class.obj");
  auto fields = type->getFields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    Type* fieldTy = fields[i]->type;
    unsigned const structIdx = TypeUtils::classDataFieldStructIndex(type, i);
    auto* slot = builder->CreateStructGEP(classTy, object, structIdx, "arc.field.ptr");
    if (TypeUtils::containsArcManagedValue(fieldTy)) {
      llvm::Type* storageTy = getStoredAggregateFieldLlvmType(fieldTy);
      emitReleaseLoadedValue(fieldTy, builder->CreateLoad(storageTy, slot, "arc.field.load"),
                             false);
      continue;
    }
    if (type->getDisplayName() == "str" && fieldTy != nullptr && fieldTy->is(BaseType::TY_STRING)) {
      llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
      auto* freeBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.str.free", parentFn);
      auto* doneBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.str.free.done", parentFn);
      auto* raw = builder->CreateLoad(builder->getPtrTy(), slot, "arc.str.raw");
      builder->CreateCondBr(
          builder->CreateICmpEQ(raw, llvm::ConstantPointerNull::get(builder->getPtrTy())), doneBlock,
          freeBlock);
      builder->SetInsertPoint(freeBlock);
      emitFree(raw);
      builder->CreateBr(doneBlock);
      builder->SetInsertPoint(doneBlock);
    }
  }
  emitArcFreePayload(object);
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcClosureDestroyFunction(const std::string& key,
                                                   llvm::StructType* envStructTy,
                                                   const std::vector<lesma::Type*>& captureTypes)
    -> llvm::Function* {
  if (auto it = arcClosureDestroyFns.find(key); it != arcClosureDestroyFns.end()) {
    return it->second;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage,
                                    "__lesma_arc_destroy_env_" + key, *theModule);
  arcClosureDestroyFns[key] = fn;
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto* env = builder->CreateBitCast(fn->getArg(0U), llvm::PointerType::get(envStructTy->getContext(), 0U),
                                     "arc.env");
  for (unsigned i = 0; i < captureTypes.size(); ++i) {
    if (!TypeUtils::containsArcManagedValue(captureTypes[i])) {
      continue;
    }
    auto* slot = builder->CreateStructGEP(envStructTy, env, i, "arc.env.slot");
    llvm::Type* storageTy = getStoredAggregateFieldLlvmType(captureTypes[i]);
    emitReleaseLoadedValue(captureTypes[i], builder->CreateLoad(storageTy, slot, "arc.env.load"),
                           false);
  }
  emitArcFreePayload(env);
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::emitRuntimeStderrMessage(std::string_view message) -> void {
  auto writeFn = theModule->getOrInsertFunction(
      "write", llvm::FunctionType::get(
                   builder->getInt64Ty(),
                   {builder->getInt32Ty(), builder->getPtrTy(), builder->getInt64Ty()}, false));
  llvm::GlobalVariable* messageGlobal = builder->CreateGlobalString(message, "runtime.errmsg");
  builder->CreateCall(writeFn, {builder->getInt32(2),
                                builder->CreateBitCast(messageGlobal, builder->getPtrTy()),
                                builder->getInt64(message.size())});
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
  auto* newHandle = emitArcAlloc(headerSize, getOrCreateArcDestroyFunction(listType),
                                 "list.copy.header");
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
  if (elementType->is(BaseType::TY_ARRAY) || isNestedListLike() ||
      TypeUtils::containsArcManagedValue(elementType)) {
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
    } else if (isNestedListLike()) {
      lesma::Value oldValue("", elementType, oldElement);
      auto copiedValue = callMethodByName({}, &oldValue, "copy");
      copiedElement = copiedValue->getLlvmValue();
    } else {
      copiedElement = oldElement;
      emitRetainLoadedValue(elementType, copiedElement, false);
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
  if (auto it = specializedClassSymbolsByType.find(classTy);
      it != specializedClassSymbolsByType.end()) {
    return it->second;
  }
  if (auto envIt = specializedClassTypeEnvs.find(classTy);
      envIt != specializedClassTypeEnvs.end()) {
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
      if (auto it = specializedClassTemplateOf.find(classTy);
          it != specializedClassTemplateOf.end()) {
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
    if (auto it = specializedClassTypeEnvs.find(sym->getType());
        it != specializedClassTypeEnvs.end()) {
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
  if (auto it = specializedClassTypeEnvs.find(existentialTy);
      it != specializedClassTypeEnvs.end()) {
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
