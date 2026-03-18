#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>

namespace lesma {
class Value; // Forward declaration instead of include to break circular
             // dependency
enum class BaseType : std::uint8_t {
  TY_INVALID,
  TY_INT,
  TY_FLOAT,
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
};

class Type;

struct Field {
  std::string name;
  Type* type;
  std::unique_ptr<Value> defaultValue;

  // Constructor for fields without default value
  Field(std::string n, Type* t);

  // Constructor for fields with default value
  Field(std::string n, Type* t, std::unique_ptr<Value> defVal);

  ~Field();
  Field(Field&&) noexcept;
  auto operator=(Field&&) noexcept -> Field&;
  Field(const Field&) = delete;
  auto operator=(const Field&) -> Field& = delete;
};

class Type {
  BaseType baseType;
  llvm::Type* llvmType;

  // Non-owning references to other Types (owned elsewhere)
  Type* elementType;
  Type* returnType;
  std::string genericName;
  // Owned collection of Fields
  std::vector<std::unique_ptr<Field>> fields;
  bool signedInt = true;

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
    return isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_STRING, BaseType::TY_BOOL});
  }
  [[nodiscard]] auto isOneOf(const std::vector<BaseType>& baseTypes) const -> bool {
    return std::any_of(baseTypes.begin(), baseTypes.end(),
                       [this](BaseType type) -> bool { return type == this->baseType; });
  }
  [[nodiscard]] auto getBaseType() const -> BaseType { return baseType; }
  [[nodiscard]] auto getElementType() const -> Type* { return elementType; }
  [[nodiscard]] auto getReturnType() const -> Type* { return returnType; }
  [[nodiscard]] auto getLlvmType() const -> llvm::Type* { return llvmType; }
  [[nodiscard]] auto getGenericName() const -> std::string { return genericName; }
  [[nodiscard]] auto isSigned() const -> bool { return signedInt; }

  // Returns raw pointers for non-owning access
  [[nodiscard]] auto getFields() -> std::vector<Field*> {
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
  auto addField(std::unique_ptr<Field> field) -> void { fields.push_back(std::move(field)); }

  auto isEqual(Type* rhs) const -> bool {
    if (rhs == nullptr) {
      return false;
    }
    if (this == rhs) {
      return true;
    }

    if (this->getBaseType() != rhs->getBaseType()) {
      return false;
    }

    // Class/enum types: compare by LLVM type identity only; do not fall through
    // to element-type logic when either llvmType is null (would incorrectly
    // return true for distinct types that both have null elementType).
    if (isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
      if (llvmType != nullptr && rhs->llvmType != nullptr) {
        return llvmType == rhs->llvmType;
      }
      return false;
    }

    Type const* thisElementType = this->getElementType();
    Type* rhsElementType = rhs->getElementType();

    if (thisElementType == nullptr && rhsElementType == nullptr) {
      return true;
    }
    if (thisElementType == nullptr || rhsElementType == nullptr) {
      return false;
    }

    return thisElementType->isEqual(rhsElementType);
  }

  [[nodiscard]] auto toString() const -> std::string {
    std::string result;

    switch (baseType) {
    case BaseType::TY_INVALID:
      result = "Invalid";
      break;
    case BaseType::TY_INT:
      result = "Int";
      break;
    case BaseType::TY_FLOAT:
      result = "Float";
      break;
    case BaseType::TY_STRING:
      result = "String";
      break;
    case BaseType::TY_BOOL:
      result = "Bool";
      break;
    case BaseType::TY_PTR:
      result = "Pointer";
      break;
    case BaseType::TY_ARRAY:
      result = "Array";
      break;
    case BaseType::TY_VOID:
      result = "Void";
      break;
    case BaseType::TY_FUNCTION:
      result = "Function";
      break;
    case BaseType::TY_GENERIC:
      result = genericName;
      break;
    case BaseType::TY_CLASS:
      result = "Class";
      break;
    case BaseType::TY_ENUM:
      result = "Enum";
      break;
    case BaseType::TY_IMPORT:
      result = "Import";
      break;
    }

    if (elementType != nullptr) {
      result += "<" + elementType->toString() + ">";
    }

    if (!fields.empty()) {
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
