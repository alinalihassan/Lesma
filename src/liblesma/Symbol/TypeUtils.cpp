#include "TypeUtils.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

auto findIndexInEnumVariants(const Type* enumType, const std::string& variantName) -> int {
  if (enumType == nullptr || !enumType->is(BaseType::TY_ENUM)) {
    return -1;
  }
  auto variants = enumType->getEnumVariants();
  for (size_t i = 0; i < variants.size(); ++i) {
    if (variants[i] != nullptr && variants[i]->name == variantName) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

auto findEnumVariant(const Type* enumType, const std::string& variantName) -> const EnumVariant* {
  if (enumType == nullptr || !enumType->is(BaseType::TY_ENUM)) {
    return nullptr;
  }
  for (EnumVariant* variant : enumType->getEnumVariants()) {
    if (variant != nullptr && variant->name == variantName) {
      return variant;
    }
  }
  return nullptr;
}

auto classDataFieldStructIndex(Type* classTy, unsigned logicalIndex) -> unsigned {
  if (classTy != nullptr && classTy->is(BaseType::TY_CLASS)) {
    return logicalIndex + 1U;
  }
  return logicalIndex;
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

auto findStaticFieldInClass(Type* classType, const std::string& field) -> Field* {
  if (classType == nullptr || !classType->is(BaseType::TY_CLASS)) {
    return nullptr;
  }
  for (Field* candidate : classType->getStaticFields()) {
    if (candidate != nullptr && candidate->name == field) {
      return candidate;
    }
  }
  return nullptr;
}

auto isNominalTypeForIdentity(Type const* t) -> bool {
  return t != nullptr && (t->isNominal() || t->is(BaseType::TY_ARRAY));
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

auto isArcReferenceType(Type const* t) -> bool {
  if (t == nullptr) {
    return false;
  }
  if (t->is(BaseType::TY_CLASS) || t->is(BaseType::TY_ARRAY)) {
    return true;
  }
  return t->is(BaseType::TY_PTR) && t->getElementType() != nullptr &&
         t->getElementType()->is(BaseType::TY_CLASS);
}

auto containsArcManagedValue(Type const* t) -> bool {
  if (t == nullptr) {
    return false;
  }
  if (isArcReferenceType(t)) {
    return true;
  }
  switch (t->getBaseType()) {
  case BaseType::TY_FUNCTION:
  case BaseType::TY_ANY:
  case BaseType::TY_TRAIT_EXISTENTIAL:
    return true;
  case BaseType::TY_TUPLE:
    return std::ranges::any_of(
        t->getFields(), [](Field const* field) { return containsArcManagedValue(field->type); });
  case BaseType::TY_ENUM:
    return std::ranges::any_of(t->getEnumVariants(), [](EnumVariant const* variant) {
      if (variant == nullptr) {
        return false;
      }
      return std::ranges::any_of(variant->payloadTypes,
                                 [](Type const* payload) { return containsArcManagedValue(payload); });
    });
  case BaseType::TY_UNION:
    return std::ranges::any_of(t->getUnionMembers(),
                               [](Type const* member) { return containsArcManagedValue(member); });
  default:
    return false;
  }
}

auto makeSpecializedClassKey(Type* classTemplate, const std::vector<std::string>& genericParamNames,
                             const std::unordered_map<std::string, Type*>& env) -> std::string {
  std::ostringstream key;
  key << classTemplate->toString();
  for (const auto& name : genericParamNames) {
    auto it = env.find(name);
    key << "|";
    if (it != env.end()) {
      key << it->second->toString();
      continue;
    }
    key << "<unbound:" << name << ">";
  }
  return key.str();
}

auto canonicalizeUnionMembers(std::vector<Type*> arms)
    -> std::pair<std::vector<Type*>, std::string> {
  std::vector<Type*> flat;
  flat.reserve(arms.size());
  auto appendFlattened = [&](Type* cur, auto&& self) -> void {
    if (cur == nullptr) {
      return;
    }
    if (cur->is(BaseType::TY_UNION)) {
      for (Type* inner : cur->getUnionMembers()) {
        self(inner, self);
      }
    } else {
      flat.push_back(cur);
    }
  };
  for (Type* a : arms) {
    appendFlattened(a, appendFlattened);
  }
  std::vector<Type*> unique;
  unique.reserve(flat.size());
  for (Type* m : flat) {
    bool duplicate = false;
    for (Type* u : unique) {
      if (m->isEqual(u)) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      unique.push_back(m);
    }
  }
  std::ranges::sort(unique, [](Type* a, Type* b) { return a->toString() < b->toString(); });
  std::string displayName;
  for (size_t i = 0; i < unique.size(); ++i) {
    if (i > 0U) {
      displayName += " | ";
    }
    displayName += unique[i]->toString();
  }
  return {std::move(unique), std::move(displayName)};
}

auto computeOptionalPayloadMembers(Type* type) -> std::optional<OptionalPayloadMembers> {
  if (type == nullptr || !type->is(BaseType::TY_UNION)) {
    return std::nullopt;
  }
  std::vector<Type*> payloadMembers;
  bool sawNull = false;
  for (Type* member : type->getUnionMembers()) {
    if (member == nullptr) {
      continue;
    }
    if (member->is(BaseType::TY_NULL)) {
      sawNull = true;
      continue;
    }
    payloadMembers.push_back(member);
  }
  if (!sawNull || payloadMembers.empty()) {
    return std::nullopt;
  }
  auto [canonical, displayName] = canonicalizeUnionMembers(std::move(payloadMembers));
  return OptionalPayloadMembers{std::move(canonical), std::move(displayName)};
}
} // namespace lesma::TypeUtils
