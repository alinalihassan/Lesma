#pragma once

#include <string>

namespace lesma {
struct Field;
class Type;

namespace TypeUtils {
auto findIndexInFields(Type* structType, const std::string& field) -> int;
auto findTypeInFields(Type* structType, const std::string& field) -> Type*;
auto findFieldInFields(Type* structType, const std::string& field) -> Field*;
} // namespace TypeUtils
} // namespace lesma
