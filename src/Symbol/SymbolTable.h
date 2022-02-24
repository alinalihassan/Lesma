#pragma once

#include <map>
#include <utility>
#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>

#include "Common/Utils.h"

namespace lesma {
    class SymbolTableEntry { ;
    public:
        explicit SymbolTableEntry(std::string name, llvm::Value *value, llvm::Type *type) : name(std::move(name)),
                                                                                            value(value), type(type),
                                                                                            mutable_(false) {}
        explicit SymbolTableEntry(std::string name, llvm::Value *value, llvm::Type *type, bool mutable_) : name(std::move(name)),
                                                                                            value(value), type(type),
                                                                                            mutable_(mutable_) {}

        [[nodiscard]] std::string getName() { return name; }
        [[nodiscard]] llvm::Value *getValue() { return value; }
        [[nodiscard]] llvm::Type *getType() { return type; }
        [[nodiscard]] bool getMutability() const { return mutable_; }

        std::string toString() {
            std::string type_str, value_str;
            llvm::raw_string_ostream rso(type_str), rso2(value_str);
            type->print(rso);
            value->print(rso2);
            return name + ": " + type_str + " = " + value_str;
        }

    private:
        std::string name;
        llvm::Value *value;
        llvm::Type *type;
        bool mutable_;
    };

    class SymbolTable {
    public:
        explicit SymbolTable(SymbolTable *parent) : parent(parent) {};

        SymbolTableEntry *lookup(const std::string &symbolName);

        void insertSymbol(const std::string &name, llvm::Value *value, llvm::Type *type);
        void insertSymbol(const std::string &name, llvm::Value *value, llvm::Type *type, bool mutable_);

        SymbolTable *createChildBlock(const std::string& blockName);

        SymbolTable *getParent();

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
        std::map<std::string, SymbolTable*> children;
        std::map<std::string, SymbolTableEntry*> symbols;
    };
}