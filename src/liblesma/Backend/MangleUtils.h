#pragma once

#include <string>

#include <llvm/Support/SMLoc.h>

namespace lesma {
class Type;

namespace MangleUtils {
auto getTypeMangledName(llvm::SMRange span, Type* type) -> std::string;
auto isMethod(const std::string& mangledName) -> bool;
} // namespace MangleUtils
} // namespace lesma
