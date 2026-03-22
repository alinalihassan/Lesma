#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include <llvm/Support/SourceMgr.h>

#include "DocumentStore.h"
#include "LspCompletion.h"
#include "LspUtf16.h"
#include "WorkspaceLesFiles.h"
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>
#include <lsp/types.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

namespace {

using namespace lesma;

/** Cached analysis result for a document version. */
struct DocumentAnalysisSnapshot {
  std::string content;
  int version;
  AnalysisResult result;
};

/** Cache of analysis results keyed by URI. */
class AnalysisCache {
  std::unordered_map<std::string, DocumentAnalysisSnapshot> cache;

public:
  /** Get or compute analysis for a document. */
  auto getOrAnalyze(const ::lsp::DocumentUri& uri, const lesma::lsp_srv::DocumentStore& docStore,
                    const std::string& content, int version) -> DocumentAnalysisSnapshot& {
    std::string key = docStore.uriToKey(uri);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.version == version && it->second.content == content) {
      return it->second;
    }
    // Recompute
    std::string implicitPath = docStore.getAnalyzeMainFilePath(uri).value_or("");
    auto options = std::make_unique<Options>(Options{
        SourceType::STRING,
        content,
        Debug::NONE,
        "output",
        false,
        std::move(implicitPath),
    });
    AnalysisResult result = lesma::analyze(std::move(options));
    DocumentAnalysisSnapshot snapshot{content, version, std::move(result)};
    cache[key] = std::move(snapshot);
    return cache[key];
  }

  void invalidate(const ::lsp::DocumentUri& uri, const lesma::lsp_srv::DocumentStore& docStore) {
    std::string key = docStore.uriToKey(uri);
    cache.erase(key);
  }
};

template <typename Result, typename Fn>
auto withAnalyzedDocument(const ::lsp::DocumentUri& uri,
                          const lesma::lsp_srv::DocumentStore& docStore,
                          AnalysisCache& analysisCache, const Fn& fn) -> Result {
  std::optional<lesma::lsp_srv::DocumentStore::Document> doc = docStore.getDocument(uri);
  if (!doc) {
    return {};
  }
  DocumentAnalysisSnapshot& snapshot =
      analysisCache.getOrAnalyze(uri, docStore, doc->text, doc->version);
  return fn(snapshot.result);
}

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

struct ResolvedSymbol {
  lesma::Value* value = nullptr;
  AnalysisView owner;
};

struct SymbolIdentity {
  std::string path;
  ::lsp::Range range;
  std::string name;
};

struct EnumMemberIdentity {
  std::string path;
  std::string enumName;
  std::string memberName;
  ::lsp::Range range;
};

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

auto smRangeToLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span)
    -> ::lsp::Range;

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
    std::shared_ptr<lesma::ImportedModuleAnalysis> indexedModule = module;
    if (indexedModule->parser != nullptr && indexedModule->index.symbolOccurrences.empty() &&
        !normalized.empty()) {
      if (std::shared_ptr<lesma::ImportedModuleAnalysis> lazy = getLazyImportedAnalysis(normalized)) {
        indexedModule = lazy;
      }
    }
    out.push_back(makeAnalysisView(*indexedModule));
    collectAnalysisViewsRecursive(indexedModule->importedModules, visited, out);
  }
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

/** All .les files to consider for workspace-wide references: Git rules when possible, else walk FS. */
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
  // LLVM returns 1-based line/column, LSP uses 0-based
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

auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) -> unsigned;

auto smRangesEqual(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange lhs,
                   llvm::SMRange rhs) -> bool {
  return lhs.isValid() && rhs.isValid() &&
         getOffsetFromSMLoc(srcMgr, bufferId, lhs.Start) ==
             getOffsetFromSMLoc(srcMgr, bufferId, rhs.Start) &&
         getOffsetFromSMLoc(srcMgr, bufferId, lhs.End) == getOffsetFromSMLoc(srcMgr, bufferId, rhs.End);
}

template <typename FuncLike>
auto resolveFuncLikeDeclarationSymbol(const FuncLike* node, llvm::SourceMgr* srcMgr, unsigned bufferId,
                                      llvm::SMRange declarationSpan) -> lesma::Value* {
  if (node == nullptr) {
    return nullptr;
  }
  if (smRangesEqual(srcMgr, bufferId, node->getNameSpan(), declarationSpan)) {
    return node->getResolvedSymbol();
  }
  if (node->getGenericScope() != nullptr) {
    for (const lesma::GenericParamDecl& genericParam : node->getGenericParamDecls()) {
      if (smRangesEqual(srcMgr, bufferId, genericParam.span, declarationSpan)) {
        return node->getGenericScope()->lookup(genericParam.name);
      }
    }
  }
  for (lesma::Parameter* param : node->getParameters()) {
    if (param != nullptr && smRangesEqual(srcMgr, bufferId, param->nameSpan, declarationSpan)) {
      return param->getResolvedSymbol();
    }
  }
  return nullptr;
}

auto resolveDeclarationSymbolInStmt(const lesma::Statement* stmt, llvm::SourceMgr* srcMgr,
                                    unsigned bufferId, llvm::SMRange declarationSpan)
    -> lesma::Value* {
  if (stmt == nullptr) {
    return nullptr;
  }
  if (auto const* varDecl = dynamic_cast<const lesma::VarDecl*>(stmt)) {
    lesma::Literal* ident = varDecl->getIdentifier();
    if (ident != nullptr && smRangesEqual(srcMgr, bufferId, ident->getSpan(), declarationSpan)) {
      return varDecl->getResolvedSymbol();
    }
    return nullptr;
  }
  if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(stmt)) {
    if (lesma::Value* value =
            resolveFuncLikeDeclarationSymbol(func, srcMgr, bufferId, declarationSpan)) {
      return value;
    }
    if (func->getBody() != nullptr) {
      return resolveDeclarationSymbolInStmt(func->getBody(), srcMgr, bufferId, declarationSpan);
    }
    return nullptr;
  }
  if (auto const* ext = dynamic_cast<const lesma::ExternFuncDecl*>(stmt)) {
    return resolveFuncLikeDeclarationSymbol(ext, srcMgr, bufferId, declarationSpan);
  }
  if (auto const* klass = dynamic_cast<const lesma::Class*>(stmt)) {
    if (smRangesEqual(srcMgr, bufferId, klass->getNameSpan(), declarationSpan)) {
      return klass->getResolvedSymbol();
    }
    if (klass->getGenericScope() != nullptr) {
      for (const lesma::GenericParamDecl& genericParam : klass->getGenericParamDecls()) {
        if (smRangesEqual(srcMgr, bufferId, genericParam.span, declarationSpan)) {
          return klass->getGenericScope()->lookup(genericParam.name);
        }
      }
    }
    for (lesma::VarDecl* field : klass->getFields()) {
      if (lesma::Value* value =
              resolveDeclarationSymbolInStmt(field, srcMgr, bufferId, declarationSpan)) {
        return value;
      }
    }
    for (lesma::FuncDecl* method : klass->getMethods()) {
      if (lesma::Value* value =
              resolveDeclarationSymbolInStmt(method, srcMgr, bufferId, declarationSpan)) {
        return value;
      }
    }
    return nullptr;
  }
  if (auto const* enumNode = dynamic_cast<const lesma::Enum*>(stmt)) {
    if (smRangesEqual(srcMgr, bufferId, enumNode->getNameSpan(), declarationSpan)) {
      return enumNode->getResolvedSymbol();
    }
    return nullptr;
  }
  if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
    for (lesma::Compound* block : ifNode->getBlocks()) {
      if (lesma::Value* value =
              resolveDeclarationSymbolInStmt(block, srcMgr, bufferId, declarationSpan)) {
        return value;
      }
    }
    return nullptr;
  }
  if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
    return resolveDeclarationSymbolInStmt(whileNode->getBlock(), srcMgr, bufferId, declarationSpan);
  }
  if (auto const* forNode = dynamic_cast<const lesma::ForIn*>(stmt)) {
    lesma::Literal* ident = forNode->getIdentifier();
    if (ident != nullptr && smRangesEqual(srcMgr, bufferId, ident->getSpan(), declarationSpan)) {
      return ident->getResolvedSymbol();
    }
    return resolveDeclarationSymbolInStmt(forNode->getBlock(), srcMgr, bufferId, declarationSpan);
  }
  if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
    return resolveDeclarationSymbolInStmt(defer->getStatement(), srcMgr, bufferId, declarationSpan);
  }
  if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
    for (lesma::Statement* child : compound->getChildren()) {
      if (lesma::Value* value =
              resolveDeclarationSymbolInStmt(child, srcMgr, bufferId, declarationSpan)) {
        return value;
      }
    }
  }
  return nullptr;
}

auto resolveSymbolByDeclarationIdentity(const AnalysisResult& result,
                                        const lesma::IndexedDeclarationIdentity& declaration)
    -> std::optional<ResolvedSymbol> {
  std::optional<AnalysisView> analysis = findAnalysisViewForPath(result, declaration.filePath);
  if (!analysis || !isUsableAnalysis(*analysis)) {
    return std::nullopt;
  }
  for (lesma::Statement* stmt : analysis->ast->getChildren()) {
    if (lesma::Value* value =
            resolveDeclarationSymbolInStmt(stmt, analysis->sourceMgr, analysis->bufferId,
                                           declaration.span)) {
      return ResolvedSymbol{.value = value, .owner = *analysis};
    }
  }
  return std::nullopt;
}

auto locationForIndexedDeclaration(const AnalysisResult& result,
                                   const lesma::IndexedDeclarationIdentity& declaration)
    -> std::optional<::lsp::Location> {
  std::optional<AnalysisView> analysis = findAnalysisViewForPath(result, declaration.filePath);
  if (!analysis || !isUsableAnalysis(*analysis)) {
    return std::nullopt;
  }
  return ::lsp::Location{
      .uri = uriFromPath(declaration.filePath),
      .range = smRangeToLspRange(analysis->sourceMgr, analysis->bufferId, declaration.span),
  };
}

auto declarationBelongsToAnalysis(const AnalysisView& analysis,
                                  const lesma::IndexedDeclarationIdentity& declaration) -> bool {
  return analysis.mainFilePath != nullptr &&
         normalizePath(*analysis.mainFilePath) == normalizePath(declaration.filePath);
}

auto runAnalyzeAndPublish(const ::lsp::DocumentUri& uri,
                         const lesma::lsp_srv::DocumentStore& docStore, const std::string& content,
                         int version, AnalysisCache& analysisCache,
                         ::lsp::MessageHandler& messageHandler) -> void {
  DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(uri, docStore, content, version);
  AnalysisResult& result = snapshot.result;

  std::vector<::lsp::Diagnostic> lspDiagnostics;
  for (const auto& d : result.diagnostics) {
    ::lsp::Range range = smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, d.span);
    lspDiagnostics.push_back(::lsp::Diagnostic{
        .range = range,
        .message = d.message,
        .severity = ::lsp::Opt<::lsp::DiagnosticSeverityEnum>(::lsp::DiagnosticSeverity::Error),
    });
  }

  messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
      ::lsp::PublishDiagnosticsParams{
          .uri = uri,
          .diagnostics = std::move(lspDiagnostics),
      });
}

/** Get the class or enum name for a type by looking it up in the symbol table. */
auto getTypeName(lesma::Type* type, lesma::SymbolTable* rootScope) -> std::string {
  if (type == nullptr || rootScope == nullptr) {
    return "";
  }
  // For class and enum types, look up the name by finding the TYPE_SYMBOL Value
  // that has this type (classes/enums are stored as TYPE_SYMBOL values)
  if (type->is(lesma::BaseType::TY_CLASS) || type->is(lesma::BaseType::TY_ENUM)) {
    // Search through all symbols to find the TYPE_SYMBOL with this type
    std::function<lesma::Value*(lesma::SymbolTable*)> findTypeSymbol =
        [&](lesma::SymbolTable* scope) -> lesma::Value* {
      if (scope == nullptr) {
        return nullptr;
      }
      // Check symbols in this scope
      for (lesma::Value* sym : scope->getSymbols()) {
        if (sym->getCategory() == lesma::ValueCategory::TYPE_SYMBOL && sym->getType() == type) {
          return sym;
        }
      }
      // Recursively check parent scopes
      return findTypeSymbol(scope->getParent());
    };
    lesma::Value* typeSymbol = findTypeSymbol(rootScope);
    if (typeSymbol != nullptr) {
      return typeSymbol->getName();
    }
  }
  // Handle pointer types - get the element type name
  if (type->is(lesma::BaseType::TY_PTR) && type->getElementType() != nullptr) {
    lesma::Type* elementType = type->getElementType();
    if (elementType->is(lesma::BaseType::TY_CLASS) || elementType->is(lesma::BaseType::TY_ENUM)) {
      std::string elementName = getTypeName(elementType, rootScope);
      if (!elementName.empty()) {
        return elementName + "*";
      }
    }
  }
  return "";
}

