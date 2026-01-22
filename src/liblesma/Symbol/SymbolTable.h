#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Value.h"

#include "liblesma/Symbol/Type.h"

namespace lesma {
class SymbolTable {
public:
  explicit SymbolTable(SymbolTable* parent) : parent(parent) {};
  ~SymbolTable() = default;

  SymbolTable(const SymbolTable&) = delete;
  auto operator=(const SymbolTable&) -> SymbolTable& = delete;
  SymbolTable(SymbolTable&&) = default;
  auto operator=(SymbolTable&&) -> SymbolTable& = default;

  auto LookupFunction(const std::string& symbolName,
                      std::vector<lesma::Type*> paramTypes) -> Value*;
  auto Lookup(const std::string& name) -> Value*;
  auto LookupStruct(const std::string& name) -> Value*;
  auto LookupType(const std::string& symbolName) -> Type*;
  auto InsertSymbol(std::unique_ptr<Value> symbol) -> void;
  auto InsertType(const std::string& name, std::unique_ptr<Type> type) -> void;
  // For imported types - stores non-owning reference (caller must ensure Type
  // outlives SymbolTable)
  auto InsertTypeRef(const std::string& name, Type* type) -> void;
  auto CreateChildBlock(const std::string& blockName) -> SymbolTable*;
  auto GetParent() -> SymbolTable*;

  // Returns raw pointers for non-owning access
  [[nodiscard]] auto GetSymbols() -> std::vector<Value*> {
    std::vector<Value*> result;
    result.reserve(symbols.size());
    for (const auto& [key, val] : symbols) {
      result.push_back(val.get());
    }
    return result;
  }

  [[nodiscard]] auto GetTypes() -> std::vector<Type*> {
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

  auto GetChild(const std::string& scopeId) -> SymbolTable*;

  [[nodiscard]] auto ToString(int ind) -> std::string {
    std::string res;
    for (const auto& [key, symbol] : symbols) {
      res += std::string(ind, ' ') + "Symbols: \n";
      res += std::string(ind + 2, ' ') + symbol->ToString() + '\n';
    }
    for (const auto& [key, child] : children) {
      res += std::string(ind, ' ') + "Tables: \n";
      res += std::string(ind + 2, ' ') + key + ": \n";
      res += child->ToString(ind + 2) + '\n';
    }

    return res;
  }

private:
  SymbolTable* parent;
  std::unordered_map<std::string, std::unique_ptr<SymbolTable>> children;
  std::unordered_multimap<std::string, std::unique_ptr<Value>> symbols;
  std::unordered_map<std::string, std::unique_ptr<Type>> types;
  // Non-owning references to imported Types (must outlive this SymbolTable)
  std::unordered_map<std::string, Type*> typeRefs;
};
} // namespace lesma
