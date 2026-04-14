#include <algorithm>
#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

#include "Codegen.h"

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

using namespace lesma;
using namespace llvm;

auto Codegen::isLesmaTypeReadyForArcTypeMangling(lesma::Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (type->is(BaseType::TY_GENERIC) || type->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return true;
  }
  if (type->getLlvmType() == nullptr) {
    return false;
  }
  if (type->is(BaseType::TY_ARRAY)) {
    return isLesmaTypeReadyForArcTypeMangling(type->getElementType());
  }
  if (type->is(BaseType::TY_PTR)) {
    return isLesmaTypeReadyForArcTypeMangling(type->getElementType());
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    return std::ranges::all_of(type->getFields(), [](Field* field) -> bool {
      return field != nullptr && isLesmaTypeReadyForArcTypeMangling(field->type);
    });
  }
  if (type->is(BaseType::TY_ENUM)) {
    return llvm::isa<llvm::StructType>(type->getLlvmType()) &&
           std::ranges::all_of(type->getEnumVariants(), [](EnumVariant* variant) -> bool {
             return variant != nullptr &&
                    std::ranges::all_of(variant->payloadTypes, [](lesma::Type* payloadType) -> bool {
                      return payloadType != nullptr &&
                             isLesmaTypeReadyForArcTypeMangling(payloadType);
                    });
           });
  }
  if (type->is(BaseType::TY_CLASS)) {
    return llvm::isa<llvm::StructType>(type->getLlvmType());
  }
  if (type->is(BaseType::TY_TUPLE)) {
    return std::ranges::all_of(type->getFields(), [](Field* field) -> bool {
      return field != nullptr && isLesmaTypeReadyForArcTypeMangling(field->type);
    });
  }
  if (type->is(BaseType::TY_UNION)) {
    return std::ranges::all_of(type->getUnionMembers(), [](lesma::Type* m) -> bool {
      return m != nullptr && isLesmaTypeReadyForArcTypeMangling(m);
    });
  }
  if (type->is(BaseType::TY_IMPORT) || type->is(BaseType::TY_INVALID)) {
    return false;
  }
  return true;
}

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
  if (destroyFn == nullptr) {
    throw CodegenError({}, "ARC allocation '{}' is missing a destroy function", name.str());
  }
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* totalSize = builder->CreateAdd(payloadSize, headerSize, name + ".arc.bytes");
  auto* raw = emitMalloc(totalSize, name + ".arc.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           name + ".arc.header");
  builder->CreateStore(builder->getInt64(1), builder->CreateStructGEP(headerTy, headerPtr, 0U));
  llvm::Value* destroyValue = builder->CreateBitCast(destroyFn, builder->getPtrTy());
  builder->CreateStore(destroyValue, builder->CreateStructGEP(headerTy, headerPtr, 1U));
  auto* payload =
      builder->CreateInBoundsGEP(builder->getInt8Ty(), raw, headerSize, name + ".arc.payload");
  builder->CreateMemSet(payload, builder->getInt8(0), payloadSize, llvm::MaybeAlign(1U));
  auto* payloadPtr = builder->CreateBitCast(payload, builder->getPtrTy(), name);
  if (emitArcDebug) {
    emitArcDebugDelta(1, payloadPtr, "[arc] alloc");
  }
  return payloadPtr;
}

auto Codegen::emitArcFreePayload(llvm::Value* payloadPtr) -> void {
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* payloadRaw = builder->CreateBitCast(payloadPtr, builder->getPtrTy(), "arc.payload.raw");
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(), payloadRaw,
                                         builder->CreateNeg(headerSize), "arc.raw");
  if (emitArcDebug) {
    emitArcDebugDelta(-1, payloadPtr, "[arc] free");
  }
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
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(),
                                         builder->CreateBitCast(payloadPtr, builder->getPtrTy()),
                                         builder->CreateNeg(headerSize), "arc.retain.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           "arc.retain.header");
  auto* refSlot = builder->CreateStructGEP(headerTy, headerPtr, 0U);
  auto* refCount = builder->CreateLoad(builder->getInt64Ty(), refSlot, "arc.retain.count");
  if (emitArcDebug) {
    emitArcDebugValidateRefcount(refCount, "[arc] retain on zero-count object\n");
  }
  auto* next = builder->CreateAdd(refCount, builder->getInt64(1), "arc.retain.next");
  builder->CreateStore(next, refSlot);
  emitArcDebugTraceCounts("[arc] retain", payloadPtr, refCount, next);
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
}

