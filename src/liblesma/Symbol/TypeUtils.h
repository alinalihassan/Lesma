#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lesma {
struct Field;
class Type;

namespace TypeUtils {
struct OptionalPayloadMembers {
  std::vector<Type*> members;
  std::string displayName;
};

auto findIndexInFields(Type* structType, const std::string& field) -> int;
/** LLVM struct index for the \p logicalIndex-th class data field (after the vtable pointer). */
auto classDataFieldStructIndex(Type* classTy, unsigned logicalIndex) -> unsigned;
auto findTypeInFields(Type* structType, const std::string& field) -> Type*;
auto findFieldInFields(Type* structType, const std::string& field) -> Field*;
/** Class static `let`/`var` only (not instance fields). */
auto findStaticFieldInClass(Type* classType, const std::string& field) -> Field*;
/** Class, enum, or array (buffer): types compared by `is` using display-name fallback when
 * `isEqual` is false. */
[[nodiscard]] auto isNominalTypeForIdentity(Type const* t) -> bool;
/** Return value is passed as a pointer (class instance, trait existential, or already a pointer).
 */
[[nodiscard]] auto passesByPointerInAbi(Type const* t) -> bool;
/** Stable specialization registry key shared by typecheck and codegen. */
auto makeSpecializedClassKey(Type* classTemplate, const std::vector<std::string>& genericParamNames,
                             const std::unordered_map<std::string, Type*>& env) -> std::string;
/** Flatten nested \c TY_UNION arms (null arms skipped, matching prior substitution behavior),
 *  dedupe with \c Type::isEqual, sort by \c toString(), and build the `a | b` display string.
 *  Used everywhere \c setUnionMembers runs so mangling and specialization caches stay stable. */
[[nodiscard]] auto canonicalizeUnionMembers(std::vector<Type*> arms)
    -> std::pair<std::vector<Type*>, std::string>;
/** If \p type is an optional union (`T | null` or wider), return its canonical non-null payload
 *  members and display name; otherwise return \c std::nullopt. */
[[nodiscard]] auto computeOptionalPayloadMembers(Type* type) -> std::optional<OptionalPayloadMembers>;
} // namespace TypeUtils
} // namespace lesma
