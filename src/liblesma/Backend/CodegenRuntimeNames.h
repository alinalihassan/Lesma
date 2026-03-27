#pragma once

#include <string_view>

namespace lesma::codegen::runtime {

inline constexpr std::string_view LLVM_MODULE_NAME = "Lesma";
inline constexpr std::string_view IMPLICIT_STDLIB_MODULE = "base.les";

inline constexpr std::string_view MALLOC = "malloc";
inline constexpr std::string_view CALLOC = "calloc";
inline constexpr std::string_view REALLOC = "realloc";
inline constexpr std::string_view FREE = "free";
inline constexpr std::string_view MEMCPY = "memcpy";
inline constexpr std::string_view STRLEN = "strlen";
inline constexpr std::string_view STRSTR = "strstr";
inline constexpr std::string_view EXIT = "exit";

} // namespace lesma::codegen::runtime
