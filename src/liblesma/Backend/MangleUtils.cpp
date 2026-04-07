#include "MangleUtils.h"

#include <string>
#include <string_view>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SMLoc.h>

#include "fmt/format.h"

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Symbol/Type.h"

namespace {

// FNV-1a 64-bit: published test-vector constants (same as Wikipedia / chongo’s FNV page),
// not magic picks — xor each byte into the hash, then multiply by the FNV prime.
// Deterministic everywhere; std::hash<std::string> is not.
auto stableHash64(std::string_view data) -> unsigned long long {
  constexpr unsigned long long offsetBasis = 0xCBF29CE484222325ULL;
  constexpr unsigned long long prime = 0x100000001B3ULL;
  unsigned long long h = offsetBasis;
  for (char ch : data) {
    h ^= static_cast<unsigned char>(ch);
    h *= prime;
  }
  return h;
}

} // namespace

namespace lesma::MangleUtils {
auto getTypeMangledName(llvm::SMRange span, Type* type) -> std::string {
  if (type->is(BaseType::TY_GENERIC)) {
    return "(gen_" + type->getGenericName() + ")";
  }
  if (type->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return "(exist_" + type->getDisplayName() + ")";
  }
  auto* llvmTy = type->getLlvmType();
  if (llvmTy == nullptr) {
    throw CodegenError(span, "Type has no LLVM type for mangling: {}", type->toString());
  }
  if (type->is(BaseType::TY_BOOL)) {
    return "b";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(8)) {
    return "c";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(16)) {
    return "i16";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(32)) {
    return "i32";
  }
  if (type->is(BaseType::TY_INT)) {
    return "i";
  }
  if (type->is(BaseType::TY_FLOAT32)) {
    return "f32";
  }
  if (type->is(BaseType::TY_FLOAT)) {
    return "f";
  }
  if (type->is(BaseType::TY_STRING)) {
    return "str";
  }
  if (type->is(BaseType::TY_VOID)) {
    return "void";
  }
  if (type->is(BaseType::TY_NULL)) {
    return "null";
  }
  if (type->is(BaseType::TY_ARRAY)) {
    return "(arr_" + getTypeMangledName(span, type->getElementType()) + ")";
  }
  if (type->is(BaseType::TY_PTR)) {
    return "(ptr_" + getTypeMangledName(span, type->getElementType()) + ")";
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    std::string paramStr;
    for (const auto& field : type->getFields()) {
      paramStr += getTypeMangledName(span, field->type) + "_";
    }
    return "(func_" + paramStr + ")";
  }
  if (type->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    if (auto* structTy = llvm::dyn_cast<llvm::StructType>(llvmTy)) {
      return "(struct_" + structTy->getName().str() + ")";
    }
    throw CodegenError(span, "Class/Enum type does not have LLVM struct type");
  }
  if (type->is(BaseType::TY_TUPLE)) {
    std::string s = "tup_";
    for (auto* f : type->getFields()) {
      if (f == nullptr) {
        throw CodegenError(span, "unresolved tuple field: null Field* in tuple type {}",
                           type->toString());
      }
      if (f->type == nullptr) {
        throw CodegenError(span, "unresolved tuple field type: field '{}' in tuple {}", f->name,
                           type->toString());
      }
      s += getTypeMangledName(span, f->type) + "_";
    }
    return "(" + s + ")";
  }
  if (type->is(BaseType::TY_UNION)) {
    std::string s = "union_";
    for (Type* m : type->getUnionMembers()) {
      if (m == nullptr) {
        throw CodegenError(span, "unresolved union member in {}", type->toString());
      }
      s += getTypeMangledName(span, m) + "_";
    }
    return "(" + s + ")";
  }

  throw CodegenError(span, "Unknown type found during mangling");
}

auto isMethod(const std::string& mangledName) -> bool {
  return mangledName.find("::") != std::string::npos;
}

auto getGlobalVariableSymbolName(const std::string& modulePathNormalized,
                                 const std::string& variableName) -> std::string {
  std::string const norm = modulePathNormalized.empty()
                               ? std::string("<stdin>")
                               : normalizeResolvedFilesystemPath(modulePathNormalized);
  std::string const key = norm + '\0' + variableName;
  unsigned long long const h = stableHash64(key);
  return fmt::format("__lesma_g_{:x}_{}", static_cast<unsigned long long>(h), variableName);
}

auto getImportedModuleInitSymbolName(const std::string& modulePathNormalized) -> std::string {
  std::string const norm = modulePathNormalized.empty()
                               ? std::string("<stdin>")
                               : normalizeResolvedFilesystemPath(modulePathNormalized);
  unsigned long long const h = stableHash64(norm);
  return fmt::format("__lesma_mod_init_{:x}", static_cast<unsigned long long>(h));
}
} // namespace lesma::MangleUtils
