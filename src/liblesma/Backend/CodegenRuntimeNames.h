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
inline constexpr std::string_view ARC_DEBUG_DELTA = "__lesma_arc_debug_delta";
inline constexpr std::string_view ARC_DEBUG_REPORT = "__lesma_arc_debug_report";
inline constexpr std::string_view ARC_DEBUG_CLEANUP_BEGIN = "__lesma_arc_debug_cleanup_begin";
inline constexpr std::string_view ARC_DEBUG_CLEANUP_STEP = "__lesma_arc_debug_cleanup_step";
inline constexpr std::string_view ARC_DEBUG_MODULE_ROOTS_TOTAL =
    "__lesma_arc_debug_module_roots_total";
inline constexpr std::string_view ARC_DEBUG_MODULE_ROOTS_REMAINING =
    "__lesma_arc_debug_module_roots_remaining";

} // namespace lesma::codegen::runtime
