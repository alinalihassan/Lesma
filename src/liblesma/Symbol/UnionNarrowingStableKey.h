#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

#include "llvm/Support/SMLoc.h"

#include "liblesma/Symbol/Value.h"

namespace lesma {

/** Stable identity for union narrowing maps: declaration site after resolving lambda capture
 *  shadows, or the anchored \c Value* when no declaration span is recorded. */
struct UnionNarrowingStableKey {
  std::string_view declarationFilePath;
  llvm::SMRange declarationSpan;
  const Value* fallbackAnchor = nullptr;

  [[nodiscard]] friend bool operator==(UnionNarrowingStableKey a,
                                       UnionNarrowingStableKey b) noexcept {
    const bool aLoc = a.declarationSpan.isValid();
    const bool bLoc = b.declarationSpan.isValid();
    if (aLoc && bLoc) {
      return a.declarationFilePath == b.declarationFilePath &&
             a.declarationSpan.Start == b.declarationSpan.Start &&
             a.declarationSpan.End == b.declarationSpan.End;
    }
    if (!aLoc && !bLoc) {
      return a.fallbackAnchor == b.fallbackAnchor;
    }
    return false;
  }
};

struct UnionNarrowingStableKeyHash {
  [[nodiscard]] auto operator()(UnionNarrowingStableKey k) const noexcept -> std::size_t {
    if (k.declarationSpan.isValid()) {
      std::size_t h = std::hash<std::string_view>{}(k.declarationFilePath);
      h ^= std::hash<const void*>{}(k.declarationSpan.Start.getPointer()) + 0x9e3779b9U +
           (h << 6U) + (h >> 2U);
      h ^= std::hash<const void*>{}(k.declarationSpan.End.getPointer()) + 0x9e3779b9U + (h << 6U) +
           (h >> 2U);
      return h;
    }
    return std::hash<const void*>{}(k.fallbackAnchor);
  }
};

struct UnionNarrowingStableKeyEq {
  [[nodiscard]] auto operator()(UnionNarrowingStableKey a, UnionNarrowingStableKey b) const noexcept
      -> bool {
    return a == b;
  }
};

[[nodiscard]] inline auto unionNarrowingStableKeyForSymbol(Value* sym) -> UnionNarrowingStableKey {
  Value* anchor = sym;
  while (anchor != nullptr && anchor->getClosureSlotOuter() != nullptr) {
    anchor = anchor->getClosureSlotOuter();
  }
  if (anchor == nullptr) {
    return {std::string_view{}, llvm::SMRange(), nullptr};
  }
  llvm::SMRange span = anchor->getDeclarationSpan();
  if (span.isValid()) {
    return {std::string_view(anchor->getDeclarationFilePath()), span, nullptr};
  }
  return {std::string_view{}, llvm::SMRange(), anchor};
}

} // namespace lesma
