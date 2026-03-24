#include "TypeUtils.h"

#include "liblesma/Symbol/Type.h"

namespace lesma::TypeUtils {
auto findIndexInFields(Type* structType, const std::string& field) -> int {
  for (unsigned int i = 0; i < structType->getFields().size(); i++) {
    if (structType->getFields()[i]->name == field) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

auto findTypeInFields(Type* structType, const std::string& field) -> Type* {
  for (const auto& i : structType->getFields()) {
    if (i->name == field) {
      return i->type;
    }
  }
  return nullptr;
}

auto findFieldInFields(Type* structType, const std::string& field) -> Field* {
  for (Field* candidate : structType->getFields()) {
    if (candidate != nullptr && candidate->name == field) {
      return candidate;
    }
  }
  return nullptr;
}

auto isNominalTypeForIdentity(Type const* t) -> bool {
  return t != nullptr && t->isOneOf({BaseType::TY_CLASS, BaseType::TY_ARRAY, BaseType::TY_ENUM});
}

auto passesByPointerInAbi(Type const* t) -> bool {
  if (t == nullptr) {
    return false;
  }
  if (t->is(BaseType::TY_PTR)) {
    return true;
  }
  return t->is(BaseType::TY_CLASS) || t->is(BaseType::TY_TRAIT_EXISTENTIAL);
}
} // namespace lesma::TypeUtils
