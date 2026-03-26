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

auto optionalPayloadUsesNullablePointer(Type const* inner) -> bool {
  if (inner == nullptr) {
    return false;
  }
  if (inner->is(BaseType::TY_CLASS) || inner->is(BaseType::TY_STRING) ||
      inner->is(BaseType::TY_ARRAY) || inner->is(BaseType::TY_FUNCTION) ||
      inner->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return true;
  }
  if (inner->is(BaseType::TY_PTR) && inner->getElementType() != nullptr) {
    Type const* elem = inner->getElementType();
    return elem->is(BaseType::TY_CLASS) || elem->is(BaseType::TY_TRAIT_EXISTENTIAL);
  }
  return false;
}
} // namespace lesma::TypeUtils
