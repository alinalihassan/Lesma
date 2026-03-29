#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace lesma {
struct Field;
class Type;

namespace TypeUtils {
auto findIndexInFields(Type* structType, const std::string& field) -> int;
/** LLVM struct index for the \p logicalIndex-th class data field (after the vtable pointer). */
auto classDataFieldStructIndex(Type* classTy, unsigned logicalIndex) -> unsigned;
auto findTypeInFields(Type* structType, const std::string& field) -> Type*;
auto findFieldInFields(Type* structType, const std::string& field) -> Field*;
/** Class, enum, or array (buffer): types compared by `is` using display-name fallback when
 * `isEqual` is false. */
[[nodiscard]] auto isNominalTypeForIdentity(Type const* t) -> bool;
/** Return value is passed as a pointer (class instance, trait existential, or already a pointer).
 */
[[nodiscard]] auto passesByPointerInAbi(Type const* t) -> bool;
/** Stable specialization registry key shared by typecheck and codegen. */
auto makeSpecializedClassKey(Type* classTemplate, const std::vector<std::string>& genericParamNames,
                             const std::unordered_map<std::string, Type*>& env) -> std::string;
} // namespace TypeUtils
} // namespace lesma
