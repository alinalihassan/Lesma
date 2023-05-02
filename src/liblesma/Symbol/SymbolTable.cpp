#include "SymbolTable.h"
#include <llvm/IR/DerivedTypes.h>

using namespace lesma;

/**
 * Insert a new symbol into the current symbol table. If it is a parameter, append its name to the paramNames vector
 *
 * @param entry Symbol Table Entry
 */
void SymbolTable::insertSymbol(Value *entry) {
    symbols.emplace(entry->getName(), entry);
}

/**
 * Insert a new symbol into the current symbol table. If it is a parameter, append its name to the paramNames vector
 *
 * @param entry Symbol Table Entry
 */
void SymbolTable::insertType(const std::string &name, Type *type) {
    types.insert_or_assign(name, type);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
Value *SymbolTable::lookupClassConstructor(const std::string &className, std::vector<lesma::Type *> paramTypes) {
    const std::string name = "new";
    auto range = symbols.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (!it->second->getType()->is(TY_FUNCTION))
            continue;
        // TODO: Currently checking by LLVM's Struct name, we shouldn't rely on that
        if (!(it->second->getType()->getFields()[0]->type->is(TY_PTR) &&
              it->second->getType()->getFields()[0]->type->getElementType()->is(TY_CLASS) &&
              it->second->getType()->getFields()[0]->type->getElementType()->getLLVMType()->isStructTy() &&
              llvm::cast<llvm::StructType>(it->second->getType()->getFields()[0]->type->getElementType()->getLLVMType())->getName().str() == className))
            continue;

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

        return it->second;
    }

    if (parent == nullptr) {
        return nullptr;
    }

    return parent->lookupFunction(name, paramTypes);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
Value *SymbolTable::lookupFunction(const std::string &name, std::vector<lesma::Type *> paramTypes) {
    auto range = symbols.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (!it->second->getType()->is(TY_FUNCTION))
            continue;
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

        return it->second;
    }

    if (parent == nullptr) {
        return nullptr;
    }

    return parent->lookupFunction(name, paramTypes);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
Value *SymbolTable::lookup(const std::string &name) {
    for (const auto &sym: symbols) {
        if (sym.first == name) {
            return sym.second;
        }
    }

    if (parent == nullptr) {
        return nullptr;
    }

    return parent->lookup(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
Value *SymbolTable::lookupStruct(const std::string &name) {
    for (auto sym: symbols) {
        if (sym.second->getType()->getLLVMType() != nullptr && sym.second->getType()->isOneOf({TY_CLASS, TY_ENUM}) &&
            llvm::cast<llvm::StructType>(sym.second->getType()->getLLVMType())->getName() == name)
            return sym.second;
    }

    if (parent == nullptr) {
        return nullptr;
    }

    return parent->lookupStruct(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
Type *SymbolTable::lookupType(const std::string &name) {
    if (types.find(name) == types.end()) {
        if (parent == nullptr) return nullptr;
        return parent->lookupType(name);
    }

    return types.at(name);
}

/**
 * Create a child leaf for the tree of symbol tables and return it
 *
 * @param blockName Name of the child scope
 * @return Newly created child table
 */
SymbolTable *SymbolTable::createChildBlock(const std::string &blockName) {
    int idx = 1;
    while (children.find(blockName + std::to_string(idx)) != children.end())
        idx++;
    children.insert({blockName + std::to_string(idx), new SymbolTable(this)});
    return children.at(blockName + std::to_string(idx));
}

/**
 * Navigate to parent table of the current one in the tree structure
 *
 * @return Pointer to the parent symbol table
 */
SymbolTable *SymbolTable::getParent() {
    return parent;
}

/**
 * Navigate to a child table of the current one in the tree structure
 *
 * @param scopeId Name of the child scope
 * @return Pointer to the child symbol table
 */
SymbolTable *SymbolTable::getChild(const std::string &scopeId) {
    if (children.empty()) return nullptr;
    if (children.find(scopeId) == children.end()) return nullptr;
    return children.at(scopeId);
}