// Precondition: `payloadPtr` is non-null. Callers that may have a null payload must route through
// `emitArcReleaseNullable()`, which performs the null check before delegating here.
auto Codegen::emitArcRelease(llvm::Value* payloadPtr) -> void {
  auto* headerTy = getOrCreateArcHeaderType();
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(headerTy).getFixedValue());
  auto* raw = builder->CreateInBoundsGEP(builder->getInt8Ty(),
                                         builder->CreateBitCast(payloadPtr, builder->getPtrTy()),
                                         builder->CreateNeg(headerSize), "arc.release.raw");
  auto* headerPtr = builder->CreateBitCast(raw, llvm::PointerType::get(headerTy->getContext(), 0U),
                                           "arc.release.header");
  auto* refSlot = builder->CreateStructGEP(headerTy, headerPtr, 0U);
  auto* destroySlot = builder->CreateStructGEP(headerTy, headerPtr, 1U);
  auto* refCount = builder->CreateLoad(builder->getInt64Ty(), refSlot, "arc.release.count");
  if (emitArcDebug) {
    emitArcDebugValidateRefcount(refCount, "[arc] release on zero-count object\n");
  }
  auto* next = builder->CreateSub(refCount, builder->getInt64(1), "arc.release.next");
  builder->CreateStore(next, refSlot);
  emitArcDebugTraceCounts("[arc] release", payloadPtr, refCount, next);

  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  auto* destroyBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "arc.release.destroy", parentFn);
  auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.release.done", parentFn);
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
  case BaseType::TY_ENUM: {
    bool const hasArcVariantPayload =
        std::ranges::any_of(type->getEnumVariants(), [](EnumVariant const* variant) {
          if (variant == nullptr) {
            return false;
          }
          return std::ranges::any_of(variant->payloadTypes, TypeUtils::containsArcManagedValue);
        });
    if (!hasArcVariantPayload) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.enum.retain.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.enum.tag");
    emitForEachEnumVariantPayloadWithTagDispatch(
        type, slot, tagVal, "arc.enum.retain", [this](Type* payloadType, llvm::Value* payload) {
          emitRetainLoadedValue(payloadType, payload, false);
        });
    return;
  }
  case BaseType::TY_UNION: {
    bool const hasArcMember =
        std::ranges::any_of(type->getUnionMembers(), TypeUtils::containsArcManagedValue);
    if (!hasArcMember) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.union.retain.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.union.tag");
    emitForEachUnionMemberWithTagDispatch(type, slot, tagVal, "arc.union.retain",
                                          [this](Type* member, llvm::Value* payload) {
                                            emitRetainLoadedValue(member, payload, false);
                                          });
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

auto Codegen::emitReleaseLoadedValue(lesma::Type* type, llvm::Value* value,
                                     bool storesFuncValuePair) -> void {
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
      emitReleaseLoadedValue(fields[i]->type,
                             builder->CreateExtractValue(value, {i}, "arc.tuple.v"), false);
    }
    return;
  }
  case BaseType::TY_ENUM: {
    bool const hasArcVariantPayload =
        std::ranges::any_of(type->getEnumVariants(), [](EnumVariant const* variant) {
          if (variant == nullptr) {
            return false;
          }
          return std::ranges::any_of(variant->payloadTypes, TypeUtils::containsArcManagedValue);
        });
    if (!hasArcVariantPayload) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.enum.release.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.enum.tag");
    emitForEachEnumVariantPayloadWithTagDispatch(
        type, slot, tagVal, "arc.enum.release", [this](Type* payloadType, llvm::Value* payload) {
          emitReleaseLoadedValue(payloadType, payload, false);
        });
    return;
  }
  case BaseType::TY_UNION: {
    bool const hasArcMember =
        std::ranges::any_of(type->getUnionMembers(), TypeUtils::containsArcManagedValue);
    if (!hasArcMember) {
      return;
    }
    llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
    auto* slot = createAllocaInEntry(parentFn, type->getLlvmType(), "arc.union.release.slot");
    builder->CreateStore(value, slot);
    llvm::Value* tagVal = builder->CreateExtractValue(value, {0U}, "arc.union.tag");
    emitForEachUnionMemberWithTagDispatch(type, slot, tagVal, "arc.union.release",
                                          [this](Type* member, llvm::Value* payload) {
                                            emitReleaseLoadedValue(member, payload, false);
                                          });
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

auto Codegen::emitForEachEnumVariantPayloadWithTagDispatch(
    lesma::Type* enumTy, llvm::Value* enumSlot, llvm::Value* tagVal, std::string_view blockStem,
    const std::function<void(lesma::Type*, llvm::Value*)>& callback) -> void {
  if (enumTy == nullptr || enumSlot == nullptr || tagVal == nullptr) {
    return;
  }
  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock* mergeBlock =
      llvm::BasicBlock::Create(theModule->getContext(), std::string(blockStem) + ".done", parentFn);
  llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
  auto variants = enumTy->getEnumVariants();
  for (unsigned idx = 0; idx < variants.size(); ++idx) {
    EnumVariant* variant = variants[idx];
    if (variant == nullptr || variant->payloadTypes.empty()) {
      continue;
    }
    Type* payloadType = getEnumVariantAggregatePayloadType(enumTy, idx);
    if (!TypeUtils::containsArcManagedValue(payloadType)) {
      continue;
    }
    auto* matchBlock = llvm::BasicBlock::Create(theModule->getContext(),
                                                std::string(blockStem) + ".match", parentFn);
    auto* nextBlock = llvm::BasicBlock::Create(theModule->getContext(),
                                               std::string(blockStem) + ".next", parentFn);
    builder->SetInsertPoint(currentBlock);
    builder->CreateCondBr(
        builder->CreateICmpEQ(tagVal, llvm::ConstantInt::get(tagVal->getType(), idx)), matchBlock,
        nextBlock);
    builder->SetInsertPoint(matchBlock);
    callback(payloadType, emitEnumPayloadLoadFromSlot(enumSlot, enumTy, idx));
    builder->CreateBr(mergeBlock);
    currentBlock = nextBlock;
  }
  builder->SetInsertPoint(currentBlock);
  builder->CreateBr(mergeBlock);
  builder->SetInsertPoint(mergeBlock);
}

auto Codegen::emitForEachUnionMemberWithTagDispatch(
    lesma::Type* unionTy, llvm::Value* unionSlot, llvm::Value* tagVal, std::string_view blockStem,
    const std::function<void(lesma::Type*, llvm::Value*)>& callback) -> void {
  if (unionTy == nullptr || unionSlot == nullptr || tagVal == nullptr) {
    return;
  }
  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock* mergeBlock =
      llvm::BasicBlock::Create(theModule->getContext(), std::string(blockStem) + ".done", parentFn);
  llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
  for (unsigned idx = 0; idx < unionTy->getUnionMembers().size(); ++idx) {
    Type* member = unionTy->getUnionMembers()[idx];
    if (!TypeUtils::containsArcManagedValue(member)) {
      continue;
    }
    auto* matchBlock = llvm::BasicBlock::Create(theModule->getContext(),
                                                std::string(blockStem) + ".match", parentFn);
    auto* nextBlock = llvm::BasicBlock::Create(theModule->getContext(),
                                               std::string(blockStem) + ".next", parentFn);
    builder->SetInsertPoint(currentBlock);
    builder->CreateCondBr(
        builder->CreateICmpEQ(tagVal, llvm::ConstantInt::get(tagVal->getType(), idx)), matchBlock,
        nextBlock);
    builder->SetInsertPoint(matchBlock);
    callback(member, emitUnionPayloadLoadFromSlot(unionSlot, unionTy, member));
    builder->CreateBr(mergeBlock);
    currentBlock = nextBlock;
  }
  builder->SetInsertPoint(currentBlock);
  builder->CreateBr(mergeBlock);
  builder->SetInsertPoint(mergeBlock);
}

auto Codegen::emitReleaseTrackedSlot(const ArcTrackedSlot& tracked) -> void {
  if (tracked.slot == nullptr || tracked.type == nullptr ||
      !TypeUtils::containsArcManagedValue(tracked.type)) {
    return;
  }
  if (llvm::Function* releaseFn = getOrCreateArcStorageReleaseFunction(tracked.type);
      releaseFn != nullptr) {
    auto* releaseTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
    builder->CreateCall(releaseTy, releaseFn, {tracked.slot});
    return;
  }
  llvm::Type* slotTy = tracked.type->is(BaseType::TY_FUNCTION) && tracked.storesFuncValuePair
                           ? static_cast<llvm::Type*>(getFuncValuePairLlvmType())
                           : getStoredAggregateFieldLlvmType(tracked.type);
  if (slotTy == nullptr) {
    return;
  }
  emitReleaseLoadedValue(tracked.type,
                         builder->CreateLoad(slotTy, tracked.slot, "arc.slot.release"),
                         tracked.storesFuncValuePair);
}

auto Codegen::pushArcOwnedSlotFrame() -> void { arcOwnedSlotFrames.emplace_back(); }

auto Codegen::popArcOwnedSlotFrame(bool emitCleanup) -> void {
  if (arcOwnedSlotFrames.empty()) {
    return;
  }
  if (emitCleanup) {
    for (auto it = arcOwnedSlotFrames.back().rbegin(); it != arcOwnedSlotFrames.back().rend();
         ++it) {
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

auto Codegen::registerModuleArcRoot(llvm::GlobalVariable* slot, lesma::Type* type,
                                    const std::string& debugName, bool storesFuncValuePair)
    -> void {
  if (slot == nullptr || type == nullptr || !TypeUtils::containsArcManagedValue(type)) {
    return;
  }
  for (const ModuleArcTrackedRoot& tracked : moduleArcTrackedRoots) {
    if (tracked.slot == slot) {
      return;
    }
  }
  moduleArcTrackedRoots.push_back(ModuleArcTrackedRoot{slot, type, storesFuncValuePair, debugName});
}

auto Codegen::emitReleaseRegisteredModuleArcRoots() -> void {
  for (auto it = moduleArcTrackedRoots.rbegin(); it != moduleArcTrackedRoots.rend(); ++it) {
    if (emitArcTrace) {
      emitArcDebugTraceModuleRoot(*it);
    }
    if (llvm::Function* releaseFn = getOrCreateArcStorageReleaseFunction(it->type);
        releaseFn != nullptr) {
      auto* releaseTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
      builder->CreateCall(releaseTy, releaseFn, {it->slot});
    } else {
      llvm::Type* storageTy = it->slot->getValueType();
      emitReleaseLoadedValue(it->type, builder->CreateLoad(storageTy, it->slot, "arc.root.load"),
                             it->storesFuncValuePair);
    }
    builder->CreateStore(llvm::Constant::getNullValue(it->slot->getValueType()), it->slot);
    if (emitArcDebug) {
      builder->CreateCall(getOrCreateArcDebugCleanupStepFunction());
    }
  }
}

auto Codegen::getOrCreateArcStorageRetainFunction(lesma::Type* type) -> llvm::Function* {
  if (type == nullptr) {
    return nullptr;
  }
  if (!isLesmaTypeReadyForArcTypeMangling(type)) {
    return nullptr;
  }
  const std::string typeKey = MangleUtils::getTypeMangledName({}, type);
  if (auto it = arcStorageRetainFns.find(typeKey); it != arcStorageRetainFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_retain_storage_" + typeKey;
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcStorageRetainFns[typeKey] = fn;
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
  if (!isLesmaTypeReadyForArcTypeMangling(type)) {
    return nullptr;
  }
  const std::string typeKey = MangleUtils::getTypeMangledName({}, type);
  if (auto it = arcStorageReleaseFns.find(typeKey); it != arcStorageReleaseFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_release_storage_" + typeKey;
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcStorageReleaseFns[typeKey] = fn;
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
  if (!isLesmaTypeReadyForArcTypeMangling(type)) {
    return nullptr;
  }
  const std::string typeKey = MangleUtils::getTypeMangledName({}, type);
  if (auto it = arcPayloadDestroyFns.find(typeKey); it != arcPayloadDestroyFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_destroy_payload_" + typeKey;
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcPayloadDestroyFns[typeKey] = fn;
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
  if (!isLesmaTypeReadyForArcTypeMangling(type)) {
    return nullptr;
  }
  const std::string typeKey = MangleUtils::getTypeMangledName({}, type);
  if (auto it = arcDestroyFns.find(typeKey); it != arcDestroyFns.end()) {
    return it->second;
  }
  std::string fnName = "__lesma_arc_destroy_" + typeKey;
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::PrivateLinkage, fnName, *theModule);
  arcDestroyFns[typeKey] = fn;

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  llvm::Value* payload = fn->getArg(0U);

  if (type->is(BaseType::TY_ARRAY)) {
    auto* listHandle = builder->CreateBitCast(
        payload, llvm::PointerType::get(builder->getContext(), 0U), "arc.list.handle");
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

  if (type->is(BaseType::TY_ENUM)) {
    auto* enumTy = llvm::cast<llvm::StructType>(getOrCreateLlvmType(type));
    auto* enumValue = builder->CreateBitCast(payload, llvm::PointerType::get(enumTy->getContext(), 0U),
                                             "arc.enum.obj");
    auto* tagPtr = builder->CreateStructGEP(enumTy, enumValue, 0U, "arc.enum.tag.ptr");
    auto* tagVal = builder->CreateLoad(getOrCreateEnumTagLlvmType(type), tagPtr, "arc.enum.tag");
    emitForEachEnumVariantPayloadWithTagDispatch(
        type, enumValue, tagVal, "arc.enum.destroy", [this](Type* payloadType, llvm::Value* payload) {
          emitReleaseLoadedValue(payloadType, payload, false);
        });
    emitArcFreePayload(enumValue);
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
    if (type->isBuiltinStringClass() && fieldTy != nullptr && fieldTy->is(BaseType::TY_STRING)) {
      llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
      auto* freeBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.str.free", parentFn);
      auto* doneBlock =
          llvm::BasicBlock::Create(theModule->getContext(), "arc.str.free.done", parentFn);
      auto* raw = builder->CreateLoad(builder->getPtrTy(), slot, "arc.str.raw");
      builder->CreateCondBr(
          builder->CreateICmpEQ(raw, llvm::ConstantPointerNull::get(builder->getPtrTy())),
          doneBlock, freeBlock);
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
  auto* env = builder->CreateBitCast(
      fn->getArg(0U), llvm::PointerType::get(envStructTy->getContext(), 0U), "arc.env");
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

auto Codegen::getOrCreateAsyncRuntimeInitFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_INIT},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt64Ty()}, false));
}

auto Codegen::getOrCreateAsyncRuntimeShutdownFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_SHUTDOWN},
      llvm::FunctionType::get(builder->getVoidTy(), {}, false));
}

auto Codegen::getOrCreateAsyncRuntimeRegisterTaskFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_REGISTER_TASK},
      llvm::FunctionType::get(builder->getVoidTy(),
                              {builder->getPtrTy(), builder->getPtrTy(), builder->getPtrTy(),
                               builder->getPtrTy()},
                              false));
}

