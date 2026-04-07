#pragma once

#include <string>

#include <llvm/Support/SMLoc.h>

namespace lesma {
class Type;

namespace MangleUtils {
auto getTypeMangledName(llvm::SMRange span, Type* type) -> std::string;
auto isMethod(const std::string& mangledName) -> bool;
/** Stable global symbol for exported module variables (cross-module linkage). */
auto getGlobalVariableSymbolName(const std::string& modulePathNormalized,
                                 const std::string& variableName) -> std::string;
/** Unique per-module top-level init symbol for JIT (replaces internal `main`). */
auto getImportedModuleInitSymbolName(const std::string& modulePathNormalized) -> std::string;
/** Unique per-module ARC cleanup symbol for JIT shutdown ordering. */
auto getImportedModuleFiniSymbolName(const std::string& modulePathNormalized) -> std::string;
} // namespace MangleUtils
} // namespace lesma
