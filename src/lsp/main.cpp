#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/Support/SourceMgr.h>

#include "DocumentStore.h"
#include "LspAnalysisGraph.h"
#include "LspCompletion.h"
#include "LspSourceHelpers.h"
#include "LspTypeFormat.h"
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>
#include <lsp/types.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

namespace {

using namespace lesma;
using namespace lesma::lsp_srv;

[[nodiscard]] auto vectorContainsString(const std::vector<std::string>& haystack,
                                        const std::string& needle) -> bool {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

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
        .sourceType = SourceType::STRING,
        .source = content,
        .debug = Debug::NONE,
        .outputFilename = "output",
        .timer = false,
        .implicitFilePath = std::move(implicitPath),
    });
    AnalysisResult result = lesma::analyze(std::move(options));
    DocumentAnalysisSnapshot snapshot{
        .content = content, .version = version, .result = std::move(result)};
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

struct ResolvedSymbol {
  lesma::Value* value = nullptr;
  AnalysisView owner;
};

struct SymbolIdentity {
  std::string path;
  ::lsp::Range range;
  std::string name;
};

template <typename FuncLike>
auto resolveFuncLikeDeclarationSymbol(const FuncLike* node, llvm::SourceMgr* srcMgr,
                                      unsigned bufferId, llvm::SMRange declarationSpan)
    -> lesma::Value* {
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
    std::vector<lesma::Literal*> const names = varDecl->getVarLiterals();
    std::vector<lesma::Value*> const& rs = varDecl->getResolvedSymbols();
    for (size_t i = 0; i < names.size(); ++i) {
      lesma::Literal* ident = names[i];
      if (ident != nullptr && smRangesEqual(srcMgr, bufferId, ident->getSpan(), declarationSpan)) {
        if (i < rs.size()) {
          return rs[i];
        }
        if (names.size() == 1U) {
          return varDecl->getResolvedSymbol();
        }
        return nullptr;
      }
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
    if (enumNode->getResolvedSymbol() != nullptr &&
        enumNode->getResolvedSymbol()->getType() != nullptr) {
      std::vector<lesma::Field*> const fields =
          enumNode->getResolvedSymbol()->getType()->getFields();
      std::vector<lesma::NamedSpan> const valueDecls = lesma::getEnumValueDecls(enumNode);
      for (size_t i = 0; i < valueDecls.size() && i < fields.size(); ++i) {
        if (smRangesEqual(srcMgr, bufferId, valueDecls[i].span, declarationSpan)) {
          return fields[i] != nullptr ? fields[i]->getDeclarationSymbol() : nullptr;
        }
      }
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

auto resolveSymbolByDeclarationIdentity(AnalysisResult& result,
                                        const lesma::IndexedDeclarationIdentity& declaration)
    -> std::optional<ResolvedSymbol> {
  std::optional<AnalysisView> analysis = findAnalysisViewForPath(result, declaration.filePath);
  if (!analysis || !isUsableAnalysis(*analysis)) {
    return std::nullopt;
  }
  for (lesma::Statement* stmt : analysis->ast->getChildren()) {
    if (lesma::Value* value = resolveDeclarationSymbolInStmt(
            stmt, analysis->sourceMgr, analysis->bufferId, declaration.span)) {
      return ResolvedSymbol{.value = value, .owner = *analysis};
    }
  }
  return std::nullopt;
}

auto locationForIndexedDeclaration(AnalysisResult& result,
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

/** `Value::declarationSpan` lives in `getDeclarationFilePath()`; `ResolvedSymbol::owner` is often
 * the referring document. Map the span using the defining file's SourceMgr buffer. When the
 * declaration path is known but that module has no usable AnalysisView, returns std::nullopt so
 * callers do not pair `uriFromPath(declPath)` with coordinates from the referring buffer. */
auto lspRangeForValueDeclaration(AnalysisResult& result, lesma::Value* value,
                                 const AnalysisView& fallbackOwner) -> std::optional<::lsp::Range> {
  if (value == nullptr) {
    return ::lsp::Range{
        .start = ::lsp::Position{.line = 0U, .character = 0U},
        .end = ::lsp::Position{.line = 0U, .character = 0U},
    };
  }
  llvm::SMRange const declSpan = value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    return ::lsp::Range{
        .start = ::lsp::Position{.line = 0U, .character = 0U},
        .end = ::lsp::Position{.line = 0U, .character = 0U},
    };
  }
  std::string declPath = value->getDeclarationFilePath();
  if (declPath.empty() && fallbackOwner.mainFilePath != nullptr) {
    declPath = *fallbackOwner.mainFilePath;
  }
  if (!declPath.empty()) {
    if (std::optional<AnalysisView> declView = findAnalysisViewForPath(result, declPath);
        declView && isUsableAnalysis(*declView)) {
      return smRangeToLspRange(declView->sourceMgr, declView->bufferId, declSpan);
    }
    return std::nullopt;
  }
  return smRangeToLspRange(fallbackOwner.sourceMgr, fallbackOwner.bufferId, declSpan);
}

auto runAnalyzeAndPublish(const ::lsp::DocumentUri& uri,
                          const lesma::lsp_srv::DocumentStore& docStore, const std::string& content,
                          int version, AnalysisCache& analysisCache,
                          ::lsp::MessageHandler& messageHandler) -> void {
  if (std::optional<std::string> path = docStore.getPath(uri)) {
    invalidateLazyImportedAnalysesAffectedBy(*path);
  }
  DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(uri, docStore, content, version);
  AnalysisResult& result = snapshot.result;

  std::optional<std::string> const analyzedPathOpt = docStore.getPath(uri);
  std::string const primaryNorm = analyzedPathOpt ? normalizePath(*analyzedPathOpt) : std::string{};

  std::vector<::lsp::Diagnostic> primaryDiags;
  std::unordered_map<std::string, std::vector<::lsp::Diagnostic>> otherDiags;

  for (const auto& d : result.diagnostics) {
    llvm::SourceMgr* mgr = nullptr;
    unsigned bufId = 0;
    std::string displayPath;
    resolveAnalysisDiagnosticSource(result, d, mgr, bufId, displayPath);

    ::lsp::DiagnosticSeverity const sev = d.severity == lesma::AnalysisDiagnosticSeverity::Warning
                                              ? ::lsp::DiagnosticSeverity::Warning
                                              : ::lsp::DiagnosticSeverity::Error;
    ::lsp::Range const range = smRangeToLspRange(mgr, bufId, d.span);

    std::string dNorm;
    if (!displayPath.empty()) {
      dNorm = normalizePath(displayPath);
    }
    if (dNorm.empty() && !result.mainFilePath.empty()) {
      dNorm = normalizePath(result.mainFilePath);
    }

    ::lsp::Diagnostic lspDiag{.range = range,
                              .message = d.message,
                              .severity = ::lsp::Opt<::lsp::DiagnosticSeverityEnum>(sev)};

    if (!primaryNorm.empty() && dNorm == primaryNorm) {
      primaryDiags.push_back(std::move(lspDiag));
    } else if (!primaryNorm.empty() && !dNorm.empty() && dNorm != primaryNorm) {
      otherDiags[dNorm].push_back(std::move(lspDiag));
    } else {
      primaryDiags.push_back(std::move(lspDiag));
    }
  }

  messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
      ::lsp::PublishDiagnosticsParams{
          .uri = uri,
          .diagnostics = std::move(primaryDiags),
      });

  for (auto& entry : otherDiags) {
    messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
        ::lsp::PublishDiagnosticsParams{
            .uri = uriFromPath(entry.first),
            .diagnostics = std::move(entry.second),
        });
  }
}

/** Build hover text from a symbol: name, kind, and type in readable Markdown. */
auto formatHoverContent(lesma::Value* value, lesma::SymbolTable* rootScope,
                        lesma::Type* typeDisplayOverride = nullptr) -> std::string {
  if (value == nullptr) {
    return "";
  }
  lesma::Type* type = typeDisplayOverride != nullptr ? typeDisplayOverride : value->getType();
  std::string typeStr = type != nullptr ? type->toString() : "?";
  std::string const& name = value->getName();

  // For class/enum types, use the actual class/enum name instead of "Class"/"Enum"
  std::string typeName = getTypeName(type, rootScope);
  if (!typeName.empty()) {
    typeStr = typeName;
  }

  switch (value->getCategory()) {
  case lesma::ValueCategory::CALLABLE_SYMBOL: {
    std::string headline = name;
    if (auto spell = lesma::OperatorUtils::surfaceSpellingForMangledOperator(name)) {
      headline = std::string(*spell);
    }
    return "**" + headline + "**\n\nType: `" + formatTypeName(type, rootScope) + "`";
  }
  case lesma::ValueCategory::TYPE_SYMBOL: {
    if (type != nullptr && type->is(lesma::BaseType::TY_GENERIC)) {
      return "type parameter `" + name + "`";
    }
    // For TYPE_SYMBOL: enum, trait, or class
    if (type != nullptr && type->is(lesma::BaseType::TY_ENUM)) {
      return "enum `" + name + "`";
    }
    if (type != nullptr && type->is(lesma::BaseType::TY_TRAIT_EXISTENTIAL)) {
      return "trait `" + name + "`";
    }
    if (value->getDeclarationKind() == lesma::ValueDeclarationKind::TRAIT) {
      return "trait `" + name + "`";
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
  return name == "int" || name == "float" || name == "bool" || name == "cstr" || name == "void";
}

using InnermostFunc =
    lesma::lsp_srv::InnermostFuncAtOffset<const lesma::FuncDecl, const lesma::Class>;

[[nodiscard]] auto findInnermostFuncContaining(lesma::Compound* ast, unsigned targetOffset,
                                               llvm::SourceMgr* sm, unsigned bid) -> InnermostFunc {
  return lesma::lsp_srv::findInnermostFuncContainingAst<const lesma::FuncDecl, const lesma::Class>(
      ast, targetOffset, sm, bid);
}

/** Find function (or method) whose signature contains the cursor (name or any parameter).
 * Used so parameter names in "func foo(x: int)" resolve to the parameter symbol. */
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

auto lookupValueForHover(lesma::Compound* ast, lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                         unsigned bufferId, unsigned line, unsigned character,
                         const std::string& name, bool isTypePosition) -> lesma::Value* {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  auto const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));

  // If cursor is in a function's signature (e.g. on a parameter or function name), use the resolved
  // symbol. Use the symbol the typechecker resolved for this exact overload (getResolvedSymbol).
  InnermostFunc const sigFunc = findFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId);
  if (sigFunc.func != nullptr) {
    if (lesma::Value* value = lookupNameInCallableContext(lesma::makeFuncLikeDeclView(sigFunc.func),
                                                          root, name, isTypePosition)) {
      return value;
    }
  }

  if (auto const* sigExtern =
          findExternFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId)) {
    if (lesma::Value* value = lookupNameInCallableContext(lesma::makeFuncLikeDeclView(sigExtern),
                                                          root, name, isTypePosition)) {
      return value;
    }
  }

  // Cursor in a function body: use the innermost function's resolved symbol (correct overload).
  InnermostFunc const inner = findInnermostFuncContaining(ast, targetOffset, srcMgr, bufferId);
  if (inner.func != nullptr) {
    if (lesma::Value* value = lookupNameInCallableContext(lesma::makeFuncLikeDeclView(inner.func),
                                                          root, name, isTypePosition)) {
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

auto findIndexedSymbolOccurrenceAtBufferOffset(const AnalysisView& analysis, unsigned targetOffset)
    -> const lesma::IndexedSymbolOccurrence* {
  if (!isUsableAnalysis(analysis) || analysis.index == nullptr) {
    return nullptr;
  }
  const lesma::IndexedSymbolOccurrence* best = nullptr;
  unsigned bestLen = 0U;
  bool bestContains = false;
  for (const lesma::IndexedSymbolOccurrence& occurrence : analysis.index->symbolOccurrences) {
    if (!occurrence.span.isValid()) {
      continue;
    }
    unsigned const start =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.Start);
    unsigned const end =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, occurrence.span.End);
    bool const contains = targetOffset >= start && targetOffset < end;
    bool const justAfter = targetOffset == end;
    if (!contains && !justAfter) {
      continue;
    }
    unsigned const len = end > start ? end - start : 0U;
    if (best == nullptr || (contains && !bestContains) ||
        (contains == bestContains && len < bestLen)) {
      best = &occurrence;
      bestLen = len;
      bestContains = contains;
    }
  }
  return best;
}

auto findIndexedSymbolOccurrenceAtCursor(const AnalysisView& analysis, unsigned line,
                                         unsigned character)
    -> const lesma::IndexedSymbolOccurrence* {
  if (!isUsableAnalysis(analysis) || analysis.index == nullptr) {
    return nullptr;
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));
  return findIndexedSymbolOccurrenceAtBufferOffset(analysis, targetOffset);
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

auto collectSemanticTokens(AnalysisResult& analysisResult, unsigned bufferId)
    -> std::vector<std::uint32_t>;

auto appendCallParameterInlayHints(const AnalysisResult& analysisResult, unsigned bufferId,
                                   const ::lsp::Range& range, std::vector<::lsp::InlayHint>& hints)
    -> void;

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
  unsigned const rangeStart =
      static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(
          text, range.start.line, range.start.character));
  unsigned const rangeEnd =
      static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(
          text, range.end.line, range.end.character));

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
    std::vector<lesma::Literal*> const names = varDecl->getVarLiterals();
    std::vector<lesma::Value*> const& rs = varDecl->getResolvedSymbols();
    for (size_t i = 0; i < names.size(); ++i) {
      lesma::Literal* ident = names[i];
      lesma::Value* symbol = nullptr;
      if (i < rs.size()) {
        symbol = rs[i];
      } else if (names.size() == 1U) {
        symbol = varDecl->getResolvedSymbol();
      }
      if (ident == nullptr || symbol == nullptr || symbol->getType() == nullptr) {
        continue;
      }
      llvm::SMRange span = ident->getSpan();
      if (!span.isValid() || !isInRequestedRange(span)) {
        continue;
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
    }
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

  appendCallParameterInlayHints(analysisResult, bufferId, range, hints);
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

auto resolveCanonicalSymbolAtCursor(AnalysisResult& result, unsigned line, unsigned character,
                                    const CursorIdentifier& id) -> std::optional<ResolvedSymbol>;

auto resolveExpressionTypeAtOffset(const lesma::Expression* expr, lesma::Compound* ast,
                                   lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                                   unsigned bufferId, unsigned targetOffset) -> lesma::Type*;

auto resolveSubscriptResultType(lesma::Type* baseType, lesma::Type* indexType, lesma::Compound* ast,
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
    case lesma::TokenType::STRING_TYPE:
      return root->lookupType("cstr");
    case lesma::TokenType::STRING:
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
  if (auto const* subscript = dynamic_cast<const lesma::SubscriptOp*>(expr)) {
    lesma::Type* leftType = resolveExpressionTypeAtOffset(subscript->getLeft(), ast, root, srcMgr,
                                                          bufferId, targetOffset);
    lesma::Type* indexType = resolveExpressionTypeAtOffset(subscript->getIndex(), ast, root, srcMgr,
                                                           bufferId, targetOffset);
    if (leftType == nullptr) {
      return nullptr;
    }
    lesma::Type* baseType = leftType;
    if (baseType->is(lesma::BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
      baseType = baseType->getElementType();
    }
    if (baseType->is(lesma::BaseType::TY_ARRAY) && baseType->getElementType() != nullptr) {
      (void) indexType;
      return baseType->getElementType();
    }
    return resolveSubscriptResultType(baseType, indexType, ast, root, srcMgr, bufferId,
                                      targetOffset);
  }
  if (auto const* unary = dynamic_cast<const lesma::UnaryOp*>(expr)) {
    lesma::Type* operand = resolveExpressionTypeAtOffset(unary->getExpression(), ast, root, srcMgr,
                                                         bufferId, targetOffset);
    if (operand == nullptr) {
      return nullptr;
    }
    switch (unary->getOperator()) {
    case lesma::TokenType::MINUS:
      if (operand->isOneOf(
              {lesma::BaseType::TY_INT, lesma::BaseType::TY_FLOAT, lesma::BaseType::TY_FLOAT32}) ||
          operand->is(lesma::BaseType::TY_GENERIC)) {
        return operand;
      }
      return nullptr;
    case lesma::TokenType::BANG:
    case lesma::TokenType::NOT:
      return root->lookupType("bool");
    case lesma::TokenType::STAR:
      if (operand->is(lesma::BaseType::TY_PTR) && operand->getElementType() != nullptr) {
        return operand->getElementType();
      }
      return nullptr;
    case lesma::TokenType::AMPERSAND:
    default:
      return nullptr;
    }
  }
  if (auto const* sip = dynamic_cast<const lesma::StringInterpolation*>(expr)) {
    lesma::Type* strClass = sip->getResolvedStrClassType();
    if (strClass != nullptr) {
      return strClass;
    }
    return root->lookupType("str");
  }
  if (auto const* list = dynamic_cast<const lesma::ListLiteral*>(expr)) {
    if (list->getResolvedType() != nullptr) {
      return list->getResolvedType();
    }
    lesma::Type* elementType = nullptr;
    for (lesma::Expression* element : list->getElements()) {
      lesma::Type* t =
          resolveExpressionTypeAtOffset(element, ast, root, srcMgr, bufferId, targetOffset);
      if (t == nullptr) {
        return nullptr;
      }
      if (t->is(lesma::BaseType::TY_CLASS)) {
        return nullptr;
      }
      if (elementType == nullptr) {
        elementType = t;
      } else if (!elementType->isEqual(t)) {
        return nullptr;
      }
    }
    return nullptr;
  }
  if (auto const* dict = dynamic_cast<const lesma::DictLiteral*>(expr)) {
    if (dict->getResolvedType() != nullptr) {
      return dict->getResolvedType();
    }
    return nullptr;
  }
  if (auto const* tup = dynamic_cast<const lesma::TupleLiteral*>(expr)) {
    if (tup->getResolvedType() != nullptr) {
      return tup->getResolvedType();
    }
    return nullptr;
  }
  return nullptr;
}

auto findActiveCallInExpr(const lesma::Expression* expr, llvm::SourceMgr* srcMgr, unsigned bufferId,
                          unsigned targetOffset, const lesma::Expression* receiver,
                          ActiveCallSite& best) -> void {
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
    return;
  }
  if (auto const* si = dynamic_cast<const lesma::StringInterpolation*>(expr)) {
    for (lesma::Expression* e : si->getExprs()) {
      findActiveCallInExpr(e, srcMgr, bufferId, targetOffset, nullptr, best);
    }
    return;
  }
  if (auto const* dict = dynamic_cast<const lesma::DictLiteral*>(expr)) {
    for (lesma::Expression* k : dict->getKeys()) {
      findActiveCallInExpr(k, srcMgr, bufferId, targetOffset, nullptr, best);
    }
    for (lesma::Expression* v : dict->getValues()) {
      findActiveCallInExpr(v, srcMgr, bufferId, targetOffset, nullptr, best);
    }
    return;
  }
}

auto findActiveCallInStmt(const lesma::Statement* stmt, llvm::SourceMgr* srcMgr, unsigned bufferId,
                          unsigned targetOffset, ActiveCallSite& best) -> void {
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
  } else if (auto const* forIn = dynamic_cast<const lesma::ForIn*>(stmt)) {
    findActiveCallInExpr(forIn->getIterable(), srcMgr, bufferId, targetOffset, nullptr, best);
    findActiveCallInStmt(forIn->getBlock(), srcMgr, bufferId, targetOffset, best);
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
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));

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
  if (receiverType->is(lesma::BaseType::TY_UNION)) {
    return std::ranges::any_of(receiverType->getUnionMembers(), [selfType](lesma::Type* m) -> bool {
      return receiverMatchesSelf(m, selfType);
    });
  }
  if (receiverType->isEqual(selfType)) {
    return true;
  }
  if (receiverType->is(lesma::BaseType::TY_PTR) && receiverType->getElementType() != nullptr &&
      receiverType->getElementType()->isEqual(selfType)) {
    return true;
  }
  if (selfType->is(lesma::BaseType::TY_PTR) && selfType->getElementType() != nullptr &&
      receiverType->isEqual(selfType->getElementType())) {
    return true;
  }
  return false;
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

