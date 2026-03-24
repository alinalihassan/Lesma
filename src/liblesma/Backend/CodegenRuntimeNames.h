#pragma once

#include <string_view>

namespace lesma::codegen::runtime {

inline constexpr std::string_view kLlvmModuleName = "Lesma";
inline constexpr std::string_view kImplicitStdlibModule = "base.les";

inline constexpr std::string_view kMalloc = "malloc";
inline constexpr std::string_view kCalloc = "calloc";
inline constexpr std::string_view kRealloc = "realloc";
inline constexpr std::string_view kFree = "free";
inline constexpr std::string_view kMemcpy = "memcpy";
inline constexpr std::string_view kStrlen = "strlen";
inline constexpr std::string_view kStrstr = "strstr";
inline constexpr std::string_view kExit = "exit";

inline constexpr std::string_view kLesmaArcAlloc = "lesma_arc_alloc";
inline constexpr std::string_view kLesmaArcRetain = "lesma_arc_retain";
inline constexpr std::string_view kLesmaArcRelease = "lesma_arc_release";

} // namespace lesma::codegen::runtime