auto formatTypeName(lesma::Type* type, lesma::SymbolTable* rootScope) -> std::string {
  if (type == nullptr) {
    return "?";
  }
  std::string namedType = getTypeName(type, rootScope);
  if (!namedType.empty()) {
    return namedType;
  }
  if (type->is(lesma::BaseType::TY_PTR) && type->getElementType() != nullptr) {
    return formatTypeName(type->getElementType(), rootScope) + "*";
  }
  if (type->is(lesma::BaseType::TY_ARRAY) && type->getElementType() != nullptr) {
    return "list<" + formatTypeName(type->getElementType(), rootScope) + ">";
  }
  if (type->is(lesma::BaseType::TY_INT)) {
    return type->isSigned() ? "int" : "uint";
  }
  if (type->is(lesma::BaseType::TY_FLOAT)) {
    return "float";
  }
  if (type->is(lesma::BaseType::TY_STRING)) {
    return "string";
  }
  if (type->is(lesma::BaseType::TY_BOOL)) {
    return "bool";
  }
  if (type->is(lesma::BaseType::TY_VOID)) {
    return "void";
  }
  return type->toString();
}

auto formatCallableHoverType(lesma::Type* type, lesma::SymbolTable* rootScope) -> std::string {
  if (type == nullptr || !type->is(lesma::BaseType::TY_FUNCTION)) {
    return formatTypeName(type, rootScope);
  }
  std::string out = "Function(";
  std::vector<lesma::Field*> fields = type->getFields();
  bool first = true;
  size_t paramOffset = (!fields.empty() && fields[0] != nullptr && fields[0]->name == "self") ? 1U : 0U;
  for (size_t i = paramOffset; i < fields.size(); ++i) {
    lesma::Field* field = fields[i];
    if (field == nullptr) {
      continue;
    }
    if (!first) {
      out += ", ";
    }
    first = false;
    out += field->name + ": " + formatTypeName(field->type, rootScope);
  }
  if (type->isVarArgs()) {
    if (!first) {
      out += ", ";
    }
    out += "...";
  }
  out += ")";
  if (lesma::Type* returnType = type->getReturnType();
      returnType != nullptr && !returnType->is(lesma::BaseType::TY_VOID)) {
    out += " -> " + formatTypeName(returnType, rootScope);
  }
  return out;
}

/** Build hover text from a symbol: name, kind, and type in readable Markdown. */
auto formatHoverContent(lesma::Value* value, lesma::SymbolTable* rootScope) -> std::string {
  if (value == nullptr) {
    return "";
  }
  lesma::Type* type = value->getType();
  std::string typeStr = type != nullptr ? type->toString() : "?";
  std::string const& name = value->getName();

  // For class/enum types, use the actual class/enum name instead of "Class"/"Enum"
  std::string typeName = getTypeName(type, rootScope);
  if (!typeName.empty()) {
    typeStr = typeName;
  }

  switch (value->getCategory()) {
  case lesma::ValueCategory::CALLABLE_SYMBOL:
    return "**" + name + "**\n\nType: `" + formatCallableHoverType(type, rootScope) + "`";
  case lesma::ValueCategory::TYPE_SYMBOL: {
    if (type != nullptr && type->is(lesma::BaseType::TY_GENERIC)) {
      return "type parameter `" + name + "`";
    }
    // For TYPE_SYMBOL (classes/enums), show as "enum `Name`" or "class `Name`"
    if (type != nullptr && type->is(lesma::BaseType::TY_ENUM)) {
      return "enum `" + name + "`";
    }
    return "class `" + name + "`";
  }
  case lesma::ValueCategory::MODULE_SYMBOL:
    return "**" + name + "**\n\n(import)";
  case lesma::ValueCategory::ADDRESSABLE_STORAGE:
  case lesma::ValueCategory::DIRECT_VALUE:
    return "**" + name + "**\n\nType: `" + typeStr + "`";
  }
  return "**" + name + "**\n\nType: `" + typeStr + "`";
}

auto isBuiltinTypeName(const std::string& name) -> bool {
  return name == "int" || name == "float" || name == "bool" || name == "str" || name == "void";
}

auto containsGenericParam(const std::vector<std::string>& genericParams, const std::string& name)
    -> bool {
  return std::ranges::find(genericParams, name) != genericParams.end();
}

auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) -> unsigned {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

struct InnermostFunc {
  const lesma::FuncDecl* func = nullptr;
  const lesma::Class* enclosingClass = nullptr;
};

auto considerFunc(const lesma::FuncDecl* f, const lesma::Class* cls, unsigned targetOffset,
                  llvm::SourceMgr* sm, unsigned bid, InnermostFunc& best, unsigned& bestLen)
    -> void {
  if (f->getBody() == nullptr) {
    return;
  }
  llvm::SMRange span = f->getBody()->getSpan();
  if (!span.isValid()) {
    return;
  }
  unsigned const a = getOffsetFromSMLoc(sm, bid, span.Start);
  unsigned const b = getOffsetFromSMLoc(sm, bid, span.End);
  if (targetOffset < a || targetOffset >= b) {
    return;
  }
  unsigned const len = b - a;
  if (best.func == nullptr || len < bestLen) {
    best.func = f;
    best.enclosingClass = cls;
    bestLen = len;
  }
}

auto scanCompoundForFuncs(lesma::Compound* c, lesma::Class* cls, unsigned targetOffset,
                          llvm::SourceMgr* sm, unsigned bid, InnermostFunc& best,
                          unsigned& bestLen) -> void {
  if (c == nullptr) {
    return;
  }
  for (lesma::Statement* s : c->getChildren()) {
    if (auto* f = dynamic_cast<lesma::FuncDecl*>(s)) {
      considerFunc(f, cls, targetOffset, sm, bid, best, bestLen);
      if (f->getBody() != nullptr) {
        scanCompoundForFuncs(f->getBody(), cls, targetOffset, sm, bid, best, bestLen);
      }
    } else if (auto* st = dynamic_cast<lesma::Class*>(s)) {
      for (lesma::FuncDecl* m : st->getMethods()) {
        considerFunc(m, st, targetOffset, sm, bid, best, bestLen);
        if (m->getBody() != nullptr) {
          scanCompoundForFuncs(m->getBody(), st, targetOffset, sm, bid, best, bestLen);
        }
      }
    }
  }
}

InnermostFunc findInnermostFuncContaining(lesma::Compound* ast, unsigned targetOffset,
                                          llvm::SourceMgr* sm, unsigned bid) {
  InnermostFunc best;
  unsigned bestLen = 0U;
  scanCompoundForFuncs(ast, nullptr, targetOffset, sm, bid, best, bestLen);
  return best;
}

/** Find function (or method) whose signature contains the cursor (name or any parameter).
 * Used so parameter names in "def foo(x: Int)" resolve to the parameter symbol. */
InnermostFunc findFuncWithCursorInSignature(lesma::Compound* ast, unsigned targetOffset,
                                            llvm::SourceMgr* sm, unsigned bid) {
  InnermostFunc out;
  std::function<void(lesma::Statement*, const lesma::Class*)> scan = [&](lesma::Statement* stmt,
                                                                         const lesma::Class* cls) {
    if (stmt == nullptr || out.func != nullptr) {
      return;
    }
    auto const inSpan = [&](llvm::SMRange span) -> bool {
      if (!span.isValid()) {
        return false;
      }
      unsigned a = getOffsetFromSMLoc(sm, bid, span.Start);
      unsigned b = getOffsetFromSMLoc(sm, bid, span.End);
      return targetOffset >= a && targetOffset < b;
    };
    if (auto const* f = dynamic_cast<const lesma::FuncDecl*>(stmt)) {
      if (inSpan(f->getSpan()) || inSpan(f->getNameSpan())) {
        out.func = f;
        out.enclosingClass = cls;
        return;
      }
      for (lesma::Parameter* p : f->getParameters()) {
        if (p != nullptr && inSpan(p->nameSpan)) {
          out.func = f;
          out.enclosingClass = cls;
          return;
        }
      }
      if (f->getBody() != nullptr) {
        for (lesma::Statement* s : f->getBody()->getChildren()) {
          scan(s, cls);
          if (out.func != nullptr) {
            return;
          }
        }
      }
      return;
    }
    if (dynamic_cast<const lesma::ExternFuncDecl*>(stmt) != nullptr) {
      return;
    }
    if (auto const* c = dynamic_cast<const lesma::Class*>(stmt)) {
      for (lesma::FuncDecl* m : c->getMethods()) {
        if (m != nullptr) {
          scan(m, c);
          if (out.func != nullptr) {
            return;
          }
        }
      }
    }
    if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
      for (lesma::Statement* s : compound->getChildren()) {
        scan(s, cls);
        if (out.func != nullptr) {
          return;
        }
      }
      return;
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
      for (lesma::Compound* block : ifNode->getBlocks()) {
        scan(block, cls);
        if (out.func != nullptr) {
          return;
        }
      }
      return;
    }
    if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
      scan(whileNode->getBlock(), cls);
      return;
    }
    if (auto const* forNode = dynamic_cast<const lesma::ForIn*>(stmt)) {
      scan(forNode->getBlock(), cls);
      return;
    }
    if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
      scan(defer->getStatement(), cls);
    }
  };
  for (lesma::Statement* s : ast->getChildren()) {
    scan(s, nullptr);
    if (out.func != nullptr) {
      break;
    }
  }
  return out;
}

auto findExternFuncWithCursorInSignature(const lesma::Compound* ast, unsigned targetOffset,
                                         llvm::SourceMgr* sm, unsigned bid)
    -> const lesma::ExternFuncDecl* {
  const lesma::ExternFuncDecl* out = nullptr;
  std::function<void(const lesma::Statement*)> scan = [&](const lesma::Statement* stmt) -> void {
    if (stmt == nullptr || out != nullptr) {
      return;
    }
    auto const inSpan = [&](llvm::SMRange span) -> bool {
      if (!span.isValid()) {
        return false;
      }
      unsigned a = getOffsetFromSMLoc(sm, bid, span.Start);
      unsigned b = getOffsetFromSMLoc(sm, bid, span.End);
      return targetOffset >= a && targetOffset < b;
    };
    if (auto const* f = dynamic_cast<const lesma::ExternFuncDecl*>(stmt)) {
      if (inSpan(f->getSpan()) || inSpan(f->getNameSpan())) {
        out = f;
        return;
      }
      for (lesma::Parameter* p : f->getParameters()) {
        if (p != nullptr && inSpan(p->nameSpan)) {
          out = f;
          return;
        }
      }
      return;
    }
    if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
      for (lesma::Statement* child : compound->getChildren()) {
        scan(child);
        if (out != nullptr) {
          return;
        }
      }
      return;
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
      for (lesma::Compound* block : ifNode->getBlocks()) {
        scan(block);
        if (out != nullptr) {
          return;
        }
      }
      return;
    }
    if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
      scan(whileNode->getBlock());
      return;
    }
    if (auto const* forNode = dynamic_cast<const lesma::ForIn*>(stmt)) {
      scan(forNode->getBlock());
      return;
    }
    if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
      scan(defer->getStatement());
    }
  };
  for (lesma::Statement* stmt : ast->getChildren()) {
    scan(stmt);
    if (out != nullptr) {
      break;
    }
  }
  return out;
}

auto findEnclosingClassContaining(const lesma::Compound* ast, unsigned targetOffset,
                                  llvm::SourceMgr* sm, unsigned bid) -> const lesma::Class* {
  const lesma::Class* out = nullptr;
  unsigned bestLen = 0U;
  std::function<void(const lesma::Statement*)> scan = [&](const lesma::Statement* stmt) -> void {
    if (stmt == nullptr) {
      return;
    }
    if (auto const* klass = dynamic_cast<const lesma::Class*>(stmt)) {
      llvm::SMRange span = klass->getSpan();
      if (span.isValid()) {
        unsigned a = getOffsetFromSMLoc(sm, bid, span.Start);
        unsigned b = getOffsetFromSMLoc(sm, bid, span.End);
        if (targetOffset >= a && targetOffset < b) {
          unsigned len = b - a;
          if (out == nullptr || len < bestLen) {
            out = klass;
            bestLen = len;
          }
        }
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        if (method != nullptr && method->getBody() != nullptr) {
          for (lesma::Statement* child : method->getBody()->getChildren()) {
            scan(child);
          }
        }
      }
      return;
    }
    if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
      for (lesma::Statement* child : compound->getChildren()) {
        scan(child);
      }
      return;
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
      for (lesma::Compound* block : ifNode->getBlocks()) {
        scan(block);
      }
      return;
    }
    if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
      scan(whileNode->getBlock());
      return;
    }
    if (auto const* forNode = dynamic_cast<const lesma::ForIn*>(stmt)) {
      scan(forNode->getBlock());
      return;
    }
    if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
      scan(defer->getStatement());
    }
  };
  for (lesma::Statement* stmt : ast->getChildren()) {
    scan(stmt);
  }
  return out;
}

