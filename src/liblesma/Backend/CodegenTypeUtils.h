#pragma once

#include <memory>

#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/SMLoc.h>

namespace lesma {
class Type;
class Value;

namespace CodegenTypeUtils {
auto getExtendedType(Type* left, Type* right) -> Type*;
auto cast(llvm::SMRange span, Value* val, Type* type, llvm::IRBuilder<>* builder)
    -> std::unique_ptr<Value>;
} // namespace CodegenTypeUtils
} // namespace lesma