auto resolveSubscriptResultType(lesma::Type* baseType, lesma::Type* indexType, lesma::Compound* ast,
                                lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                                unsigned bufferId, unsigned targetOffset) -> lesma::Type* {
  if (baseType == nullptr || root == nullptr) {
    return nullptr;
  }
  lesma::SymbolTable* scope = activeScopeForOffset(ast, root, srcMgr, bufferId, targetOffset);
  if (scope == nullptr) {
    scope = root;
  }
  std::vector<lesma::Type*> indexArgs;
  if (indexType != nullptr) {
    indexArgs.push_back(indexType);
  }
  std::vector<CallableCandidate> candidates = collectCallableCandidates(
      scope, std::string{lesma::OperatorUtils::SUBSCRIPT_GET_NAME}, baseType, indexArgs);
  if (candidates.empty()) {
    return nullptr;
  }
  lesma::Type* fnType = candidates.front().value->getType();
  return fnType != nullptr ? fnType->getReturnType() : nullptr;
}

auto buildSignatureHelp(AnalysisResult& result, unsigned line, unsigned character)
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
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(text, line, character));
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

auto appendCallParameterInlayHints(const AnalysisResult& analysisResult, unsigned bufferId,
                                   const ::lsp::Range& range, std::vector<::lsp::InlayHint>& hints)
    -> void {
  llvm::SourceMgr* srcMgr = analysisResult.sourceMgr.get();
  lesma::SymbolTable* root = analysisResult.rootScope.get();
  lesma::Compound* ast = analysisResult.parser ? analysisResult.parser->getAst() : nullptr;
  if (srcMgr == nullptr || root == nullptr || ast == nullptr) {
    return;
  }
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return;
  }
  llvm::StringRef const text = buf->getBuffer();
  unsigned const rangeStart =
      static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(
          text, range.start.line, range.start.character));
  unsigned const rangeEnd =
      static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(
          text, range.end.line, range.end.character));

  auto isOffsetInRange = [&](unsigned offset) -> bool {
    return offset >= rangeStart && offset <= rangeEnd;
  };

  auto tryHintCall = [&](const lesma::FuncCall* call, const lesma::Expression* receiver) -> void {
    if (call == nullptr) {
      return;
    }
    llvm::SMRange const callSpan = call->getSpan();
    if (!callSpan.isValid()) {
      return;
    }
    unsigned const callStartOffset = getOffsetFromSMLoc(srcMgr, bufferId, callSpan.Start);
    lesma::SymbolTable* scope = activeScopeForOffset(ast, root, srcMgr, bufferId, callStartOffset);
    if (scope == nullptr) {
      scope = root;
    }
    std::vector<lesma::Expression*> const args = call->getArguments();
    std::vector<CallableCandidate> candidates;

    lesma::Value* resolvedSym = call->getResolvedSymbol();
    if (resolvedSym != nullptr && resolvedSym->getType() != nullptr &&
        resolvedSym->getType()->is(lesma::BaseType::TY_FUNCTION)) {
      std::vector<lesma::Field*> const rf = resolvedSym->getType()->getFields();
      unsigned paramOffset = 0U;
      if (receiver != nullptr && !rf.empty() && rf[0] != nullptr && rf[0]->name == "self") {
        paramOffset = 1U;
      }
      candidates.push_back(CallableCandidate{.value = resolvedSym, .paramOffset = paramOffset});
    } else {
      std::vector<lesma::Type*> argTypes;
      argTypes.reserve(args.size());
      for (lesma::Expression* arg : args) {
        lesma::Type* argType =
            resolveExpressionTypeAtOffset(arg, ast, root, srcMgr, bufferId, callStartOffset);
        if (argType == nullptr) {
          return;
        }
        argTypes.push_back(argType);
      }
      lesma::Type* receiverType = nullptr;
      if (receiver != nullptr) {
        receiverType =
            resolveExpressionTypeAtOffset(receiver, ast, root, srcMgr, bufferId, callStartOffset);
      }
      if (receiver == nullptr || receiverType != nullptr) {
        candidates = collectCallableCandidates(scope, call->getName(), receiverType, argTypes);
      }
      if (resolvedSym != nullptr) {
        std::vector<CallableCandidate> narrowed;
        for (const CallableCandidate& c : candidates) {
          if (c.value == resolvedSym) {
            narrowed.push_back(c);
          }
        }
        if (narrowed.size() == 1U) {
          candidates = std::move(narrowed);
        }
      }
    }
    if (candidates.size() != 1U) {
      return;
    }
    CallableCandidate const& cand = candidates.front();
    if (cand.value == nullptr || cand.value->getType() == nullptr) {
      return;
    }
    std::vector<lesma::Field*> fields = cand.value->getType()->getFields();
    for (size_t i = 0; i < args.size(); ++i) {
      size_t const fieldIdx = cand.paramOffset + i;
      if (fieldIdx >= fields.size()) {
        break;
      }
      lesma::Field* field = fields[fieldIdx];
      if (field == nullptr || field->name.empty()) {
        continue;
      }
      lesma::Expression* arg = args[i];
      llvm::SMRange argSpan = arg->getSpan();
      if (!argSpan.isValid()) {
        continue;
      }
      unsigned const argStart = getOffsetFromSMLoc(srcMgr, bufferId, argSpan.Start);
      if (!isOffsetInRange(argStart)) {
        continue;
      }
      ::lsp::Range const argLspRange = smRangeToLspRange(srcMgr, bufferId, argSpan);
      ::lsp::Position const hintPos = argLspRange.start;
      std::string const label = field->name + ":";
      ::lsp::InlayHint hint;
      hint.position = hintPos;
      hint.label = ::lsp::String(label);
      hint.kind = ::lsp::Opt<::lsp::InlayHintKindEnum>(::lsp::InlayHintKind::Parameter);
      hints.push_back(std::move(hint));
    }
  };

  std::function<void(const lesma::Expression*, const lesma::Expression*)> walkExpr =
      [&](const lesma::Expression* expr, const lesma::Expression* dotReceiver) -> void {
    if (expr == nullptr) {
      return;
    }
    if (auto const* call = dynamic_cast<const lesma::FuncCall*>(expr)) {
      tryHintCall(call, dotReceiver);
      for (lesma::Expression* arg : call->getArguments()) {
        walkExpr(arg, nullptr);
      }
      return;
    }
    if (auto const* dot = dynamic_cast<const lesma::DotOp*>(expr)) {
      if (dot->getRight() != nullptr) {
        walkExpr(dot->getRight(), dot->getLeft());
      }
      if (dot->getLeft() != nullptr) {
        walkExpr(dot->getLeft(), nullptr);
      }
      return;
    }
    if (auto const* binary = dynamic_cast<const lesma::BinaryOp*>(expr)) {
      walkExpr(binary->getLeft(), nullptr);
      walkExpr(binary->getRight(), nullptr);
      return;
    }
    if (auto const* unary = dynamic_cast<const lesma::UnaryOp*>(expr)) {
      walkExpr(unary->getExpression(), nullptr);
      return;
    }
    if (auto const* castOp = dynamic_cast<const lesma::CastOp*>(expr)) {
      walkExpr(castOp->getExpression(), nullptr);
      return;
    }
    if (auto const* isOp = dynamic_cast<const lesma::IsOp*>(expr)) {
      walkExpr(isOp->getLeft(), nullptr);
      return;
    }
    if (auto const* si = dynamic_cast<const lesma::StringInterpolation*>(expr)) {
      for (lesma::Expression* e : si->getExprs()) {
        walkExpr(e, nullptr);
      }
      return;
    }
    if (auto const* sub = dynamic_cast<const lesma::SubscriptOp*>(expr)) {
      walkExpr(sub->getLeft(), nullptr);
      walkExpr(sub->getIndex(), nullptr);
      return;
    }
    if (auto const* list = dynamic_cast<const lesma::ListLiteral*>(expr)) {
      for (lesma::Expression* el : list->getElements()) {
        walkExpr(el, nullptr);
      }
      return;
    }
    if (auto const* dict = dynamic_cast<const lesma::DictLiteral*>(expr)) {
      for (lesma::Expression* k : dict->getKeys()) {
        walkExpr(k, nullptr);
      }
      for (lesma::Expression* v : dict->getValues()) {
        walkExpr(v, nullptr);
      }
      return;
    }
    if (auto const* tup = dynamic_cast<const lesma::TupleLiteral*>(expr)) {
      for (lesma::Expression* el : tup->getElements()) {
        walkExpr(el, nullptr);
      }
    }
  };

  std::function<void(const lesma::Statement*)> walkStmt =
      [&](const lesma::Statement* stmt) -> void {
    if (stmt == nullptr) {
      return;
    }
    if (auto const* exprStmt = dynamic_cast<const lesma::ExpressionStatement*>(stmt)) {
      walkExpr(exprStmt->getExpression(), nullptr);
    } else if (auto const* varDecl = dynamic_cast<const lesma::VarDecl*>(stmt)) {
      walkExpr(varDecl->getValue(), nullptr);
    } else if (auto const* assign = dynamic_cast<const lesma::Assignment*>(stmt)) {
      walkExpr(assign->getLeftHandSide(), nullptr);
      walkExpr(assign->getRightHandSide(), nullptr);
    } else if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
      for (lesma::Expression* cond : ifNode->getConds()) {
        walkExpr(cond, nullptr);
      }
      for (lesma::Compound* block : ifNode->getBlocks()) {
        walkStmt(block);
      }
    } else if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
      walkExpr(whileNode->getCond(), nullptr);
      walkStmt(whileNode->getBlock());
    } else if (auto const* forIn = dynamic_cast<const lesma::ForIn*>(stmt)) {
      walkExpr(forIn->getIterable(), nullptr);
      walkStmt(forIn->getBlock());
    } else if (auto const* ret = dynamic_cast<const lesma::Return*>(stmt)) {
      walkExpr(ret->getValue(), nullptr);
    } else if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
      walkStmt(defer->getStatement());
    } else if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
      for (lesma::Statement* child : compound->getChildren()) {
        walkStmt(child);
      }
    } else if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(stmt)) {
      walkStmt(func->getBody());
    } else if (auto const* klass = dynamic_cast<const lesma::Class*>(stmt)) {
      for (lesma::VarDecl* field : klass->getFields()) {
        walkStmt(field);
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        walkStmt(method);
      }
    }
  };

  for (lesma::Statement* stmt : ast->getChildren()) {
    walkStmt(stmt);
  }
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
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));
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