auto lookupNameInCallableContext(const lesma::FuncLikeDeclView& view, lesma::SymbolTable* root,
                                 const std::string& name, bool isTypePosition) -> lesma::Value* {
  lesma::Value* funcSym = view.resolvedSymbol;
  if (funcSym == nullptr && root != nullptr) {
    funcSym = root->lookup(view.name);
  }
  if (funcSym != nullptr && name == view.name) {
    return funcSym;
  }
  if (isTypePosition && view.genericScope != nullptr) {
    if (lesma::Value* value = view.genericScope->lookup(name)) {
      return value;
    }
  }
  if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
    if (lesma::Value* value = funcSym->getBodyScope()->lookup(name)) {
      return value;
    }
  }
  return nullptr;
}

auto lookupValueForHover(lesma::Compound* ast, lesma::SymbolTable* root,
                         llvm::SourceMgr* srcMgr, unsigned bufferId, unsigned line,
                         unsigned character, const std::string& name, bool isTypePosition)
    -> lesma::Value* {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  auto const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));

  // If cursor is in a function's signature (e.g. on a parameter or function name), use the resolved
  // symbol. Use the symbol the typechecker resolved for this exact overload (getResolvedSymbol).
  InnermostFunc const sigFunc = findFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId);
  if (sigFunc.func != nullptr) {
    if (lesma::Value* value =
            lookupNameInCallableContext(lesma::makeFuncLikeDeclView(sigFunc.func), root, name,
                                        isTypePosition)) {
      return value;
    }
  }

  if (auto const* sigExtern =
          findExternFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId)) {
    if (lesma::Value* value =
            lookupNameInCallableContext(lesma::makeFuncLikeDeclView(sigExtern), root, name,
                                        isTypePosition)) {
      return value;
    }
  }

  // Cursor in a function body: use the innermost function's resolved symbol (correct overload).
  InnermostFunc const inner = findInnermostFuncContaining(ast, targetOffset, srcMgr, bufferId);
  if (inner.func != nullptr) {
    if (lesma::Value* value =
            lookupNameInCallableContext(lesma::makeFuncLikeDeclView(inner.func), root, name,
                                        isTypePosition)) {
      return value;
    }
  }
  if (auto const* enclosingClass =
          findEnclosingClassContaining(ast, targetOffset, srcMgr, bufferId)) {
    if (isTypePosition && enclosingClass->getGenericScope() != nullptr) {
      if (lesma::Value* v = enclosingClass->getGenericScope()->lookup(name)) {
        return v;
      }
    }
  }
  return root->lookup(name);
}

/** Identifier at cursor plus optional "dot base" and source range (for hover.range to avoid
 * duplicate symbol in UI). */
struct CursorIdentifier {
  std::string name;
  std::optional<std::string> dotBase;
  std::optional<::lsp::Range> range;
  bool isTypePosition = false;
};

auto optionalLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span)
    -> std::optional<::lsp::Range> {
  return span.isValid() ? std::optional<::lsp::Range>(smRangeToLspRange(srcMgr, bufferId, span))
                        : std::nullopt;
}

auto makeCursorIdentifier(const std::string& name, std::optional<std::string> dotBase,
                          std::optional<::lsp::Range> range, bool isTypePosition = false)
    -> CursorIdentifier {
  return CursorIdentifier{
      .name = name,
      .dotBase = std::move(dotBase),
      .range = range,
      .isTypePosition = isTypePosition,
  };
}

auto makeCursorIdentifierFromSpan(const std::string& name, llvm::SMRange span,
                                  llvm::SourceMgr* srcMgr, unsigned bufferId,
                                  bool isTypePosition = false,
                                  std::optional<std::string> dotBase = std::nullopt)
    -> CursorIdentifier {
  return makeCursorIdentifier(name, std::move(dotBase), optionalLspRange(srcMgr, bufferId, span),
                              isTypePosition);
}

auto findIndexedSymbolOccurrenceAtCursor(const AnalysisView& analysis, unsigned line,
                                         unsigned character) -> const lesma::IndexedSymbolOccurrence* {
  if (!isUsableAnalysis(analysis) || analysis.index == nullptr) {
    return nullptr;
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
  const lesma::IndexedSymbolOccurrence* best = nullptr;
  unsigned bestLen = 0U;
  bool bestContains = false;
  for (const lesma::IndexedSymbolOccurrence& occurrence : analysis.index->symbolOccurrences) {
    if (!occurrence.span.isValid()) {
      continue;
    }
    unsigned const start = getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.Start);
    unsigned const end = getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.End);
    bool const contains = targetOffset >= start && targetOffset < end;
    bool const justAfter = targetOffset == end;
    if (!contains && !justAfter) {
      continue;
    }
    unsigned const len = end > start ? end - start : 0U;
    if (best == nullptr || (contains && !bestContains) || (contains == bestContains && len < bestLen)) {
      best = &occurrence;
      bestLen = len;
      bestContains = contains;
    }
  }
  return best;
}

/** Find identifier at cursor position by walking AST to find the identifier token. */
auto findIdentifierAtCursor(const AnalysisView& analysis, unsigned line, unsigned character)
    -> std::optional<CursorIdentifier> {
  const lesma::IndexedSymbolOccurrence* best =
      findIndexedSymbolOccurrenceAtCursor(analysis, line, character);
  if (best == nullptr) {
    return std::nullopt;
  }
  return makeCursorIdentifierFromSpan(best->name, best->span, analysis.sourceMgr, analysis.bufferId,
                                      best->isTypePosition, best->dotBase);
}

auto collectSemanticTokens(const AnalysisResult& analysisResult, unsigned bufferId)
    -> std::vector<std::uint32_t>;

auto collectInlayHints(const AnalysisResult& analysisResult, unsigned bufferId,
                       const ::lsp::Range& range) -> std::vector<::lsp::InlayHint> {
  std::vector<::lsp::InlayHint> hints;
  llvm::SourceMgr* srcMgr = analysisResult.sourceMgr.get();
  lesma::SymbolTable* root = analysisResult.rootScope.get();
  lesma::Compound* ast = analysisResult.parser ? analysisResult.parser->getAst() : nullptr;
  if (srcMgr == nullptr || root == nullptr || ast == nullptr) {
    return hints;
  }

  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return hints;
  }

  llvm::StringRef const text = buf->getBuffer();
  unsigned const rangeStart = static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspPosition(
      text, range.start.line, range.start.character));
  unsigned const rangeEnd = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(text, range.end.line, range.end.character));

  auto isInRequestedRange = [&](llvm::SMRange span) -> bool {
    if (!span.isValid()) {
      return false;
    }
    unsigned const offset = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
    return offset >= rangeStart && offset <= rangeEnd;
  };

  auto maybeAddVarHint = [&](const lesma::VarDecl* varDecl) -> void {
    if (varDecl == nullptr || varDecl->getType() != nullptr) {
      return;
    }
    lesma::Literal* ident = varDecl->getIdentifier();
    lesma::Value* symbol = varDecl->getResolvedSymbol();
    if (ident == nullptr || symbol == nullptr || symbol->getType() == nullptr) {
      return;
    }
    llvm::SMRange span = ident->getSpan();
    if (!span.isValid() || !isInRequestedRange(span)) {
      return;
    }

    ::lsp::Range identRange = smRangeToLspRange(srcMgr, bufferId, span);
    ::lsp::Position insertPos = identRange.end;
    std::string typeText = ": " + formatTypeName(symbol->getType(), root);

    ::lsp::InlayHint hint;
    hint.position = insertPos;
    hint.label = ::lsp::String(typeText);
    hint.kind = ::lsp::Opt<::lsp::InlayHintKindEnum>(::lsp::InlayHintKind::Type);
    hint.textEdits = ::lsp::Opt<::lsp::Array<::lsp::TextEdit>>({::lsp::TextEdit{
        .range = ::lsp::Range{.start = insertPos, .end = insertPos},
        .newText = typeText,
    }});
    hint.tooltip = ::lsp::Opt<::lsp::OneOf<::lsp::String, ::lsp::MarkupContent>>(
        ::lsp::String("Insert inferred type annotation"));
    hints.push_back(std::move(hint));
  };

  std::function<void(const lesma::Statement*)> visitStmt =
      [&](const lesma::Statement* node) -> void {
    if (node == nullptr) {
      return;
    }
    if (auto const* varDecl = dynamic_cast<const lesma::VarDecl*>(node)) {
      maybeAddVarHint(varDecl);
      return;
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(node)) {
      for (lesma::Compound* block : ifNode->getBlocks()) {
        if (block != nullptr) {
          for (lesma::Statement* s : block->getChildren()) {
            visitStmt(s);
          }
        }
      }
      return;
    }
    if (auto const* whileNode = dynamic_cast<const lesma::While*>(node)) {
      if (whileNode->getBlock() != nullptr) {
        for (lesma::Statement* s : whileNode->getBlock()->getChildren()) {
          visitStmt(s);
        }
      }
      return;
    }
    if (auto const* compound = dynamic_cast<const lesma::Compound*>(node)) {
      for (lesma::Statement* s : compound->getChildren()) {
        visitStmt(s);
      }
      return;
    }
    if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(node)) {
      if (func->getBody() != nullptr) {
        for (lesma::Statement* s : func->getBody()->getChildren()) {
          visitStmt(s);
        }
      }
      return;
    }
    if (auto const* klass = dynamic_cast<const lesma::Class*>(node)) {
      for (lesma::VarDecl* field : klass->getFields()) {
        maybeAddVarHint(field);
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        visitStmt(method);
      }
    }
  };

  for (lesma::Statement* stmt : ast->getChildren()) {
    visitStmt(stmt);
  }

  return hints;
}

auto activeScopeForOffset(lesma::Compound* ast, lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                          unsigned bufferId, unsigned targetOffset) -> lesma::SymbolTable* {
  if (ast == nullptr || root == nullptr || srcMgr == nullptr) {
    return root;
  }
  InnermostFunc inner = findInnermostFuncContaining(ast, targetOffset, srcMgr, bufferId);
  if (inner.func != nullptr) {
    if (lesma::Value* funcSym = inner.func->getResolvedSymbol()) {
      if (funcSym->getBodyScope() != nullptr) {
        return funcSym->getBodyScope();
      }
    }
  }
  return root;
}

struct ActiveCallSite {
  const lesma::FuncCall* call = nullptr;
  const lesma::Expression* receiver = nullptr;
  unsigned spanLength = 0U;
};

auto resolveCanonicalSymbolAtCursor(const AnalysisResult& result, unsigned line, unsigned character,
                                    const CursorIdentifier& id) -> std::optional<ResolvedSymbol>;
auto findEnumMemberDeclarationAtCursor(const AnalysisView& analysis, unsigned line,
                                       unsigned character,
                                       const std::string& memberName) -> std::optional<std::string>;

auto resolveExpressionTypeAtOffset(const lesma::Expression* expr, lesma::Compound* ast,
                                   lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                                   unsigned bufferId, unsigned targetOffset) -> lesma::Type*;

auto resolveMethodReturnType(const lesma::FuncCall* call, const lesma::Expression* receiver,
                             lesma::Compound* ast, lesma::SymbolTable* root,
                             llvm::SourceMgr* srcMgr, unsigned bufferId, unsigned targetOffset)
    -> lesma::Type* {
  if (call == nullptr || receiver == nullptr || root == nullptr) {
    return nullptr;
  }
  lesma::Type* receiverType =
      resolveExpressionTypeAtOffset(receiver, ast, root, srcMgr, bufferId, targetOffset);
  if (receiverType == nullptr) {
    return nullptr;
  }
  if (receiverType->is(lesma::BaseType::TY_PTR) && receiverType->getElementType() != nullptr) {
    receiverType = receiverType->getElementType();
  }
  lesma::Type* selfType = receiverType;
  if (!selfType->is(lesma::BaseType::TY_PTR)) {
    selfType = nullptr;
  }
  std::vector<lesma::Type*> argTypes;
  if (selfType != nullptr) {
    argTypes.push_back(selfType);
  }
  for (lesma::Expression* arg : call->getArguments()) {
    lesma::Type* argType =
        resolveExpressionTypeAtOffset(arg, ast, root, srcMgr, bufferId, targetOffset);
    if (argType == nullptr) {
      return nullptr;
    }
    if (argType->is(lesma::BaseType::TY_CLASS)) {
      return nullptr;
    }
    argTypes.push_back(argType);
  }
  lesma::SymbolTable* scope = activeScopeForOffset(ast, root, srcMgr, bufferId, targetOffset);
  if (scope == nullptr) {
    scope = root;
  }
  lesma::Value* method = scope->lookupFunction(call->getName(), argTypes);
  if (method == nullptr || method->getType() == nullptr) {
    return nullptr;
  }
  return method->getType()->getReturnType();
}

