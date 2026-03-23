#pragma once

#include <string>

namespace lesma {
class SymbolTable;
class Type;
} // namespace lesma

namespace lesma::lsp_srv {

/** Resolve display name for class/enum (and some pointer forms) via the symbol table. */
auto getTypeName(Type* type, SymbolTable* rootScope) -> std::string;

/** Human-readable type spelling for LSP (matches list<T> for array-backed lists, etc.). */
auto formatTypeName(Type* type, SymbolTable* rootScope) -> std::string;

} // namespace lesma::lsp_srv