auto resolveImportedSymbol(AnalysisResult& result, const AnalysisView& analysis,
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

/** Same set as Codegen::isBuiltinListBuiltinMethodName — `__buffer<T>` dot calls. */
auto isBuiltinBufferListMethodName(const std::string& methodName) -> bool {
  return OperatorUtils::isBuiltinListMethodName(methodName);
}

auto findTopLevelClassNamed(Compound* compound, std::string_view className) -> Class* {
  if (compound == nullptr) {
    return nullptr;
  }
  for (Statement* stmt : compound->getChildren()) {
    auto* klass = dynamic_cast<Class*>(stmt);
    if (klass != nullptr && klass->getIdentifier() == className) {
      return klass;
    }
  }
  return nullptr;
}

/** Buffer methods share semantics with `export class list` in stdlib/base.les. */
auto findBuiltinBufferListMethodInStdlib(AnalysisResult& result, const std::string& methodName)
    -> std::optional<ResolvedSymbol> {
  for (const AnalysisView& view : collectAnalysisViews(result)) {
    if (!isUsableAnalysis(view) || view.mainFilePath == nullptr || view.ast == nullptr) {
      continue;
    }
    std::string const p = normalizePath(*view.mainFilePath);
    if (!p.ends_with("base.les")) {
      continue;
    }
    Class* listClass = findTopLevelClassNamed(view.ast, "list");
    if (listClass == nullptr) {
      continue;
    }
    for (FuncDecl* method : listClass->getMethods()) {
      if (method != nullptr && method->getName() == methodName) {
        Value* sym = method->getResolvedSymbol();
        if (sym != nullptr) {
          return ResolvedSymbol{.value = sym, .owner = view};
        }
      }
    }
  }
  return std::nullopt;
}

auto resolveMethodSymbolAtCursor(AnalysisResult& result, const AnalysisView& analysis,
                                 unsigned line, unsigned character, const CursorIdentifier& id)
    -> std::optional<ResolvedSymbol> {
  std::optional<ActiveCallSite> activeCall = findActiveCallSite(analysis, line, character);
  if (!activeCall || activeCall->call == nullptr || activeCall->receiver == nullptr ||
      activeCall->call->getName() != id.name || analysis.rootScope == nullptr ||
      analysis.sourceMgr == nullptr) {
    return std::nullopt;
  }
  if (id.dotBase.has_value()) {
    auto const* receiverLit = dynamic_cast<const lesma::Literal*>(activeCall->receiver);
    if (receiverLit != nullptr && receiverLit->getType() == lesma::TokenType::IDENTIFIER &&
        receiverLit->getValue() != *id.dotBase) {
      return std::nullopt;
    }
  }

  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));
  std::vector<lesma::Type*> argTypes;
  for (lesma::Expression* arg : activeCall->call->getArguments()) {
    lesma::Type* argType = resolveExpressionTypeAtOffset(
        arg, analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId, targetOffset);
    if (argType == nullptr) {
      return std::nullopt;
    }
    argTypes.push_back(argType);
  }

  lesma::Type* receiverType =
      resolveExpressionTypeAtOffset(activeCall->receiver, analysis.ast, analysis.rootScope,
                                    analysis.sourceMgr, analysis.bufferId, targetOffset);
  if (receiverType == nullptr) {
    return std::nullopt;
  }

  for (const AnalysisView& candidateAnalysis : collectAnalysisViews(result)) {
    if (!isUsableAnalysis(candidateAnalysis) || candidateAnalysis.rootScope == nullptr) {
      continue;
    }
    std::vector<CallableCandidate> candidates =
        collectCallableCandidates(candidateAnalysis.rootScope, id.name, receiverType, argTypes);
    if (!candidates.empty() && candidates.front().value != nullptr) {
      return ResolvedSymbol{.value = candidates.front().value, .owner = candidateAnalysis};
    }
  }

  lesma::Type* bufferReceiver = receiverType;
  if (bufferReceiver != nullptr && bufferReceiver->is(lesma::BaseType::TY_PTR) &&
      bufferReceiver->getElementType() != nullptr) {
    bufferReceiver = bufferReceiver->getElementType();
  }
  if (bufferReceiver != nullptr && bufferReceiver->is(lesma::BaseType::TY_ARRAY) &&
      isBuiltinBufferListMethodName(id.name)) {
    return findBuiltinBufferListMethodInStdlib(result, id.name);
  }
  return std::nullopt;
}