auto Codegen::getOrCreateAsyncRuntimeStartTaskFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_START_TASK},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
}

auto Codegen::getOrCreateAsyncRuntimeWaitTaskFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_WAIT_TASK},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
}

auto Codegen::getOrCreateAsyncRuntimeReleaseTaskFunction() -> llvm::FunctionCallee {
  return theModule->getOrInsertFunction(
      std::string{codegen::runtime::ASYNC_RUNTIME_RELEASE_TASK},
      llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
}

auto Codegen::getOrCreateAsyncResumeHelperFunction() -> llvm::Function* {
  constexpr std::string_view name = "__lesma_async_resume_helper";
  auto* fn = theModule->getFunction(std::string{name});
  if (fn != nullptr) {
    return fn;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  fn = llvm::Function::Create(fnTy, llvm::Function::PrivateLinkage, std::string{name}, *theModule);
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto coroResumeFn = getCoroutineIntrinsic(llvm::Intrinsic::coro_resume);
  builder->CreateCall(coroResumeFn, {fn->getArg(0U)});
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateAsyncDoneHelperFunction() -> llvm::Function* {
  constexpr std::string_view name = "__lesma_async_done_helper";
  auto* fn = theModule->getFunction(std::string{name});
  if (fn != nullptr) {
    return fn;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getInt1Ty(), {builder->getPtrTy()}, false);
  fn = llvm::Function::Create(fnTy, llvm::Function::PrivateLinkage, std::string{name}, *theModule);
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto coroDoneFn = getCoroutineIntrinsic(llvm::Intrinsic::coro_done);
  llvm::Value* done = builder->CreateCall(coroDoneFn, {fn->getArg(0U)}, "async.done");
  builder->CreateRet(done);
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateAsyncDestroyHelperFunction() -> llvm::Function* {
  constexpr std::string_view name = "__lesma_async_destroy_helper";
  auto* fn = theModule->getFunction(std::string{name});
  if (fn != nullptr) {
    return fn;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false);
  fn = llvm::Function::Create(fnTy, llvm::Function::PrivateLinkage, std::string{name}, *theModule);
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto coroDestroyFn = getCoroutineIntrinsic(llvm::Intrinsic::coro_destroy);
  builder->CreateCall(coroDestroyFn, {fn->getArg(0U)});
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcDebugDeltaFunction() -> llvm::Function* {
  if (arcDebugDeltaFn != nullptr) {
    return arcDebugDeltaFn;
  }
  auto* fn = theModule->getFunction(std::string{codegen::runtime::ARC_DEBUG_DELTA});
  if (fn == nullptr) {
    auto* fnTy = llvm::FunctionType::get(builder->getInt64Ty(), {builder->getInt64Ty()}, false);
    fn = llvm::Function::Create(fnTy, llvm::Function::LinkOnceODRLinkage,
                                std::string{codegen::runtime::ARC_DEBUG_DELTA}, *theModule);
  }
  arcDebugDeltaFn = fn;
  if (!fn->empty()) {
    return fn;
  }

  fn->addFnAttr(llvm::Attribute::NoInline);
  auto* liveCount =
      theModule->getGlobalVariable(std::string{codegen::runtime::ARC_DEBUG_LIVE_COUNT}, true);
  if (liveCount == nullptr) {
    liveCount = new llvm::GlobalVariable(*theModule, builder->getInt64Ty(), false,
                                         llvm::GlobalValue::CommonLinkage, builder->getInt64(0),
                                         std::string{codegen::runtime::ARC_DEBUG_LIVE_COUNT});
  }

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto* current = builder->CreateLoad(builder->getInt64Ty(), liveCount, "arc.debug.live.current");
  auto* next = builder->CreateAdd(current, fn->getArg(0U), "arc.debug.live.next");
  builder->CreateStore(next, liveCount);
  builder->CreateRet(next);
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcDebugReportFunction() -> llvm::Function* {
  if (arcDebugReportFn != nullptr) {
    return arcDebugReportFn;
  }
  auto* fn = theModule->getFunction(std::string{codegen::runtime::ARC_DEBUG_REPORT});
  if (fn == nullptr) {
    auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {}, false);
    fn = llvm::Function::Create(fnTy, llvm::Function::LinkOnceODRLinkage,
                                std::string{codegen::runtime::ARC_DEBUG_REPORT}, *theModule);
  }
  arcDebugReportFn = fn;
  if (!fn->empty()) {
    return fn;
  }

  fn->addFnAttr(llvm::Attribute::NoInline);
  auto* liveCount =
      theModule->getGlobalVariable(std::string{codegen::runtime::ARC_DEBUG_LIVE_COUNT}, true);
  if (liveCount == nullptr) {
    liveCount = new llvm::GlobalVariable(*theModule, builder->getInt64Ty(), false,
                                         llvm::GlobalValue::CommonLinkage, builder->getInt64(0),
                                         std::string{codegen::runtime::ARC_DEBUG_LIVE_COUNT});
  }
  auto* rootsTotal = theModule->getGlobalVariable(
      std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_TOTAL}, true);
  if (rootsTotal == nullptr) {
    rootsTotal = new llvm::GlobalVariable(
        *theModule, builder->getInt64Ty(), false, llvm::GlobalValue::CommonLinkage,
        builder->getInt64(0), std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_TOTAL});
  }
  auto* rootsRemaining = theModule->getGlobalVariable(
      std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING}, true);
  if (rootsRemaining == nullptr) {
    rootsRemaining = new llvm::GlobalVariable(
        *theModule, builder->getInt64Ty(), false, llvm::GlobalValue::CommonLinkage,
        builder->getInt64(0), std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING});
  }

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto printfFn = theModule->getOrInsertFunction(
      "printf", llvm::FunctionType::get(builder->getInt32Ty(), {builder->getPtrTy()}, true));
  auto* format = builder->CreateBitCast(
      builder->CreateGlobalString("[arc] live objects: %lld (roots tracked: %lld, "
                                  "roots remaining: %lld)\n",
                                  "arc.debug.report"),
      builder->getPtrTy());
  auto* count = builder->CreateLoad(builder->getInt64Ty(), liveCount, "arc.debug.live.report");
  auto* total = builder->CreateLoad(builder->getInt64Ty(), rootsTotal, "arc.debug.roots.total");
  auto* remaining =
      builder->CreateLoad(builder->getInt64Ty(), rootsRemaining, "arc.debug.roots.remaining");
  builder->CreateCall(printfFn, {format, count, total, remaining});
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcDebugCleanupBeginFunction() -> llvm::Function* {
  if (arcDebugCleanupBeginFn != nullptr) {
    return arcDebugCleanupBeginFn;
  }
  auto* fn = theModule->getFunction(std::string{codegen::runtime::ARC_DEBUG_CLEANUP_BEGIN});
  if (fn == nullptr) {
    auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt64Ty()}, false);
    fn = llvm::Function::Create(fnTy, llvm::Function::LinkOnceODRLinkage,
                                std::string{codegen::runtime::ARC_DEBUG_CLEANUP_BEGIN}, *theModule);
  }
  arcDebugCleanupBeginFn = fn;
  if (!fn->empty()) {
    return fn;
  }

  auto* rootsTotal = theModule->getGlobalVariable(
      std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_TOTAL}, true);
  if (rootsTotal == nullptr) {
    rootsTotal = new llvm::GlobalVariable(
        *theModule, builder->getInt64Ty(), false, llvm::GlobalValue::CommonLinkage,
        builder->getInt64(0), std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_TOTAL});
  }
  auto* rootsRemaining = theModule->getGlobalVariable(
      std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING}, true);
  if (rootsRemaining == nullptr) {
    rootsRemaining = new llvm::GlobalVariable(
        *theModule, builder->getInt64Ty(), false, llvm::GlobalValue::CommonLinkage,
        builder->getInt64(0), std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING});
  }

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto* currentTotal =
      builder->CreateLoad(builder->getInt64Ty(), rootsTotal, "arc.debug.cleanup.total.current");
  auto* currentRemaining = builder->CreateLoad(builder->getInt64Ty(), rootsRemaining,
                                               "arc.debug.cleanup.remaining.current");
  auto* delta = fn->getArg(0U);
  builder->CreateStore(builder->CreateAdd(currentTotal, delta), rootsTotal);
  builder->CreateStore(builder->CreateAdd(currentRemaining, delta), rootsRemaining);
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::getOrCreateArcDebugCleanupStepFunction() -> llvm::Function* {
  if (arcDebugCleanupStepFn != nullptr) {
    return arcDebugCleanupStepFn;
  }
  auto* fn = theModule->getFunction(std::string{codegen::runtime::ARC_DEBUG_CLEANUP_STEP});
  if (fn == nullptr) {
    auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {}, false);
    fn = llvm::Function::Create(fnTy, llvm::Function::LinkOnceODRLinkage,
                                std::string{codegen::runtime::ARC_DEBUG_CLEANUP_STEP}, *theModule);
  }
  arcDebugCleanupStepFn = fn;
  if (!fn->empty()) {
    return fn;
  }

  auto* rootsRemaining = theModule->getGlobalVariable(
      std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING}, true);
  if (rootsRemaining == nullptr) {
    rootsRemaining = new llvm::GlobalVariable(
        *theModule, builder->getInt64Ty(), false, llvm::GlobalValue::CommonLinkage,
        builder->getInt64(0), std::string{codegen::runtime::ARC_DEBUG_MODULE_ROOTS_REMAINING});
  }

  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  builder->SetInsertPoint(entry);
  auto* current =
      builder->CreateLoad(builder->getInt64Ty(), rootsRemaining, "arc.debug.cleanup.step.current");
  auto* next = builder->CreateSub(current, builder->getInt64(1), "arc.debug.cleanup.step.next");
  builder->CreateStore(next, rootsRemaining);
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::emitArcDebugDelta(std::int64_t delta, llvm::Value* payloadPtr,
                                std::string_view traceMessagePrefix) -> void {
  if (!emitArcDebug) {
    return;
  }
  auto* next = builder->CreateCall(getOrCreateArcDebugDeltaFunction(), {builder->getInt64(delta)});
  if (!emitArcTrace) {
    return;
  }
  auto printfFn = theModule->getOrInsertFunction(
      "printf", llvm::FunctionType::get(builder->getInt32Ty(), {builder->getPtrTy()}, true));
  auto* format = builder->CreateBitCast(
      builder->CreateGlobalString(std::string(traceMessagePrefix) + " ptr=%p live=%lld\n",
                                  "arc.debug.live.trace"),
      builder->getPtrTy());
  builder->CreateCall(printfFn, {format, payloadPtr, next});
}

