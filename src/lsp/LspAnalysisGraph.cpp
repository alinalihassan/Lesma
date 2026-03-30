#include "LspAnalysisGraph.h"

#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "LspSourceHelpers.h"
#include "WorkspaceLesFiles.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/Value.h"

namespace lesma::lsp_srv {
namespace {

using DependencySet = std::unordered_set<std::string>;

struct DependencyNode {
  DependencySet imports;
  DependencySet importedBy;
};

struct LazyImportedAnalysisCacheState {
  std::unordered_map<std::string, std::shared_ptr<lesma::ImportedModuleAnalysis>> cache;
  std::unordered_map<std::string, DependencyNode> graph;
  std::mutex cacheMutex;
};

auto lazyImportedAnalysisCacheState() -> LazyImportedAnalysisCacheState& {
  static LazyImportedAnalysisCacheState state;
  return state;
}

auto eraseDependencyNodeIfUnused(LazyImportedAnalysisCacheState& state, const std::string& path)
    -> void {
  auto node = state.graph.find(path);
  if (node == state.graph.end()) {
    return;
  }
  if (state.cache.contains(path) || !node->second.imports.empty() ||
      !node->second.importedBy.empty()) {
    return;
  }
  state.graph.erase(node);
}

auto addDependencyEdge(LazyImportedAnalysisCacheState& state, const std::string& importer,
                       const std::string& dependency) -> void {
  if (importer.empty() || dependency.empty() || importer == dependency) {
    return;
  }
  state.graph[importer].imports.insert(dependency);
  state.graph[dependency].importedBy.insert(importer);
}

auto removeImportsForNode(LazyImportedAnalysisCacheState& state, const std::string& importer)
    -> void {
  auto node = state.graph.find(importer);
  if (node == state.graph.end()) {
    return;
  }
  std::vector<std::string> dependencies(node->second.imports.begin(), node->second.imports.end());
  for (std::string const& dependency : dependencies) {
    auto dependencyNode = state.graph.find(dependency);
    if (dependencyNode == state.graph.end()) {
      continue;
    }
    dependencyNode->second.importedBy.erase(importer);
    eraseDependencyNodeIfUnused(state, dependency);
  }
  node->second.imports.clear();
  eraseDependencyNodeIfUnused(state, importer);
}

auto collectDirectDependencyPaths(const lesma::ImportedModuleAnalysis& imported) -> DependencySet {
  DependencySet dependencies;
  for (auto const& [path, module] : imported.importedModules) {
    std::string normalized =
        normalizePath(path.empty() && module != nullptr ? module->mainFilePath : path);
    if (!normalized.empty()) {
      dependencies.insert(std::move(normalized));
    }
  }
  for (auto const& [alias, path] : imported.importAliasToPath) {
    (void) alias;
    std::string normalized = normalizePath(path);
    if (!normalized.empty()) {
      dependencies.insert(std::move(normalized));
    }
  }
  for (auto const& [localName, source] : imported.importedNameToSource) {
    (void) localName;
    std::string normalized = normalizePath(source.first);
    if (!normalized.empty()) {
      dependencies.insert(std::move(normalized));
    }
  }
  return dependencies;
}

auto recordDependencyEdgesForImporter(LazyImportedAnalysisCacheState& state,
                                      const std::string& importer,
                                      const lesma::ImportedModuleAnalysis& imported) -> void {
  removeImportsForNode(state, importer);
  for (std::string const& dependency : collectDirectDependencyPaths(imported)) {
    addDependencyEdge(state, importer, dependency);
  }
}

auto removeDependencyNode(LazyImportedAnalysisCacheState& state, const std::string& path) -> void {
  auto node = state.graph.find(path);
  if (node == state.graph.end()) {
    return;
  }
  std::vector<std::string> dependencies(node->second.imports.begin(), node->second.imports.end());
  std::vector<std::string> importers(node->second.importedBy.begin(),
                                     node->second.importedBy.end());
  for (std::string const& dependency : dependencies) {
    auto dependencyNode = state.graph.find(dependency);
    if (dependencyNode == state.graph.end()) {
      continue;
    }
    dependencyNode->second.importedBy.erase(path);
    eraseDependencyNodeIfUnused(state, dependency);
  }
  for (std::string const& importer : importers) {
    auto importerNode = state.graph.find(importer);
    if (importerNode == state.graph.end()) {
      continue;
    }
    importerNode->second.imports.erase(path);
    eraseDependencyNodeIfUnused(state, importer);
  }
  state.graph.erase(node);
}

auto eraseCachedAnalysisEntry(LazyImportedAnalysisCacheState& state, const std::string& path)
    -> void {
  state.cache.erase(path);
  removeDependencyNode(state, path);
}

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
    if (std::filesystem::exists(current / ".git", ec)) {
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
  if (std::optional<std::vector<std::string>> viaGit =
          tryListLesFilesViaGitRepository(workspaceRoot)) {
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
  imported->typeCache = std::move(analyzed.typeCache);
  imported->rootScope = std::move(analyzed.rootScope);
  imported->index = std::move(analyzed.index);
  imported->importAliasToPath = std::move(analyzed.importAliasToPath);
  imported->importedNameToSource = std::move(analyzed.importedNameToSource);
  imported->importedModules = std::move(analyzed.importedModules);
  return imported;
}

auto getLazyImportedAnalysis(const std::string& path)
    -> std::shared_ptr<lesma::ImportedModuleAnalysis> {
  std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    return nullptr;
  }
  auto& cacheState = lazyImportedAnalysisCacheState();

  auto lookupCached = [&]() -> std::shared_ptr<lesma::ImportedModuleAnalysis> {
    std::lock_guard<std::mutex> lock(cacheState.cacheMutex);
    auto it = cacheState.cache.find(normalized);
    return it != cacheState.cache.end() ? it->second : nullptr;
  };

  if (std::shared_ptr<lesma::ImportedModuleAnalysis> hit = lookupCached()) {
    return hit;
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
    return lookupCached();
  }

  std::shared_ptr<lesma::ImportedModuleAnalysis> imported =
      makeImportedModuleAnalysis(std::move(analyzed));
  std::lock_guard<std::mutex> lock(cacheState.cacheMutex);
  if (auto it = cacheState.cache.find(normalized); it != cacheState.cache.end()) {
    return it->second;
  }
  cacheState.cache[normalized] = imported;
  recordDependencyEdgesForImporter(cacheState, normalized, *imported);
  return imported;
}

} // namespace

auto makeAnalysisView(AnalysisResult& result) -> AnalysisView {
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

auto makeAnalysisView(lesma::ImportedModuleAnalysis& result) -> AnalysisView {
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
  std::filesystem::path fsPath(path);
  std::error_code ec;
  std::filesystem::path normalized = std::filesystem::weakly_canonical(fsPath, ec);
  if (!ec) {
    return normalized.make_preferred().string();
  }
  normalized = std::filesystem::absolute(fsPath, ec);
  if (!ec) {
    normalized = normalized.lexically_normal();
    return normalized.make_preferred().string();
  }
  normalized = fsPath.lexically_normal();
  return normalized.make_preferred().string();
}

auto invalidateLazyImportedAnalysis(const std::string& path) -> void {
  std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    return;
  }
  auto& cacheState = lazyImportedAnalysisCacheState();
  std::lock_guard<std::mutex> lock(cacheState.cacheMutex);
  eraseCachedAnalysisEntry(cacheState, normalized);
}

auto invalidateLazyImportedAnalysesAffectedBy(const std::string& path) -> void {
  std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    return;
  }
  auto& cacheState = lazyImportedAnalysisCacheState();
  std::lock_guard<std::mutex> lock(cacheState.cacheMutex);
  DependencySet affected;
  std::vector<std::string> pending{normalized};
  while (!pending.empty()) {
    std::string current = std::move(pending.back());
    pending.pop_back();
    if (!affected.insert(current).second) {
      continue;
    }
    auto node = cacheState.graph.find(current);
    if (node == cacheState.graph.end()) {
      continue;
    }
    pending.insert(pending.end(), node->second.importedBy.begin(), node->second.importedBy.end());
  }
  for (std::string const& affectedPath : affected) {
    eraseCachedAnalysisEntry(cacheState, affectedPath);
  }
}