auto resolveCanonicalSymbolAtCursor(AnalysisResult& result, const AnalysisView& analysis,
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
  if (std::optional<ResolvedSymbol> method =
          resolveMethodSymbolAtCursor(result, analysis, line, character, id)) {
    return method;
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

auto resolveCanonicalSymbolAtCursor(AnalysisResult& result, unsigned line, unsigned character,
                                    const CursorIdentifier& id) -> std::optional<ResolvedSymbol> {
  return resolveCanonicalSymbolAtCursor(result, makeAnalysisView(result), line, character, id);
}

auto symbolIdentityForResolved(AnalysisResult& result, const ResolvedSymbol& resolved)
    -> std::optional<SymbolIdentity> {
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
  std::optional<::lsp::Range> declRange =
      lspRangeForValueDeclaration(result, resolved.value, resolved.owner);
  if (!declRange) {
    return std::nullopt;
  }
  return SymbolIdentity{
      .path = normalizePath(path),
      .range = *declRange,
      .name = resolved.value->getName(),
  };
}

auto symbolIdentityForIndexedDeclaration(AnalysisResult& result,
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
constexpr unsigned STATIC = 1U << 2U;
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

auto collectSemanticTokens(AnalysisResult& analysisResult, unsigned bufferId)
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
    return type != nullptr &&
           type->isOneOf({lesma::BaseType::TY_INT, lesma::BaseType::TY_FLOAT,
                          lesma::BaseType::TY_FLOAT32, lesma::BaseType::TY_STRING,
                          lesma::BaseType::TY_BOOL, lesma::BaseType::TY_VOID});
  };

  auto semanticTokenTypeForResolved = [](const ResolvedSymbol& resolved, bool isTypePosition,
                                         bool isMemberAccess) -> SemanticTokenType {
    return semanticTokenTypeFromIndexedKind(lesma::indexedTokenKindFromResolvedSymbol(
        resolved.value, isTypePosition, isMemberAccess, lesma::IndexedTokenKind::Variable));
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
    if (declSpan.isValid() && !declPath.empty() && normalizePath(declPath) == currentPath) {
      if (std::optional<::lsp::Range> declRange =
              lspRangeForValueDeclaration(analysisResult, resolved.value, resolved.owner);
          declRange && rangeEquals(*declRange, occurrenceRange)) {
        modifiers |= semantic_token_modifier::DECLARATION;
      }
    }
    if (isDefaultLibraryType(resolved.value->getType())) {
      modifiers |= semantic_token_modifier::DEFAULT_LIBRARY;
    }
    if (resolved.value->isStaticMethod()) {
      modifiers |= semantic_token_modifier::STATIC;
    } else if (lesma::Type* declCls = resolved.value->getMemberDeclaredInClass();
               declCls != nullptr &&
               lesma::TypeUtils::findStaticFieldInClass(declCls, resolved.value->getName()) !=
                   nullptr) {
      modifiers |= semantic_token_modifier::STATIC;
    }
    return modifiers;
  };

  for (const lesma::IndexedSymbolOccurrence& occurrence : analysis.index->symbolOccurrences) {
    if (!occurrence.span.isValid()) {
      continue;
    }
    if (!occurrence.isTypePosition && !occurrence.isMemberAccess &&
        !occurrence.dotBase.has_value() && occurrence.name == "self") {
      continue;
    }
    llvm::SMRange const highlightSpan =
        occurrence.semanticHighlightSpan.value_or(occurrence.span);
    ::lsp::Range const range = smRangeToLspRange(srcMgr, bufferId, occurrence.span);
    CursorIdentifier const id =
        makeCursorIdentifierFromSpan(occurrence.name, occurrence.span, srcMgr, bufferId,
                                     occurrence.isTypePosition, occurrence.dotBase);
    if (std::optional<ResolvedSymbol> resolved = resolveCanonicalSymbolAtCursor(
            analysisResult, range.start.line, range.start.character, id)) {
      appendRawTokenFromSpan(
          highlightSpan,
          semanticTokenTypeForResolved(*resolved, id.isTypePosition, occurrence.isMemberAccess),
          semanticTokenModifiersForResolved(*resolved, range, occurrence.modifiers));
      continue;
    }
    if (occurrence.fallbackTokenKind == lesma::IndexedTokenKind::EnumMember) {
      appendRawTokenFromSpan(highlightSpan, SemanticTokenType::EnumMember, occurrence.modifiers);
      continue;
    }
    if (occurrence.fallbackTokenKind.has_value()) {
      appendRawTokenFromSpan(highlightSpan,
                             semanticTokenTypeFromIndexedKind(*occurrence.fallbackTokenKind),
                             occurrence.modifiers);
    }
  }

  std::ranges::sort(rawTokens,
                    [](const RawSemanticToken& lhs, const RawSemanticToken& rhs) -> bool {
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
                              [](const RawSemanticToken& lhs, const RawSemanticToken& rhs) -> bool {
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

auto collectReferences(AnalysisResult& result, unsigned line, unsigned character,
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
  const lesma::IndexedSymbolOccurrence* targetOccurrence =
      findIndexedSymbolOccurrenceAtCursor(mainAnalysis, line, character);

  std::optional<ResolvedSymbol> targetResolved =
      resolveCanonicalSymbolAtCursor(result, mainAnalysis, line, character, *id);
  std::optional<SymbolIdentity> targetIdentity;
  if (targetOccurrence != nullptr && targetOccurrence->declaration.has_value() &&
      declarationBelongsToAnalysis(mainAnalysis, *targetOccurrence->declaration)) {
    targetIdentity =
        symbolIdentityForIndexedDeclaration(result, *targetOccurrence->declaration, id->name);
  }
  if (!targetIdentity) {
    if (targetResolved) {
      targetIdentity = symbolIdentityForResolved(result, *targetResolved);
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
          occurrenceIdentity =
              symbolIdentityForIndexedDeclaration(result, *occurrence.declaration, occurrence.name);
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
          occurrenceIdentity = symbolIdentityForResolved(result, *resolved);
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
      std::vector<lesma::Literal*> const names = varDecl->getVarLiterals();
      std::vector<lesma::Value*> const& rs = varDecl->getResolvedSymbols();
      for (size_t i = 0; i < names.size(); ++i) {
        lesma::Literal* lit = names[i];
        if (lit == nullptr) {
          continue;
        }
        lesma::Value* value = nullptr;
        if (i < rs.size()) {
          value = rs[i];
        } else if (names.size() == 1U) {
          value = varDecl->getResolvedSymbol();
        }
        std::optional<std::string> typeDetail = std::nullopt;
        if (value != nullptr) {
          typeDetail = formatTypeName(value->getType(), result.rootScope.get());
        }
        symbols.push_back(makeDocumentSymbol(
            lit->getValue(), ::lsp::SymbolKind::Variable,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, varDecl->getSpan()),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, lit->getSpan()),
            typeDetail));
      }
      continue;
    }
    if (auto* func = dynamic_cast<lesma::FuncDecl*>(stmt)) {
      lesma::Value* value = func->getResolvedSymbol();
      std::string symName = func->getName();
      if (auto spell = lesma::OperatorUtils::surfaceSpellingForMangledOperator(symName)) {
        symName = std::string(*spell);
      }
      symbols.push_back(makeDocumentSymbol(
          symName, ::lsp::SymbolKind::Function,
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
        std::optional<std::string> fieldDetail = std::nullopt;
        if (value != nullptr) {
          fieldDetail = formatTypeName(value->getType(), result.rootScope.get());
          if (field->getIsStatic()) {
            fieldDetail = fieldDetail.has_value() && !fieldDetail->empty()
                              ? *fieldDetail + " (static)"
                              : std::optional<std::string>("static");
          }
        } else if (field->getIsStatic()) {
          fieldDetail = "static";
        }
        children.push_back(makeDocumentSymbol(
            field->getIdentifier()->getValue(), ::lsp::SymbolKind::Field,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, field->getSpan()),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId,
                              field->getIdentifier()->getSpan()),
            fieldDetail));
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        lesma::Value* value = method->getResolvedSymbol();
        std::string methodName = method->getName();
        if (auto spell = lesma::OperatorUtils::surfaceSpellingForMangledOperator(methodName)) {
          methodName = std::string(*spell);
        }
        std::optional<std::string> methodDetail =
            value != nullptr && value->getType() != nullptr &&
                    value->getType()->getReturnType() != nullptr
                ? std::optional<std::string>(
                      formatTypeName(value->getType()->getReturnType(), result.rootScope.get()))
                : std::nullopt;
        if (method->getIsStatic()) {
          methodDetail = methodDetail.has_value() && !methodDetail->empty()
                             ? *methodDetail + " (static)"
                             : std::optional<std::string>("static");
        }
        children.push_back(makeDocumentSymbol(
            methodName, ::lsp::SymbolKind::Method,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId,
                              funcDeclFullSpan(method)),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, method->getNameSpan()),
            methodDetail));
      }
      symbol.children = ::lsp::Opt<::lsp::Array<::lsp::DocumentSymbol>>(
          ::lsp::Array<::lsp::DocumentSymbol>(children.begin(), children.end()));
      symbols.push_back(std::move(symbol));
    }
  }

  return symbols;
}

