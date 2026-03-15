#pragma once

#include <string>
#include <vector>

#include <llvm/Support/SMLoc.h>

namespace lesma {
class Type;

namespace MangleUtils {
auto getTypeMangledName(llvm::SMRange span, Type* type) -> std::string;
auto isMethod(const std::string& mangledName) -> bool;
auto isMangled(std::string name) -> bool;
auto getDemangledName(const std::string& mangledName) -> std::string;
} // namespace MangleUtils
} // namespace lesma
