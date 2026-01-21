#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>

#include "liblesma/Symbol/Value.h"


namespace lesma {
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
        BaseType baseType;
        llvm::Type *llvmType;

        Type *elementType;
        Type *returnType;
        std::vector<Field *> fields;
        bool signedInt = true;

    public:
        explicit Type(BaseType baseType) : baseType(baseType), llvmType(nullptr), elementType(nullptr), returnType(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType) : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, Type *elementType) : baseType(baseType), llvmType(llvmType), elementType(elementType), returnType(nullptr) {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, std::vector<Field *> fields) : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr), fields(std::move(fields)) {}

        [[nodiscard]] bool is(BaseType type) const { return baseType == type; }
        [[nodiscard]] bool isPrimitive() const { return isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_STRING, BaseType::TY_BOOL}); }
        [[nodiscard]] bool isOneOf(const std::vector<BaseType> &baseTypes) const {
            return std::any_of(baseTypes.begin(), baseTypes.end(), [this](BaseType type) { return type == this->baseType; });
        }
        [[nodiscard]] BaseType getBaseType() const { return baseType; }
        [[nodiscard]] Type *getElementType() const { return elementType; }
        [[nodiscard]] Type *getReturnType() const { return returnType; }
        [[nodiscard]] llvm::Type *getLLVMType() const { return llvmType; }
        [[nodiscard]] std::vector<Field *> const &getFields() const { return fields; }
        [[nodiscard]] bool isSigned() const { return signedInt; }

        void setLLVMType(llvm::Type *type) { llvmType = type; }
        void setBaseType(BaseType type) { baseType = type; }
        void setElementType(lesma::Type *type) { elementType = type; }
        void setReturnType(lesma::Type *type) { returnType = type; }

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
                for (const auto &field: fields) {
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
}// namespace lesma