auto invalidateAllLazyImportedAnalyses() -> void {
  auto& cacheState = lazyImportedAnalysisCacheState();
  std::lock_guard<std::mutex> lock(cacheState.cacheMutex);
  cacheState.cache.clear();
  cacheState.graph.clear();
}

auto uriFromPath(const std::string& path) -> ::lsp::DocumentUri {
  std::string normalized = normalizePath(path);
  return ::lsp::FileUri::fromPath(normalized.empty() ? path : normalized);
}

/** \p span → LSP range using negotiated UTF-8 positions. LLVM `getLineAndColumn` column is a
 * 1-based byte index from the line start; LSP `character` is 0-based UTF-8 code units. */
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
  std::optional<::lsp::Location> fallbackLocation;
  for (lesma::Statement* stmt : analysis.ast->getChildren()) {
    auto const* importNode = dynamic_cast<const lesma::Import*>(stmt);
    if (importNode == nullptr) {
      continue;
    }
    for (const lesma::NamedSpan& binding : lesma::getImportLocalBindings(importNode)) {
      llvm::SMRange const aliasSpan = importNode->getAliasSpan();
      bool const isModuleAlias = aliasSpan.isValid() && binding.name == importNode->getAlias() &&
                                 binding.span.isValid() && binding.span.Start == aliasSpan.Start &&
                                 binding.span.End == aliasSpan.End;
      if (binding.name != name || !binding.span.isValid()) {
        continue;
      }
      if (preferModuleAlias == isModuleAlias) {
        return ::lsp::Location{
            .uri = uriFromPath(*analysis.mainFilePath),
            .range = smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, binding.span),
        };
      }
      if (!fallbackLocation.has_value()) {
        fallbackLocation = ::lsp::Location{
            .uri = uriFromPath(*analysis.mainFilePath),
            .range = smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, binding.span),
        };
      }
    }
  }
  return fallbackLocation;
}

