#include "CodegenTypeUtils.h"

#include <memory>

#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/SMLoc.h>

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

namespace lesma::CodegenTypeUtils {
auto getExtendedType(Type* left, Type* right) -> Type* {
  if (left->isEqual(right)) {
    return left;
  }

  if (left->is(BaseType::TY_BOOL) && right->is(BaseType::TY_BOOL)) {
    return left;
  }

  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    // Multiple int widths (i32, i64, etc.) are intentional for FFI; we pick
    // the wider type when unifying.
    if (left->getLlvmType()->getIntegerBitWidth() > right->getLlvmType()->getIntegerBitWidth()) {
      return left;
    }
    if (left->getLlvmType()->getIntegerBitWidth() < right->getLlvmType()->getIntegerBitWidth()) {
      return right;
    }
    if (!left->isSigned()) {
      return left;
    }
    if (!right->isSigned()) {
      return right;
    }
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT32)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT32)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT32) && right->is(BaseType::TY_FLOAT32)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    if (left->getLlvmType()->isFP128Ty() || right->getLlvmType()->isFP128Ty()) {
      return left->getLlvmType()->isFP128Ty() ? left : right;
    }
    if (left->getLlvmType()->isDoubleTy() || right->getLlvmType()->isDoubleTy()) {
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

  if (type->is(BaseType::TY_CLASS) && val->getType()->is(BaseType::TY_PTR)) {
    Type* fromElem = val->getType()->getElementType();
    if (fromElem != nullptr && fromElem->is(BaseType::TY_CLASS)) {
      for (Type* t = fromElem; t != nullptr; t = t->getClassSuperclass()) {
        if (t->isEqual(type)) {
          auto out = std::make_unique<Value>("", type, val->getLlvmValue());
          out->setArcOwnedValue(val->getArcOwnedValue());
          return out;
        }
      }
    }
  }

  if (type->is(BaseType::TY_INT)) {
    if (val->getType()->isFloatingPoint()) {
      auto* casted = type->isSigned()
                         ? builder->CreateFPToSI(val->getLlvmValue(), type->getLlvmType())
                         : builder->CreateFPToUI(val->getLlvmValue(), type->getLlvmType());
      return std::make_unique<Value>("", type, casted);
    }
    if (val->getType()->is(BaseType::TY_INT)) {
      return std::make_unique<Value>("", type,
                                     builder->CreateIntCast(val->getLlvmValue(),
                                                            type->getLlvmType(),
                                                            val->getType()->isSigned()));
    }
  } else if (type->isFloatingPoint()) {
    if (val->getType()->is(BaseType::TY_INT)) {
      auto* casted = val->getType()->isSigned()
                         ? builder->CreateSIToFP(val->getLlvmValue(), type->getLlvmType())
                         : builder->CreateUIToFP(val->getLlvmValue(), type->getLlvmType());
      return std::make_unique<Value>("", type, casted);
    }
    if (val->getType()->isFloatingPoint()) {
      return std::make_unique<Value>(
          "", type, builder->CreateFPCast(val->getLlvmValue(), type->getLlvmType()));
    }
  } else if (type->is(BaseType::TY_STRING)) {
    if (val->getType()->is(BaseType::TY_PTR)) {
      Type* elem = val->getType()->getElementType();
      const bool isPtrToVoid = elem != nullptr && elem->is(BaseType::TY_VOID);
      const bool isPtrToByte =
          elem != nullptr && elem->is(BaseType::TY_INT) && elem->getLlvmType() != nullptr &&
          elem->getLlvmType()->isIntegerTy() && elem->getLlvmType()->getIntegerBitWidth() == 8U;
      if (isPtrToVoid || isPtrToByte) {
        auto out = std::make_unique<Value>(
            "", type, builder->CreateBitCast(val->getLlvmValue(), type->getLlvmType()));
        out->setArcOwnedValue(val->getArcOwnedValue());
        return out;
      }
    }
  }

  if (type->is(BaseType::TY_PTR) && val->getType()->is(BaseType::TY_PTR)) {
    Type* toElem = type->getElementType();
    Type* fromElem = val->getType()->getElementType();
    if (fromElem != nullptr && toElem != nullptr && fromElem->is(BaseType::TY_CLASS) &&
        toElem->is(BaseType::TY_CLASS)) {
      for (Type* t = fromElem; t != nullptr; t = t->getClassSuperclass()) {
        if (t->isEqual(toElem)) {
          auto out = std::make_unique<Value>(
              "", type, builder->CreateBitCast(val->getLlvmValue(), type->getLlvmType()));
          out->setArcOwnedValue(val->getArcOwnedValue());
          return out;
        }
      }
    }
  }

  if (val->getType()->is(BaseType::TY_FUNCTION) && type->is(BaseType::TY_FUNCTION)) {
    auto out = std::make_unique<Value>(*val);
    out->setType(type);
    return out;
  }

  if (val->getType()->is(BaseType::TY_TUPLE) && type->is(BaseType::TY_TUPLE)) {
    std::vector<Field*> const fromFields = val->getType()->getFields();
    std::vector<Field*> const toFields = type->getFields();
    if (fromFields.size() != toFields.size()) {
      throw CodegenError(span, "Tuple cast arity mismatch: {} vs {}",
                         MangleUtils::getTypeMangledName(span, val->getType()),
                         MangleUtils::getTypeMangledName(span, type));
    }
    llvm::Type* outStructTy = type->getLlvmType();
    if (outStructTy == nullptr) {
      throw CodegenError(span, "Tuple cast target has no LLVM type");
    }
    llvm::Value* srcAgg = val->getLlvmValue();
    if (srcAgg == nullptr) {
      throw CodegenError(span, "Tuple cast source has no LLVM value");
    }
    llvm::Value* agg = llvm::UndefValue::get(outStructTy);
    for (size_t i = 0; i < fromFields.size(); ++i) {
      if (toFields[i]->type == nullptr) {
        throw CodegenError(span, "Tuple cast target field {} has no type", i);
      }
      llvm::Value* ev =
          builder->CreateExtractValue(srcAgg, static_cast<unsigned>(i), "tup.cast.elem");
      auto elem = std::make_unique<Value>("", fromFields[i]->type, ev);
      auto casted = CodegenTypeUtils::cast(span, elem.get(), toFields[i]->type, builder);
      agg = builder->CreateInsertValue(agg, casted->getLlvmValue(), static_cast<unsigned>(i),
                                       "tup.cast");
    }
    auto out = std::make_unique<Value>("", type, agg);
    out->setArcOwnedValue(val->getArcOwnedValue());
    return out;
  }

  throw CodegenError(span, "Unsupported Cast between {} and {}",
                     MangleUtils::getTypeMangledName(span, val->getType()),
                     MangleUtils::getTypeMangledName(span, type));
}
} // namespace lesma::CodegenTypeUtils