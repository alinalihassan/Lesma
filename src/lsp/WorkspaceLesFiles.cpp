#include "WorkspaceLesFiles.h"

#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include <git2.h>

namespace lesma::lsp_srv {
namespace {

std::once_flag libgit2InitFlag;

void ensureLibgit2Initialized() {
  std::call_once(libgit2InitFlag, []() { static_cast<void>(git_libgit2_init()); });
}

auto normalizeAbsolutePathString(std::filesystem::path const& p) -> std::string {
  std::error_code ec;
  std::filesystem::path const abs = std::filesystem::absolute(p, ec);
  return (!ec ? abs : p).lexically_normal().string();
}

auto isLesRelativePath(char const* path) -> bool {
  if (path == nullptr) {
    return false;
  }
  auto const len = std::strlen(path);
  return len >= 4 && std::memcmp(path + len - 4, ".les", 4) == 0;
}

auto nonEmpty(char const* p) -> char const* { return (p != nullptr && p[0] != '\0') ? p : nullptr; }

auto pathFromStatusEntry(git_status_entry const* entry) -> char const* {
  if (entry->index_to_workdir != nullptr) {
    git_diff_delta const* const d = entry->index_to_workdir;
    if (char const* p = nonEmpty(d->new_file.path)) {
      return p;
    }
    if (char const* p = nonEmpty(d->old_file.path)) {
      return p;
    }
  }
  if (entry->head_to_index != nullptr) {
    git_diff_delta const* const d = entry->head_to_index;
    if (char const* p = nonEmpty(d->new_file.path)) {
      return p;
    }
    if (char const* p = nonEmpty(d->old_file.path)) {
      return p;
    }
  }
  return nullptr;
}

} // namespace

auto tryListLesFilesViaGitRepository(std::string const& workspaceRoot)
    -> std::optional<std::vector<std::string>> {
  std::error_code ec;
  std::filesystem::path const rootPath = std::filesystem::absolute(workspaceRoot, ec);
  if (ec || rootPath.empty()) {
    return std::nullopt;
  }
  if (!std::filesystem::exists(rootPath / ".git", ec)) {
    return std::nullopt;
  }

  ensureLibgit2Initialized();

  git_repository* repoRaw = nullptr;
  std::string const rootStr = normalizeAbsolutePathString(rootPath);
  if (git_repository_open(&repoRaw, rootStr.c_str()) != 0) {
    return std::nullopt;
  }
  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(repoRaw,
                                                                       git_repository_free);

  std::unordered_set<std::string> relPaths;
  auto addRel = [&](char const* rel) {
    if (isLesRelativePath(rel)) {
      relPaths.emplace(rel);
    }
  };
  auto removeRel = [&](char const* rel) {
    if (isLesRelativePath(rel)) {
      relPaths.erase(rel);
    }
  };

  git_index* idxRaw = nullptr;
  if (git_repository_index(&idxRaw, repo.get()) != 0) {
    return std::nullopt;
  }
  std::unique_ptr<git_index, decltype(&git_index_free)> idx(idxRaw, git_index_free);
  if (git_index_read(idx.get(), 0) != 0) {
    return std::nullopt;
  }

  size_t const indexCount = git_index_entrycount(idx.get());
  for (size_t i = 0; i < indexCount; ++i) {
    git_index_entry const* e = git_index_get_byindex(idx.get(), i);
    if (e == nullptr || e->path == nullptr) {
      continue;
    }
    if (e->mode == GIT_FILEMODE_COMMIT) {
      continue;
    }
    addRel(e->path);
  }

  git_status_list* stRaw = nullptr;
  git_status_options opts{};
  opts.version = GIT_STATUS_OPTIONS_VERSION;
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
               GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

  if (git_status_list_new(&stRaw, repo.get(), &opts) != 0) {
    return std::nullopt;
  }
  std::unique_ptr<git_status_list, decltype(&git_status_list_free)> stList(stRaw,
                                                                           git_status_list_free);

  size_t const stCount = git_status_list_entrycount(stList.get());
  for (size_t i = 0; i < stCount; ++i) {
    git_status_entry const* entry = git_status_byindex(stList.get(), i);
    if (entry == nullptr) {
      continue;
    }
    unsigned int const st = entry->status;
    if ((st & GIT_STATUS_IGNORED) != 0) {
      continue;
    }
    if ((st & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED)) != 0U &&
        entry->head_to_index != nullptr) {
      removeRel(entry->head_to_index->old_file.path);
    }
    if ((st & (GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED)) != 0U &&
        entry->index_to_workdir != nullptr) {
      removeRel(entry->index_to_workdir->old_file.path);
    }
    if ((st & GIT_STATUS_WT_NEW) != 0U) {
      addRel(pathFromStatusEntry(entry));
    }
    if ((st & GIT_STATUS_INDEX_RENAMED) != 0U && entry->head_to_index != nullptr) {
      addRel(entry->head_to_index->new_file.path);
    }
    if ((st & GIT_STATUS_WT_RENAMED) != 0U && entry->index_to_workdir != nullptr) {
      addRel(entry->index_to_workdir->new_file.path);
    }
  }

  std::vector<std::string> out;
  out.reserve(relPaths.size());
  for (std::string const& rel : relPaths) {
    std::filesystem::path const full = std::filesystem::path(rootStr) / rel;
    std::error_code existsEc;
    if (!std::filesystem::exists(full, existsEc)) {
      continue;
    }
    out.push_back(normalizeAbsolutePathString(full));
  }
  return out;
}

} // namespace lesma::lsp_srv
