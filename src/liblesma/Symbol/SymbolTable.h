#pragma once

#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <map>
#include <utility>

#include "Value.h"
#include "liblesma/Common/Utils.h"

namespace lesma {
    class SymbolTable {
    public:
        explicit SymbolTable(SymbolTable *parent) : parent(parent){};
        ~SymbolTable() {
            for (auto const &[key, val]: children)
                delete val;
            for (auto const &[key, val]: symbols)
                delete val;
            // TODO: Check if we have leaks for SymbolTypes, it crashes import_to_std because of class import
            // for (auto const &[key, val]: types)
            //     delete val;
        }

        Value *lookup(const std::string &symbolName);
        Value *lookupStructByName(const std::string &name);
        Type *lookupType(const std::string &symbolName);
        void insertSymbol(Value *symbol);
        void insertType(const std::string &name, Type *type);
        SymbolTable *createChildBlock(const std::string &blockName);
        SymbolTable *getParent();
        std::map<std::string, Value *> getSymbols() { return symbols; }
        std::map<std::string, Type *> getTypes() { return types; }

        SymbolTable *getChild(const std::string &tableName);

        std::string toString(int ind) {
            std::string res;
            for (auto symbol: symbols) {
                res += std::string(ind, ' ') + "Symbols: \n";
                res += std::string(ind + 2, ' ') + symbol.second->toString() + '\n';
            }
            for (auto child: children) {
                res += std::string(ind, ' ') + "Tables: \n";
                res += std::string(ind + 2, ' ') + child.first + ": \n";
                res += child.second->toString(ind + 2) + '\n';
            }

            return res;
        }

    private:
        SymbolTable *parent;
        std::map<std::string, SymbolTable *> children;
        std::map<std::string, Value *> symbols;
        std::map<std::string, Type *> types;
    };
}// namespace lesma