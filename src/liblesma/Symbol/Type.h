#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/SMLoc.h>

namespace lesma {
class Value; // Forward declaration instead of include to break circular
             // dependency
enum class BaseType : std::uint8_t {
  TY_INVALID,
  TY_INT,
  TY_FLOAT,
  /** IEEE binary32 (`float` in LLVM); distinct from `TY_FLOAT` (typically f64). */
  TY_FLOAT32,
  TY_STRING,
  TY_BOOL,
  TY_PTR,
  TY_ARRAY,
  TY_VOID,
  TY_FUNCTION,
  TY_GENERIC,
  TY_CLASS,
  TY_ENUM,
  TY_IMPORT,
  /** Existential trait type (e.g. `Drawable` as a value type): layout { ptr payload, ptr witness }.
   */
  TY_TRAIT_EXISTENTIAL,
  /** Structural product type `(T1, T2, ...)` lowered to LLVM struct. */
  TY_TUPLE,
};

class Type;

struct Field {
  std::string name;
  Type* type;
  std::unique_ptr<Value> defaultValue;
  std::unique_ptr<Value> declarationSymbol;
  llvm::SMRange declarationSpan;
  std::string declarationFilePath;

  // Constructor for fields without default value
  Field(std::string n, Type* t);

  // Constructor for fields with default value
  Field(std::string n, Type* t, std::unique_ptr<Value> defVal);

  ~Field();
  Field(Field&&) noexcept;
  auto operator=(Field&&) noexcept -> Field&;
  Field(const Field&) = delete;
  auto operator=(const Field&) -> Field& = delete;

  [[nodiscard]] auto getDeclarationSpan() const -> llvm::SMRange { return declarationSpan; }
  [[nodiscard]] auto getDeclarationFilePath() const -> const std::string& {
    return declarationFilePath;
  }
  [[nodiscard]] auto getDeclarationSymbol() const -> Value* { return declarationSymbol.get(); }
  auto setDeclarationSpan(llvm::SMRange span) -> void { declarationSpan = span; }
  auto setDeclarationFilePath(std::string path) -> void { declarationFilePath = std::move(path); }
  auto setDeclarationSymbol(std::unique_ptr<Value> value) -> void;
};

class Type {
  BaseType baseType;
  llvm::Type* llvmType;

