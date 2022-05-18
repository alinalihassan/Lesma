#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <map>

enum SymbolSuperType {
    TY_INVALID,
    TY_INT,
    TY_FLOAT,
    TY_STRING,
    TY_BOOL,
    TY_FUNCTION,
    TY_STRUCT,
    TY_ENUM,
    TY_VOID,
};

class SymbolType {
    SymbolSuperType baseSuperType;
    std::vector<std::tuple<std::string, SymbolType*>> fields;

public:
    // Constructors
    explicit SymbolType(SymbolSuperType superType) : baseSuperType(superType), fields({}) {}
    explicit SymbolType(SymbolSuperType superType, std::vector<std::tuple<std::string, SymbolType*>> fields) : baseSuperType(superType), fields(std::move(fields)) {}

    // Public methods
    [[nodiscard]] bool is(SymbolSuperType superType) const;
    [[nodiscard]] bool isPrimitive() const;
    [[nodiscard]] bool isOneOf(const std::vector<SymbolSuperType> &superTypes) const;
    [[nodiscard]] SymbolSuperType getSuperType() const;
    [[nodiscard]] std::vector<std::tuple<std::string, SymbolType*>> getFields() const;
    friend bool operator==(const SymbolType &lhs, const SymbolType &rhs);
    friend bool operator!=(const SymbolType &lhs, const SymbolType &rhs);
};