auto resolveExpressionTypeAtOffset(const lesma::Expression* expr, lesma::Compound* ast,
                                   lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                                   unsigned bufferId, unsigned targetOffset) -> lesma::Type* {
  if (expr == nullptr || root == nullptr) {
    return nullptr;
  }
  if (auto const* lit = dynamic_cast<const lesma::Literal*>(expr)) {
    switch (lit->getType()) {
    case lesma::TokenType::BOOL:
    case lesma::TokenType::BOOL_TYPE:
      return root->lookupType("bool");
    case lesma::TokenType::INTEGER:
    case lesma::TokenType::INT_TYPE:
      return root->lookupType("int");
    case lesma::TokenType::DOUBLE:
    case lesma::TokenType::FLOAT_TYPE:
      return root->lookupType("float");
    case lesma::TokenType::STRING:
    case lesma::TokenType::STRING_TYPE:
      return root->lookupType("str");
    case lesma::TokenType::IDENTIFIER: {
      lesma::SymbolTable* scope = activeScopeForOffset(ast, root, srcMgr, bufferId, targetOffset);
      if (scope == nullptr) {
        scope = root;
      }
      lesma::Value* value = scope->lookup(lit->getValue());
      return value != nullptr ? value->getType() : nullptr;
    }
    default:
      return nullptr;
    }
  }
  if (auto const* call = dynamic_cast<const lesma::FuncCall*>(expr)) {
    std::vector<lesma::Type*> argTypes;
    for (lesma::Expression* arg : call->getArguments()) {
      lesma::Type* argType =
          resolveExpressionTypeAtOffset(arg, ast, root, srcMgr, bufferId, targetOffset);
      if (argType == nullptr) {
        return nullptr;
      }
      if (argType->is(lesma::BaseType::TY_CLASS)) {
        return nullptr;
      }
      argTypes.push_back(argType);
    }
    lesma::SymbolTable* scope = activeScopeForOffset(ast, root, srcMgr, bufferId, targetOffset);
    if (scope == nullptr) {
      scope = root;
    }
    lesma::Value* callee = scope->lookupFunction(call->getName(), argTypes);
    if (callee != nullptr && callee->getType() != nullptr) {
      return callee->getType()->getReturnType();
    }
    return nullptr;
  }
  if (auto const* dot = dynamic_cast<const lesma::DotOp*>(expr)) {
    lesma::Type* baseType =
        resolveExpressionTypeAtOffset(dot->getLeft(), ast, root, srcMgr, bufferId, targetOffset);
    if (baseType == nullptr) {
      return nullptr;
    }
    if (auto const* memberCall = dynamic_cast<const lesma::FuncCall*>(dot->getRight())) {
      return resolveMethodReturnType(memberCall, dot->getLeft(), ast, root, srcMgr, bufferId,
                                     targetOffset);
    }
    if (baseType->is(lesma::BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
      baseType = baseType->getElementType();
    }
    if (auto const* rightLit = dynamic_cast<const lesma::Literal*>(dot->getRight())) {
      if (rightLit->getType() == lesma::TokenType::IDENTIFIER) {
        return lesma::TypeUtils::findTypeInFields(baseType, rightLit->getValue());
      }
    }
    return nullptr;
  }
  return nullptr;
}

auto findActiveCallInExpr(const lesma::Expression* expr, llvm::SourceMgr* srcMgr,
                          unsigned bufferId, unsigned targetOffset,
                          const lesma::Expression* receiver, ActiveCallSite& best) -> void {
  if (expr == nullptr) {
    return;
  }
  llvm::SMRange span = expr->getSpan();
  if (!span.isValid()) {
    return;
  }
  unsigned const start = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
  unsigned const end = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
  if (targetOffset < start || targetOffset > end) {
    return;
  }

  if (auto const* call = dynamic_cast<const lesma::FuncCall*>(expr)) {
    unsigned const len = end - start;
    if (best.call == nullptr || len < best.spanLength) {
      best.call = call;
      best.receiver = receiver;
      best.spanLength = len;
    }
    for (lesma::Expression* arg : call->getArguments()) {
      findActiveCallInExpr(arg, srcMgr, bufferId, targetOffset, nullptr, best);
    }
    return;
  }
  if (auto const* dot = dynamic_cast<const lesma::DotOp*>(expr)) {
    if (dot->getRight() != nullptr) {
      findActiveCallInExpr(dot->getRight(), srcMgr, bufferId, targetOffset, dot->getLeft(), best);
    }
    if (dot->getLeft() != nullptr) {
      findActiveCallInExpr(dot->getLeft(), srcMgr, bufferId, targetOffset, nullptr, best);
    }
    return;
  }
  if (auto const* binary = dynamic_cast<const lesma::BinaryOp*>(expr)) {
    findActiveCallInExpr(binary->getLeft(), srcMgr, bufferId, targetOffset, nullptr, best);
    findActiveCallInExpr(binary->getRight(), srcMgr, bufferId, targetOffset, nullptr, best);
    return;
  }
  if (auto const* unary = dynamic_cast<const lesma::UnaryOp*>(expr)) {
    findActiveCallInExpr(unary->getExpression(), srcMgr, bufferId, targetOffset, nullptr, best);
    return;
  }
  if (auto const* castOp = dynamic_cast<const lesma::CastOp*>(expr)) {
    findActiveCallInExpr(castOp->getExpression(), srcMgr, bufferId, targetOffset, nullptr, best);
    return;
  }
  if (auto const* isOp = dynamic_cast<const lesma::IsOp*>(expr)) {
    findActiveCallInExpr(isOp->getLeft(), srcMgr, bufferId, targetOffset, nullptr, best);
  }
}

auto findActiveCallInStmt(const lesma::Statement* stmt, llvm::SourceMgr* srcMgr,
                          unsigned bufferId, unsigned targetOffset, ActiveCallSite& best)
    -> void {
  if (stmt == nullptr) {
    return;
  }
  if (auto const* exprStmt = dynamic_cast<const lesma::ExpressionStatement*>(stmt)) {
    findActiveCallInExpr(exprStmt->getExpression(), srcMgr, bufferId, targetOffset, nullptr, best);
  } else if (auto const* varDecl = dynamic_cast<const lesma::VarDecl*>(stmt)) {
    findActiveCallInExpr(varDecl->getValue(), srcMgr, bufferId, targetOffset, nullptr, best);
  } else if (auto const* assign = dynamic_cast<const lesma::Assignment*>(stmt)) {
    findActiveCallInExpr(assign->getLeftHandSide(), srcMgr, bufferId, targetOffset, nullptr, best);
    findActiveCallInExpr(assign->getRightHandSide(), srcMgr, bufferId, targetOffset, nullptr, best);
  } else if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
    for (lesma::Expression* cond : ifNode->getConds()) {
      findActiveCallInExpr(cond, srcMgr, bufferId, targetOffset, nullptr, best);
    }
    for (lesma::Compound* block : ifNode->getBlocks()) {
      findActiveCallInStmt(block, srcMgr, bufferId, targetOffset, best);
    }
  } else if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
    findActiveCallInExpr(whileNode->getCond(), srcMgr, bufferId, targetOffset, nullptr, best);
    findActiveCallInStmt(whileNode->getBlock(), srcMgr, bufferId, targetOffset, best);
  } else if (auto const* ret = dynamic_cast<const lesma::Return*>(stmt)) {
    findActiveCallInExpr(ret->getValue(), srcMgr, bufferId, targetOffset, nullptr, best);
  } else if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
    findActiveCallInStmt(defer->getStatement(), srcMgr, bufferId, targetOffset, best);
  } else if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
    for (lesma::Statement* child : compound->getChildren()) {
      findActiveCallInStmt(child, srcMgr, bufferId, targetOffset, best);
    }
  } else if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(stmt)) {
    findActiveCallInStmt(func->getBody(), srcMgr, bufferId, targetOffset, best);
  } else if (auto const* klass = dynamic_cast<const lesma::Class*>(stmt)) {
    for (lesma::FuncDecl* method : klass->getMethods()) {
      findActiveCallInStmt(method, srcMgr, bufferId, targetOffset, best);
    }
  }
}

auto findActiveCallSite(const AnalysisView& analysis, unsigned line, unsigned character)
    -> std::optional<ActiveCallSite> {
  if (!isUsableAnalysis(analysis)) {
    return std::nullopt;
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));

  ActiveCallSite best;
  for (lesma::Statement* stmt : analysis.ast->getChildren()) {
    findActiveCallInStmt(stmt, analysis.sourceMgr, analysis.bufferId, targetOffset, best);
  }
  if (best.call == nullptr) {
    return std::nullopt;
  }
  llvm::SMRange span = best.call->getSpan();
  if (!span.isValid()) {
    return std::nullopt;
  }
  unsigned const start = getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, span.Start);
  unsigned const end = getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, span.End);
  if (targetOffset < start || targetOffset > end) {
    return std::nullopt;
  }
  return best;
}

auto activeParameterIndex(const lesma::FuncCall* call, llvm::StringRef text, unsigned targetOffset,
                          llvm::SourceMgr* srcMgr, unsigned bufferId) -> unsigned {
  if (call == nullptr) {
    return 0U;
  }
  llvm::SMRange span = call->getSpan();
  if (!span.isValid()) {
    return 0U;
  }
  unsigned const start = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
  unsigned const end = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
  unsigned const scanEnd = std::min(targetOffset, end);
  size_t openParen = text.find('(', static_cast<size_t>(start + call->getName().size()));
  if (openParen == llvm::StringRef::npos || openParen >= scanEnd) {
    return 0U;
  }
  unsigned argIndex = 0U;
  int depth = 0;
  for (size_t i = openParen + 1U; i < static_cast<size_t>(scanEnd); ++i) {
    char ch = text[i];
    if (ch == '(' || ch == '[' || ch == '{') {
      ++depth;
    } else if (ch == ')' || ch == ']' || ch == '}') {
      if (depth > 0) {
        --depth;
      }
    } else if (ch == ',' && depth == 0) {
      ++argIndex;
    }
  }
  return argIndex;
}

struct CallableCandidate {
  lesma::Value* value = nullptr;
  unsigned paramOffset = 0U;
};

auto receiverMatchesSelf(lesma::Type* receiverType, lesma::Type* selfType) -> bool {
  if (receiverType == nullptr || selfType == nullptr) {
    return false;
  }
  if (receiverType->is(lesma::BaseType::TY_CLASS)) {
    return false;
  }
  return receiverType->isEqual(selfType) ||
         (receiverType->is(lesma::BaseType::TY_PTR) && receiverType->getElementType() != nullptr &&
          receiverType->isEqual(selfType));
}

auto collectCallableCandidates(lesma::SymbolTable* scope, const std::string& name,
                               lesma::Type* receiverType,
                               const std::vector<lesma::Type*>& typedArgs)
    -> std::vector<CallableCandidate> {
  std::vector<CallableCandidate> candidates;
  std::unordered_set<lesma::Value*> seen;
  for (lesma::SymbolTable* current = scope; current != nullptr; current = current->getParent()) {
    for (lesma::Value* sym : current->getSymbols()) {
      if (sym == nullptr || sym->getCategory() != lesma::ValueCategory::CALLABLE_SYMBOL ||
          sym->getName() != name || sym->getType() == nullptr ||
          !sym->getType()->is(lesma::BaseType::TY_FUNCTION) || !seen.insert(sym).second) {
        continue;
      }
      std::vector<lesma::Field*> fields = sym->getType()->getFields();
      unsigned paramOffset = 0U;
      if (receiverType != nullptr) {
        if (fields.empty() || fields[0] == nullptr || fields[0]->name != "self" ||
            !receiverMatchesSelf(receiverType, fields[0]->type)) {
          continue;
        }
        paramOffset = 1U;
      } else if (!fields.empty() && fields[0] != nullptr && fields[0]->name == "self") {
        continue;
      }
      if (typedArgs.size() > fields.size() - paramOffset && !sym->getType()->isVarArgs()) {
        continue;
      }
      bool matches = true;
      for (size_t i = 0; i < typedArgs.size(); ++i) {
        if (paramOffset + i >= fields.size()) {
          matches = sym->getType()->isVarArgs();
          break;
        }
        lesma::Type* expected = fields[paramOffset + i]->type;
        lesma::Type* actual = typedArgs[i];
        if (expected == nullptr || actual == nullptr) {
          continue;
        }
        if (!expected->isEqual(actual) && !expected->is(lesma::BaseType::TY_GENERIC)) {
          matches = false;
          break;
        }
      }
      if (matches) {
        candidates.push_back(CallableCandidate{.value = sym, .paramOffset = paramOffset});
      }
    }
  }
  return candidates;
}

