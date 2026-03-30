#pragma once

#include <vector>

#include <lsp/types.h>

#include "liblesma/Driver/AnalysisResult.h"

namespace lesma::lsp_srv {

struct CompletionOutcome {
  std::vector<::lsp::CompletionItem> items;
  /** When true, the LSP response uses CompletionList.isIncomplete so the client re-requests after
   * further typing (e.g. after '/' while editing an import path). */
  bool isIncomplete = false;
};

/** Completion items for member access, plain identifiers, or import path segments. */
[[nodiscard]] auto completionItems(AnalysisResult& result, unsigned line, unsigned character)
    -> CompletionOutcome;

} // namespace lesma::lsp_srv
