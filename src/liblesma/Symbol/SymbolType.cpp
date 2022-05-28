#include "SymbolType.h"

/**
 * Check if the current type is of a certain super type
 *
 * @param superType Super type to check for
 * @return Applicable or not
 */
bool SymbolType::is(SymbolSuperType superType) const { return getSuperType() == superType; }

/**
 * Check if the current type is one of the primitive types
 *
 * @return Primitive or not
 */
bool SymbolType::isPrimitive() const {
    return isOneOf({TY_INT, TY_FLOAT, TY_STRING, TY_BOOL});
}

/**
 * Check if the current type is amongst a collection of certain super types
 *
 * @param superTypes Vector of super types
 * @return Applicable or not
 */
bool SymbolType::isOneOf(const std::vector<SymbolSuperType> &superTypes) const {
    SymbolSuperType superType = getSuperType();
    return std::any_of(superTypes.begin(), superTypes.end(), [&superType](int type) { return type == superType; });
}

/**
 * Retrieve the super type of the current type
 *
 * @return Super type
 */
SymbolSuperType SymbolType::getSuperType() const { return baseSuperType; }

/**
 * Retrieve the super type of the current type
 *
 * @return Super type
 */
SymbolType *SymbolType::getElementType() const { return elementType; }

/**
 * Retrieve the fields of the current type
 *
 * @return Vector with tuples of name and SymbolType
 */
std::vector<std::tuple<std::string, SymbolType *>> SymbolType::getFields() const { return fields; }

bool operator==(const SymbolType &lhs, const SymbolType &rhs) { return lhs.baseSuperType == rhs.baseSuperType; }

bool operator!=(const SymbolType &lhs, const SymbolType &rhs) { return lhs.baseSuperType != rhs.baseSuperType; }