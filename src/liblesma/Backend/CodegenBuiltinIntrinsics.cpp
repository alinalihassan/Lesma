#include "Codegen.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

namespace lesma {
namespace {

enum class BuiltinIntrinsicKind : std::uint8_t {
  CstrByteAt,
  CstrByteSet,
  StrConcat,
  StrSlice,
  CstrIndexOf,
  CstrOffset,
  BufferNew,
  BufferLen,
  BufferCopy,
  BufferClear,
  BufferPush,
  BufferGet,
  BufferSet,
  BufferPop,
};

[[nodiscard]] auto classifyBuiltinIntrinsic(std::string_view name)
    -> std::optional<BuiltinIntrinsicKind> {
  static const std::unordered_map<std::string_view, BuiltinIntrinsicKind> kTable = {
      {"__cstr_byte_at", BuiltinIntrinsicKind::CstrByteAt},
      {"__cstr_byte_set", BuiltinIntrinsicKind::CstrByteSet},
      {"__str_concat", BuiltinIntrinsicKind::StrConcat},
      {"__str_slice", BuiltinIntrinsicKind::StrSlice},
      {"__cstr_index_of", BuiltinIntrinsicKind::CstrIndexOf},
      {"__cstr_offset", BuiltinIntrinsicKind::CstrOffset},
      {"__buffer_new", BuiltinIntrinsicKind::BufferNew},
      {"__buffer_len", BuiltinIntrinsicKind::BufferLen},
      {"__buffer_copy", BuiltinIntrinsicKind::BufferCopy},
      {"__buffer_clear", BuiltinIntrinsicKind::BufferClear},
      {"__buffer_push", BuiltinIntrinsicKind::BufferPush},
      {"__buffer_get", BuiltinIntrinsicKind::BufferGet},
      {"__buffer_set", BuiltinIntrinsicKind::BufferSet},
      {"__buffer_pop", BuiltinIntrinsicKind::BufferPop},
  };
  auto const it = kTable.find(name);
  if (it == kTable.end()) {
    return std::nullopt;
  }
  return it->second;
}

} // namespace

auto Codegen::isListIntrinsicName(const std::string& functionName) const -> bool {
  return classifyBuiltinIntrinsic(functionName).has_value();
}

auto Codegen::genListIntrinsicCall(const FuncCall* node, const std::vector<lesma::Type*>& paramTypes,
                                   const std::vector<llvm::Value*>& paramsLLVM)
    -> std::unique_ptr<lesma::Value> {
  auto const kindOpt = classifyBuiltinIntrinsic(node->getName());
  if (!kindOpt.has_value()) {
    throw CodegenError(node->getSpan(), "Unknown compiler intrinsic {}", node->getName());
  }
  BuiltinIntrinsicKind const kind = *kindOpt;

  switch (kind) {
  case BuiltinIntrinsicKind::CstrByteAt: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__cstr_byte_at expects cstr and index");
    }
    llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
    auto* gep = builder->CreateInBoundsGEP(i8, paramsLLVM[0], paramsLLVM[1], "cstr.byte.ptr");
    auto* byteVal = builder->CreateLoad(i8, gep);
    auto* int8Ty = cacheType(std::make_unique<Type>(BaseType::TY_INT, i8));
    int8Ty->setIntWidth(8);
    int8Ty->setSigned(false);
    return std::make_unique<Value>("", int8Ty, byteVal);
  }
  case BuiltinIntrinsicKind::CstrByteSet: {
    if (paramTypes.size() != 3U || paramsLLVM.size() != 3U) {
      throw CodegenError(node->getSpan(), "__cstr_byte_set expects cstr, index, and int8");
    }
    llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
    auto* gep = builder->CreateInBoundsGEP(i8, paramsLLVM[0], paramsLLVM[1], "cstr.set.ptr");
    llvm::Value* v = paramsLLVM[2];
    if (!v->getType()->isIntegerTy(8)) {
      v = builder->CreateTrunc(v, i8);
    }
    builder->CreateStore(v, gep);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  case BuiltinIntrinsicKind::StrConcat: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__str_concat expects two cstr arguments");
    }
    llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
    auto strlenFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::kStrlen}.c_str(),
        llvm::FunctionType::get(builder->getInt64Ty(), {builder->getPtrTy()}, false));
    auto memcpyFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::kMemcpy}.c_str(),
        llvm::FunctionType::get(
            builder->getPtrTy(),
            {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()}, false));
    llvm::Value* la = paramsLLVM[0];
    llvm::Value* lb = paramsLLVM[1];
    auto* lenA = builder->CreateCall(strlenFn, {la}, "strcat.lenA");
    auto* lenB = builder->CreateCall(strlenFn, {lb}, "strcat.lenB");
    auto* total = builder->CreateAdd(
        builder->CreateAdd(lenA, lenB), builder->getInt64(1), "strcat.total");
    llvm::Value* buf = emitMalloc(total, "strcat.buf");
    builder->CreateCall(memcpyFn, {buf, la, lenA});
    auto* tail = builder->CreateInBoundsGEP(i8, buf, lenA, "strcat.tail");
    builder->CreateCall(memcpyFn, {tail, lb, lenB});
    auto* endPtr = builder->CreateInBoundsGEP(
        i8, buf, builder->CreateSub(total, builder->getInt64(1)), "strcat.nul");
    builder->CreateStore(llvm::ConstantInt::get(i8, 0), endPtr);
    auto* cstrTy = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    return std::make_unique<Value>("", cstrTy, buf);
  }
  case BuiltinIntrinsicKind::StrSlice: {
    if (paramTypes.size() != 3U || paramsLLVM.size() != 3U) {
      throw CodegenError(node->getSpan(), "__str_slice expects cstr, start, and length");
    }
    llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
    auto memcpyFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::kMemcpy}.c_str(),
        llvm::FunctionType::get(
            builder->getPtrTy(),
            {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()}, false));
    llvm::Value* s = paramsLLVM[0];
    llvm::Value* start = paramsLLVM[1];
    llvm::Value* len = paramsLLVM[2];
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* emptyBlock = llvm::BasicBlock::Create(theModule->getContext(), "slice.empty", parentFunction);
    auto* copyBlock = llvm::BasicBlock::Create(theModule->getContext(), "slice.copy", parentFunction);
    auto* mergeBlock = llvm::BasicBlock::Create(theModule->getContext(), "slice.merge", parentFunction);
    auto* lenPos = builder->CreateICmpSGT(len, builder->getInt64(0));
    builder->CreateCondBr(lenPos, copyBlock, emptyBlock);

    builder->SetInsertPoint(emptyBlock);
    llvm::Value* emptyBuf = emitMalloc(builder->getInt64(1), "slice.empty.buf");
    builder->CreateStore(llvm::ConstantInt::get(i8, 0), emptyBuf);
    builder->CreateBr(mergeBlock);

    builder->SetInsertPoint(copyBlock);
    auto* total = builder->CreateAdd(len, builder->getInt64(1), "slice.total");
    llvm::Value* buf = emitMalloc(total, "slice.buf");
    auto* src = builder->CreateInBoundsGEP(i8, s, start, "slice.src");
    builder->CreateCall(memcpyFn, {buf, src, len});
    auto* nulPtr = builder->CreateInBoundsGEP(i8, buf, len, "slice.nul");
    builder->CreateStore(llvm::ConstantInt::get(i8, 0), nulPtr);
    builder->CreateBr(mergeBlock);

    builder->SetInsertPoint(mergeBlock);
    auto* phi = builder->CreatePHI(builder->getPtrTy(), 2, "slice.result");
    phi->addIncoming(emptyBuf, emptyBlock);
    phi->addIncoming(buf, copyBlock);
    auto* cstrTy = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    return std::make_unique<Value>("", cstrTy, phi);
  }
  case BuiltinIntrinsicKind::CstrIndexOf: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__cstr_index_of expects haystack and needle");
    }
    auto strstrFn = theModule->getOrInsertFunction(
        std::string{codegen::runtime::kStrstr}.c_str(),
        llvm::FunctionType::get(builder->getPtrTy(),
                                {builder->getPtrTy(), builder->getPtrTy()}, false));
    llvm::Value* hay = paramsLLVM[0];
    llvm::Value* needle = paramsLLVM[1];
    llvm::Value* found = builder->CreateCall(strstrFn, {hay, needle}, "idx.found");
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* okBlock = llvm::BasicBlock::Create(theModule->getContext(), "idx.ok", parentFunction);
    auto* failBlock = llvm::BasicBlock::Create(theModule->getContext(), "idx.fail", parentFunction);
    auto* mergeBlock = llvm::BasicBlock::Create(theModule->getContext(), "idx.merge", parentFunction);
    builder->CreateCondBr(
        builder->CreateICmpEQ(found, llvm::ConstantPointerNull::get(builder->getPtrTy())), failBlock,
        okBlock);

    builder->SetInsertPoint(failBlock);
    builder->CreateBr(mergeBlock);

    builder->SetInsertPoint(okBlock);
    auto* diff = builder->CreateSub(builder->CreatePtrToInt(found, builder->getInt64Ty()),
                                    builder->CreatePtrToInt(hay, builder->getInt64Ty()), "idx.diff");
    builder->CreateBr(mergeBlock);

    builder->SetInsertPoint(mergeBlock);
    auto* phi = builder->CreatePHI(builder->getInt64Ty(), 2, "idx.result");
    phi->addIncoming(builder->getInt64(-1), failBlock);
    phi->addIncoming(diff, okBlock);
    auto* intTy = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    intTy->setIntWidth(64);
    return std::make_unique<Value>("", intTy, phi);
  }
  case BuiltinIntrinsicKind::CstrOffset: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__cstr_offset expects cstr and offset");
    }
    llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
    auto* out = builder->CreateInBoundsGEP(i8, paramsLLVM[0], paramsLLVM[1], "cstr.off");
    auto* cstrTy = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    return std::make_unique<Value>("", cstrTy, out);
  }
  case BuiltinIntrinsicKind::BufferNew: {
    auto explicitTypeArgs = node->getExplicitTypeArgs();
    if (explicitTypeArgs.size() != 1U) {
      throw CodegenError(node->getSpan(), "__buffer_new<T>() expects exactly one type argument");
    }
    explicitTypeArgs.front()->accept(*this);
    auto* listType =
        cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, result->getType()));
    listType->setDisplayName("__buffer");
    getOrCreateLlvmType(listType);
    auto* listStructTy = getOrCreateListStructType(listType);
    auto* headerSize = builder->getInt64(
        theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
    auto* listHandle = emitMalloc(headerSize, "buffer.header");
    emitStoreListDataPtr(listType, listHandle, llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(listType, listHandle, builder->getInt64(0));
    emitStoreListCapacity(listType, listHandle, builder->getInt64(0));
    return std::make_unique<Value>("", listType, listHandle);
  }
  case BuiltinIntrinsicKind::BufferLen:
  case BuiltinIntrinsicKind::BufferCopy:
  case BuiltinIntrinsicKind::BufferClear:
  case BuiltinIntrinsicKind::BufferPush:
  case BuiltinIntrinsicKind::BufferGet:
  case BuiltinIntrinsicKind::BufferSet:
  case BuiltinIntrinsicKind::BufferPop:
    break;
  }

  if (paramTypes.empty() || paramsLLVM.empty()) {
    throw CodegenError(node->getSpan(), "Buffer intrinsic {} requires a buffer argument",
                       node->getName());
  }
  auto* listType = paramTypes.front();
  auto* listHandle = paramsLLVM.front();
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) ||
      listType->getElementType() == nullptr) {
    throw CodegenError(node->getSpan(), "Buffer intrinsic {} requires __buffer<T>", node->getName());
  }

  switch (kind) {
  case BuiltinIntrinsicKind::BufferLen: {
    auto* lenTy = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    lenTy->setIntWidth(64);
    return std::make_unique<Value>("", lenTy, emitListLength(listType, listHandle));
  }
  case BuiltinIntrinsicKind::BufferCopy:
    return std::make_unique<Value>("", listType, emitListDeepCopy(listType, listHandle));
  case BuiltinIntrinsicKind::BufferClear: {
    auto* currentData = emitListDataPtr(listType, listHandle);
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* freeBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "list.clear.free", parentFunction);
    auto* doneBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "list.clear.done", parentFunction);
    builder->CreateCondBr(
        builder->CreateICmpNE(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
        freeBlock, doneBlock);
    builder->SetInsertPoint(freeBlock);
    emitFree(currentData);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    emitStoreListDataPtr(listType, listHandle, llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(listType, listHandle, builder->getInt64(0));
    emitStoreListCapacity(listType, listHandle, builder->getInt64(0));
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  case BuiltinIntrinsicKind::BufferPush: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__buffer_push expects buffer and value");
    }
    auto* length = emitListLength(listType, listHandle);
    auto* nextLength = builder->CreateAdd(length, builder->getInt64(1));
    emitListEnsureCapacity(listType, listHandle, nextLength);
    auto* elementPtr =
        builder->CreateGEP(getListStoredElementType(listType),
                           emitListDataPtr(listType, listHandle), length, "list.push.ptr");
    Value pushedArg("", paramTypes[1], paramsLLVM[1]);
    builder->CreateStore(
        getListStoredElementValue(node->getSpan(), &pushedArg, listType->getElementType()),
        elementPtr);
    emitStoreListLength(listType, listHandle, nextLength);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  case BuiltinIntrinsicKind::BufferGet: {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__buffer_get expects buffer and index");
    }
    auto* elementPtr = emitListElementPointer(node->getSpan(), listType, listHandle, paramsLLVM[1]);
    return std::make_unique<Value>(
        "", listType->getElementType(),
        builder->CreateLoad(getListStoredElementType(listType), elementPtr));
  }
  case BuiltinIntrinsicKind::BufferSet: {
    if (paramTypes.size() != 3U || paramsLLVM.size() != 3U) {
      throw CodegenError(node->getSpan(), "__buffer_set expects buffer, index, and value");
    }
    auto* elementPtr = emitListElementPointer(node->getSpan(), listType, listHandle, paramsLLVM[1]);
    Value setArg("", paramTypes[2], paramsLLVM[2]);
    builder->CreateStore(
        getListStoredElementValue(node->getSpan(), &setArg, listType->getElementType()),
        elementPtr);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  case BuiltinIntrinsicKind::BufferPop: {
    auto* length = emitListLength(listType, listHandle);
    emitListBoundsCheck(node->getSpan(), listType, listHandle,
                        builder->CreateSub(length, builder->getInt64(1)));
    auto* newLength = builder->CreateSub(length, builder->getInt64(1), "list.pop.len");
    auto* elementPtr =
        builder->CreateGEP(getListStoredElementType(listType),
                           emitListDataPtr(listType, listHandle), newLength, "list.pop.ptr");
    auto* poppedValue = builder->CreateLoad(getListStoredElementType(listType), elementPtr);
    emitStoreListLength(listType, listHandle, newLength);
    return std::make_unique<Value>("", listType->getElementType(), poppedValue);
  }
  default:
    break;
  }

  throw CodegenError(node->getSpan(), "Unhandled compiler intrinsic {}", node->getName());
}

} // namespace lesma
