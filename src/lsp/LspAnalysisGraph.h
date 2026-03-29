#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/Support/SourceMgr.h>

#include <lsp/types.h>

#include "liblesma/Driver/AnalysisResult.h"

namespace lesma {
class Value;
}

namespace lesma::lsp_srv {

struct AnalysisView {
  llvm::SourceMgr* sourceMgr = nullptr;
  unsigned bufferId = 0;
  const std::string* mainFilePath = nullptr;
  lesma::Compound* ast = nullptr;
  const lesma::AnalysisIndex* index = nullptr;
  lesma::SymbolTable* rootScope = nullptr;
  const lesma::ImportAliasMap* importAliasToPath = nullptr;
  const lesma::ImportedNameSourceMap* importedNameToSource = nullptr;
  const std::unordered_map<std::string, std::shared_ptr<lesma::ImportedModuleAnalysis>>*
      importedModules = nullptr;
};

auto makeAnalysisView(AnalysisResult& result) -> AnalysisView;
auto makeAnalysisView(lesma::ImportedModuleAnalysis& result) -> AnalysisView;
auto isUsableAnalysis(const AnalysisView& analysis) -> bool;

auto normalizePath(const std::string& path) -> std::string;
auto invalidateLazyImportedAnalysis(const std::string& path) -> void;
auto invalidateLazyImportedAnalysesAffectedBy(const std::string& path) -> void;
auto invalidateAllLazyImportedAnalyses() -> void;
auto uriFromPath(const std::string& path) -> ::lsp::DocumentUri;
auto smRangeToLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span)
    -> ::lsp::Range;

auto findModuleImportPathByAlias(const AnalysisView& analysis, const std::string& alias)
    -> std::optional<std::string>;
auto findLocalImportBindingLocation(const AnalysisView& analysis, const std::string& name,
                                    bool preferModuleAlias) -> std::optional<::lsp::Location>;

auto collectAnalysisViews(AnalysisResult& result) -> std::vector<AnalysisView>;
auto collectReferenceAnalysisViews(AnalysisResult& result, bool includeWorkspace)
    -> std::vector<AnalysisView>;
auto findAnalysisViewForPath(AnalysisResult& result, const std::string& path)
    -> std::optional<AnalysisView>;

/** Leading `#` lines above a class or function-like declaration (see
 * `extractLineCommentDocumentationAboveDecl`). */
[[nodiscard]] auto documentationCommentAboveDeclaration(AnalysisResult& result, lesma::Value* value,
                                                          const AnalysisView& fallbackOwner)
    -> std::string;

} // namespace lesma::lsp_srv