  // Non-owning references to other Types (owned elsewhere)
  Type* elementType;
  Type* returnType;
  /** For TY_CLASS: direct superclass (single inheritance), or nullptr. */
  Type* classSuperclass = nullptr;
  /** True once another class declares this type as its base (needs vtable dispatch via base ptr). */
  bool classHasDerivedClass = false;
  std::string genericName;
  std::string displayName;
  /** Declared generic parameter names in order (for TY_CLASS and TY_FUNCTION). */
  std::vector<std::string> genericParams;
  /** Parallel to genericParams: trait intersection bounds per generic parameter (TY_FUNCTION). */
  std::vector<std::vector<std::string>> genericParamTraitBounds;
  /** For TY_CLASS: explicit `impl Trait` names from the declaration. */
  std::vector<std::string> implTraitNames;
  /** For TY_CLASS: instance method names in vtable order (root-to-leaf, overrides share a slot). */
  std::vector<std::string> classVtableMethodOrder;
  // Owned collection of Fields
  std::vector<std::unique_ptr<Field>> fields;
  llvm::SMRange declarationSpan;
  std::string declarationFilePath;
  bool varArgs = false;
  bool signedInt = true;
  /** For TY_INT when LLVM type is not yet set: 0 means default width (64). */
  std::uint16_t intWidth = 0;

public:
  explicit Type(BaseType baseType)
      : baseType(baseType), llvmType(nullptr), elementType(nullptr), returnType(nullptr) {}
  explicit Type(BaseType baseType, llvm::Type* llvmType)
      : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr) {}
  explicit Type(BaseType baseType, llvm::Type* llvmType, Type* elementType)
      : baseType(baseType), llvmType(llvmType), elementType(elementType), returnType(nullptr) {}
  explicit Type(std::string genericName)
      : baseType(BaseType::TY_GENERIC), llvmType(nullptr), elementType(nullptr),
        returnType(nullptr), genericName(std::move(genericName)) {}
  explicit Type(BaseType baseType, llvm::Type* llvmType, std::vector<std::unique_ptr<Field>> fields)
      : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr),
        fields(std::move(fields)) {}

  ~Type() = default;
  Type(const Type&) = delete;
  auto operator=(const Type&) -> Type& = delete;
  Type(Type&&) = default;
  auto operator=(Type&&) -> Type& = default;

  [[nodiscard]] auto is(BaseType type) const -> bool { return baseType == type; }
  [[nodiscard]] auto isPrimitive() const -> bool {
    return isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_FLOAT32, BaseType::TY_STRING,
                    BaseType::TY_BOOL});
  }
  /** `class` and `enum`: compared by nominal identity in \c isEqual (not structural shape). */
  [[nodiscard]] auto isNominal() const -> bool {
    return isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM});
  }
  [[nodiscard]] auto isFloatingPoint() const -> bool {
    return baseType == BaseType::TY_FLOAT || baseType == BaseType::TY_FLOAT32;
  }
  [[nodiscard]] auto isOneOf(const std::vector<BaseType>& baseTypes) const -> bool {
    return std::ranges::any_of(baseTypes,
                               [this](BaseType type) { return type == this->baseType; });
  }
  [[nodiscard]] auto getBaseType() const -> BaseType { return baseType; }
  [[nodiscard]] auto getElementType() const -> Type* { return elementType; }
  [[nodiscard]] auto getReturnType() const -> Type* { return returnType; }
  [[nodiscard]] auto getLlvmType() const -> llvm::Type* { return llvmType; }
  [[nodiscard]] auto getGenericName() const -> std::string { return genericName; }
  [[nodiscard]] auto getDisplayName() const -> const std::string& { return displayName; }
  /** Declared generic parameter names in order (for class/function types). */
  [[nodiscard]] auto getGenericParams() const -> const std::vector<std::string>& {
    return genericParams;
  }
  [[nodiscard]] auto getDeclarationSpan() const -> llvm::SMRange { return declarationSpan; }
  [[nodiscard]] auto getDeclarationFilePath() const -> const std::string& {
    return declarationFilePath;
  }
  [[nodiscard]] auto isVarArgs() const -> bool { return varArgs; }
  [[nodiscard]] auto isSigned() const -> bool { return signedInt; }
  /** Resolved integer width for TY_INT (defaults to 64 for plain `int`). */
  [[nodiscard]] auto getIntWidth() const -> unsigned {
    if (baseType != BaseType::TY_INT) {
      return 0U;
    }
    if (llvmType != nullptr && llvmType->isIntegerTy()) {
      return llvmType->getIntegerBitWidth();
    }
    if (intWidth != 0) {
      return intWidth;
    }
    return 64U;
  }
  auto setIntWidth(std::uint16_t width) -> void { intWidth = width; }
  auto setSigned(bool value) -> void { signedInt = value; }

  // Returns raw pointers for non-owning access
  [[nodiscard]] auto getFields() const -> std::vector<Field*> {
    std::vector<Field*> result;
    result.reserve(fields.size());
    for (const auto& field : fields) {
      result.push_back(field.get());
    }
    return result;
  }

  auto setLlvmType(llvm::Type* type) -> void { llvmType = type; }
  auto setBaseType(BaseType type) -> void { baseType = type; }
  auto setElementType(Type* type) -> void { elementType = type; }
  auto setReturnType(Type* type) -> void { returnType = type; }
  auto setGenericName(std::string name) -> void { genericName = std::move(name); }
  auto setDisplayName(std::string name) -> void { displayName = std::move(name); }
  auto setGenericParams(std::vector<std::string> params) -> void {
    genericParams = std::move(params);
  }
  [[nodiscard]] auto getGenericParamTraitBounds() const
      -> const std::vector<std::vector<std::string>>& {
    return genericParamTraitBounds;
  }
  auto setGenericParamTraitBounds(std::vector<std::vector<std::string>> bounds) -> void {
    genericParamTraitBounds = std::move(bounds);
  }
  [[nodiscard]] auto getImplTraitNames() const -> const std::vector<std::string>& {
    return implTraitNames;
  }
  auto setImplTraitNames(std::vector<std::string> names) -> void {
    implTraitNames = std::move(names);
  }
  [[nodiscard]] auto getClassSuperclass() const -> Type* { return classSuperclass; }
  auto setClassSuperclass(Type* super) -> void { classSuperclass = super; }
  [[nodiscard]] auto getClassHasDerivedClass() const -> bool { return classHasDerivedClass; }
  auto setClassHasDerivedClass(bool value) -> void { classHasDerivedClass = value; }
  [[nodiscard]] auto getClassVtableMethodOrder() const -> const std::vector<std::string>& {
    return classVtableMethodOrder;
  }
  auto setClassVtableMethodOrder(std::vector<std::string> order) -> void {
    classVtableMethodOrder = std::move(order);
  }
  auto setDeclarationSpan(llvm::SMRange span) -> void { declarationSpan = span; }
  auto setDeclarationFilePath(std::string path) -> void { declarationFilePath = std::move(path); }
  auto setVarArgs(bool value) -> void { varArgs = value; }
  auto addField(std::unique_ptr<Field> field) -> void { fields.push_back(std::move(field)); }
  /** Replace all fields (e.g. refresh a placeholder specialization after the template is complete).
   */
  auto replaceFields(std::vector<std::unique_ptr<Field>> newFields) -> void {
    fields = std::move(newFields);
  }

  auto isEqual(Type* rhs) const -> bool {
    std::set<std::pair<Type const*, Type const*>> active;
    return isEqualImpl(rhs, active);
  }

  /** When both sides are TY_FUNCTION: compares varargs, generic parameter names, and trait bounds.
   */
  [[nodiscard]] auto functionGenericSignatureEqual(Type const* rhs) const -> bool {
    if (rhs == nullptr || baseType != BaseType::TY_FUNCTION ||
        rhs->getBaseType() != BaseType::TY_FUNCTION) {
      return false;
    }
    if (varArgs != rhs->isVarArgs()) {
      return false;
    }
    const std::vector<std::string>& lp = getGenericParams();
    const std::vector<std::string>& rp = rhs->getGenericParams();
    if (lp.size() != rp.size()) {
      return false;
    }
    for (size_t i = 0; i < lp.size(); ++i) {
      if (lp[i] != rp[i]) {
        return false;
      }
    }
    const auto& lb = getGenericParamTraitBounds();
    const auto& rb = rhs->getGenericParamTraitBounds();
    if (lb.size() != rb.size()) {
      return false;
    }
    for (size_t i = 0; i < lb.size(); ++i) {
      if (lb[i] != rb[i]) {
        return false;
      }
    }
    return true;
  }

