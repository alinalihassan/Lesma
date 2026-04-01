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
} // namespace lesma::TypeUtils
