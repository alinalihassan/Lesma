#pragma once

#include <optional>
#include <string>
#include <vector>

/** If \p workspaceRoot is a Git work tree, list absolute normalized paths to non-ignored `.les`
 * files (tracked in the index plus untracked that respect exclude rules). Otherwise returns
 * nullopt. */
auto tryListLesFilesViaGitRepository(std::string const& workspaceRoot)
    -> std::optional<std::vector<std::string>>;
