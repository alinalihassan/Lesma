#include "SymbolTable.h"

using namespace lesma;

/**
 * Insert a new symbol into the current symbol table. If it is a parameter, append its name to the paramNames vector
 *
 * @param entry Symbol Table Entry
 */
void SymbolTable::insertSymbol(SymbolTableEntry* entry) {
    symbols.insert_or_assign(entry->getName(), entry);
}

/**
 * Insert a new symbol into the current symbol table. If it is a parameter, append its name to the paramNames vector
 *
 * @param entry Symbol Table Entry
 */
void SymbolTable::insertType(const std::string& name, SymbolType *type) {
    types.insert_or_assign(name, type);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
SymbolTableEntry *SymbolTable::lookup(const std::string &name) {
    if (symbols.find(name) == symbols.end()) {
        if (parent == nullptr) return nullptr;
        return parent->lookup(name);
    }

    return symbols.at(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
SymbolType *SymbolTable::lookupType(const std::string &name) {
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