private:
  auto isEqualImpl(Type const* rhs, std::set<std::pair<Type const*, Type const*>>& active) const
      -> bool {
    if (rhs == nullptr) {
      return false;
    }
    if (this == rhs) {
      return true;
    }
    if (this->getBaseType() != rhs->getBaseType()) {
      return false;
    }

    const auto pairKey = std::pair<Type const*, Type const*>(this, rhs);
    if (!active.insert(pairKey).second) {
      return true;
    }
    struct ActiveGuard {
      std::set<std::pair<Type const*, Type const*>>* const s;
      std::pair<Type const*, Type const*> key;
      ActiveGuard(std::set<std::pair<Type const*, Type const*>>* setPtr,
                  std::pair<Type const*, Type const*> k)
          : s(setPtr), key(std::move(k)) {}
      ActiveGuard(const ActiveGuard&) = delete;
      auto operator=(const ActiveGuard&) -> ActiveGuard& = delete;
      ActiveGuard(ActiveGuard&&) = delete;
      auto operator=(ActiveGuard&&) -> ActiveGuard& = delete;
      ~ActiveGuard() { s->erase(key); }
    } guard{&active, pairKey};

    // Class/enum types: when both have LLVM types, compare by pointer identity
    // (or same non-empty displayName, then structure) for lowered/import
    // variants. When neither is lowered yet, require matching non-empty
    // displayName before structural comparison so distinct nominal types are
    // not equated by shape alone.
    if (isNominal()) {
      if (llvmType != nullptr && rhs->llvmType != nullptr) {
        if (llvmType == rhs->llvmType) {
          return true;
        }
        if (displayName == rhs->displayName && !displayName.empty()) {
          // Fall through to structural comparison for semantically identical
          // specializations materialized through different codegen/import paths.
        } else {
          return false;
        }
      } else if (llvmType == nullptr && rhs->llvmType == nullptr) {
        if (displayName != rhs->displayName || displayName.empty()) {
          return false;
        }
      }
      // Semantic identity: same nominal name (when pre-LLVM), same generic
      // params and fields.
      const std::vector<std::string>& lp = getGenericParams();
      const std::vector<std::string>& rp = rhs->getGenericParams();
      if (lp.size() != rp.size()) {
        return false;
      }
      for (size_t i = 0; i < lp.size(); ++i) {
        if (lp[i] != rp[i]) {
          return false;
        }
      }
      auto lf = getFields();
      auto rf = rhs->getFields();
      if (lf.size() != rf.size()) {
        return false;
      }
      for (size_t i = 0; i < lf.size(); ++i) {
        if (lf[i]->name != rf[i]->name) {
          return false;
        }
        Type* lt = lf[i]->type;
        Type* rt = rf[i]->type;
        if (lt == nullptr || rt == nullptr) {
          if (lt != rt) {
            return false;
          }
          continue;
        }
        if (!lt->isEqualImpl(rt, active)) {
          return false;
        }
      }
      return true;
    }

    switch (baseType) {
    case BaseType::TY_INT: {
      if (isSigned() != rhs->isSigned()) {
        return false;
      }
      return getIntWidth() == rhs->getIntWidth();
    }
    case BaseType::TY_FLOAT:
    case BaseType::TY_FLOAT32: {
      llvm::Type* l = getLlvmType();
      llvm::Type* r = rhs->getLlvmType();
      if (l == nullptr && r == nullptr) {
        return true;
      }
      if (l == nullptr || r == nullptr) {
        llvm::Type* concrete = (l != nullptr) ? l : r;
        return concrete != nullptr && concrete->isFloatingPointTy();
      }
      if (!l->isFloatingPointTy() || !r->isFloatingPointTy()) {
        return false;
      }
      return l == r;
    }
    case BaseType::TY_STRING:
    case BaseType::TY_BOOL:
    case BaseType::TY_VOID:
    case BaseType::TY_INVALID:
    case BaseType::TY_IMPORT:
      return true;
    case BaseType::TY_PTR:
    case BaseType::TY_ARRAY: {
      Type const* thisElementType = getElementType();
      Type* rhsElementType = rhs->getElementType();
      if (thisElementType == nullptr && rhsElementType == nullptr) {
        return true;
      }
      if (thisElementType == nullptr || rhsElementType == nullptr) {
        return false;
      }
      return thisElementType->isEqualImpl(rhsElementType, active);
    }
    case BaseType::TY_FUNCTION: {
      if (!functionGenericSignatureEqual(rhs)) {
        return false;
      }
      auto lf = getFields();
      auto rf = rhs->getFields();
      if (lf.size() != rf.size()) {
        return false;
      }
      for (size_t i = 0; i < lf.size(); ++i) {
        if (!lf[i]->type->isEqualImpl(rf[i]->type, active)) {
          return false;
        }
      }
      Type* lret = getReturnType();
      Type* rret = rhs->getReturnType();
      if (lret == nullptr && rret == nullptr) {
        return true;
      }
      if (lret == nullptr || rret == nullptr) {
        return false;
      }
      return lret->isEqualImpl(rret, active);
    }
    case BaseType::TY_GENERIC:
      return genericName == rhs->getGenericName();
    case BaseType::TY_TRAIT_EXISTENTIAL:
      return displayName == rhs->getDisplayName() && !displayName.empty();
    case BaseType::TY_TUPLE: {
      auto lf = getFields();
      auto rf = rhs->getFields();
      if (lf.size() != rf.size()) {
        return false;
      }
      for (size_t i = 0; i < lf.size(); ++i) {
        Type* lt = lf[i]->type;
        Type* rt = rf[i]->type;
        if (lt == nullptr || rt == nullptr) {
          if (lt != rt) {
            return false;
          }
          continue;
        }
        if (!lt->isEqualImpl(rt, active)) {
          return false;
        }
      }
      return true;
    }
    case BaseType::TY_CLASS:
    case BaseType::TY_ENUM:
      // Handled above; unreachable but required for switch completeness.
      return llvmType != nullptr && rhs->llvmType != nullptr && llvmType == rhs->llvmType;
    }
    return false;
  }