/** When the receiver is a class union and multiple methods match, return every definition. */
auto tryResolveUnionMultiMethodDefinitionLocations(AnalysisResult& result, unsigned line,
                                                   unsigned character)
    -> std::vector<::lsp::Location> {
  if (result.parser == nullptr || result.sourceMgr == nullptr) {
    return {};
  }
  AnalysisView analysis = makeAnalysisView(result);
  if (!isUsableAnalysis(analysis) || analysis.ast == nullptr || analysis.rootScope == nullptr) {
    return {};
  }
  std::optional<CursorIdentifier> id = findIdentifierAtCursor(analysis, line, character);
  if (!id) {
    return {};
  }
  std::optional<ActiveCallSite> activeCall = findActiveCallSite(analysis, line, character);
  if (!activeCall || activeCall->call == nullptr || activeCall->receiver == nullptr ||
      activeCall->call->getName() != id->name) {
    return {};
  }
  auto const* buf = analysis.sourceMgr->getMemoryBuffer(analysis.bufferId);
  if (buf == nullptr) {
    return {};
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(buf->getBuffer(), line, character));
  std::vector<lesma::Type*> argTypes;
  for (lesma::Expression* arg : activeCall->call->getArguments()) {
    lesma::Type* argType = resolveExpressionTypeAtOffset(
        arg, analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId, targetOffset);
    if (argType == nullptr) {
      return {};
    }
    argTypes.push_back(argType);
  }
  lesma::Type* receiverType =
      resolveExpressionTypeAtOffset(activeCall->receiver, analysis.ast, analysis.rootScope,
                                    analysis.sourceMgr, analysis.bufferId, targetOffset);
  llvm::SMRange const receiverSpan = activeCall->receiver->getSpan();
  if (receiverSpan.isValid()) {
    unsigned const recvStart =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, receiverSpan.Start);
    unsigned const recvEnd =
        getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, receiverSpan.End);
    if (const lesma::IndexedSymbolOccurrence* recvOcc =
            findIndexedSymbolOccurrenceAtBufferOffset(analysis, recvStart);
        recvOcc != nullptr && recvOcc->flowSensitiveType != nullptr) {
      unsigned const occStart =
          getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, recvOcc->span.Start);
      unsigned const occEnd =
          getOffsetFromSMLoc(analysis.sourceMgr, analysis.bufferId, recvOcc->span.End);
      if (occStart >= recvStart && occEnd <= recvEnd) {
        receiverType = recvOcc->flowSensitiveType;
      }
    }
  }
  if (receiverType == nullptr || !receiverType->is(lesma::BaseType::TY_UNION)) {
    return {};
  }
  std::vector<std::pair<CallableCandidate, AnalysisView>> aggregated;
  for (const AnalysisView& candidateAnalysis : collectAnalysisViews(result)) {
    if (!isUsableAnalysis(candidateAnalysis) || candidateAnalysis.rootScope == nullptr) {
      continue;
    }
    std::vector<CallableCandidate> perView =
        collectCallableCandidates(candidateAnalysis.rootScope, id->name, receiverType, argTypes);
    for (const CallableCandidate& cand : perView) {
      aggregated.emplace_back(cand, candidateAnalysis);
    }
  }
  if (aggregated.size() <= 1U) {
    return {};
  }
  std::vector<::lsp::Location> out;
  std::unordered_set<std::string> seen;
  for (const auto& [cand, candidateAnalysis] : aggregated) {
    if (cand.value == nullptr) {
      continue;
    }
    std::string declPath = cand.value->getDeclarationFilePath();
    if (declPath.empty() && candidateAnalysis.mainFilePath != nullptr) {
      declPath = *candidateAnalysis.mainFilePath;
    }
    if (declPath.empty()) {
      continue;
    }
    std::optional<::lsp::Range> mappedRange =
        lspRangeForValueDeclaration(result, cand.value, candidateAnalysis);
    if (!mappedRange) {
      continue;
    }
    std::string const key = normalizePath(declPath) + "#" +
                            std::to_string(mappedRange->start.line) + ":" +
                            std::to_string(mappedRange->start.character);
    if (!seen.insert(key).second) {
      continue;
    }
    out.push_back(::lsp::Location{
        .uri = uriFromPath(declPath),
        .range = *mappedRange,
    });
  }
  return out;
}

