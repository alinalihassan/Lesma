#pragma once

#include <algorithm>
#include <map>
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

class SymbolType {
    SymbolSuperType baseSuperType;
    SymbolType *elementType;
    std::vector<std::tuple<std::string, SymbolType *>> fields;

public:
    // Constructors
    explicit SymbolType(SymbolSuperType superType) : baseSuperType(superType), elementType(nullptr), fields({}) {}
    explicit SymbolType(SymbolSuperType superType, std::vector<std::tuple<std::string, SymbolType *>> fields, SymbolType *elementType) : baseSuperType(superType), elementType(elementType), fields(std::move(fields)) {}

    // Public methods
    [[nodiscard]] bool is(SymbolSuperType superType) const;
    [[nodiscard]] bool isPrimitive() const;
    [[nodiscard]] bool isOneOf(const std::vector<SymbolSuperType> &superTypes) const;
    [[nodiscard]] SymbolSuperType getSuperType() const;
    [[nodiscard]] SymbolType *getElementType() const;
    [[nodiscard]] std::vector<std::tuple<std::string, SymbolType *>> getFields() const;
    friend bool operator==(const SymbolType &lhs, const SymbolType &rhs);
    friend bool operator!=(const SymbolType &lhs, const SymbolType &rhs);
};
