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
    explicit SymbolTable(SymbolTable *parent) : parent_(parent) {};
    ~SymbolTable() = default;

    SymbolTable(const SymbolTable &) = delete;
    auto operator=(const SymbolTable &) -> SymbolTable & = delete;
    SymbolTable(SymbolTable &&) = default;
    auto operator=(SymbolTable &&) -> SymbolTable & = default;

    auto lookupFunction(const std::string &symbolName, std::vector<lesma::Type *> paramTypes) -> Value *;
    auto lookup(const std::string &name) -> Value *;
    auto lookupStruct(const std::string &name) -> Value *;
    auto lookupType(const std::string &symbolName) -> Type *;
    auto insertSymbol(std::unique_ptr<Value> symbol) -> void;
    auto insertType(const std::string &name, std::unique_ptr<Type> type) -> void;
    // For imported types - stores non-owning reference (caller must ensure Type outlives SymbolTable)
    auto insertTypeRef(const std::string &name, Type *type) -> void;
    auto createChildBlock(const std::string &blockName) -> SymbolTable *;
    auto getParent() -> SymbolTable *;

    // Returns raw pointers for non-owning access
    auto getSymbols() -> std::vector<Value *> {
        std::vector<Value *> result;
        result.reserve(symbols_.size());
        for (const auto &[key, val]: symbols_) { result.push_back(val.get()); }
        return result;
    }

    auto getTypes() -> std::vector<Type *> {
        std::vector<Type *> result;
        result.reserve(types_.size() + typeRefs_.size());
        for (const auto &[key, val]: types_) { result.push_back(val.get()); }
        for (const auto &[key, val]: typeRefs_) { result.push_back(val); }
        return result;
    }

    auto getChild(const std::string &scopeId) -> SymbolTable *;

    auto toString(int ind) -> std::string {
        std::string res;
        for (const auto &[key, symbol]: symbols_) {
            res += std::string(ind, ' ') + "Symbols: \n";
            res += std::string(ind + 2, ' ') + symbol->toString() + '\n';
        }
        for (const auto &[key, child]: children_) {
            res += std::string(ind, ' ') + "Tables: \n";
            res += std::string(ind + 2, ' ') + key + ": \n";
            res += child->toString(ind + 2) + '\n';
        }

        return res;
    }

private:
    SymbolTable *parent_;
    std::unordered_map<std::string, std::unique_ptr<SymbolTable>> children_;
    std::unordered_multimap<std::string, std::unique_ptr<Value>> symbols_;
    std::unordered_map<std::string, std::unique_ptr<Type>> types_;
    // Non-owning references to imported Types (must outlive this SymbolTable)
    std::unordered_map<std::string, Type *> typeRefs_;
};
}  // namespace lesma
