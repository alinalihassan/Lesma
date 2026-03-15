#pragma once

#include <string>

namespace lesma {
class Type;

namespace TypeUtils {
auto findIndexInFields(Type* structType, const std::string& field) -> int;
auto findTypeInFields(Type* structType, const std::string& field) -> Type*;
} // namespace TypeUtils
} // namespace lesma
