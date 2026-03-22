#include "LspAnalysisGraph.h"

#include <filesystem>
#include <unordered_set>
#include <utility>

#include "WorkspaceLesFiles.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/Driver.h"

namespace lesma::lsp_srv {
namespace {

auto resolveImportAbsolutePath(const AnalysisView& analysis, const lesma::Import* importNode)
    -> std::string {
  if (importNode == nullptr) {
    return {};
  }
  if (importNode->isStd() || analysis.mainFilePath == nullptr || analysis.mainFilePath->empty()) {
    return importNode->getFilePath();
  }
  std::filesystem::path const baseDir =
      std::filesystem::absolute(std::filesystem::path(*analysis.mainFilePath)).parent_path();
  return normalizePath((baseDir / importNode->getFilePath()).string());
}

auto getLazyImportedAnalysis(const std::string& path)
    -> std::shared_ptr<lesma::ImportedModuleAnalysis>;

auto collectAnalysisViewsRecursive(
    const std::unordered_map<std::string, std::shared_ptr<lesma::ImportedModuleAnalysis>>& modules,
    std::unordered_set<std::string>& visited, std::vector<AnalysisView>& out) -> void {
  for (auto const& [path, module] : modules) {
    if (module == nullptr) {
      continue;
    }
    std::string normalized = normalizePath(path.empty() ? module->mainFilePath : path);
    if (!normalized.empty() && !visited.insert(normalized).second) {
      continue;
    }
    out.push_back(makeAnalysisView(*module));
    collectAnalysisViewsRecursive(module->importedModules, visited, out);
  }
}

auto findWorkspaceRoot(const std::string& mainFilePath) -> std::string {
  if (mainFilePath.empty()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::path current =
      std::filesystem::absolute(std::filesystem::path(mainFilePath), ec).parent_path();
  if (ec) {
    return {};
  }
  while (!current.empty()) {
    if (std::filesystem::exists(current / ".git", ec) ||
        std::filesystem::exists(current / "CMakeLists.txt", ec)) {
      return current.lexically_normal().string();
    }
    std::filesystem::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return std::filesystem::path(mainFilePath).parent_path().lexically_normal().string();
}

auto collectLesFilePathsInWorkspace(std::string const& workspaceRoot) -> std::vector<std::string> {
  if (std::optional<std::vector<std::string>> viaGit = tryListLesFilesViaGitRepository(workspaceRoot)) {
    std::vector<std::string> paths;
    paths.reserve(viaGit->size());
    for (std::string const& p : *viaGit) {
      paths.push_back(normalizePath(p));
    }
    return paths;
  }

  std::vector<std::string> paths;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator const end;
  std::filesystem::recursive_directory_iterator it(
      workspaceRoot, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return paths;
  }
  while (it != end) {
    std::filesystem::path const path = it->path();
    if (it->is_directory(ec)) {
      if (path.filename() == ".git") {
        it.disable_recursion_pending();
      }
      it.increment(ec);
      if (ec) {
        break;
      }
      continue;
    }
    if (!ec && it->is_regular_file(ec) && path.extension() == ".les") {
      paths.push_back(normalizePath(path.lexically_normal().string()));
    }
    it.increment(ec);
    if (ec) {
      break;
    }
  }
  return paths;
}

auto makeImportedModuleAnalysis(AnalysisResult analyzed)
    -> std::shared_ptr<lesma::ImportedModuleAnalysis> {
  auto imported = std::make_shared<lesma::ImportedModuleAnalysis>();
  imported->sourceMgr = std::move(analyzed.sourceMgr);
  imported->mainBufferId = analyzed.mainBufferId;
  imported->mainFilePath = std::move(analyzed.mainFilePath);
  imported->parser = std::move(analyzed.parser);
  imported->rootScope = std::move(analyzed.rootScope);
  imported->typeCache = std::move(analyzed.typeCache);
  imported->index = std::move(analyzed.index);
  imported->importAliasToPath = std::move(analyzed.importAliasToPath);
  imported->importedNameToSource = std::move(analyzed.importedNameToSource);
  imported->importedModules = std::move(analyzed.importedModules);
  return imported;
}

auto getLazyImportedAnalysis(const std::string& path)
    -> std::shared_ptr<lesma::ImportedModuleAnalysis> {
  static std::unordered_map<std::string, std::shared_ptr<lesma::ImportedModuleAnalysis>> cache;
  std::string normalized = normalizePath(path);
  auto existing = cache.find(normalized);
  if (existing != cache.end()) {
    return existing->second;
  }
  auto options = std::make_unique<Options>(Options{
      SourceType::FILE,
      normalized,
      Debug::NONE,
      "output",
      false,
      "",
  });
  AnalysisResult analyzed = lesma::analyze(std::move(options));
  AnalysisView view = makeAnalysisView(analyzed);
  if (!isUsableAnalysis(view)) {
    cache[normalized] = nullptr;
    return nullptr;
  }
  std::shared_ptr<lesma::ImportedModuleAnalysis> imported =
      makeImportedModuleAnalysis(std::move(analyzed));
  cache[normalized] = imported;
  return imported;
}

} // namespace

auto makeAnalysisView(const AnalysisResult& result) -> AnalysisView {
  return AnalysisView{
      .sourceMgr = result.sourceMgr.get(),
      .bufferId = result.mainBufferId,
      .mainFilePath = &result.mainFilePath,
      .ast = result.parser ? result.parser->getAst() : nullptr,
      .index = &result.index,
      .rootScope = result.rootScope.get(),
      .importAliasToPath = &result.importAliasToPath,
      .importedNameToSource = &result.importedNameToSource,
      .importedModules = &result.importedModules,
  };
}

auto makeAnalysisView(const lesma::ImportedModuleAnalysis& result) -> AnalysisView {
  return AnalysisView{
      .sourceMgr = result.sourceMgr.get(),
      .bufferId = result.mainBufferId,
      .mainFilePath = &result.mainFilePath,
      .ast = result.parser ? result.parser->getAst() : nullptr,
      .index = &result.index,
      .rootScope = result.rootScope.get(),
      .importAliasToPath = &result.importAliasToPath,
      .importedNameToSource = &result.importedNameToSource,
      .importedModules = &result.importedModules,
  };
}

auto isUsableAnalysis(const AnalysisView& analysis) -> bool {
  return analysis.sourceMgr != nullptr && analysis.rootScope != nullptr &&
         analysis.ast != nullptr && analysis.mainFilePath != nullptr;
}

auto normalizePath(const std::string& path) -> std::string {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::path const absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
  return (!ec ? absolute : std::filesystem::path(path)).lexically_normal().string();
}

auto uriFromPath(const std::string& path) -> ::lsp::DocumentUri {
  std::string normalized = normalizePath(path);
  return ::lsp::FileUri::fromPath(normalized.empty() ? path : normalized);
}

auto smRangeToLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span)
    -> ::lsp::Range {
  if (!span.isValid()) {
    return ::lsp::Range{
        .start = ::lsp::Position{.line = 0U, .character = 0U},
        .end = ::lsp::Position{.line = 0U, .character = 0U},
    };
  }
  auto startLc = srcMgr->getLineAndColumn(span.Start, bufferId);
  auto endLc = srcMgr->getLineAndColumn(span.End, bufferId);
  return ::lsp::Range{
      .start =
          ::lsp::Position{
              .line = startLc.first > 0U ? startLc.first - 1U : 0U,
              .character = startLc.second > 0U ? startLc.second - 1U : 0U,
          },
      .end =
          ::lsp::Position{
              .line = endLc.first > 0U ? endLc.first - 1U : 0U,
              .character = endLc.second > 0U ? endLc.second - 1U : 0U,
          },
  };
}

auto findModuleImportPathByAlias(const AnalysisView& analysis, const std::string& alias)
    -> std::optional<std::string> {
  if (!isUsableAnalysis(analysis)) {
    return std::nullopt;
  }
  if (analysis.importAliasToPath != nullptr) {
    auto pathIt = analysis.importAliasToPath->find(alias);
    if (pathIt != analysis.importAliasToPath->end()) {
      return normalizePath(pathIt->second);
    }
  }
  for (lesma::Statement* stmt : analysis.ast->getChildren()) {
    auto const* importNode = dynamic_cast<const lesma::Import*>(stmt);
    if (importNode == nullptr) {
      continue;
    }
    if (importNode->getAlias() == alias) {
      std::string path = resolveImportAbsolutePath(analysis, importNode);
      if (!path.empty()) {
        return path;
      }
    }
  }
  return std::nullopt;
}

auto findLocalImportBindingLocation(const AnalysisView& analysis, const std::string& name,
                                    bool preferModuleAlias) -> std::optional<::lsp::Location> {
  if (!isUsableAnalysis(analysis) || analysis.mainFilePath == nullptr) {
    return std::nullopt;
  }
  for (lesma::Statement* stmt : analysis.ast->getChildren()) {
    auto const* importNode = dynamic_cast<const lesma::Import*>(stmt);
    if (importNode == nullptr) {
      continue;
    }
    for (const lesma::NamedSpan& binding : lesma::getImportLocalBindings(importNode)) {
      llvm::SMRange const aliasSpan = importNode->getAliasSpan();
      bool const isModuleAlias =
          aliasSpan.isValid() && binding.name == importNode->getAlias() && binding.span.isValid() &&
          binding.span.Start == aliasSpan.Start && binding.span.End == aliasSpan.End;
      if (binding.name == name && binding.span.isValid() &&
          ((preferModuleAlias && isModuleAlias) || (!preferModuleAlias && !isModuleAlias))) {
        return ::lsp::Location{
            .uri = uriFromPath(*analysis.mainFilePath),
            .range = smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, binding.span),
        };
      }
    }
  }
  return std::nullopt;
}

auto collectAnalysisViews(const AnalysisResult& result) -> std::vector<AnalysisView> {
  std::vector<AnalysisView> out;
  AnalysisView mainView = makeAnalysisView(result);
  if (!isUsableAnalysis(mainView)) {
    return out;
  }
  out.push_back(mainView);
  std::unordered_set<std::string> visited;
  std::string mainPath = normalizePath(result.mainFilePath);
  if (!mainPath.empty()) {
    visited.insert(mainPath);
  }
  collectAnalysisViewsRecursive(result.importedModules, visited, out);
  return out;
}

auto collectReferenceAnalysisViews(const AnalysisResult& result, bool includeWorkspace)
    -> std::vector<AnalysisView> {
  std::vector<AnalysisView> out = collectAnalysisViews(result);
  if (!includeWorkspace) {
    return out;
  }
  std::unordered_set<std::string> visited;
  for (const AnalysisView& analysis : out) {
    if (analysis.mainFilePath != nullptr) {
      visited.insert(normalizePath(*analysis.mainFilePath));
    }
  }
  std::string const workspaceRoot = findWorkspaceRoot(result.mainFilePath);
  if (workspaceRoot.empty()) {
    return out;
  }
  for (std::string const& normalized : collectLesFilePathsInWorkspace(workspaceRoot)) {
    if (!normalized.empty() && visited.insert(normalized).second) {
      if (std::shared_ptr<lesma::ImportedModuleAnalysis> imported =
              getLazyImportedAnalysis(normalized)) {
        AnalysisView analysis = makeAnalysisView(*imported);
        if (isUsableAnalysis(analysis)) {
          out.push_back(analysis);
        }
      }
    }
  }
  return out;
}

auto findAnalysisViewForPath(const AnalysisResult& result, const std::string& path)
    -> std::optional<AnalysisView> {
  std::string target = normalizePath(path);
  for (const AnalysisView& view : collectAnalysisViews(result)) {
    if (view.mainFilePath != nullptr && normalizePath(*view.mainFilePath) == target) {
      return view;
    }
  }
  if (std::shared_ptr<lesma::ImportedModuleAnalysis> imported = getLazyImportedAnalysis(target)) {
    return makeAnalysisView(*imported);
  }
  return std::nullopt;
}

} // namespace lesma::lsp_srv
