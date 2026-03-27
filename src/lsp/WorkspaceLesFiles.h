#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lesma::lsp_srv {

/** If \p workspaceRoot is a Git work tree, list absolute normalized paths to non-ignored `.les`
 * files (tracked in the index plus untracked that respect exclude rules). Otherwise returns
 * nullopt. */
[[nodiscard]] auto tryListLesFilesViaGitRepository(std::string const& workspaceRoot)
    -> std::optional<std::vector<std::string>>;

} // namespace lesma::lsp_srv