auto buildSignatureHelp(const AnalysisResult& result, unsigned line, unsigned character)
    -> std::optional<::lsp::SignatureHelp> {
  AnalysisView analysis = makeAnalysisView(result);
  if (!isUsableAnalysis(analysis)) {
    return std::nullopt;
  }
  std::optional<ActiveCallSite> activeCall = findActiveCallSite(analysis, line, character);
  if (!activeCall || activeCall->call == nullptr) {
    return std::nullopt;
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  llvm::StringRef text = buf->getBuffer();
  unsigned const targetOffset =
      static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspPosition(text, line, character));
  lesma::SymbolTable* scope = activeScopeForOffset(
      analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId, targetOffset);
  if (scope == nullptr) {
    scope = analysis.rootScope;
  }

  std::vector<lesma::Type*> argTypes;
  for (lesma::Expression* arg : activeCall->call->getArguments()) {
    lesma::Type* argType = resolveExpressionTypeAtOffset(
        arg, analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId, targetOffset);
    if (argType != nullptr) {
      argTypes.push_back(argType);
    }
  }
  lesma::Type* receiverType = nullptr;
  if (activeCall->receiver != nullptr) {
    receiverType =
        resolveExpressionTypeAtOffset(activeCall->receiver, analysis.ast, analysis.rootScope,
                                      analysis.sourceMgr, analysis.bufferId, targetOffset);
  }

  std::vector<CallableCandidate> candidates =
      collectCallableCandidates(scope, activeCall->call->getName(), receiverType, argTypes);
  if (candidates.empty()) {
    return std::nullopt;
  }

  unsigned const activeParam = activeParameterIndex(activeCall->call, text, targetOffset,
                                                    analysis.sourceMgr, analysis.bufferId);
  ::lsp::SignatureHelp help{
      .signatures = {},
      .activeSignature = ::lsp::Opt<unsigned>(0U),
      .activeParameter = ::lsp::Opt<unsigned>(activeParam),
  };

  for (const CallableCandidate& candidate : candidates) {
    if (candidate.value == nullptr || candidate.value->getType() == nullptr) {
      continue;
    }
    std::vector<lesma::Field*> fields = candidate.value->getType()->getFields();
    std::string label = candidate.value->getName() + "(";
    ::lsp::Array<::lsp::ParameterInformation> params;
    bool first = true;
    for (size_t i = candidate.paramOffset; i < fields.size(); ++i) {
      lesma::Field* field = fields[i];
      if (field == nullptr) {
        continue;
      }
      std::string paramLabel = field->name + ": " + formatTypeName(field->type, analysis.rootScope);
      if (!first) {
        label += ", ";
      }
      first = false;
      label += paramLabel;
      ::lsp::ParameterInformation paramInfo;
      paramInfo.label = ::lsp::String(paramLabel);
      params.push_back(std::move(paramInfo));
    }
    label += ")";
    lesma::Type* returnType = candidate.value->getType()->getReturnType();
    if (returnType != nullptr && !returnType->is(lesma::BaseType::TY_VOID)) {
      label += " -> " + formatTypeName(returnType, analysis.rootScope);
    }
    ::lsp::SignatureInformation sig;
    sig.label = label;
    sig.parameters = ::lsp::Opt<::lsp::Array<::lsp::ParameterInformation>>(std::move(params));
    sig.activeParameter = ::lsp::Opt<unsigned>(activeParam);
    help.signatures.push_back(std::move(sig));
  }
  if (help.signatures.empty()) {
    return std::nullopt;
  }
  return help;
}

auto lookupFunctionOrSymbol(lesma::SymbolTable* scope, const std::string& name,
                            const std::vector<lesma::Type*>& argTypes) -> lesma::Value* {
  if (scope == nullptr) {
    return nullptr;
  }
  if (lesma::Value* function = scope->lookupFunction(name, argTypes)) {
    return function;
  }
  return scope->lookup(name);
}

auto resolveCallArgumentTypesAtCursor(const AnalysisView& analysis, unsigned line,
                                      unsigned character, const CursorIdentifier& id)
    -> std::optional<std::vector<lesma::Type*>> {
  std::optional<ActiveCallSite> activeCall = findActiveCallSite(analysis, line, character);
  if (!activeCall || activeCall->call == nullptr || activeCall->call->getName() != id.name) {
    return std::nullopt;
  }
  if (id.dotBase.has_value()) {
    auto const* receiverLit = dynamic_cast<const lesma::Literal*>(activeCall->receiver);
    if (receiverLit == nullptr || receiverLit->getType() != lesma::TokenType::IDENTIFIER ||
        receiverLit->getValue() != *id.dotBase) {
      return std::nullopt;
    }
  } else if (activeCall->receiver != nullptr) {
    return std::nullopt;
  }

  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
  std::vector<lesma::Type*> argTypes;
  for (lesma::Expression* arg : activeCall->call->getArguments()) {
    lesma::Type* argType = resolveExpressionTypeAtOffset(
        arg, analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId, targetOffset);
    if (argType == nullptr) {
      return std::nullopt;
    }
    argTypes.push_back(argType);
  }
  return argTypes;
}

auto resolveImportedSymbol(const AnalysisResult& result, const AnalysisView& analysis,
                           const std::string& modulePath, const std::string& symbolName,
                           unsigned line, unsigned character, const CursorIdentifier& id)
    -> std::optional<ResolvedSymbol> {
  std::optional<AnalysisView> targetAnalysis = findAnalysisViewForPath(result, modulePath);
  if (!targetAnalysis || !isUsableAnalysis(*targetAnalysis)) {
    return std::nullopt;
  }
  std::optional<std::vector<lesma::Type*>> argTypes =
      resolveCallArgumentTypesAtCursor(analysis, line, character, id);
  lesma::Value* resolved = lookupFunctionOrSymbol(targetAnalysis->rootScope, symbolName,
                                                  argTypes.value_or(std::vector<lesma::Type*>{}));
  if (resolved == nullptr) {
    return std::nullopt;
  }
  return ResolvedSymbol{.value = resolved, .owner = *targetAnalysis};
}

auto resolveCanonicalSymbolAtCursor(const AnalysisResult& result, const AnalysisView& analysis,
                                    unsigned line, unsigned character, const CursorIdentifier& id)
    -> std::optional<ResolvedSymbol> {
  if (!isUsableAnalysis(analysis)) {
    return std::nullopt;
  }
  if (const lesma::IndexedSymbolOccurrence* occurrence =
          findIndexedSymbolOccurrenceAtCursor(analysis, line, character);
      occurrence != nullptr && occurrence->declaration.has_value() &&
      declarationBelongsToAnalysis(analysis, *occurrence->declaration)) {
    if (std::optional<ResolvedSymbol> resolved =
            resolveSymbolByDeclarationIdentity(result, *occurrence->declaration)) {
      return resolved;
    }
  }

  if (id.dotBase.has_value()) {
    if (std::optional<std::string> modulePath =
            findModuleImportPathByAlias(analysis, *id.dotBase)) {
      if (std::optional<ResolvedSymbol> imported =
              resolveImportedSymbol(result, analysis, *modulePath, id.name, line, character, id)) {
        return imported;
      }
    }
  }

  lesma::Value* local =
      lookupValueForHover(analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId,
                          line, character, id.name, id.isTypePosition);
  if (local != nullptr && local->getType() != nullptr &&
      !local->getType()->is(lesma::BaseType::TY_IMPORT)) {
    return ResolvedSymbol{.value = local, .owner = analysis};
  }

  if (!id.dotBase.has_value() && analysis.importedNameToSource != nullptr) {
    auto importedIt = analysis.importedNameToSource->find(id.name);
    if (importedIt != analysis.importedNameToSource->end()) {
      if (std::optional<ResolvedSymbol> imported =
              resolveImportedSymbol(result, analysis, importedIt->second.first,
                                    importedIt->second.second, line, character, id)) {
        return imported;
      }
    }
  }

  if (local != nullptr) {
    return ResolvedSymbol{.value = local, .owner = analysis};
  }
  return std::nullopt;
}

auto resolveCanonicalSymbolAtCursor(const AnalysisResult& result, unsigned line, unsigned character,
                                    const CursorIdentifier& id) -> std::optional<ResolvedSymbol> {
  return resolveCanonicalSymbolAtCursor(result, makeAnalysisView(result), line, character, id);
}

auto symbolIdentityForResolved(const ResolvedSymbol& resolved) -> std::optional<SymbolIdentity> {
  if (resolved.value == nullptr || !isUsableAnalysis(resolved.owner)) {
    return std::nullopt;
  }
  llvm::SMRange declSpan = resolved.value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    return std::nullopt;
  }
  std::string path = resolved.value->getDeclarationFilePath();
  if (path.empty() && resolved.owner.mainFilePath != nullptr) {
    path = *resolved.owner.mainFilePath;
  }
  return SymbolIdentity{
      .path = normalizePath(path),
      .range = smRangeToLspRange(resolved.owner.sourceMgr, resolved.owner.bufferId, declSpan),
      .name = resolved.value->getName(),
  };
}

auto symbolIdentityForIndexedDeclaration(const AnalysisResult& result,
                                         const lesma::IndexedDeclarationIdentity& declaration,
                                         const std::string& name) -> std::optional<SymbolIdentity> {
  std::optional<::lsp::Location> location = locationForIndexedDeclaration(result, declaration);
  if (!location) {
    return std::nullopt;
  }
  return SymbolIdentity{
      .path = normalizePath(declaration.filePath),
      .range = location->range,
      .name = name,
  };
}

auto rangeEquals(const ::lsp::Range& lhs, const ::lsp::Range& rhs) -> bool {
  return lhs.start.line == rhs.start.line && lhs.start.character == rhs.start.character &&
         lhs.end.line == rhs.end.line && lhs.end.character == rhs.end.character;
}

/** Semantic token type indices (must match the advertised legend). */
enum class SemanticTokenType : std::uint8_t {
  Namespace = 0,
  Class = 1,
  Enum = 2,
  EnumMember = 3,
  Type = 4,
  TypeParameter = 5,
  Function = 6,
  Method = 7,
  Parameter = 8,
  Variable = 9,
  Property = 10,
};

namespace semantic_token_modifier {
constexpr unsigned DECLARATION = 1U << 0U;
constexpr unsigned DEFAULT_LIBRARY = 1U << 1U;
} // namespace semantic_token_modifier

struct RawSemanticToken {
  unsigned line = 0U;
  unsigned startChar = 0U;
  unsigned length = 0U;
  unsigned typeIndex = 0U;
  unsigned modifiers = 0U;
};

auto semanticTokenTypeFromIndexedKind(lesma::IndexedTokenKind kind) -> SemanticTokenType {
  switch (kind) {
  case lesma::IndexedTokenKind::Namespace:
    return SemanticTokenType::Namespace;
  case lesma::IndexedTokenKind::Class:
    return SemanticTokenType::Class;
  case lesma::IndexedTokenKind::Enum:
    return SemanticTokenType::Enum;
  case lesma::IndexedTokenKind::EnumMember:
    return SemanticTokenType::EnumMember;
  case lesma::IndexedTokenKind::Type:
    return SemanticTokenType::Type;
  case lesma::IndexedTokenKind::TypeParameter:
    return SemanticTokenType::TypeParameter;
  case lesma::IndexedTokenKind::Function:
    return SemanticTokenType::Function;
  case lesma::IndexedTokenKind::Method:
    return SemanticTokenType::Method;
  case lesma::IndexedTokenKind::Parameter:
    return SemanticTokenType::Parameter;
  case lesma::IndexedTokenKind::Variable:
    return SemanticTokenType::Variable;
  case lesma::IndexedTokenKind::Property:
    return SemanticTokenType::Property;
  }
  return SemanticTokenType::Variable;
}

