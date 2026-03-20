#pragma once

#include <string>
#include <vector>

#include "liblesma/Driver/AnalysisResult.h"

namespace lesma::lsp_srv {

/** Member names after `.` when the base type is a class or enum (fields + methods). */
[[nodiscard]] auto memberCompletionNames(const AnalysisResult& result, unsigned line,
                                           unsigned character) -> std::vector<std::string>;

} // namespace lesma::lsp_srv