auto Codegen::emitArcDebugTraceCounts(std::string_view traceMessagePrefix, llvm::Value* payloadPtr,
                                      llvm::Value* before, llvm::Value* after) -> void {
  if (!emitArcTrace) {
    return;
  }
  auto printfFn = theModule->getOrInsertFunction(
      "printf", llvm::FunctionType::get(builder->getInt32Ty(), {builder->getPtrTy()}, true));
  auto* format =
      builder->CreateBitCast(builder->CreateGlobalString(std::string(traceMessagePrefix) +
                                                             " ptr=%p before=%lld after=%lld\n",
                                                         "arc.debug.count.trace"),
                             builder->getPtrTy());
  builder->CreateCall(printfFn, {format, payloadPtr, before, after});
}

auto Codegen::emitArcDebugValidateRefcount(llvm::Value* refCount, std::string_view message)
    -> void {
  if (!emitArcDebug) {
    return;
  }
  llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
  auto* failBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.debug.fail", parentFn);
  auto* okBlock = llvm::BasicBlock::Create(theModule->getContext(), "arc.debug.ok", parentFn);
  builder->CreateCondBr(builder->CreateICmpSGE(refCount, builder->getInt64(1)), okBlock, failBlock);
  builder->SetInsertPoint(failBlock);
  emitRuntimeStderrMessage(message);
  emitExit(1);
  builder->CreateUnreachable();
  builder->SetInsertPoint(okBlock);
}