public:
  [[nodiscard]] auto toString() const -> std::string {
    std::string result;

    switch (baseType) {
    case BaseType::TY_INVALID:
      result = "Invalid";
      break;
    case BaseType::TY_INT: {
      const unsigned w = getIntWidth();
      if (w == 64U) {
        result = signedInt ? "int" : "uint";
      } else {
        result = signedInt ? ("int" + std::to_string(w)) : ("uint" + std::to_string(w));
      }
      break;
    }
    case BaseType::TY_FLOAT:
      result = "float";
      break;
    case BaseType::TY_FLOAT32:
      result = "float32";
      break;
    case BaseType::TY_STRING:
      result = "cstr";
      break;
    case BaseType::TY_BOOL:
      result = "bool";
      break;
    case BaseType::TY_PTR:
      result = elementType != nullptr ? "*" + elementType->toString() : "*";
      break;
    case BaseType::TY_ARRAY:
      result = displayName.empty() ? "list" : displayName;
      break;
    case BaseType::TY_VOID:
      result = "void";
      break;
    case BaseType::TY_FUNCTION:
      result = "Function";
      break;
    case BaseType::TY_GENERIC:
      result = genericName;
      break;
    case BaseType::TY_CLASS:
      result = displayName.empty() ? "Class" : displayName;
      break;
    case BaseType::TY_ENUM:
      result = displayName.empty() ? "Enum" : displayName;
      break;
    case BaseType::TY_IMPORT:
      result = "Import";
      break;
    case BaseType::TY_TRAIT_EXISTENTIAL:
      result = displayName.empty() ? "trait" : displayName;
      break;
    case BaseType::TY_TUPLE: {
      if (!displayName.empty()) {
        result = displayName;
        break;
      }
      result = "tuple<";
      auto fs = getFields();
      for (size_t i = 0; i < fs.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += fs[i]->type != nullptr ? fs[i]->type->toString() : "?";
      }
      result += ">";
      break;
    }
    }

    if (elementType != nullptr && baseType != BaseType::TY_PTR) {
      result += "<" + elementType->toString() + ">";
    }

    if (!fields.empty() && !isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM, BaseType::TY_TUPLE})) {
      result += baseType == BaseType::TY_FUNCTION ? " ( " : " { ";
      for (const auto& field : fields) {
        result += field->name + ": " + field->type->toString() + "; ";
      }
      result += baseType == BaseType::TY_FUNCTION ? ")" : "}";
    }

    if (returnType != nullptr) {
      result += " -> " + returnType->toString();
    }

    return result;
  }
};
} // namespace lesma
