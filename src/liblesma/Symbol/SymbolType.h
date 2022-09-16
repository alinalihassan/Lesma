#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum SymbolSuperType {
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

class SymbolType;

struct Field {
    std::string name;
    SymbolType *type;
};

class SymbolType {
    SymbolSuperType baseSuperType;
    SymbolType *elementType;
    std::vector<std::unique_ptr<Field>> fields;

public:
    // Constructors
    explicit SymbolType(SymbolSuperType superType) : baseSuperType(superType), elementType(nullptr), fields() {}
    explicit SymbolType(SymbolSuperType superType, std::vector<std::unique_ptr<Field>> fields, SymbolType *elementType) : baseSuperType(superType), elementType(elementType), fields(std::move(fields)) {}

    // Public methods
    [[nodiscard]] bool is(SymbolSuperType superType) const;
    [[nodiscard]] bool isPrimitive() const;
    [[nodiscard]] bool isOneOf(const std::vector<SymbolSuperType> &superTypes) const;
    [[nodiscard]] SymbolSuperType getSuperType() const;
    [[nodiscard]] SymbolType *getElementType() const;
    [[nodiscard]] std::vector<std::unique_ptr<Field>> const &getFields() const;
    friend bool operator==(const SymbolType &lhs, const SymbolType &rhs);
    friend bool operator!=(const SymbolType &lhs, const SymbolType &rhs);
};
