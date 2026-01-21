#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>

namespace lesma {
    class Value;// Forward declaration instead of include to break circular dependency
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
    class Value;

    struct Field {
        std::string name;
        Type *type;
        Value *defaultValue = nullptr;
    };

    class Type {
        BaseType baseType_;
        llvm::Type *llvmType_;

        Type *elementType_;
        Type *returnType_;
        std::vector<Field *> fields_;
        bool signedInt_ = true;

    public:
        explicit Type(BaseType baseType) : baseType_(baseType), llvmType_(nullptr), elementType_(nullptr), returnType_(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType) : baseType_(baseType), llvmType_(llvmType), elementType_(nullptr), returnType_(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, Type *elementType) : baseType_(baseType), llvmType_(llvmType), elementType_(elementType), returnType_(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, std::vector<Field *> fields) : baseType_(baseType), llvmType_(llvmType), elementType_(nullptr), returnType_(nullptr), fields_(std::move(fields)) {}

        [[nodiscard]] bool is(BaseType type) const { return baseType_ == type; }
        [[nodiscard]] bool isPrimitive() const { return isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_STRING, BaseType::TY_BOOL}); }
        [[nodiscard]] bool isOneOf(const std::vector<BaseType> &baseTypes) const {
            return std::any_of(baseTypes.begin(), baseTypes.end(), [this](BaseType type) { return type == this->baseType_; });
        }
        [[nodiscard]] BaseType getBaseType() const { return baseType_; }
        [[nodiscard]] Type *getElementType() const { return elementType_; }
        [[nodiscard]] Type *getReturnType() const { return returnType_; }
        [[nodiscard]] llvm::Type *getLLVMType() const { return llvmType_; }
        [[nodiscard]] std::vector<Field *> const &getFields() const { return fields_; }
        [[nodiscard]] bool isSigned() const { return signedInt_; }

        void setLLVMType(llvm::Type *type) { llvmType_ = type; }
        void setBaseType(BaseType type) { baseType_ = type; }
        void setElementType(lesma::Type *type) { elementType_ = type; }
        void setReturnType(lesma::Type *type) { returnType_ = type; }

        bool isEqual(Type *rhs) const {
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

        [[nodiscard]] std::string toString() const {
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
                for (const auto &field: fields_) {
                    result += field->name + ": " + field->type->toString() + "; ";
                }
                result += baseType_ == BaseType::TY_FUNCTION ? ")" : "}";
            }

            if (returnType_ != nullptr) {
                result += " -> " + returnType_->toString();
            }

            return result;
        }
    };
}// namespace lesma