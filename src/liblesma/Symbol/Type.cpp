#include "Type.h"

using namespace lesma;

/**
 * Check if the current type is of a certain super type
 *
 * @param superType Super type to check for
 * @return Applicable or not
 */
bool Type::is(BaseType superType) const { return getBaseType() == superType; }

/**
 * Check if the current type is one of the primitive types
 *
 * @return Primitive or not
 */
bool Type::isPrimitive() const {
    return isOneOf({TY_INT, TY_FLOAT, TY_STRING, TY_BOOL});
}

/**
 * Check if the current type is amongst a collection of certain super types
 *
 * @param superTypes Vector of super types
 * @return Applicable or not
 */
bool Type::isOneOf(const std::vector<BaseType> &superTypes) const {
    BaseType superType = getBaseType();
    return std::any_of(superTypes.begin(), superTypes.end(), [&superType](int type) { return type == superType; });
}

/**
 * Retrieve the super type of the current type
 *
 * @return Super type
 */
BaseType Type::getBaseType() const { return baseType; }

/**
 * Retrieve the super type of the current type
 *
 * @return Super type
 */
Type *Type::getElementType() const { return elementType; }

/**
 * Retrieve the fields of the current type
 *
 * @return Vector with tuples of name and Type
 */
std::vector<std::unique_ptr<Field>> const &Type::getFields() const { return fields; }

bool operator==(const Type &lhs, const Type &rhs) { return lhs.getBaseType() == rhs.getBaseType(); }

bool operator!=(const Type &lhs, const Type &rhs) { return lhs.getBaseType() != rhs.getBaseType(); }