auto collectAnalysisViews(AnalysisResult& result) -> std::vector<AnalysisView> {
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

auto collectReferenceAnalysisViews(AnalysisResult& result, bool includeWorkspace)
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

auto findAnalysisViewForPath(AnalysisResult& result, const std::string& path)
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

namespace {

[[nodiscard]] auto wantsLeadingCommentDocumentation(lesma::ValueDeclarationKind k) -> bool {
  switch (k) {
  case lesma::ValueDeclarationKind::CLASS:
  case lesma::ValueDeclarationKind::ENUM:
  case lesma::ValueDeclarationKind::FUNCTION:
  case lesma::ValueDeclarationKind::METHOD:
  case lesma::ValueDeclarationKind::TRAIT:
    return true;
  default:
    return false;
  }
}

} // namespace

auto documentationCommentAboveDeclaration(AnalysisResult& result, lesma::Value* value,
                                        const AnalysisView& fallbackOwner) -> std::string {
  if (value == nullptr || !wantsLeadingCommentDocumentation(value->getDeclarationKind())) {
    return {};
  }
  llvm::SMRange const declSpan = value->getDeclarationSpan();
  if (!declSpan.isValid() || !declSpan.Start.isValid()) {
    return {};
  }
  std::string declPath = value->getDeclarationFilePath();
  if (declPath.empty() && fallbackOwner.mainFilePath != nullptr) {
    declPath = *fallbackOwner.mainFilePath;
  }
  llvm::SourceMgr* srcMgr = nullptr;
  unsigned bufferId = 0;
  if (!declPath.empty()) {
    std::optional<AnalysisView> declView = findAnalysisViewForPath(result, declPath);
    if (!declView || !isUsableAnalysis(*declView)) {
      return {};
    }
    if (declView->sourceMgr->FindBufferContainingLoc(declSpan.Start) != declView->bufferId) {
      return {};
    }
    srcMgr = declView->sourceMgr;
    bufferId = declView->bufferId;
  } else {
    if (fallbackOwner.sourceMgr == nullptr ||
        fallbackOwner.sourceMgr->FindBufferContainingLoc(declSpan.Start) != fallbackOwner.bufferId) {
      return {};
    }
    srcMgr = fallbackOwner.sourceMgr;
    bufferId = fallbackOwner.bufferId;
  }
  auto const* memBuf = srcMgr->getMemoryBuffer(bufferId);
  if (memBuf == nullptr) {
    return {};
  }
  unsigned const offset = getOffsetFromSMLoc(srcMgr, bufferId, declSpan.Start);
  return extractLineCommentDocumentationAboveDecl(memBuf->getBuffer(), offset);
}

} // namespace lesma::lsp_srv