auto collectSemanticTokens(const AnalysisResult& analysisResult, unsigned bufferId)
    -> std::vector<std::uint32_t> {
  std::vector<RawSemanticToken> rawTokens;
  AnalysisView analysis = makeAnalysisView(analysisResult);
  llvm::SourceMgr* srcMgr = analysis.sourceMgr;
  if (!isUsableAnalysis(analysis) || analysis.index == nullptr) {
    return {};
  }
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return {};
  }
  std::string const currentPath = normalizePath(analysisResult.mainFilePath);

  auto appendRawTokenFromSpan = [&](llvm::SMRange span, SemanticTokenType type,
                                    unsigned modifiers) -> void {
    if (!span.isValid()) {
      return;
    }
    auto [line1, startCol1] = srcMgr->getLineAndColumn(span.Start, bufferId);
    auto [endLine1, endCol1] = srcMgr->getLineAndColumn(span.End, bufferId);
    unsigned const line0 = line1 > 0U ? line1 - 1U : 0U;
    unsigned const startChar = startCol1 > 0U ? startCol1 - 1U : 0U;
    unsigned length = 0U;
    if (endLine1 == line1 && endCol1 >= startCol1) {
      length = endCol1 - startCol1;
    } else {
      unsigned const startOffset = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
      llvm::StringRef text = buf->getBuffer();
      size_t const newlinePos = text.find('\n', static_cast<size_t>(startOffset));
      size_t const lineEnd = newlinePos == llvm::StringRef::npos ? text.size() : newlinePos;
      length = lineEnd > startOffset ? static_cast<unsigned>(lineEnd - startOffset) : 0U;
    }
    if (length == 0U) {
      return;
    }
    rawTokens.push_back(RawSemanticToken{
        .line = line0,
        .startChar = startChar,
        .length = length,
        .typeIndex = static_cast<unsigned>(type),
        .modifiers = modifiers,
    });
  };

  auto isDefaultLibraryType = [](const lesma::Type* type) -> bool {
    return type != nullptr && type->isOneOf({lesma::BaseType::TY_INT, lesma::BaseType::TY_FLOAT,
                                             lesma::BaseType::TY_STRING, lesma::BaseType::TY_BOOL,
                                             lesma::BaseType::TY_VOID});
  };

  auto semanticTokenTypeForResolved = [](const ResolvedSymbol& resolved, bool isTypePosition,
                                         bool isMemberAccess) -> SemanticTokenType {
    lesma::Value* value = resolved.value;
    lesma::Type* type = value != nullptr ? value->getType() : nullptr;
    if (value == nullptr) {
      return SemanticTokenType::Variable;
    }
    if (isTypePosition) {
      if (type != nullptr && type->is(lesma::BaseType::TY_GENERIC)) {
        return SemanticTokenType::TypeParameter;
      }
      if (value->getCategory() == lesma::ValueCategory::TYPE_SYMBOL && type != nullptr) {
        if (type->is(lesma::BaseType::TY_CLASS)) {
          return SemanticTokenType::Class;
        }
        if (type->is(lesma::BaseType::TY_ENUM)) {
          return SemanticTokenType::Enum;
        }
      }
      return SemanticTokenType::Type;
    }
    switch (value->getCategory()) {
    case lesma::ValueCategory::MODULE_SYMBOL:
      return SemanticTokenType::Namespace;
    case lesma::ValueCategory::TYPE_SYMBOL:
      if (type != nullptr && type->is(lesma::BaseType::TY_GENERIC)) {
        return SemanticTokenType::TypeParameter;
      }
      if (type != nullptr && type->is(lesma::BaseType::TY_CLASS)) {
        return SemanticTokenType::Class;
      }
      if (type != nullptr && type->is(lesma::BaseType::TY_ENUM)) {
        return SemanticTokenType::Enum;
      }
      return SemanticTokenType::Type;
    case lesma::ValueCategory::CALLABLE_SYMBOL:
      return isMemberAccess ? SemanticTokenType::Method : SemanticTokenType::Function;
    case lesma::ValueCategory::ADDRESSABLE_STORAGE:
    case lesma::ValueCategory::DIRECT_VALUE:
      return isMemberAccess ? SemanticTokenType::Property : SemanticTokenType::Variable;
    }
    return SemanticTokenType::Variable;
  };

  auto semanticTokenModifiersForResolved = [&](const ResolvedSymbol& resolved,
                                               const ::lsp::Range& occurrenceRange,
                                               unsigned forcedModifiers) -> unsigned {
    unsigned modifiers = forcedModifiers;
    if (resolved.value == nullptr) {
      return modifiers;
    }
    llvm::SMRange const declSpan = resolved.value->getDeclarationSpan();
    std::string declPath = resolved.value->getDeclarationFilePath();
    if (declPath.empty() && resolved.owner.mainFilePath != nullptr) {
      declPath = *resolved.owner.mainFilePath;
    }
    if (declSpan.isValid() && !declPath.empty() && normalizePath(declPath) == currentPath &&
        rangeEquals(smRangeToLspRange(resolved.owner.sourceMgr, resolved.owner.bufferId, declSpan),
                    occurrenceRange)) {
      modifiers |= semantic_token_modifier::DECLARATION;
    }
    if (isDefaultLibraryType(resolved.value->getType())) {
      modifiers |= semantic_token_modifier::DEFAULT_LIBRARY;
    }
    return modifiers;
  };

  for (const lesma::IndexedSymbolOccurrence& occurrence : analysis.index->symbolOccurrences) {
    if (!occurrence.span.isValid()) {
      continue;
    }
    ::lsp::Range const range = smRangeToLspRange(srcMgr, bufferId, occurrence.span);
    CursorIdentifier const id = makeCursorIdentifierFromSpan(
        occurrence.name, occurrence.span, srcMgr, bufferId, occurrence.isTypePosition,
        occurrence.dotBase);
    if (std::optional<ResolvedSymbol> resolved = resolveCanonicalSymbolAtCursor(
            analysisResult, range.start.line, range.start.character, id)) {
      appendRawTokenFromSpan(
          occurrence.span,
          semanticTokenTypeForResolved(*resolved, id.isTypePosition, occurrence.isMemberAccess),
          semanticTokenModifiersForResolved(*resolved, range, occurrence.modifiers));
      continue;
    }
    if (id.dotBase.has_value() && !id.isTypePosition) {
      appendRawTokenFromSpan(occurrence.span, SemanticTokenType::EnumMember, occurrence.modifiers);
      continue;
    }
    if (occurrence.fallbackTokenKind.has_value()) {
      appendRawTokenFromSpan(occurrence.span,
                             semanticTokenTypeFromIndexedKind(*occurrence.fallbackTokenKind),
                             occurrence.modifiers);
    }
  }

  std::ranges::sort(rawTokens, [](const RawSemanticToken& lhs,
                                  const RawSemanticToken& rhs) -> bool {
    if (lhs.line != rhs.line) {
      return lhs.line < rhs.line;
    }
    if (lhs.startChar != rhs.startChar) {
      return lhs.startChar < rhs.startChar;
    }
    if (lhs.length != rhs.length) {
      return lhs.length < rhs.length;
    }
    if (lhs.typeIndex != rhs.typeIndex) {
      return lhs.typeIndex < rhs.typeIndex;
    }
    return lhs.modifiers < rhs.modifiers;
  });
  rawTokens.erase(std::unique(rawTokens.begin(), rawTokens.end(),
                              [](const RawSemanticToken& lhs,
                                 const RawSemanticToken& rhs) -> bool {
                                return lhs.line == rhs.line && lhs.startChar == rhs.startChar &&
                                       lhs.length == rhs.length && lhs.typeIndex == rhs.typeIndex &&
                                       lhs.modifiers == rhs.modifiers;
                              }),
                  rawTokens.end());

  std::vector<std::uint32_t> data;
  data.reserve(rawTokens.size() * 5U);
  unsigned prevLine = 0U;
  unsigned prevChar = 0U;
  for (const RawSemanticToken& token : rawTokens) {
    unsigned const deltaLine = token.line > prevLine ? (token.line - prevLine) : 0U;
    unsigned const deltaStartChar =
        token.line > prevLine ? token.startChar : (token.startChar - prevChar);
    data.push_back(deltaLine);
    data.push_back(deltaStartChar);
    data.push_back(token.length);
    data.push_back(token.typeIndex);
    data.push_back(token.modifiers);
    prevLine = token.line;
    prevChar = token.startChar;
  }
  return data;
}

auto collectReferences(const AnalysisResult& result, unsigned line, unsigned character,
                       bool includeDeclaration) -> std::vector<::lsp::Location> {
  std::vector<::lsp::Location> locations;
  std::unordered_set<std::string> seen;
  auto appendUniqueLocation = [&](::lsp::Location location) -> void {
    std::string const key = location.uri.toString() + ":" +
                            std::to_string(location.range.start.line) + ":" +
                            std::to_string(location.range.start.character) + ":" +
                            std::to_string(location.range.end.line) + ":" +
                            std::to_string(location.range.end.character);
    if (seen.insert(key).second) {
      locations.push_back(std::move(location));
    }
  };
  AnalysisView mainAnalysis = makeAnalysisView(result);
  if (!isUsableAnalysis(mainAnalysis)) {
    return locations;
  }
  std::optional<CursorIdentifier> id = findIdentifierAtCursor(mainAnalysis, line, character);
  if (!id) {
    return locations;
  }

  std::optional<ResolvedSymbol> targetResolved =
      resolveCanonicalSymbolAtCursor(result, mainAnalysis, line, character, *id);
  std::optional<SymbolIdentity> targetIdentity;
  if (const lesma::IndexedSymbolOccurrence* occurrence =
          findIndexedSymbolOccurrenceAtCursor(mainAnalysis, line, character);
      occurrence != nullptr && occurrence->declaration.has_value() &&
      declarationBelongsToAnalysis(mainAnalysis, *occurrence->declaration)) {
    targetIdentity = symbolIdentityForIndexedDeclaration(result, *occurrence->declaration, id->name);
  }
  if (!targetIdentity) {
    if (targetResolved) {
      targetIdentity = symbolIdentityForResolved(*targetResolved);
    }
  }
  if (targetIdentity) {
    bool const includeWorkspace =
        targetResolved && targetResolved->value != nullptr && targetResolved->value->isExported();
    for (const AnalysisView& analysis : collectReferenceAnalysisViews(result, includeWorkspace)) {
      if (analysis.index == nullptr) {
        continue;
      }
      ::lsp::DocumentUri uri =
          uriFromPath(analysis.mainFilePath != nullptr ? *analysis.mainFilePath : std::string{});
      for (const lesma::IndexedSymbolOccurrence& occurrence : analysis.index->symbolOccurrences) {
        ::lsp::Range const occurrenceRange =
            smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, occurrence.span);
        std::optional<SymbolIdentity> occurrenceIdentity;
        if (occurrence.declaration.has_value() &&
            declarationBelongsToAnalysis(analysis, *occurrence.declaration)) {
          occurrenceIdentity = symbolIdentityForIndexedDeclaration(result, *occurrence.declaration,
                                                                   occurrence.name);
        }
        if (!occurrenceIdentity) {
          std::optional<ResolvedSymbol> resolved = resolveCanonicalSymbolAtCursor(
              result, analysis, occurrenceRange.start.line, occurrenceRange.start.character,
              makeCursorIdentifier(occurrence.name, occurrence.dotBase, occurrenceRange,
                                   occurrence.isTypePosition));
          if (!resolved) {
            resolved = resolveCanonicalSymbolAtCursor(
                result, analysis, occurrenceRange.end.line, occurrenceRange.end.character,
                makeCursorIdentifier(occurrence.name, occurrence.dotBase, occurrenceRange,
                                     occurrence.isTypePosition));
          }
          if (!resolved) {
            continue;
          }
          occurrenceIdentity = symbolIdentityForResolved(*resolved);
        }
        if (!occurrenceIdentity || occurrenceIdentity->path != targetIdentity->path ||
            occurrenceIdentity->name != targetIdentity->name ||
            !rangeEquals(occurrenceIdentity->range, targetIdentity->range)) {
          continue;
        }
        if (!includeDeclaration && occurrenceIdentity->path == targetIdentity->path &&
            rangeEquals(occurrenceRange, targetIdentity->range)) {
          continue;
        }
        appendUniqueLocation(::lsp::Location{.uri = uri, .range = occurrenceRange});
      }
    }
    return locations;
  }

  std::optional<std::string> enumName =
      findEnumMemberDeclarationAtCursor(mainAnalysis, line, character, id->name);
  if (id->dotBase || enumName) {
    std::string targetEnum = enumName ? *enumName : *id->dotBase;
    for (const AnalysisView& analysis : collectAnalysisViews(result)) {
      if (analysis.index == nullptr) {
        continue;
      }
      ::lsp::DocumentUri uri =
          uriFromPath(analysis.mainFilePath != nullptr ? *analysis.mainFilePath : std::string{});
      for (const lesma::IndexedEnumMemberOccurrence& occurrence :
           analysis.index->enumMemberOccurrences) {
        ::lsp::Range const occurrenceRange =
            smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, occurrence.span);
        if (occurrence.enumName == targetEnum && occurrence.memberName == id->name) {
          if (!includeDeclaration && id->range && rangeEquals(occurrenceRange, *id->range)) {
            continue;
          }
          appendUniqueLocation(::lsp::Location{.uri = uri, .range = occurrenceRange});
        }
      }
    }
  }

  return locations;
}

/** Parser records FuncDecl span only through the return type; outline should cover the whole
 * function. */
auto funcDeclFullSpan(lesma::FuncDecl* func) -> llvm::SMRange {
  if (func == nullptr) {
    return {};
  }
  llvm::SMRange const sig = func->getSpan();
  lesma::Compound* body = func->getBody();
  if (body != nullptr) {
    llvm::SMRange const bodySpan = body->getSpan();
    if (bodySpan.isValid()) {
      return llvm::SMRange{sig.Start, bodySpan.End};
    }
  }
  return sig;
}

auto makeDocumentSymbol(const std::string& name, ::lsp::SymbolKind kind, const ::lsp::Range& range,
                        const ::lsp::Range& selectionRange,
                        std::optional<std::string> detail = std::nullopt) -> ::lsp::DocumentSymbol {
  auto before = [](const ::lsp::Position& lhs, const ::lsp::Position& rhs) -> bool {
    return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.character < rhs.character);
  };
  ::lsp::Range normalizedRange = range;
  if (before(selectionRange.start, normalizedRange.start)) {
    normalizedRange.start = selectionRange.start;
  }
  if (before(normalizedRange.end, selectionRange.end)) {
    normalizedRange.end = selectionRange.end;
  }
  ::lsp::DocumentSymbol symbol{
      .name = name,
      .kind = kind,
      .range = normalizedRange,
      .selectionRange = selectionRange,
  };
  if (detail && !detail->empty()) {
    std::string const& detailText = *detail;
    symbol.detail = ::lsp::Opt<::lsp::String>(detailText);
  }
  return symbol;
}

