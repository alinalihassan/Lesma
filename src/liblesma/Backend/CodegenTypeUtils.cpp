#include "CodegenTypeUtils.h"

#include <memory>

#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/SMLoc.h>

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

namespace lesma {
namespace CodegenTypeUtils {
auto getExtendedType(Type* left, Type* right) -> Type* {
  if (left->getBaseType() == right->getBaseType()) {
    return left;
  }

  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    // Multiple int widths (i32, i64, etc.) are intentional for FFI; we pick
    // the wider type when unifying.
    if (left->getLlvmType()->getIntegerBitWidth() >
        right->getLlvmType()->getIntegerBitWidth()) {
      return left;
    }
    return right;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    if (left->getLlvmType()->isFP128Ty() || right->getLlvmType()->isFP128Ty()) {
      return left->getLlvmType()->isFP128Ty() ? left : right;
    }
    if (left->getLlvmType()->isDoubleTy() ||
        right->getLlvmType()->isDoubleTy()) {
      return left->getLlvmType()->isDoubleTy() ? left : right;
    }
    if (left->getLlvmType()->isFloatTy() || right->getLlvmType()->isFloatTy()) {
      return left->getLlvmType()->isFloatTy() ? left : right;
    }
    if (left->getLlvmType()->isHalfTy() || right->getLlvmType()->isHalfTy()) {
      return left->getLlvmType()->isHalfTy() ? left : right;
    }
  }
  return nullptr;
}

auto cast(llvm::SMRange span, Value* val, Type* type, llvm::IRBuilder<>* builder)
    -> std::unique_ptr<Value> {
  if (type == nullptr) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  if (val->getType()->isEqual(type)) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  if (type->is(BaseType::TY_INT)) {
    if (val->getType()->is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPToSI(val->getLlvmValue(), type->getLlvmType()));
    }
    if (val->getType()->is(BaseType::TY_INT)) {
      return std::make_unique<Value>("", type,
                                     builder->CreateIntCast(val->getLlvmValue(),
                                                            type->getLlvmType(),
                                                            type->isSigned()));
    }
  } else if (type->is(BaseType::TY_FLOAT)) {
    if (val->getType()->is(BaseType::TY_INT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateSIToFP(val->getLlvmValue(), type->getLlvmType()));
    }
    if (val->getType()->is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPCast(val->getLlvmValue(), type->getLlvmType()));
    }
  } else if (type->is(BaseType::TY_STRING)) {
    if (val->getType()->is(BaseType::TY_PTR) &&
        (val->getType()->getElementType()->is(BaseType::TY_INT) ||
         val->getType()->getElementType()->is(BaseType::TY_VOID))) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateBitCast(val->getLlvmValue(), type->getLlvmType()));
    }
  }

  throw CodegenError(span, "Unsupported Cast between {} and {}",
                     MangleUtils::getTypeMangledName(span, val->getType()),
                     MangleUtils::getTypeMangledName(span, type));
}
} // namespace CodegenTypeUtils
} // namespace lesma