auto Codegen::emitArcDebugTraceModuleRoot(const ModuleArcTrackedRoot& tracked) -> void {
  if (!emitArcTrace) {
    return;
  }
  auto printfFn = theModule->getOrInsertFunction(
      "printf", llvm::FunctionType::get(builder->getInt32Ty(), {builder->getPtrTy()}, true));
  auto* format = builder->CreateBitCast(
      builder->CreateGlobalString("[arc] cleanup root %s\n", "arc.debug.root.trace"),
      builder->getPtrTy());
  auto* name = builder->CreateBitCast(
      builder->CreateGlobalString(tracked.debugName, "arc.debug.root.name"), builder->getPtrTy());
  builder->CreateCall(printfFn, {format, name});
}

auto Codegen::getOrCreateModuleCleanupFunction() -> llvm::Function* {
  if (moduleCleanupFn != nullptr) {
    return moduleCleanupFn;
  }
  if (moduleArcTrackedRoots.empty()) {
    return nullptr;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {}, false);
  auto* fn = llvm::Function::Create(
      fnTy, llvm::GlobalValue::PrivateLinkage,
      MangleUtils::getImportedModuleFiniSymbolName(filename.empty() ? std::string() : filename),
      *theModule);
  moduleCleanupFn = fn;

  auto* doneGlobal = new llvm::GlobalVariable(*theModule, builder->getInt1Ty(), false,
                                              llvm::GlobalValue::PrivateLinkage,
                                              builder->getFalse(), fn->getName().str() + ".done");
  auto savedIp = builder->saveIP();
  auto* entry = llvm::BasicBlock::Create(theModule->getContext(), "entry", fn);
  auto* cleanupBlock = llvm::BasicBlock::Create(theModule->getContext(), "cleanup", fn);
  auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "done", fn);
  builder->SetInsertPoint(entry);
  auto* alreadyDone = builder->CreateLoad(builder->getInt1Ty(), doneGlobal, "arc.cleanup.done");
  builder->CreateCondBr(alreadyDone, doneBlock, cleanupBlock);

  builder->SetInsertPoint(cleanupBlock);
  builder->CreateStore(builder->getTrue(), doneGlobal);
  if (emitArcDebug) {
    builder->CreateCall(
        getOrCreateArcDebugCleanupBeginFunction(),
        {builder->getInt64(static_cast<std::int64_t>(moduleArcTrackedRoots.size()))});
  }
  emitReleaseRegisteredModuleArcRoots();
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
  builder->CreateRetVoid();
  builder->restoreIP(savedIp);
  return fn;
}