auto collectDocumentSymbols(const AnalysisResult& result) -> std::vector<::lsp::DocumentSymbol> {
  std::vector<::lsp::DocumentSymbol> symbols;
  if (result.parser == nullptr || result.sourceMgr == nullptr) {
    return symbols;
  }
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return symbols;
  }

  for (lesma::Statement* stmt : ast->getChildren()) {
    if (auto* varDecl = dynamic_cast<lesma::VarDecl*>(stmt)) {
      lesma::Value* value = varDecl->getResolvedSymbol();
      symbols.push_back(makeDocumentSymbol(
          varDecl->getIdentifier()->getValue(), ::lsp::SymbolKind::Variable,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, varDecl->getSpan()),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId,
                            varDecl->getIdentifier()->getSpan()),
          value != nullptr
              ? std::optional<std::string>(formatTypeName(value->getType(), result.rootScope.get()))
              : std::nullopt));
      continue;
    }
    if (auto* func = dynamic_cast<lesma::FuncDecl*>(stmt)) {
      lesma::Value* value = func->getResolvedSymbol();
      symbols.push_back(makeDocumentSymbol(
          func->getName(), ::lsp::SymbolKind::Function,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, funcDeclFullSpan(func)),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, func->getNameSpan()),
          value != nullptr && value->getType() != nullptr &&
                  value->getType()->getReturnType() != nullptr
              ? std::optional<std::string>(
                    formatTypeName(value->getType()->getReturnType(), result.rootScope.get()))
              : std::nullopt));
      continue;
    }
    if (auto* ext = dynamic_cast<lesma::ExternFuncDecl*>(stmt)) {
      lesma::Value* value = ext->getResolvedSymbol();
      symbols.push_back(makeDocumentSymbol(
          ext->getName(), ::lsp::SymbolKind::Function,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, ext->getSpan()),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, ext->getNameSpan()),
          value != nullptr && value->getType() != nullptr &&
                  value->getType()->getReturnType() != nullptr
              ? std::optional<std::string>(
                    formatTypeName(value->getType()->getReturnType(), result.rootScope.get()))
              : std::nullopt));
      continue;
    }
    if (auto* enumNode = dynamic_cast<lesma::Enum*>(stmt)) {
      ::lsp::DocumentSymbol symbol = makeDocumentSymbol(
          enumNode->getIdentifier(), ::lsp::SymbolKind::Enum,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, enumNode->getSpan()),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, enumNode->getNameSpan()));
      std::vector<::lsp::DocumentSymbol> children;
      for (const lesma::NamedSpan& valueDecl : lesma::getEnumValueDecls(enumNode)) {
        children.push_back(makeDocumentSymbol(
            valueDecl.name, ::lsp::SymbolKind::EnumMember,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, valueDecl.span),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, valueDecl.span)));
      }
      for (const ::lsp::DocumentSymbol& child : children) {
        if (child.range.end.line > symbol.range.end.line ||
            (child.range.end.line == symbol.range.end.line &&
             child.range.end.character > symbol.range.end.character)) {
          symbol.range.end = child.range.end;
        }
      }
      symbol.children = ::lsp::Opt<::lsp::Array<::lsp::DocumentSymbol>>(
          ::lsp::Array<::lsp::DocumentSymbol>(children.begin(), children.end()));
      symbols.push_back(std::move(symbol));
      continue;
    }
    if (auto* klass = dynamic_cast<lesma::Class*>(stmt)) {
      ::lsp::DocumentSymbol symbol = makeDocumentSymbol(
          klass->getIdentifier(), ::lsp::SymbolKind::Class,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, klass->getSpan()),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, klass->getNameSpan()));
      std::vector<::lsp::DocumentSymbol> children;
      for (lesma::VarDecl* field : klass->getFields()) {
        lesma::Value* value = field->getResolvedSymbol();
        children.push_back(makeDocumentSymbol(
            field->getIdentifier()->getValue(), ::lsp::SymbolKind::Field,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, field->getSpan()),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId,
                              field->getIdentifier()->getSpan()),
            value != nullptr ? std::optional<std::string>(
                                   formatTypeName(value->getType(), result.rootScope.get()))
                             : std::nullopt));
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        lesma::Value* value = method->getResolvedSymbol();
        children.push_back(makeDocumentSymbol(
            method->getName(), ::lsp::SymbolKind::Method,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId,
                              funcDeclFullSpan(method)),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, method->getNameSpan()),
            value != nullptr && value->getType() != nullptr &&
                    value->getType()->getReturnType() != nullptr
                ? std::optional<std::string>(
                      formatTypeName(value->getType()->getReturnType(), result.rootScope.get()))
                : std::nullopt));
      }
      for (const ::lsp::DocumentSymbol& child : children) {
        if (child.range.end.line > symbol.range.end.line ||
            (child.range.end.line == symbol.range.end.line &&
             child.range.end.character > symbol.range.end.character)) {
          symbol.range.end = child.range.end;
        }
      }
      symbol.children = ::lsp::Opt<::lsp::Array<::lsp::DocumentSymbol>>(
          ::lsp::Array<::lsp::DocumentSymbol>(children.begin(), children.end()));
      symbols.push_back(std::move(symbol));
    }
  }

  return symbols;
}

/** If the cursor is on an enum member declaration (e.g. Red in "enum Color { Red, Green }"),
 * return the enum name. Otherwise return nullopt. */
auto findEnumMemberDeclarationAtCursor(const AnalysisView& analysis, unsigned line,
                                       unsigned character, const std::string& memberName)
    -> std::optional<std::string> {
  if (!isUsableAnalysis(analysis) || analysis.index == nullptr) {
    return std::nullopt;
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
  for (const lesma::IndexedEnumMemberOccurrence& occurrence : analysis.index->enumMemberOccurrences) {
    if (occurrence.memberName != memberName || !occurrence.span.isValid()) {
      continue;
    }
    unsigned const startOff =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.Start);
    unsigned const endOff =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.End);
    if (targetOffset >= startOff && targetOffset < endOff) {
      return occurrence.enumName;
    }
  }
  return std::nullopt;
}

/** Resolve go-to-definition for an enum member (e.g. OptionA in MyEnum.OptionA). */
auto tryResolveEnumMemberDefinitionLocation(const AnalysisResult& result,
                                            const std::string& dotBase,
                                            const std::string& memberName)
    -> std::optional<::lsp::Location> {
  auto searchAnalysis = [&](const AnalysisView& analysis,
                            const std::string& enumName) -> std::optional<::lsp::Location> {
    if (!isUsableAnalysis(analysis) || analysis.mainFilePath == nullptr) {
      return std::nullopt;
    }
    for (lesma::Statement* stmt : analysis.ast->getChildren()) {
      auto const* e = dynamic_cast<const lesma::Enum*>(stmt);
      if (e == nullptr || e->getIdentifier() != enumName) {
        continue;
      }
      for (const lesma::NamedSpan& valueDecl : lesma::getEnumValueDecls(e)) {
        if (valueDecl.name == memberName && valueDecl.span.isValid()) {
          return ::lsp::Location{
              .uri = uriFromPath(*analysis.mainFilePath),
              .range = smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, valueDecl.span),
          };
        }
      }
    }
    return std::nullopt;
  };

  AnalysisView mainAnalysis = makeAnalysisView(result);
  if (!isUsableAnalysis(mainAnalysis)) {
    return std::nullopt;
  }
  if (std::optional<::lsp::Location> local = searchAnalysis(mainAnalysis, dotBase)) {
    return local;
  }
  auto importedIt = result.importedNameToSource.find(dotBase);
  if (importedIt != result.importedNameToSource.end()) {
    if (std::optional<AnalysisView> imported =
            findAnalysisViewForPath(result, importedIt->second.first)) {
      if (std::optional<::lsp::Location> importedLoc =
              searchAnalysis(*imported, importedIt->second.second)) {
        return importedLoc;
      }
    }
  }
  for (const AnalysisView& analysis : collectAnalysisViews(result)) {
    if (analysis.mainFilePath != nullptr &&
        normalizePath(*analysis.mainFilePath) == normalizePath(result.mainFilePath)) {
      continue;
    }
    if (std::optional<::lsp::Location> importedLoc = searchAnalysis(analysis, dotBase)) {
      return importedLoc;
    }
  }
  return std::nullopt;
}

/** Resolve definition location using compiler metadata from Value. */
auto tryResolveDefinitionLocation(const AnalysisResult& result, unsigned line, unsigned character)
    -> std::optional<::lsp::Location> {
  if (result.parser == nullptr || result.sourceMgr == nullptr) {
    return std::nullopt;
  }
  AnalysisView analysis = makeAnalysisView(result);
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return std::nullopt;
  }
  std::optional<CursorIdentifier> id = findIdentifierAtCursor(analysis, line, character);
  if (!id) {
    return std::nullopt;
  }
  if (const lesma::IndexedSymbolOccurrence* occurrence =
          findIndexedSymbolOccurrenceAtCursor(analysis, line, character);
      occurrence != nullptr && occurrence->declaration.has_value() &&
      declarationBelongsToAnalysis(analysis, *occurrence->declaration)) {
    if (std::optional<::lsp::Location> declarationLocation =
            locationForIndexedDeclaration(result, *occurrence->declaration)) {
      return declarationLocation;
    }
  }
  std::optional<ResolvedSymbol> resolved =
      resolveCanonicalSymbolAtCursor(result, line, character, *id);
  if (!resolved || resolved->value == nullptr) {
    // Enum member (e.g. MyEnum.OptionA): not a Value, resolve via AST
    if (id->dotBase) {
      auto enumMemberLoc = tryResolveEnumMemberDefinitionLocation(result, *id->dotBase, id->name);
      if (enumMemberLoc) {
        return enumMemberLoc;
      }
    }
    return std::nullopt;
  }
  llvm::SMRange declSpan = resolved->value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    if (!id->dotBase) {
      AnalysisView mainAnalysis = makeAnalysisView(result);
      if (std::optional<std::string> modulePath =
              findModuleImportPathByAlias(mainAnalysis, id->name)) {
        if (std::optional<AnalysisView> imported = findAnalysisViewForPath(result, *modulePath)) {
          return ::lsp::Location{
              .uri = uriFromPath(*imported->mainFilePath),
              .range =
                  ::lsp::Range{
                      .start = ::lsp::Position{.line = 0U, .character = 0U},
                      .end = ::lsp::Position{.line = 0U, .character = 0U},
                  },
          };
        }
      }
      if (result.importAliasToPath.contains(id->name)) {
        if (std::optional<AnalysisView> imported =
                findAnalysisViewForPath(result, result.importAliasToPath.at(id->name))) {
          return ::lsp::Location{
              .uri = uriFromPath(*imported->mainFilePath),
              .range =
                  ::lsp::Range{
                      .start = ::lsp::Position{.line = 0U, .character = 0U},
                      .end = ::lsp::Position{.line = 0U, .character = 0U},
                  },
          };
        }
      }
    }
    return std::nullopt;
  }
  std::string declPath = resolved->value->getDeclarationFilePath();
  if (declPath.empty() && resolved->owner.mainFilePath != nullptr) {
    declPath = *resolved->owner.mainFilePath;
  }
  if (declPath.empty()) {
    return std::nullopt;
  }
  ::lsp::Location loc;
  loc.uri = uriFromPath(declPath);
  loc.range = smRangeToLspRange(resolved->owner.sourceMgr, resolved->owner.bufferId, declSpan);
  return loc;
}

auto tryResolveDeclarationLocation(const AnalysisResult& result, unsigned line, unsigned character)
    -> std::optional<::lsp::Location> {
  if (result.parser == nullptr || result.sourceMgr == nullptr) {
    return std::nullopt;
  }
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return std::nullopt;
  }
  AnalysisView analysis = makeAnalysisView(result);
  std::optional<CursorIdentifier> id = findIdentifierAtCursor(analysis, line, character);
  if (!id) {
    return std::nullopt;
  }
  if (id->dotBase) {
    if (std::optional<::lsp::Location> localImport =
            findLocalImportBindingLocation(analysis, *id->dotBase, true)) {
      return localImport;
    }
  } else {
    if (std::optional<::lsp::Location> localImport =
            findLocalImportBindingLocation(analysis, id->name, false)) {
      return localImport;
    }
    if (std::optional<::lsp::Location> moduleAlias =
            findLocalImportBindingLocation(analysis, id->name, true)) {
      return moduleAlias;
    }
  }
  return tryResolveDefinitionLocation(result, line, character);
}

} // namespace

