#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Value.h"

#include "liblesma/Symbol/Type.h"

namespace lesma {
    class SymbolTable {
    public:
        explicit SymbolTable(SymbolTable *parent) : parent_(parent) {};
        ~SymbolTable() noexcept {
            for (auto const &[key, val]: children_) {
                delete val;
            }
            //            for (auto const &[key, val]: symbols)
            //                delete val;
            // TODO: Check if we have leaks for SymbolTypes, it crashes import_to_std because of class import
            // for (auto const &[key, val]: types)
            //      delete val;
        }

        SymbolTable(const SymbolTable &) = delete;
        SymbolTable &operator=(const SymbolTable &) = delete;
        SymbolTable(SymbolTable &&) = default;
        SymbolTable &operator=(SymbolTable &&) = default;

        Value *lookupFunction(const std::string &symbolName, std::vector<lesma::Type *> paramTypes);
        Value *lookup(const std::string &name);
        Value *lookupStruct(const std::string &name);
        Type *lookupType(const std::string &symbolName);
        void insertSymbol(Value *symbol);
        void insertType(const std::string &name, Type *type);
        SymbolTable *createChildBlock(const std::string &blockName);
        SymbolTable *getParent();
        std::unordered_multimap<std::string, Value *> getSymbols() { return symbols_; }
        std::unordered_map<std::string, Type *> getTypes() { return types_; }

        SymbolTable *getChild(const std::string &scopeId);

        std::string toString(int ind) {
            std::string res;
            for (auto symbol: symbols_) {
                res += std::string(ind, ' ') + "Symbols: \n";
                res += std::string(ind + 2, ' ') + symbol.second->toString() + '\n';
            }
            for (auto child: children_) {
                res += std::string(ind, ' ') + "Tables: \n";
                res += std::string(ind + 2, ' ') + child.first + ": \n";
                res += child.second->toString(ind + 2) + '\n';
            }

            return res;
        }

    private:
        SymbolTable *parent_;
        std::unordered_map<std::string, SymbolTable *> children_;
        std::unordered_multimap<std::string, Value *> symbols_;
        std::unordered_map<std::string, Type *> types_;
    };
}// namespace lesma