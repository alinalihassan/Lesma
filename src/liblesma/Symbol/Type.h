#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>
#include <llvm/Support/SMLoc.h>

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
  std::string genericName;
  std::string displayName;
  /** Declared generic parameter names in order (for TY_CLASS and TY_FUNCTION). */
  std::vector<std::string> genericParams;
  // Owned collection of Fields
  std::vector<std::unique_ptr<Field>> fields;
  llvm::SMRange declarationSpan;
  std::string declarationFilePath;
  bool varArgs = false;
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
  auto setDeclarationSpan(llvm::SMRange span) -> void { declarationSpan = span; }
  auto setDeclarationFilePath(std::string path) -> void { declarationFilePath = std::move(path); }
  auto setVarArgs(bool value) -> void { varArgs = value; }
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

    // Class/enum types: when both have LLVM types, compare by pointer identity;
    // otherwise compare by structure (genericParams + fields) so that types are
    // equal before LLVM lowering.
    if (isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
      if (llvmType != nullptr && rhs->llvmType != nullptr) {
        return llvmType == rhs->llvmType;
      }
      // Semantic identity when llvmType not yet set: same generic params and fields.
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
        // Cycle check: same (this, rhs) pair avoids infinite recursion (e.g. class with *Self).
        if ((lt == this && rt == rhs) || (lt == rhs && rt == this)) {
          continue;
        }
        if (!lt->isEqual(rt)) {
          return false;
        }
      }
      return true;
    }

    switch (baseType) {
    case BaseType::TY_INT: {
      llvm::Type* l = getLlvmType();
      llvm::Type* r = rhs->getLlvmType();
      if (l == nullptr && r == nullptr) {
        return isSigned() == rhs->isSigned();
      }
      if (l == nullptr || r == nullptr) {
        llvm::Type* concrete = (l != nullptr) ? l : r;
        return concrete != nullptr && concrete->isIntegerTy() && isSigned() == rhs->isSigned();
      }
      if (!l->isIntegerTy() || !r->isIntegerTy()) {
        return false;
      }
      return isSigned() == rhs->isSigned() && l->getIntegerBitWidth() == r->getIntegerBitWidth();
    }
    case BaseType::TY_FLOAT: {
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
      return thisElementType->isEqual(rhsElementType);
    }
    case BaseType::TY_FUNCTION: {
      if (varArgs != rhs->isVarArgs()) {
        return false;
      }
      auto lf = getFields();
      auto rf = rhs->getFields();
      if (lf.size() != rf.size()) {
        return false;
      }
      for (size_t i = 0; i < lf.size(); ++i) {
        if (!lf[i]->type->isEqual(rf[i]->type)) {
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
      return lret->isEqual(rret);
    }
    case BaseType::TY_GENERIC:
      return genericName == rhs->getGenericName();
    case BaseType::TY_CLASS:
    case BaseType::TY_ENUM:
      // Handled above; unreachable but required for switch completeness.
      return llvmType != nullptr && rhs->llvmType != nullptr && llvmType == rhs->llvmType;
    }
    return false;
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
      result = displayName.empty() ? "Class" : displayName;
      break;
    case BaseType::TY_ENUM:
      result = displayName.empty() ? "Enum" : displayName;
      break;
    case BaseType::TY_IMPORT:
      result = "Import";
      break;
    }

    if (elementType != nullptr) {
      result += "<" + elementType->toString() + ">";
    }

    if (!fields.empty() && !isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
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