auto main() -> int {
  try {
    auto connection = ::lsp::Connection(::lsp::io::standardIO());
    auto messageHandler = ::lsp::MessageHandler(connection);
    lesma::lsp_srv::DocumentStore docStore;
    AnalysisCache analysisCache;

    std::atomic<bool> running{true};

    messageHandler.add<::lsp::requests::Initialize>([](const ::lsp::requests::Initialize::Params&)
                                                        -> ::lsp::requests::Initialize::Result {
      ::lsp::ServerCapabilities caps;
      caps.positionEncoding =
          ::lsp::Opt<::lsp::PositionEncodingKindEnum>(::lsp::PositionEncodingKind::UTF16);
      caps.textDocumentSync =
          ::lsp::Opt<::lsp::OneOf<::lsp::TextDocumentSyncOptions, ::lsp::TextDocumentSyncKindEnum>>(
              ::lsp::TextDocumentSyncOptions{.openClose = true,
                                             .change = ::lsp::TextDocumentSyncKind::Full});
      caps.hoverProvider = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::HoverOptions>>(true);
      caps.definitionProvider = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::DefinitionOptions>>(true);
      caps.declarationProvider = ::lsp::Opt<
          ::lsp::OneOf<bool, ::lsp::DeclarationOptions, ::lsp::DeclarationRegistrationOptions>>(
          ::lsp::OneOf<bool, ::lsp::DeclarationOptions, ::lsp::DeclarationRegistrationOptions>{
              true});
      caps.completionProvider = ::lsp::Opt<::lsp::CompletionOptions>(::lsp::CompletionOptions{
          .triggerCharacters = ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string(".")}),
      });
      caps.signatureHelpProvider =
          ::lsp::Opt<::lsp::SignatureHelpOptions>(::lsp::SignatureHelpOptions{
              .triggerCharacters =
                  ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string("("), std::string(",")}),
              .retriggerCharacters = ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string(",")}),
          });
      caps.referencesProvider = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::ReferenceOptions>>(true);
      caps.documentSymbolProvider =
          ::lsp::Opt<::lsp::OneOf<bool, ::lsp::DocumentSymbolOptions>>(true);
      caps.semanticTokensProvider = ::lsp::Opt<
          ::lsp::OneOf<::lsp::SemanticTokensOptions, ::lsp::SemanticTokensRegistrationOptions>>(
          ::lsp::SemanticTokensOptions{
              .legend =
                  ::lsp::SemanticTokensLegend{
                      .tokenTypes =
                          ::lsp::Array<::lsp::String>{"namespace", "class", "enum", "enumMember",
                                                      "type", "typeParameter", "function", "method",
                                                      "parameter", "variable", "property"},
                      .tokenModifiers =
                          ::lsp::Array<::lsp::String>{"declaration", "defaultLibrary"},
                  },
              .full = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::SemanticTokensOptionsFull>>(true),
          });
      caps.inlayHintProvider = ::lsp::Opt<
          ::lsp::OneOf<bool, ::lsp::InlayHintOptions, ::lsp::InlayHintRegistrationOptions>>(true);
      return ::lsp::requests::Initialize::Result{
          .capabilities = caps,
          .serverInfo =
              ::lsp::Opt<::lsp::InitializeResultServerInfo>(::lsp::InitializeResultServerInfo{
                  .name = "lesma-lsp",
                  .version = "0.1.0",
              }),
      };
    });

    messageHandler.add<::lsp::notifications::Exit>([&running]() { running = false; });

    messageHandler.add<::lsp::notifications::Initialized>(
        [](const ::lsp::notifications::Initialized::Params&) -> void {});

    messageHandler.add<::lsp::notifications::TextDocument_DidOpen>(
        [&messageHandler, &docStore,
         &analysisCache](const ::lsp::notifications::TextDocument_DidOpen::Params& params) {
          const auto& doc = params.textDocument;
          docStore.open(doc.uri, doc.version, doc.text);
          runAnalyzeAndPublish(doc.uri, docStore, doc.text, doc.version, analysisCache,
                               messageHandler);
        });

    messageHandler.add<::lsp::notifications::TextDocument_DidChange>(
        [&messageHandler, &docStore,
         &analysisCache](const ::lsp::notifications::TextDocument_DidChange::Params& params) {
          auto uri = params.textDocument.uri;
          auto doc = docStore.getDocument(uri);
          if (!doc) {
            return;
          }
          std::string text;
          for (auto const& change : params.contentChanges) {
            if (std::holds_alternative<::lsp::TextDocumentContentChangeEvent_Text>(change)) {
              text = std::get<::lsp::TextDocumentContentChangeEvent_Text>(change).text;
              break;
            }
          }
          if (text.empty()) {
            text = doc->text;
          }
          if (!text.empty()) {
            docStore.change(uri, params.textDocument.version, text);
            runAnalyzeAndPublish(uri, docStore, text, params.textDocument.version, analysisCache,
                                 messageHandler);
          }
        });

    messageHandler.add<::lsp::notifications::TextDocument_DidClose>(
        [&messageHandler, &docStore,
         &analysisCache](const ::lsp::notifications::TextDocument_DidClose::Params& params) {
          docStore.close(params.textDocument.uri);
          analysisCache.invalidate(params.textDocument.uri, docStore);
          messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
              ::lsp::PublishDiagnosticsParams{.uri = params.textDocument.uri, .diagnostics = {}});
        });

    messageHandler.add<::lsp::requests::TextDocument_Hover>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_Hover::Params& params)
            -> ::lsp::TextDocument_HoverResult {
          return withAnalyzedDocument<::lsp::TextDocument_HoverResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_HoverResult {
                AnalysisView analysis = makeAnalysisView(result);
                if (result.parser == nullptr) {
                  return {};
                }
                lesma::Compound* ast = result.parser->getAst();
                if (ast == nullptr) {
                  return {};
                }
                unsigned line = params.position.line;
                unsigned character = params.position.character;
                std::optional<CursorIdentifier> id = findIdentifierAtCursor(analysis, line, character);
                if (!id) {
                  return {};
                }
                std::optional<ResolvedSymbol> resolved =
                    resolveCanonicalSymbolAtCursor(result, line, character, *id);
                if (resolved && resolved->value != nullptr && resolved->value->getType() != nullptr) {
                  std::string hoverText =
                      formatHoverContent(resolved->value, resolved->owner.rootScope);
                  ::lsp::Hover hover;
                  hover.contents = ::lsp::MarkupContent{
                      .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                      .value = std::move(hoverText),
                  };
                  if (id->range) {
                    hover.range = id->range;
                  }
                  return {std::move(hover)};
                }
                // Enum member hover (e.g. MyEnum.OptionA): not a Value, but a field on the enum type
                if (id->dotBase && result.rootScope != nullptr) {
                  std::optional<ResolvedSymbol> baseResolved = resolveCanonicalSymbolAtCursor(
                      result, line, character,
                      makeCursorIdentifier(*id->dotBase, std::nullopt, std::nullopt));
                  lesma::Value* baseVal =
                      baseResolved ? baseResolved->value : result.rootScope->lookup(*id->dotBase);
                  if (baseVal != nullptr && baseVal->getType() != nullptr &&
                      baseVal->getType()->is(lesma::BaseType::TY_ENUM)) {
                    for (lesma::Field* field : baseVal->getType()->getFields()) {
                      if (field != nullptr && field->name == id->name) {
                        ::lsp::Hover hover;
                        hover.contents = ::lsp::MarkupContent{
                            .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                            .value = "**" + id->name + "**\n\nmember of enum `" + *id->dotBase +
                                     "`",
                        };
                        return {std::move(hover)};
                      }
                    }
                  }
                }
                // Enum member declaration hover (e.g. Red in "enum Color { Red, Green }")
                std::optional<std::string> enumName =
                    findEnumMemberDeclarationAtCursor(analysis, line, character, id->name);
                if (enumName) {
                  ::lsp::Hover hover;
                  hover.contents = ::lsp::MarkupContent{
                      .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                      .value = "**" + id->name + "**\n\nmember of enum `" + *enumName + "`",
                  };
                  return {std::move(hover)};
                }
                if (id->isTypePosition) {
                  if (isBuiltinTypeName(id->name)) {
                    ::lsp::Hover hover;
                    hover.contents = ::lsp::MarkupContent{
                        .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                        .value = "built-in type `" + id->name + "`",
                    };
                    if (id->range) {
                      hover.range = id->range;
                    }
                    return {std::move(hover)};
                  }

                  auto const* buf = result.sourceMgr->getMemoryBuffer(result.mainBufferId);
                  if (buf != nullptr) {
                    unsigned targetOffset =
                        static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspPosition(
                            buf->getBuffer(), line, character));
                    InnermostFunc sigFunc = findFuncWithCursorInSignature(
                        ast, targetOffset, result.sourceMgr.get(), result.mainBufferId);
                    InnermostFunc inner = findInnermostFuncContaining(
                        ast, targetOffset, result.sourceMgr.get(), result.mainBufferId);
                    bool inGenericScope =
                        (sigFunc.func != nullptr &&
                         containsGenericParam(sigFunc.func->getGenericParams(), id->name)) ||
                        (sigFunc.enclosingClass != nullptr &&
                         containsGenericParam(sigFunc.enclosingClass->getGenericParams(), id->name)) ||
                        (inner.func != nullptr &&
                         containsGenericParam(inner.func->getGenericParams(), id->name)) ||
                        (inner.enclosingClass != nullptr &&
                         containsGenericParam(inner.enclosingClass->getGenericParams(), id->name));
                    if (inGenericScope) {
                      ::lsp::Hover hover;
                      hover.contents = ::lsp::MarkupContent{
                          .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                          .value = "generic type parameter `" + id->name + "`",
                      };
                      if (id->range) {
                        hover.range = id->range;
                      }
                      return {std::move(hover)};
                    }
                  }
                }
                return {};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_SemanticTokens_Full>(
        [&docStore,
         &analysisCache](const ::lsp::requests::TextDocument_SemanticTokens_Full::Params& params)
            -> ::lsp::TextDocument_SemanticTokens_FullResult {
          return withAnalyzedDocument<::lsp::TextDocument_SemanticTokens_FullResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_SemanticTokens_FullResult {
                std::vector<std::uint32_t> data =
                    collectSemanticTokens(result, result.mainBufferId);
                ::lsp::SemanticTokens tokens;
                tokens.data = ::lsp::Array<std::uint32_t>(data.begin(), data.end());
                return {std::move(tokens)};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_InlayHint>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_InlayHint::Params& params)
            -> ::lsp::TextDocument_InlayHintResult {
          return withAnalyzedDocument<::lsp::TextDocument_InlayHintResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_InlayHintResult {
                std::vector<::lsp::InlayHint> hints =
                    collectInlayHints(result, result.mainBufferId, params.range);
                return {::lsp::Array<::lsp::InlayHint>(hints.begin(), hints.end())};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_SignatureHelp>(
        [&docStore,
         &analysisCache](const ::lsp::requests::TextDocument_SignatureHelp::Params& params)
            -> ::lsp::TextDocument_SignatureHelpResult {
          return withAnalyzedDocument<::lsp::TextDocument_SignatureHelpResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_SignatureHelpResult {
                auto help =
                    buildSignatureHelp(result, params.position.line, params.position.character);
                if (!help) {
                  return {};
                }
                return {std::move(*help)};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_Completion>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_Completion::Params& params)
            -> ::lsp::TextDocument_CompletionResult {
          return withAnalyzedDocument<::lsp::TextDocument_CompletionResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_CompletionResult {
                std::vector<::lsp::CompletionItem> items = lesma::lsp_srv::completionItems(
                    result, params.position.line, params.position.character);
                if (items.empty()) {
                  return {};
                }
                return {items};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_References>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_References::Params& params)
            -> ::lsp::TextDocument_ReferencesResult {
          return withAnalyzedDocument<::lsp::TextDocument_ReferencesResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_ReferencesResult {
                std::vector<::lsp::Location> refs =
                    collectReferences(result, params.position.line, params.position.character,
                                      params.context.includeDeclaration);
                if (refs.empty()) {
                  return {};
                }
                return {::lsp::Array<::lsp::Location>(refs.begin(), refs.end())};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_Definition>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_Definition::Params& params)
            -> ::lsp::TextDocument_DefinitionResult {
          return withAnalyzedDocument<::lsp::TextDocument_DefinitionResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_DefinitionResult {
                auto loc = tryResolveDefinitionLocation(result, params.position.line,
                                                       params.position.character);
                if (!loc) {
                  return {};
                }
                return {::lsp::Definition(std::move(*loc))};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_Declaration>(
        [&docStore, &analysisCache](const ::lsp::requests::TextDocument_Declaration::Params& params)
            -> ::lsp::TextDocument_DeclarationResult {
          return withAnalyzedDocument<::lsp::TextDocument_DeclarationResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_DeclarationResult {
                auto loc = tryResolveDeclarationLocation(result, params.position.line,
                                                         params.position.character);
                if (!loc) {
                  return {};
                }
                return {::lsp::Declaration(std::move(*loc))};
              });
        });

    messageHandler.add<::lsp::requests::TextDocument_DocumentSymbol>(
        [&docStore,
         &analysisCache](const ::lsp::requests::TextDocument_DocumentSymbol::Params& params)
            -> ::lsp::TextDocument_DocumentSymbolResult {
          return withAnalyzedDocument<::lsp::TextDocument_DocumentSymbolResult>(
              params.textDocument.uri, docStore, analysisCache,
              [&](AnalysisResult& result) -> ::lsp::TextDocument_DocumentSymbolResult {
                std::vector<::lsp::DocumentSymbol> symbols = collectDocumentSymbols(result);
                if (symbols.empty()) {
                  return {};
                }
                return {::lsp::Array<::lsp::DocumentSymbol>(symbols.begin(), symbols.end())};
              });
        });

    while (running) {
      messageHandler.processIncomingMessages();
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lesma-lsp: " << e.what() << '\n';
    return 1;
  }
}
