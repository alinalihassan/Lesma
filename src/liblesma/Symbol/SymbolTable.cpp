#include "SymbolTable.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Casting.h>

#include "liblesma/Symbol/Type.h"

using namespace lesma;

/**
 * Insert a new symbol into the current symbol table. If it is a parameter, append its name to the paramNames vector
 *
 * @param symbol Symbol Table Entry (takes ownership)
 */
auto SymbolTable::insertSymbol(std::unique_ptr<Value> symbol) -> void {
    auto name = symbol->getName();
    symbols_.emplace(std::move(name), std::move(symbol));
}

/**
 * Insert a new type into the current symbol table.
 *
 * @param name Name of the type
 * @param type Type to insert (takes ownership)
 */
auto SymbolTable::insertType(const std::string &name, std::unique_ptr<Type> type) -> void {
    types_.insert_or_assign(name, std::move(type));
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupFunction(const std::string &name, std::vector<lesma::Type *> paramTypes) -> Value * {
    auto range = symbols_.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (!it->second->getType()->is(BaseType::TY_FUNCTION)) {
            continue;
        }

        // Check if the parameter types match
        bool paramsMatch = true;
        std::vector<Field *> funcParamTypes = it->second->getType()->getFields();
        size_t numParams = std::max(funcParamTypes.size(), paramTypes.size());

        for (size_t i = 0; i < numParams; ++i) {
            if (i < funcParamTypes.size() && i < paramTypes.size()) {
                if (!funcParamTypes[i]->type->isEqual(paramTypes[i])) {
                    paramsMatch = false;
                    break;
                }
            } else if (i < funcParamTypes.size() && funcParamTypes[i]->defaultValue != nullptr) {
                // Use default value for missing parameter
                paramTypes.push_back(funcParamTypes[i]->type);
            } else if (i >= funcParamTypes.size() && it->second->getType()->getLLVMType()->isFunctionVarArg()) {
                // Varargs
                break;
            } else {
                paramsMatch = false;
                break;
            }
        }

        if (!paramsMatch) {
            continue;// Parameter types don't match
        }

        return it->second.get();
    }

    if (parent_ == nullptr) {
        return nullptr;
    }

    return parent_->lookupFunction(name, paramTypes);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookup(const std::string &name) -> Value * {
    for (const auto &[key, sym]: symbols_) {
        if (key == name) {
            return sym.get();
        }
    }

    if (parent_ == nullptr) {
        return nullptr;
    }

    return parent_->lookup(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupStruct(const std::string &name) -> Value * {
    for (const auto &[key, sym]: symbols_) {
        if (sym->getType()->getLLVMType() != nullptr && sym->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM}) &&
            llvm::cast<llvm::StructType>(sym->getType()->getLLVMType())->getName() == name) {
            return sym.get();
        }
    }

    if (parent_ == nullptr) {
        return nullptr;
    }

    return parent_->lookupStruct(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupType(const std::string &name) -> Type * {
    // Check owned types first
    auto it = types_.find(name);
    if (it != types_.end()) {
        return it->second.get();
    }

    // Check referenced (imported) types
    auto refIt = typeRefs_.find(name);
    if (refIt != typeRefs_.end()) {
        return refIt->second;
    }

    // Check parent scope
    if (parent_ == nullptr) {
        return nullptr;
    }
    return parent_->lookupType(name);
}

/**
 * Insert a non-owning type reference into the current symbol table.
 * Used for imported types that are owned by another scope.
 *
 * @param name Name of the type
 * @param type Pointer to type (caller must ensure Type outlives this SymbolTable)
 */
auto SymbolTable::insertTypeRef(const std::string &name, Type *type) -> void {
    typeRefs_.insert_or_assign(name, type);
}

/**
 * Create a child leaf for the tree of symbol tables and return it
 *
 * @param blockName Name of the child scope
 * @return Newly created child table
 */
auto SymbolTable::createChildBlock(const std::string &blockName) -> SymbolTable * {
    int idx = 1;
    while (children_.find(blockName + std::to_string(idx)) != children_.end()) {
        idx++;
    }
    auto key = blockName + std::to_string(idx);
    auto child = std::make_unique<SymbolTable>(this);
    auto *childPtr = child.get();
    children_.emplace(std::move(key), std::move(child));
    return childPtr;
}

/**
 * Navigate to parent table of the current one in the tree structure
 *
 * @return Pointer to the parent symbol table
 */
auto SymbolTable::getParent() -> SymbolTable * {
    return parent_;
}

/**
 * Navigate to a child table of the current one in the tree structure
 *
 * @param scopeId Name of the child scope
 * @return Pointer to the child symbol table
 */
auto SymbolTable::getChild(const std::string &scopeId) -> SymbolTable * {
    if (children_.empty()) {
        return nullptr;
    }
    auto it = children_.find(scopeId);
    if (it == children_.end()) {
        return nullptr;
    }
    return it->second.get();
}