auto analyzedModuleEntryLocation(AnalysisResult& result, const std::string& modulePath)
    -> std::optional<::lsp::Location> {
  std::optional<AnalysisView> imported = findAnalysisViewForPath(result, modulePath);
  if (!imported || imported->mainFilePath == nullptr) {
    return std::nullopt;
  }
  return ::lsp::Location{
      .uri = uriFromPath(*imported->mainFilePath),
      .range =
          ::lsp::Range{
              .start = ::lsp::Position{.line = 0U, .character = 0U},
              .end = ::lsp::Position{.line = 0U, .character = 0U},
          },
  };
}

/** Resolve definition location using compiler metadata from Value. */
auto tryResolveDefinitionLocation(AnalysisResult& result, unsigned line, unsigned character)
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
    return std::nullopt;
  }
  llvm::SMRange declSpan = resolved->value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    if (!id->dotBase) {
      AnalysisView mainAnalysis = makeAnalysisView(result);
      std::optional<std::string> modulePath = findModuleImportPathByAlias(mainAnalysis, id->name);
      if (!modulePath) {
        if (auto it = result.importAliasToPath.find(id->name);
            it != result.importAliasToPath.end()) {
          modulePath = it->second;
        }
      }
      if (modulePath) {
        if (std::optional<::lsp::Location> loc = analyzedModuleEntryLocation(result, *modulePath)) {
          return loc;
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
  std::optional<::lsp::Range> mappedRange =
      lspRangeForValueDeclaration(result, resolved->value, resolved->owner);
  if (!mappedRange) {
    return std::nullopt;
  }
  return ::lsp::Location{
      .uri = uriFromPath(declPath),
      .range = *mappedRange,
  };
}

auto tryResolveDeclarationLocation(AnalysisResult& result, unsigned line, unsigned character)
    -> std::optional<::lsp::Location> {
  if (result.parser == nullptr || result.sourceMgr == nullptr ||
      result.parser->getAst() == nullptr) {
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
          ::lsp::Opt<::lsp::PositionEncodingKindEnum>(::lsp::PositionEncodingKind::UTF8);
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
      // Letters are not listed here: listing a–z would request completion on every identifier
      // keystroke. Import paths live in strings; VS Code/Cursor need
      // editor.quickSuggestions.strings (defaults in tools/vscode/package.json) so typing inside
      // "…" still triggers completion.
      caps.completionProvider = ::lsp::Opt<::lsp::CompletionOptions>(::lsp::CompletionOptions{
          .triggerCharacters = ::lsp::Opt<::lsp::Array<::lsp::String>>(
              {std::string("\""), std::string("/"), std::string(".")}),
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
                          ::lsp::Array<::lsp::String>{"declaration", "defaultLibrary", "static"},
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
          if (std::optional<std::string> path = docStore.getPath(params.textDocument.uri)) {
            invalidateLazyImportedAnalysesAffectedBy(*path);
          }
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
                std::optional<CursorIdentifier> id =
                    findIdentifierAtCursor(analysis, line, character);
                if (!id) {
                  return {};
                }
                std::optional<ResolvedSymbol> resolved =
                    resolveCanonicalSymbolAtCursor(result, line, character, *id);
                if (resolved && resolved->value != nullptr &&
                    resolved->value->getType() != nullptr) {
                  lesma::Type* flowTy = nullptr;
                  if (const lesma::IndexedSymbolOccurrence* occ =
                          findIndexedSymbolOccurrenceAtCursor(analysis, line, character);
                      occ != nullptr && occ->flowSensitiveType != nullptr) {
                    flowTy = occ->flowSensitiveType;
                  }
                  std::string hoverText =
                      formatHoverContent(resolved->value, resolved->owner.rootScope, flowTy);
                  if (std::string doc = documentationCommentAboveDeclaration(
                          result, resolved->value, resolved->owner);
                      !doc.empty()) {
                    hoverText += "\n\n---\n\n";
                    hoverText += doc;
                  }
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
                        static_cast<unsigned>(lesma::lsp_srv::bufferByteOffsetFromLspUtf8Position(
                            buf->getBuffer(), line, character));
                    InnermostFunc sigFunc = findFuncWithCursorInSignature(
                        ast, targetOffset, result.sourceMgr.get(), result.mainBufferId);
                    InnermostFunc inner = findInnermostFuncContaining(
                        ast, targetOffset, result.sourceMgr.get(), result.mainBufferId);
                    bool inGenericScope =
                        (sigFunc.func != nullptr &&
                         vectorContainsString(sigFunc.func->getGenericParams(), id->name)) ||
                        (sigFunc.enclosingClass != nullptr &&
                         vectorContainsString(sigFunc.enclosingClass->getGenericParams(),
                                              id->name)) ||
                        (inner.func != nullptr &&
                         vectorContainsString(inner.func->getGenericParams(), id->name)) ||
                        (inner.enclosingClass != nullptr &&
                         vectorContainsString(inner.enclosingClass->getGenericParams(), id->name));
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
                lesma::lsp_srv::CompletionOutcome outcome = lesma::lsp_srv::completionItems(
                    result, params.position.line, params.position.character);
                if (outcome.items.empty() && !outcome.isIncomplete) {
                  return {};
                }
                if (outcome.isIncomplete) {
                  return ::lsp::CompletionList{.isIncomplete = true,
                                               .items = std::move(outcome.items)};
                }
                return {outcome.items};
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
                std::vector<::lsp::Location> const multiLocs =
                    tryResolveUnionMultiMethodDefinitionLocations(result, params.position.line,
                                                                  params.position.character);
                if (multiLocs.size() > 1U) {
                  return {::lsp::Definition(
                      ::lsp::Array<::lsp::Location>(multiLocs.begin(), multiLocs.end()))};
                }
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
                std::vector<::lsp::Location> const multiDecls =
                    tryResolveUnionMultiMethodDefinitionLocations(result, params.position.line,
                                                                  params.position.character);
                if (multiDecls.size() > 1U) {
                  return {::lsp::Declaration(
                      ::lsp::Array<::lsp::Location>(multiDecls.begin(), multiDecls.end()))};
                }
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
