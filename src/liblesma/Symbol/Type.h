#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>

namespace lesma {
class Value;  // Forward declaration instead of include to break circular dependency
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
    TY_CLASS,
    TY_ENUM,
    TY_IMPORT,
};

class Type;

struct Field {
    std::string name;
    Type *type;
    std::unique_ptr<Value> defaultValue;

    // Constructor for fields without default value
    Field(std::string n, Type *t) : name(std::move(n)), type(t) {}

    // Constructor for fields with default value
    Field(std::string n, Type *t, std::unique_ptr<Value> defVal)
        : name(std::move(n)), type(t), defaultValue(std::move(defVal)) {}
};

class Type {
    BaseType baseType_;
    llvm::Type *llvmType_;

    // Non-owning references to other Types (owned elsewhere)
    Type *elementType_;
    Type *returnType_;
    // Owned collection of Fields
    std::vector<std::unique_ptr<Field>> fields_;
    bool signedInt_ = true;

public:
    explicit Type(BaseType baseType)
        : baseType_(baseType), llvmType_(nullptr), elementType_(nullptr), returnType_(nullptr) {}
    explicit Type(BaseType baseType, llvm::Type *llvmType)
        : baseType_(baseType), llvmType_(llvmType), elementType_(nullptr), returnType_(nullptr) {}
    explicit Type(BaseType baseType, llvm::Type *llvmType, Type *elementType)
        : baseType_(baseType), llvmType_(llvmType), elementType_(elementType), returnType_(nullptr) {}
    explicit Type(BaseType baseType, llvm::Type *llvmType, std::vector<std::unique_ptr<Field>> fields)
        : baseType_(baseType), llvmType_(llvmType), elementType_(nullptr), returnType_(nullptr),
          fields_(std::move(fields)) {}

    ~Type() = default;
    Type(const Type &) = delete;
    auto operator=(const Type &) -> Type & = delete;
    Type(Type &&) = default;
    auto operator=(Type &&) -> Type & = default;

    [[nodiscard]] auto is(BaseType type) const -> bool { return baseType_ == type; }
    [[nodiscard]] auto isPrimitive() const -> bool {
        return isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_STRING, BaseType::TY_BOOL});
    }
    [[nodiscard]] auto isOneOf(const std::vector<BaseType> &baseTypes) const -> bool {
        return std::any_of(baseTypes.begin(), baseTypes.end(),
                           [this](BaseType type) { return type == this->baseType_; });
    }
    [[nodiscard]] auto getBaseType() const -> BaseType { return baseType_; }
    [[nodiscard]] auto getElementType() const -> Type * { return elementType_; }
    [[nodiscard]] auto getReturnType() const -> Type * { return returnType_; }
    [[nodiscard]] auto getLLVMType() const -> llvm::Type * { return llvmType_; }
    [[nodiscard]] auto isSigned() const -> bool { return signedInt_; }

    // Returns raw pointers for non-owning access
    [[nodiscard]] auto getFields() const -> std::vector<Field *> {
        std::vector<Field *> result;
        result.reserve(fields_.size());
        for (const auto &field: fields_) { result.push_back(field.get()); }
        return result;
    }

    auto setLLVMType(llvm::Type *type) -> void { llvmType_ = type; }
    auto setBaseType(BaseType type) -> void { baseType_ = type; }
    auto setElementType(Type *type) -> void { elementType_ = type; }
    auto setReturnType(Type *type) -> void { returnType_ = type; }
    auto addField(std::unique_ptr<Field> field) -> void { fields_.push_back(std::move(field)); }

    auto isEqual(Type *rhs) const -> bool {
        if (rhs == nullptr) {
            return false;
        }

        if (this->getBaseType() != rhs->getBaseType()) {
            return false;
        }

        Type *thisElementType = this->getElementType();
        Type *rhsElementType = rhs->getElementType();

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

        switch (baseType_) {
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

        if (elementType_ != nullptr) {
            result += "<" + elementType_->toString() + ">";
        }

        if (!fields_.empty()) {
            result += baseType_ == BaseType::TY_FUNCTION ? " ( " : " { ";
            for (const auto &field: fields_) { result += field->name + ": " + field->type->toString() + "; "; }
            result += baseType_ == BaseType::TY_FUNCTION ? ")" : "}";
        }

        if (returnType_ != nullptr) {
            result += " -> " + returnType_->toString();
        }

        return result;
    }
};
}  // namespace lesma
