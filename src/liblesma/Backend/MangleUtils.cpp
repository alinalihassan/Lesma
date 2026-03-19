#include "MangleUtils.h"

#include <string>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SMLoc.h>

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Symbol/Type.h"

namespace lesma {
namespace MangleUtils {
auto getTypeMangledName(llvm::SMRange span, Type* type) -> std::string {
  auto* llvmTy = type->getLlvmType();
  if (llvmTy == nullptr &&
      type->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_STRING, BaseType::TY_BOOL,
                     BaseType::TY_VOID, BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    throw CodegenError(span, "Type {} is missing LLVM type during mangling", type->toString());
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
  if (type->is(BaseType::TY_FLOAT) && llvmTy->isFloatTy()) {
    return "f32";
  }
  if (type->is(BaseType::TY_FLOAT) && llvmTy->isFloatingPointTy()) {
    return "f";
  }
  if (type->is(BaseType::TY_STRING)) {
    return "str";
  }
  if (type->is(BaseType::TY_VOID)) {
    return "void";
  }
  if (type->is(BaseType::TY_ARRAY) && llvmTy->isArrayTy()) {
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

  throw CodegenError(span, "Unknown type found during mangling");
}

auto isMethod(const std::string& mangledName) -> bool {
  return mangledName.find("::") != std::string::npos;
}

auto isMangled(std::string name) -> bool {
  if (name.empty()) {
    return false;
  }
  return name.find(':') != std::string::npos || name.at(0) == '.';
}

auto getDemangledName(const std::string& name) -> std::string {
  if (!isMangled(name)) {
    return name;
  }

  auto demangledName = name;

  // Remove class mangling
  auto classMangling = demangledName.find("::");
  if (classMangling != std::string::npos) {
    demangledName = demangledName.substr(classMangling + 2);
  }

  // Remove standard '.' mangling to differentiate from native functions
  if (!demangledName.empty() && demangledName.at(0) == '.') {
    demangledName.erase(0, 1);
  }

  // Remove parameters mangling
  auto parameterMangling = demangledName.find(':');
  if (parameterMangling != std::string::npos) {
    demangledName = demangledName.substr(0, parameterMangling);
  }

  return demangledName;
}
} // namespace MangleUtils
} // namespace lesma
