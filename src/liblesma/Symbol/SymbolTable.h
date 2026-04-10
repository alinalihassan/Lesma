#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Value.h"

#include "liblesma/Symbol/Type.h"

namespace lesma {

/** Controls how pointer-to-class parameters compare in overload lookup. */
enum class FunctionLookupKind : std::uint8_t {
  /// Allow *Derived to match a formal *Base (call sites and general resolution).
  VALUE,
  /// Require exact class type under pointers (registering / selecting an overload slot).
  OVERLOAD_IDENTITY,
};

/** Pick the best-matching overload from candidate function types (same rules as
 * SymbolTable::lookupFunction). Returns nullptr if none match. */
[[nodiscard]] auto selectBestFunctionTypeMatch(const std::vector<Type*>& candidateFunctionTypes,
                                               const std::vector<Type*>& paramTypes) -> Type*;

/** Like selectBestFunctionTypeMatch, but ignores the first formal parameter (implicit self).
 * Used when the receiver is a generic type parameter with trait bounds. */
[[nodiscard]] auto selectBestFunctionTypeMatchTail(const std::vector<Type*>& candidateFunctionTypes,
                                                   const std::vector<Type*>& paramTypesAfterSelf)
    -> Type*;

class SymbolTable {
public:
  explicit SymbolTable(SymbolTable* parent) : parent(parent) {};
  ~SymbolTable() = default;

  SymbolTable(const SymbolTable&) = delete;
  auto operator=(const SymbolTable&) -> SymbolTable& = delete;
  SymbolTable(SymbolTable&&) = default;
  auto operator=(SymbolTable&&) -> SymbolTable& = default;

  auto lookupFunction(const std::string& symbolName, std::vector<lesma::Type*> paramTypes,
                      FunctionLookupKind kind = FunctionLookupKind::VALUE,
                      Type* excludeFormalReceiverClass = nullptr,
                      Type* requiredDeclaredInClass = nullptr,
                      std::optional<size_t> requiredGenericArity = std::nullopt) -> Value*;
  /** Like \c lookupFunction(Value), but only considers overloads for which \p receiverMatches
   * returns true for the class type under the first (receiver) pointer parameter. Used for
   * `super.method(self, …)` so the subclass overload is not chosen via subtyping.
   * \p staticSuperclassType is the declared immediate superclass (e.g. \c GBase<int>); the first
   * argument \c *Derived still matches when \p Derived subclasses it. Optional \p seedGenericBindings
   * supplies class type args (e.g. \c T -> int) so trailing parameters match. */
  auto lookupSuperClassMethod(
      const std::string& symbolName, std::vector<lesma::Type*> paramTypes,
      const std::function<bool(Type* formalReceiverClass)>& receiverMatches,
      Type* staticSuperclassType,
      const std::unordered_map<std::string, Type*>* seedGenericBindings) -> Value*;
  auto lookup(const std::string& name) -> Value*;
  /** Like \c lookup, but returns the \c MODULE_SYMBOL with \c TY_IMPORT if present for \p name,
   *  even when \c lookup would prefer another binding (e.g. an imported variable alias). */
  auto lookupImportModuleSymbol(const std::string& name) -> Value*;
  /** Symbols in this table only (no parent); for lambda capture shadows vs outer names. */
  auto lookupShallow(const std::string& name) -> Value*;
  auto lookupStruct(const std::string& name) -> Value*;
  auto lookupType(const std::string& symbolName) -> Type*;
  auto insertSymbol(std::unique_ptr<Value> symbol) -> void;
  auto insertType(const std::string& name, std::unique_ptr<Type> type) -> void;
  // For imported types - stores non-owning reference (caller must ensure Type
  // outlives SymbolTable)
  auto insertTypeRef(const std::string& name, Type* type) -> void;
  auto createChildBlock(const std::string& blockName) -> SymbolTable*;
  auto getParent() -> SymbolTable*;

  // Returns raw pointers for non-owning access
  [[nodiscard]] auto getSymbols() -> std::vector<Value*> {
    std::vector<Value*> result;
    result.reserve(symbols.size());
    for (const auto& [key, val] : symbols) {
      result.push_back(val.get());
    }
    return result;
  }
  [[nodiscard]] auto getSymbols() const -> std::vector<const Value*> {
    std::vector<const Value*> result;
    result.reserve(symbols.size());
    for (const auto& [key, val] : symbols) {
      result.push_back(val.get());
    }
    return result;
  }

