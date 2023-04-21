#pragma once

#include <algorithm>
#include <llvm/IR/Type.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace lesma {
    enum BaseType {
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
    };

    class Type {
        BaseType baseType;
        llvm::Type *llvmType;

        Type *elementType;
        Type *returnType;
        std::vector<std::unique_ptr<Field>> fields;
        bool signedInt = true;

    public:
        explicit Type(BaseType baseType) : baseType(baseType), llvmType(nullptr), elementType(nullptr), returnType(nullptr), fields() {}
        explicit Type(BaseType baseType, llvm::Type *llvmType) : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr), fields() {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, Type *elementType) : baseType(baseType), llvmType(llvmType), elementType(elementType), returnType(nullptr), fields() {}
        explicit Type(BaseType baseType, llvm::Type *llvmType, std::vector<std::unique_ptr<Field>> fields) : baseType(baseType), llvmType(llvmType), elementType(nullptr), returnType(nullptr), fields(std::move(fields)) {}

        [[nodiscard]] bool is(BaseType type) const { return baseType == type; }
        [[nodiscard]] bool isPrimitive() const { return isOneOf({TY_INT, TY_FLOAT, TY_STRING, TY_BOOL}); }
        [[nodiscard]] bool isOneOf(const std::vector<BaseType> &baseTypes) const {
            return std::any_of(baseTypes.begin(), baseTypes.end(), [this](int type) { return type == this->baseType; });
        }
        [[nodiscard]] BaseType getBaseType() const { return baseType; }
        [[nodiscard]] Type *getElementType() const { return elementType; }
        [[nodiscard]] Type *getReturnType() const { return returnType; }
        [[nodiscard]] llvm::Type *getLLVMType() const { return llvmType; }
        [[nodiscard]] std::vector<std::unique_ptr<Field>> const &getFields() const { return fields; }
        [[nodiscard]] bool isSigned() const { return signedInt; }

        void setLLVMType(llvm::Type *type) { llvmType = type; }
        void setBaseType(BaseType type) { baseType = type; }
        void setElementType(lesma::Type *type) { elementType = type; }
        void setReturnType(lesma::Type *type) { returnType = type; }

        friend bool operator==(const Type &lhs, const Type &rhs) {
            return lhs.getBaseType() == rhs.getBaseType();
        }
        friend bool operator!=(const Type &lhs, const Type &rhs) {
            return !(lhs == rhs);
        }

        [[nodiscard]] std::string toString() const {
            std::string result;

            switch (baseType) {
                case TY_INVALID:
                    result = "Invalid";
                    break;
                case TY_INT:
                    result = "Int";
                    break;
                case TY_FLOAT:
                    result = "Float";
                    break;
                case TY_STRING:
                    result = "String";
                    break;
                case TY_BOOL:
                    result = "Bool";
                    break;
                case TY_PTR:
                    result = "Pointer";
                    break;
                case TY_ARRAY:
                    result = "Array";
                    break;
                case TY_VOID:
                    result = "Void";
                    break;
                case TY_FUNCTION:
                    result = "Function";
                    break;
                case TY_CLASS:
                    result = "Class";
                    break;
                case TY_ENUM:
                    result = "Enum";
                    break;
                case TY_IMPORT:
                    result = "Import";
                    break;
            }

            if (elementType) {
                result += "<" + elementType->toString() + ">";
            }

            if (!fields.empty()) {
                result += baseType == TY_FUNCTION ? " ( " : " { ";
                for (const auto &field: fields) {
                    result += field->name + ": " + field->type->toString() + "; ";
                }
                result += baseType == TY_FUNCTION ? ")" : "}";
            }

            if (returnType) {
                result += " -> " + returnType->toString();
            }

            return result;
        }
    };
}// namespace lesma