auto Codegen::emitCallPendingJitModuleFinis() -> void {
  if (pendingJitModuleFinis == nullptr) {
    return;
  }
  auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), {}, false);
  for (auto it = pendingJitModuleFinis->rbegin(); it != pendingJitModuleFinis->rend(); ++it) {
    if (it->empty()) {
      continue;
    }
    auto finiFn = theModule->getOrInsertFunction(*it, fnTy);
    builder->CreateCall(finiFn);
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

auto Codegen::emitListDeepCopy(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* newHandle =
      emitArcAlloc(headerSize, getOrCreateArcDestroyFunction(listType), "list.copy.header");
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

auto Codegen::specializedNominalEnvFor(lesma::Type* nominalTy)
    -> const std::unordered_map<std::string, lesma::Type*>* {
  if (nominalTy == nullptr || !nominalTy->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM,
                                                   BaseType::TY_TRAIT_EXISTENTIAL})) {
    return nullptr;
  }
  if (nominalTy->is(BaseType::TY_CLASS)) {
    return specializedClassEnvFor(nominalTy);
  }
  if (nominalTy->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return specializedTraitExistentialEnvFor(nominalTy);
  }
  if (nominalTy->is(BaseType::TY_ENUM)) {
    if (auto it = specializedClassTypeEnvs.find(nominalTy); it != specializedClassTypeEnvs.end()) {
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
