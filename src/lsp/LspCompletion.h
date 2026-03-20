#pragma once

#include <lsp/types.h>
#include <string>
#include <vector>

#include "liblesma/Driver/AnalysisResult.h"

namespace lesma::lsp_srv {

/** Completion items for either member access or plain identifier completion. */
[[nodiscard]] auto completionItems(const AnalysisResult& result, unsigned line,
                                   unsigned character) -> std::vector<::lsp::CompletionItem>;

} // namespace lesma::lsp_srv
