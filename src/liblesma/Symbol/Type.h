#pragma once

#include <algorithm>
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
        Type *elementType;
        std::vector<std::unique_ptr<Field>> fields;

    public:
        // Constructors
        explicit Type(BaseType superType) : baseType(superType), elementType(nullptr), fields() {}
        explicit Type(BaseType superType, std::vector<std::unique_ptr<Field>> fields, Type *elementType) : baseType(superType), elementType(elementType), fields(std::move(fields)) {}

        // Public methods
        [[nodiscard]] bool is(BaseType superType) const;
        [[nodiscard]] bool isPrimitive() const;
        [[nodiscard]] bool isOneOf(const std::vector<BaseType> &superTypes) const;
        [[nodiscard]] BaseType getBaseType() const;
        [[nodiscard]] Type *getElementType() const;
        [[nodiscard]] std::vector<std::unique_ptr<Field>> const &getFields() const;
        friend bool operator==(const Type &lhs, const Type &rhs);
        friend bool operator!=(const Type &lhs, const Type &rhs);
    };
}// namespace lesma