  [[nodiscard]] auto getTypes() -> std::vector<Type*> {
    std::vector<Type*> result;
    result.reserve(types.size() + typeRefs.size());
    for (const auto& [key, val] : types) {
      result.push_back(val.get());
    }
    for (const auto& [key, val] : typeRefs) {
      result.push_back(val);
    }
    return result;
  }
  [[nodiscard]] auto getTypes() const -> std::vector<const Type*> {
    std::vector<const Type*> result;
    result.reserve(types.size() + typeRefs.size());
    for (const auto& [key, val] : types) {
      result.push_back(val.get());
    }
    for (const auto& [key, val] : typeRefs) {
      result.push_back(val);
    }
    return result;
  }

  auto getChild(const std::string& scopeId) -> SymbolTable*;

  /** Move owning entries from \c types into \p dest and record non-owning \c typeRefs so
   * \c lookupType still works after ownership is unified elsewhere (e.g. Driver/Codegen). */
  auto releaseOwnedTypesInto(std::vector<std::unique_ptr<Type>>& dest) -> void;

  /** Deep-clone this scope and nested child scopes under \p newParent for generic specialization
   *  codegen. Fills \p oldToNew with template table pointer → clone pointer. Clears LLVM handles
   *  on cloned \c Value entries. Does not deep-copy owned \c Type objects (uses the same \c Type*
   *  as the template where applicable); copies \c typeRefs. */
  [[nodiscard]] auto cloneSubtreeForCodegen(SymbolTable* newParent,
                                            std::unordered_map<SymbolTable*, SymbolTable*>& oldToNew)
      -> std::unique_ptr<SymbolTable>;

  /** Take ownership of a subtree produced by \c cloneSubtreeForCodegen (naming like \c createChildBlock). */
  auto attachClonedChild(std::string const& blockName, std::unique_ptr<SymbolTable> child)
      -> SymbolTable*;

  /** Remap \c Value::bodyScope on this tree when it points at a template table listed in \p oldToNew. */
  auto remapValueBodyScopesForCodegenClone(std::unordered_map<SymbolTable*, SymbolTable*> const& oldToNew)
      -> void;

  [[nodiscard]] auto toString(int ind) -> std::string {
    std::string res;
    for (const auto& [key, symbol] : symbols) {
      res += std::string(ind, ' ') + "Symbols: \n";
      res += std::string(ind + 2, ' ') + symbol->toString() + '\n';
    }
    for (const auto& [key, child] : children) {
      res += std::string(ind, ' ') + "Tables: \n";
      res += std::string(ind + 2, ' ') + key + ": \n";
      res += child->toString(ind + 2) + '\n';
    }

    return res;
  }

private:
  friend auto selectBestFunctionTypeMatch(const std::vector<Type*>& candidateFunctionTypes,
                                          const std::vector<Type*>& paramTypes) -> Type*;
  friend auto selectBestFunctionTypeMatchTail(const std::vector<Type*>& candidateFunctionTypes,
                                              const std::vector<Type*>& paramTypesAfterSelf)
      -> Type*;

  static auto matchGenericParameter(Type* formalTy, Type* argTy,
                                    std::unordered_map<std::string, Type*>& genericBindings,
                                    FunctionLookupKind lookupKind) -> bool;
  static auto matchGenericParameter(
      Type* formalTy, Type* argTy, std::unordered_map<std::string, Type*>& genericBindings,
      FunctionLookupKind lookupKind, std::set<std::pair<Type*, Type*>>& activePairs) -> bool;
  [[nodiscard]] static auto matchGenericUnionAgainstUnion(
      const std::vector<Type*>& formalMembers, size_t formalIndex, const std::vector<Type*>& argMembers,
      std::vector<bool>& usedArg, std::unordered_map<std::string, Type*> trialBindings,
      std::unordered_map<std::string, Type*>& genericBindings, FunctionLookupKind lookupKind,
      std::set<std::pair<Type*, Type*>>& activePairs) -> bool;

  SymbolTable* parent;
  /** Owning types and symbols: destroy symbols before types (Values may point into \c types). */
  std::unordered_map<std::string, std::unique_ptr<Type>> types;
  std::unordered_multimap<std::string, std::unique_ptr<Value>> symbols;
  // Non-owning references to imported Types (must outlive this SymbolTable)
  std::unordered_map<std::string, Type*> typeRefs;
  /** Child scopes last so they are destroyed before this table's types/symbols. */
  std::unordered_map<std::string, std::unique_ptr<SymbolTable>> children;
};
} // namespace lesma
