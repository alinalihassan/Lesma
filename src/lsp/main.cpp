#include <atomic>
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
  DocumentAnalysisSnapshot& getOrAnalyze(const ::lsp::DocumentUri& uri,
                                         const lesma::lsp_srv::DocumentStore& docStore,
                                         const std::string& content, int version) {
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

struct AnalysisView {
  llvm::SourceMgr* sourceMgr = nullptr;
  unsigned bufferId = 0;
  const std::string* mainFilePath = nullptr;
  lesma::Compound* ast = nullptr;
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
      .rootScope = result.rootScope.get(),
      .importAliasToPath = &result.importAliasToPath,
      .importedNameToSource = &result.importedNameToSource,
      .importedModules = &result.importedModules,
  };
}

auto isUsableAnalysis(const AnalysisView& analysis) -> bool {
  return analysis.sourceMgr != nullptr && analysis.rootScope != nullptr && analysis.ast != nullptr &&
         analysis.mainFilePath != nullptr;
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

auto makeImportedModuleAnalysis(AnalysisResult analyzed)
    -> std::shared_ptr<lesma::ImportedModuleAnalysis> {
  auto imported = std::make_shared<lesma::ImportedModuleAnalysis>();
  imported->sourceMgr = std::move(analyzed.sourceMgr);
  imported->mainBufferId = analyzed.mainBufferId;
  imported->mainFilePath = std::move(analyzed.mainFilePath);
  imported->parser = std::move(analyzed.parser);
  imported->rootScope = std::move(analyzed.rootScope);
  imported->typeCache = std::move(analyzed.typeCache);
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

::lsp::Range smRangeToLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span) {
  if (!span.isValid()) {
    return ::lsp::Range{.start = {0U, 0U}, .end = {0U, 0U}};
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

void runAnalyzeAndPublish(const ::lsp::DocumentUri& uri,
                          const lesma::lsp_srv::DocumentStore& docStore, const std::string& content,
                          int version, AnalysisCache& analysisCache,
                          ::lsp::MessageHandler& messageHandler) {
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
std::string getTypeName(lesma::Type* type, lesma::SymbolTable* rootScope) {
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

std::string formatTypeName(lesma::Type* type, lesma::SymbolTable* rootScope) {
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
    return formatTypeName(type->getElementType(), rootScope) + "[]";
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

/** Build hover text from a symbol: name, kind, and type in readable Markdown. */
std::string formatHoverContent(lesma::Value* value, lesma::SymbolTable* rootScope) {
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
    return "**" + name + "**\n\nType: `" + typeStr + "`";
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

bool isBuiltinTypeName(const std::string& name) {
  return name == "int" || name == "float" || name == "bool" || name == "str" || name == "void";
}

bool containsGenericParam(const std::vector<std::string>& genericParams, const std::string& name) {
  return std::find(genericParams.begin(), genericParams.end(), name) != genericParams.end();
}

unsigned getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

struct InnermostFunc {
  lesma::FuncDecl* func = nullptr;
  lesma::Class* enclosingClass = nullptr;
};

void considerFunc(lesma::FuncDecl* f, lesma::Class* cls, unsigned targetOffset, llvm::SourceMgr* sm,
                  unsigned bid, InnermostFunc& best, unsigned& bestLen) {
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

void scanCompoundForFuncs(lesma::Compound* c, lesma::Class* cls, unsigned targetOffset,
                          llvm::SourceMgr* sm, unsigned bid, InnermostFunc& best,
                          unsigned& bestLen) {
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

enum Level { LOW, MEDIUM, HIGH };

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
  std::function<void(lesma::Statement*, lesma::Class*)> scan = [&](lesma::Statement* stmt,
                                                                     lesma::Class* cls) {
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
        out.func = const_cast<lesma::FuncDecl*>(f);
        out.enclosingClass = cls;
        return;
      }
      for (lesma::Parameter* p : f->getParameters()) {
        if (p != nullptr && inSpan(p->nameSpan)) {
          out.func = const_cast<lesma::FuncDecl*>(f);
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
          scan(m, const_cast<lesma::Class*>(c));
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
  std::function<void(const lesma::Statement*)> scan = [&](const lesma::Statement* stmt) {
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
  std::function<void(const lesma::Statement*)> scan = [&](const lesma::Statement* stmt) {
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
    }
  };
  for (lesma::Statement* stmt : ast->getChildren()) {
    scan(stmt);
  }
  return out;
}

lesma::Value* lookupValueForHover(lesma::Compound* ast, lesma::SymbolTable* root,
                                  llvm::SourceMgr* srcMgr, unsigned bufferId, unsigned line,
                                  unsigned character, const std::string& name,
                                  bool isTypePosition) {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));

  // If cursor is in a function's signature (e.g. on a parameter or function name), use the resolved symbol.
  // Use the symbol the typechecker resolved for this exact overload (getResolvedSymbol).
  InnermostFunc const sigFunc = findFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId);
  if (sigFunc.func != nullptr) {
    lesma::Value* funcSym = sigFunc.func->getResolvedSymbol();
    if (funcSym == nullptr) {
      funcSym = root->lookup(sigFunc.func->getName());
    }
    // If hovering on the function name itself, return the function symbol
    if (funcSym != nullptr && name == sigFunc.func->getName()) {
      return funcSym;
    }
    if (isTypePosition && sigFunc.func->getGenericScope() != nullptr) {
      if (lesma::Value* v = sigFunc.func->getGenericScope()->lookup(name)) {
        return v;
      }
    }
    // Otherwise, look up in the function's body scope (for parameters, locals)
    if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
      if (lesma::Value* v = funcSym->getBodyScope()->lookup(name)) {
        return v;
      }
    }
  }

  if (auto const* sigExtern =
          findExternFuncWithCursorInSignature(ast, targetOffset, srcMgr, bufferId)) {
    lesma::Value* funcSym = sigExtern->getResolvedSymbol();
    if (funcSym == nullptr) {
      funcSym = root->lookup(sigExtern->getName());
    }
    if (funcSym != nullptr && name == sigExtern->getName()) {
      return funcSym;
    }
    if (isTypePosition && sigExtern->getGenericScope() != nullptr) {
      if (lesma::Value* v = sigExtern->getGenericScope()->lookup(name)) {
        return v;
      }
    }
    if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
      if (lesma::Value* v = funcSym->getBodyScope()->lookup(name)) {
        return v;
      }
    }
  }

  // Cursor in a function body: use the innermost function's resolved symbol (correct overload).
  InnermostFunc const inner = findInnermostFuncContaining(ast, targetOffset, srcMgr, bufferId);
  if (inner.func != nullptr) {
    lesma::Value* funcSym = inner.func->getResolvedSymbol();
    if (funcSym == nullptr) {
      funcSym = root->lookup(inner.func->getName());
    }
    // If hovering on the function name itself, return the function symbol
    if (funcSym != nullptr && name == inner.func->getName()) {
      return funcSym;
    }
    if (isTypePosition && inner.func->getGenericScope() != nullptr) {
      if (lesma::Value* v = inner.func->getGenericScope()->lookup(name)) {
        return v;
      }
    }
    // Otherwise, look up in the function's body scope (for locals)
    if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
      if (lesma::Value* v = funcSym->getBodyScope()->lookup(name)) {
        return v;
      }
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

/** Identifier at cursor plus optional "dot base" and source range (for hover.range to avoid duplicate symbol in UI). */
struct CursorIdentifier {
  std::string name;
  std::optional<std::string> dotBase;
  std::optional<::lsp::Range> range;
  bool isTypePosition = false;
};

/** Find identifier at cursor position by walking AST to find the identifier token. */
std::optional<CursorIdentifier> findIdentifierAtCursor(const lesma::Compound* ast,
                                                        llvm::SourceMgr* srcMgr, unsigned bufferId,
                                                        unsigned line, unsigned character) {
  unsigned targetOffset = 0;
  {
    auto const* buf = srcMgr->getMemoryBuffer(bufferId);
    if (buf == nullptr) {
      return std::nullopt;
    }
    llvm::StringRef ref = buf->getBuffer();
    // LSP uses UTF-16 code units for `character`; map to a byte offset in the UTF-8 buffer.
    targetOffset = static_cast<unsigned>(
        lesma::lsp_srv::bufferByteOffsetFromLspPosition(ref, line, character));
  }

  std::optional<CursorIdentifier> result;
  std::function<void(const lesma::TypeExpr*)> visitTypeExpr = [&](const lesma::TypeExpr* node) {
    if (node == nullptr || result) {
      return;
    }
    llvm::SMRange span = node->getSpan();
    if (!span.isValid()) {
      return;
    }
    unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
    unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
    if (targetOffset < startOff || targetOffset >= endOff) {
      return;
    }

    if (node->getElementType() != nullptr) {
      visitTypeExpr(node->getElementType());
      if (result) {
        return;
      }
    }
    for (lesma::TypeExpr* param : node->getParams()) {
      visitTypeExpr(param);
      if (result) {
        return;
      }
    }
    if (node->getReturnType() != nullptr) {
      visitTypeExpr(node->getReturnType());
      if (result) {
        return;
      }
    }

    if (node->getType() == lesma::TokenType::PTR_TYPE ||
        node->getType() == lesma::TokenType::FUNC_TYPE) {
      return;
    }

    result = CursorIdentifier{
        node->getName(), std::nullopt,
        span.isValid() ? std::optional<::lsp::Range>(smRangeToLspRange(srcMgr, bufferId, span))
                       : std::nullopt,
        true,
    };
  };
  std::function<void(const lesma::Expression*)> visitExpr = [&](const lesma::Expression* node) {
    if (node == nullptr || result) {
      return;
    }
    llvm::SMRange span = node->getSpan();
    if (!span.isValid()) {
      return;
    }
    unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
    unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
    if (targetOffset >= startOff && targetOffset < endOff) {
      if (auto const* typeExpr = dynamic_cast<const lesma::TypeExpr*>(node)) {
        visitTypeExpr(typeExpr);
        return;
      }
      if (auto const* lit = dynamic_cast<const lesma::Literal*>(node)) {
        if (lit->getType() == lesma::TokenType::IDENTIFIER) {
          llvm::SMRange span = lit->getSpan();
          result = CursorIdentifier{lit->getValue(), std::nullopt,
                                    span.isValid() ? std::optional<::lsp::Range>(
                                        smRangeToLspRange(srcMgr, bufferId, span))
                                                   : std::nullopt};
        }
      }
      if (auto const* fc = dynamic_cast<const lesma::FuncCall*>(node)) {
        // Check if cursor is on the function name (rough heuristic)
        llvm::SMRange nameSpan = fc->getSpan();
        if (nameSpan.isValid()) {
          unsigned nameStart = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.Start);
          unsigned nameEnd = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.End);
          // Rough check: if cursor is in first part of FuncCall, it's the name
          // Otherwise, check arguments
          unsigned nameLen = nameEnd - nameStart;
          if (targetOffset < nameStart + nameLen / 2) {
            result = CursorIdentifier{fc->getName(), std::nullopt, std::nullopt};
            if (result) {
              return;
            }
          }
        }
        // Visit arguments to find identifiers in them (e.g., holder.callback)
        for (lesma::TypeExpr* typeArg : fc->getExplicitTypeArgs()) {
          visitTypeExpr(typeArg);
          if (result) {
            return;
          }
        }
        for (lesma::Expression* arg : fc->getArguments()) {
          if (arg != nullptr) {
            visitExpr(arg);
            if (result) {
              return;
            }
          }
        }
      }
      if (auto const* dot = dynamic_cast<const lesma::DotOp*>(node)) {
        // For DotOp, check which side the cursor is on by examining spans
        if (dot->getLeft() != nullptr) {
          llvm::SMRange leftSpan = dot->getLeft()->getSpan();
          if (leftSpan.isValid()) {
            unsigned leftStart = getOffsetFromSMLoc(srcMgr, bufferId, leftSpan.Start);
            unsigned leftEnd = getOffsetFromSMLoc(srcMgr, bufferId, leftSpan.End);
            if (targetOffset >= leftStart && targetOffset < leftEnd) {
              visitExpr(dot->getLeft());
              if (result) {
                return;
              }
            }
          }
        }
        if (dot->getRight() != nullptr) {
          llvm::SMRange rightSpan = dot->getRight()->getSpan();
          if (rightSpan.isValid()) {
            unsigned rightStart = getOffsetFromSMLoc(srcMgr, bufferId, rightSpan.Start);
            unsigned rightEnd = getOffsetFromSMLoc(srcMgr, bufferId, rightSpan.End);
            if (targetOffset >= rightStart && targetOffset < rightEnd) {
              std::optional<std::string> leftName;
              if (auto const* leftLit = dynamic_cast<const lesma::Literal*>(dot->getLeft())) {
                if (leftLit->getType() == lesma::TokenType::IDENTIFIER) {
                  leftName = leftLit->getValue();
                }
              }
              visitExpr(dot->getRight());
              if (result) {
                result = CursorIdentifier{result->name, leftName, result->range};
                return;
              }
            }
          }
        }
        // Fallback: if cursor is in DotOp but not in either side, try both
        visitExpr(dot->getLeft());
        if (!result && dot->getRight() != nullptr) {
          visitExpr(dot->getRight());
        }
      }
      if (auto const* bin = dynamic_cast<const lesma::BinaryOp*>(node)) {
        visitExpr(bin->getLeft());
        if (!result) {
          visitExpr(bin->getRight());
        }
      }
      if (auto const* un = dynamic_cast<const lesma::UnaryOp*>(node)) {
        visitExpr(un->getExpression());
      }
      if (auto const* castOp = dynamic_cast<const lesma::CastOp*>(node)) {
        visitExpr(castOp->getExpression());
        if (!result) {
          visitTypeExpr(castOp->getType());
        }
      }
      if (auto const* isOp = dynamic_cast<const lesma::IsOp*>(node)) {
        visitExpr(isOp->getLeft());
        if (!result) {
          visitTypeExpr(isOp->getRight());
        }
      }
    }
  };
  std::function<void(const lesma::Statement*)> visit = [&](const lesma::Statement* node) {
    if (node == nullptr || result) {
      return;
    }
    llvm::SMRange span = node->getSpan();
    if (!span.isValid()) {
      return;
    }
    unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
    unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
    // Match if cursor is within the statement span, or immediately after it (for name spans)
    bool isInSpan = targetOffset >= startOff && targetOffset < endOff;
    bool isJustAfterSpan = targetOffset >= endOff && targetOffset <= endOff + 2U;
    if (isInSpan || isJustAfterSpan) {
      if (auto const* v = dynamic_cast<const lesma::VarDecl*>(node)) {
        // Check if cursor is on the identifier itself (not just anywhere in VarDecl)
        if (v->getIdentifier() != nullptr &&
            v->getIdentifier()->getType() == lesma::TokenType::IDENTIFIER) {
          llvm::SMRange idSpan = v->getIdentifier()->getSpan();
          if (idSpan.isValid()) {
            unsigned idStart = getOffsetFromSMLoc(srcMgr, bufferId, idSpan.Start);
            unsigned idEnd = getOffsetFromSMLoc(srcMgr, bufferId, idSpan.End);
            bool isInId = targetOffset >= idStart && targetOffset < idEnd;
            bool isJustAfterId = targetOffset >= idEnd && targetOffset <= idEnd + 2U;
            if (isInId || isJustAfterId) {
              llvm::SMRange idSpan = v->getIdentifier()->getSpan();
              result = CursorIdentifier{v->getIdentifier()->getValue(), std::nullopt,
                                        idSpan.isValid()
                                            ? std::optional<::lsp::Range>(
                                                  smRangeToLspRange(srcMgr, bufferId, idSpan))
                                            : std::nullopt};
            }
          }
        }
        // Also visit the expression (initializer) to find identifiers there
        if (!result && v->getType() != nullptr) {
          visitTypeExpr(v->getType());
        }
        if (!result && v->getValue() != nullptr) {
          visitExpr(v->getValue());
        }
      }
      if (auto const* f = dynamic_cast<const lesma::FuncDecl*>(node)) {
        for (const auto& genericParam : f->getGenericParamDecls()) {
          if (!genericParam.span.isValid()) {
            continue;
          }
          unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.Start);
          unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.End);
          bool in = targetOffset >= a && targetOffset < b;
          bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
          if (in || justAfter) {
            result = CursorIdentifier{
                genericParam.name, std::nullopt,
                std::optional<::lsp::Range>(smRangeToLspRange(srcMgr, bufferId, genericParam.span)),
                true};
            return;
          }
        }
        // Check parameter names first so the first parameter shows param hover, not function
        for (lesma::Parameter* p : f->getParameters()) {
          if (p != nullptr && p->type != nullptr) {
            visitTypeExpr(p->type.get());
            if (result) {
              return;
            }
          }
          if (p != nullptr && p->nameSpan.isValid()) {
            unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.Start);
            unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.End);
            bool in = targetOffset >= a && targetOffset < b;
            bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
            if (in || justAfter) {
              result = CursorIdentifier{
                  p->name, std::nullopt,
                  p->nameSpan.isValid()
                      ? std::optional<::lsp::Range>(
                            smRangeToLspRange(srcMgr, bufferId, p->nameSpan))
                      : std::nullopt};
              break;
            }
          }
        }
        // Then check if cursor is on the function name
        if (!result) {
          llvm::SMRange nameSpan = f->getNameSpan();
          if (nameSpan.isValid()) {
            unsigned nameStart = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.Start);
            unsigned nameEnd = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.End);
            bool isInName = targetOffset >= nameStart && targetOffset < nameEnd;
            bool isJustAfterName = targetOffset >= nameEnd && targetOffset <= nameEnd + 2U;
            if (isInName || isJustAfterName) {
              result = CursorIdentifier{f->getName(), std::nullopt,
                                        nameSpan.isValid()
                                            ? std::optional<::lsp::Range>(
                                                  smRangeToLspRange(srcMgr, bufferId, nameSpan))
                                            : std::nullopt};
            }
          }
        }
        if (!result && f->getReturnType() != nullptr) {
          visitTypeExpr(f->getReturnType());
        }
        // Also visit body to find identifiers in expressions
        if (!result && f->getBody() != nullptr) {
          for (lesma::Statement* s : f->getBody()->getChildren()) {
            visit(s);
            if (result) {
              return;
            }
          }
        }
      }
      if (auto const* f = dynamic_cast<const lesma::ExternFuncDecl*>(node)) {
        for (const auto& genericParam : f->getGenericParamDecls()) {
          if (!genericParam.span.isValid()) {
            continue;
          }
          unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.Start);
          unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.End);
          bool in = targetOffset >= a && targetOffset < b;
          bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
          if (in || justAfter) {
            result = CursorIdentifier{
                genericParam.name, std::nullopt,
                std::optional<::lsp::Range>(smRangeToLspRange(srcMgr, bufferId, genericParam.span)),
                true};
            return;
          }
        }
        for (lesma::Parameter* p : f->getParameters()) {
          if (p != nullptr && p->type != nullptr) {
            visitTypeExpr(p->type.get());
            if (result) {
              return;
            }
          }
          if (p != nullptr && p->nameSpan.isValid()) {
            unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.Start);
            unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.End);
            bool in = targetOffset >= a && targetOffset < b;
            bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
            if (in || justAfter) {
              result = CursorIdentifier{
                  p->name, std::nullopt,
                  p->nameSpan.isValid()
                      ? std::optional<::lsp::Range>(
                            smRangeToLspRange(srcMgr, bufferId, p->nameSpan))
                      : std::nullopt};
              break;
            }
          }
        }
        if (!result) {
          llvm::SMRange nameSpan = f->getNameSpan();
          if (nameSpan.isValid()) {
            unsigned nameStart = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.Start);
            unsigned nameEnd = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.End);
            bool isInName = targetOffset >= nameStart && targetOffset < nameEnd;
            bool isJustAfterName = targetOffset >= nameEnd && targetOffset <= nameEnd + 2U;
            if (isInName || isJustAfterName) {
              result = CursorIdentifier{
                  f->getName(), std::nullopt,
                  nameSpan.isValid()
                      ? std::optional<::lsp::Range>(
                            smRangeToLspRange(srcMgr, bufferId, nameSpan))
                      : std::nullopt};
            }
          }
        }
        if (!result && f->getReturnType() != nullptr) {
          visitTypeExpr(f->getReturnType());
        }
      }
      if (auto const* c = dynamic_cast<const lesma::Class*>(node)) {
        for (const auto& genericParam : c->getGenericParamDecls()) {
          if (!genericParam.span.isValid()) {
            continue;
          }
          unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.Start);
          unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, genericParam.span.End);
          bool in = targetOffset >= a && targetOffset < b;
          bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
          if (in || justAfter) {
            result = CursorIdentifier{
                genericParam.name, std::nullopt,
                std::optional<::lsp::Range>(smRangeToLspRange(srcMgr, bufferId, genericParam.span)),
                true};
            return;
          }
        }
        // Check if cursor is on the class name
        llvm::SMRange nameSpan = c->getNameSpan();
        if (nameSpan.isValid()) {
          unsigned nameStart = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.Start);
          unsigned nameEnd = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.End);
          bool isInName = targetOffset >= nameStart && targetOffset < nameEnd;
          bool isJustAfterName = targetOffset >= nameEnd && targetOffset <= nameEnd + 2U;
          if (isInName || isJustAfterName) {
            result = CursorIdentifier{c->getIdentifier(), std::nullopt,
                                      nameSpan.isValid()
                                          ? std::optional<::lsp::Range>(
                                                smRangeToLspRange(srcMgr, bufferId, nameSpan))
                                          : std::nullopt};
          }
        }
        // Also visit methods and fields
        if (!result) {
          for (lesma::FuncDecl* m : c->getMethods()) {
            visit(m);
            if (result) {
              return;
            }
          }
        }
      }
      if (auto const* e = dynamic_cast<const lesma::Enum*>(node)) {
        // Check if cursor is on the enum name
        llvm::SMRange nameSpan = e->getNameSpan();
        if (nameSpan.isValid()) {
          unsigned nameStart = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.Start);
          unsigned nameEnd = getOffsetFromSMLoc(srcMgr, bufferId, nameSpan.End);
          bool isInName = targetOffset >= nameStart && targetOffset < nameEnd;
          bool isJustAfterName = targetOffset >= nameEnd && targetOffset <= nameEnd + 2U;
          if (isInName || isJustAfterName) {
            result = CursorIdentifier{e->getIdentifier(), std::nullopt,
                                      nameSpan.isValid()
                                          ? std::optional<::lsp::Range>(
                                                smRangeToLspRange(srcMgr, bufferId, nameSpan))
                                          : std::nullopt};
          }
        }
        // Check if cursor is on an enum value (e.g. RED in "enum Color\n  RED\n  GREEN").
        // Enum statement span is only the "enum" keyword, so we always check value spans here.
        if (!result) {
          std::vector<std::string> const& values = e->getValues();
          std::vector<llvm::SMRange> const& valueSpans = e->getValueSpans();
          for (size_t i = 0; i < values.size() && i < valueSpans.size(); ++i) {
            llvm::SMRange span = valueSpans[i];
            if (!span.isValid()) {
              continue;
            }
            unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
            unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
            bool isIn = targetOffset >= startOff && targetOffset < endOff;
            bool isJustAfter = targetOffset >= endOff && targetOffset <= endOff + 2U;
            if (isIn || isJustAfter) {
              result = CursorIdentifier{
                  values[i], std::nullopt,
                  span.isValid() ? std::optional<::lsp::Range>(
                                      smRangeToLspRange(srcMgr, bufferId, span))
                                : std::nullopt};
              break;
            }
          }
        }
      }
      if (auto const* compound = dynamic_cast<const lesma::Compound*>(node)) {
        for (lesma::Statement* s : compound->getChildren()) {
          visit(s);
        }
      }
      if (auto const* ifNode = dynamic_cast<const lesma::If*>(node)) {
        for (lesma::Expression* cond : ifNode->getConds()) {
          visitExpr(cond);
          if (result) {
            return;
          }
        }
        for (lesma::Compound* block : ifNode->getBlocks()) {
          if (block != nullptr) {
            visit(block);
          }
          if (result) {
            return;
          }
        }
      }
      if (auto const* whileNode = dynamic_cast<const lesma::While*>(node)) {
        if (whileNode->getCond() != nullptr) {
          visitExpr(whileNode->getCond());
        }
        if (!result && whileNode->getBlock() != nullptr) {
          visit(whileNode->getBlock());
        }
      }
      if (!result) {
        if (auto const* importNode = dynamic_cast<const lesma::Import*>(node)) {
          llvm::SMRange aliasSpan = importNode->getAliasSpan();
          if (aliasSpan.isValid()) {
            unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, aliasSpan.Start);
            unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, aliasSpan.End);
            if (targetOffset >= startOff && targetOffset < endOff) {
              result =
                  CursorIdentifier{importNode->getAlias(), std::nullopt,
                                   smRangeToLspRange(srcMgr, bufferId, aliasSpan)};
              return;
            }
          }
          for (const lesma::ImportedNameBinding& binding : importNode->getImportedNames()) {
            llvm::SMRange span = binding.aliasSpan.isValid() ? binding.aliasSpan : binding.nameSpan;
            if (!span.isValid()) {
              continue;
            }
            unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
            unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
            if (targetOffset >= startOff && targetOffset < endOff) {
              std::string localName = binding.alias.empty() ? binding.name : binding.alias;
              result =
                  CursorIdentifier{localName, std::nullopt,
                                   smRangeToLspRange(srcMgr, bufferId, span)};
              return;
            }
          }
        }
      }
    }
  };
  for (lesma::Statement* stmt : ast->getChildren()) {
    visit(stmt);
    if (result) {
      return result;
    }
    // Parameter names in function/method signatures (cursor on "x" in "def foo(x: Int)")
    if (!result) {
      std::function<void(const lesma::Statement*, lesma::Class*)> checkParams =
          [&](const lesma::Statement* node, lesma::Class* cls) {
            if (node == nullptr || result) {
              return;
            }
            if (auto const* f = dynamic_cast<const lesma::FuncDecl*>(node)) {
              for (lesma::Parameter* p : f->getParameters()) {
                if (p != nullptr && p->nameSpan.isValid()) {
                  unsigned a = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.Start);
                  unsigned b = getOffsetFromSMLoc(srcMgr, bufferId, p->nameSpan.End);
                  bool in = targetOffset >= a && targetOffset < b;
                  bool justAfter = targetOffset >= b && targetOffset <= b + 2U;
                  if (in || justAfter) {
                    result = CursorIdentifier{
                        p->name, std::nullopt,
                        p->nameSpan.isValid()
                            ? std::optional<::lsp::Range>(
                                  smRangeToLspRange(srcMgr, bufferId, p->nameSpan))
                            : std::nullopt};
                    return;
                  }
                }
              }
              if (f->getBody() != nullptr) {
                for (lesma::Statement* s : f->getBody()->getChildren()) {
                  checkParams(s, cls);
                  if (result) {
                    return;
                  }
                }
              }
              return;
            }
            if (auto const* c = dynamic_cast<const lesma::Class*>(node)) {
              for (lesma::FuncDecl* m : c->getMethods()) {
                if (m != nullptr) {
                  checkParams(m, const_cast<lesma::Class*>(c));
                  if (result) {
                    return;
                  }
                }
              }
            }
          };
      checkParams(stmt, nullptr);
    }
    // Also check expressions in statements that contain them
    if (auto const* es = dynamic_cast<const lesma::ExpressionStatement*>(stmt)) {
      if (es->getExpression() != nullptr) {
        visitExpr(es->getExpression());
        if (result) {
          return result;
        }
      }
    }
    if (auto const* v = dynamic_cast<const lesma::VarDecl*>(stmt)) {
      if (v->getValue() != nullptr) {
        visitExpr(v->getValue());
        if (result) {
          return result;
        }
      }
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
      for (lesma::Expression* cond : ifNode->getConds()) {
        visitExpr(cond);
        if (result) {
          return result;
        }
      }
    }
    if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
      if (whileNode->getCond() != nullptr) {
        visitExpr(whileNode->getCond());
        if (result) {
          return result;
        }
      }
    }
  }
  return result;
}

/** Semantic token type indices (must match server legend). */
enum class SemanticTokenType : unsigned {
  Enum = 0,
  EnumMember = 1,
  Type = 2,
};

void appendSemanticToken(std::vector<std::tuple<unsigned, unsigned, unsigned, unsigned>>& rawTokens,
                         llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span,
                         SemanticTokenType type) {
  if (!span.isValid()) {
    return;
  }
  auto [line1, startCol1] = srcMgr->getLineAndColumn(span.Start, bufferId);
  auto [endLine1, endCol1] = srcMgr->getLineAndColumn(span.End, bufferId);
  unsigned line0 = line1 > 0U ? line1 - 1U : 0U;
  unsigned startChar = startCol1 > 0U ? startCol1 - 1U : 0U;
  unsigned length = (endLine1 == line1 && endCol1 >= startCol1) ? (endCol1 - startCol1) : 0U;
  rawTokens.emplace_back(line0, startChar, length, static_cast<unsigned>(type));
}

void collectTypeSemanticTokens(
    const lesma::TypeExpr* typeExpr, llvm::SourceMgr* srcMgr, unsigned bufferId,
    std::vector<std::tuple<unsigned, unsigned, unsigned, unsigned>>& rawTokens) {
  if (typeExpr == nullptr) {
    return;
  }
  if (typeExpr->getType() != lesma::TokenType::PTR_TYPE &&
      typeExpr->getType() != lesma::TokenType::FUNC_TYPE) {
    appendSemanticToken(rawTokens, srcMgr, bufferId, typeExpr->getSpan(), SemanticTokenType::Type);
  }
  if (typeExpr->getElementType() != nullptr) {
    collectTypeSemanticTokens(typeExpr->getElementType(), srcMgr, bufferId, rawTokens);
  }
  for (lesma::TypeExpr* param : typeExpr->getParams()) {
    collectTypeSemanticTokens(param, srcMgr, bufferId, rawTokens);
  }
  if (typeExpr->getReturnType() != nullptr) {
    collectTypeSemanticTokens(typeExpr->getReturnType(), srcMgr, bufferId, rawTokens);
  }
}

/** Collect semantic tokens for enum member references (e.g. MyEnum.OptionA). */
std::vector<std::uint32_t> collectSemanticTokens(const AnalysisResult& analysisResult,
                                                  unsigned bufferId) {
  std::vector<std::tuple<unsigned, unsigned, unsigned, unsigned>> rawTokens;
  llvm::SourceMgr* srcMgr = analysisResult.sourceMgr.get();
  lesma::SymbolTable* root = analysisResult.rootScope.get();
  lesma::Compound* ast = analysisResult.parser ? analysisResult.parser->getAst() : nullptr;
  if (srcMgr == nullptr || root == nullptr || ast == nullptr) {
    return {};
  }

  std::function<void(const lesma::Expression*)> visitExpr = [&](const lesma::Expression* node) {
    if (node == nullptr) {
      return;
    }
    if (auto const* dot = dynamic_cast<const lesma::DotOp*>(node)) {
      lesma::Value* baseVal = nullptr;
      if (auto const* leftLit = dynamic_cast<const lesma::Literal*>(dot->getLeft())) {
        if (leftLit->getType() == lesma::TokenType::IDENTIFIER) {
          baseVal = root->lookup(leftLit->getValue());
        }
      }
      if (baseVal != nullptr && baseVal->getType() != nullptr &&
          baseVal->getType()->is(lesma::BaseType::TY_ENUM) && dot->getRight() != nullptr) {
        if (auto const* rightLit = dynamic_cast<const lesma::Literal*>(dot->getRight())) {
          if (rightLit->getType() == lesma::TokenType::IDENTIFIER) {
            appendSemanticToken(rawTokens, srcMgr, bufferId, rightLit->getSpan(),
                                SemanticTokenType::EnumMember);
          }
        }
      }
      if (dot->getLeft() != nullptr) {
        visitExpr(dot->getLeft());
      }
      if (dot->getRight() != nullptr) {
        visitExpr(dot->getRight());
      }
      return;
    }
    if (auto const* fc = dynamic_cast<const lesma::FuncCall*>(node)) {
      for (lesma::TypeExpr* typeArg : fc->getExplicitTypeArgs()) {
        collectTypeSemanticTokens(typeArg, srcMgr, bufferId, rawTokens);
      }
      for (lesma::Expression* arg : fc->getArguments()) {
        if (arg != nullptr) {
          visitExpr(arg);
        }
      }
      return;
    }
    if (auto const* bin = dynamic_cast<const lesma::BinaryOp*>(node)) {
      visitExpr(bin->getLeft());
      visitExpr(bin->getRight());
      return;
    }
    if (auto const* un = dynamic_cast<const lesma::UnaryOp*>(node)) {
      visitExpr(un->getExpression());
      return;
    }
    if (auto const* castOp = dynamic_cast<const lesma::CastOp*>(node)) {
      visitExpr(castOp->getExpression());
      collectTypeSemanticTokens(castOp->getType(), srcMgr, bufferId, rawTokens);
      return;
    }
    if (auto const* isOp = dynamic_cast<const lesma::IsOp*>(node)) {
      visitExpr(isOp->getLeft());
      collectTypeSemanticTokens(isOp->getRight(), srcMgr, bufferId, rawTokens);
      return;
    }
  };

  std::function<void(const lesma::Statement*)> visitStmt = [&](const lesma::Statement* node) {
    if (node == nullptr) {
      return;
    }
    // Enum member declarations (e.g. OptionA, OptionB in "enum E { OptionA, OptionB }")
    if (auto const* e = dynamic_cast<const lesma::Enum*>(node)) {
      for (llvm::SMRange span : e->getValueSpans()) {
        appendSemanticToken(rawTokens, srcMgr, bufferId, span, SemanticTokenType::EnumMember);
      }
    }
    if (auto const* es = dynamic_cast<const lesma::ExpressionStatement*>(node)) {
      if (es->getExpression() != nullptr) {
        visitExpr(es->getExpression());
      }
      return;
    }
    if (auto const* v = dynamic_cast<const lesma::VarDecl*>(node)) {
      collectTypeSemanticTokens(v->getType(), srcMgr, bufferId, rawTokens);
      if (v->getValue() != nullptr) {
        visitExpr(v->getValue());
      }
      return;
    }
    if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(node)) {
      for (const auto& genericParam : func->getGenericParamDecls()) {
        appendSemanticToken(rawTokens, srcMgr, bufferId, genericParam.span, SemanticTokenType::Type);
      }
      for (lesma::Parameter* param : func->getParameters()) {
        if (param != nullptr) {
          collectTypeSemanticTokens(param->type.get(), srcMgr, bufferId, rawTokens);
        }
      }
      collectTypeSemanticTokens(func->getReturnType(), srcMgr, bufferId, rawTokens);
      if (func->getBody() != nullptr) {
        visitStmt(func->getBody());
      }
      return;
    }
    if (auto const* ext = dynamic_cast<const lesma::ExternFuncDecl*>(node)) {
      for (const auto& genericParam : ext->getGenericParamDecls()) {
        appendSemanticToken(rawTokens, srcMgr, bufferId, genericParam.span, SemanticTokenType::Type);
      }
      for (lesma::Parameter* param : ext->getParameters()) {
        if (param != nullptr) {
          collectTypeSemanticTokens(param->type.get(), srcMgr, bufferId, rawTokens);
        }
      }
      collectTypeSemanticTokens(ext->getReturnType(), srcMgr, bufferId, rawTokens);
      return;
    }
    if (auto const* klass = dynamic_cast<const lesma::Class*>(node)) {
      for (const auto& genericParam : klass->getGenericParamDecls()) {
        appendSemanticToken(rawTokens, srcMgr, bufferId, genericParam.span, SemanticTokenType::Type);
      }
      for (lesma::VarDecl* field : klass->getFields()) {
        if (field != nullptr) {
          visitStmt(field);
        }
      }
      for (lesma::FuncDecl* method : klass->getMethods()) {
        if (method != nullptr) {
          visitStmt(method);
        }
      }
      return;
    }
    if (auto const* ifNode = dynamic_cast<const lesma::If*>(node)) {
      for (lesma::Expression* cond : ifNode->getConds()) {
        visitExpr(cond);
      }
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
      if (whileNode->getCond() != nullptr) {
        visitExpr(whileNode->getCond());
      }
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
    }
    if (auto const* f = dynamic_cast<const lesma::FuncDecl*>(node)) {
      if (f->getBody() != nullptr) {
        for (lesma::Statement* s : f->getBody()->getChildren()) {
          visitStmt(s);
        }
      }
    }
    if (auto const* c = dynamic_cast<const lesma::Class*>(node)) {
      for (lesma::FuncDecl* m : c->getMethods()) {
        visitStmt(m);
      }
    }
  };

  for (lesma::Statement* stmt : ast->getChildren()) {
    visitStmt(stmt);
  }

  std::sort(rawTokens.begin(), rawTokens.end(),
            [](const auto& a, const auto& b) {
              if (std::get<0>(a) != std::get<0>(b)) {
                return std::get<0>(a) < std::get<0>(b);
              }
              return std::get<1>(a) < std::get<1>(b);
            });

  std::vector<std::uint32_t> data;
  data.reserve(rawTokens.size() * 5U);
  unsigned prevLine = 0U;
  unsigned prevChar = 0U;
  for (const auto& [line, startChar, length, typeIndex] : rawTokens) {
    unsigned deltaLine = (line > prevLine) ? (line - prevLine) : 0U;
    unsigned deltaStartChar = (line > prevLine) ? startChar : (startChar - prevChar);
    data.push_back(deltaLine);
    data.push_back(deltaStartChar);
    data.push_back(length);
    data.push_back(typeIndex);
    data.push_back(0U); // no modifiers
    prevLine = line;
    prevChar = startChar;
  }
  return data;
}

std::vector<::lsp::InlayHint> collectInlayHints(const AnalysisResult& analysisResult,
                                                unsigned bufferId, const ::lsp::Range& range) {
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
  unsigned const rangeStart = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(text, range.start.line, range.start.character));
  unsigned const rangeEnd = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(text, range.end.line, range.end.character));

  auto isInRequestedRange = [&](llvm::SMRange span) -> bool {
    if (!span.isValid()) {
      return false;
    }
    unsigned const offset = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
    return offset >= rangeStart && offset <= rangeEnd;
  };

  auto maybeAddVarHint = [&](const lesma::VarDecl* varDecl) {
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
    hint.textEdits =
        ::lsp::Opt<::lsp::Array<::lsp::TextEdit>>({::lsp::TextEdit{
            .range = ::lsp::Range{.start = insertPos, .end = insertPos},
            .newText = typeText,
        }});
    hint.tooltip = ::lsp::Opt<::lsp::OneOf<::lsp::String, ::lsp::MarkupContent>>(
        ::lsp::String("Insert inferred type annotation"));
    hints.push_back(std::move(hint));
  };

  std::function<void(const lesma::Statement*)> visitStmt = [&](const lesma::Statement* node) {
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

auto makeLspRangeFromStartAndName(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc start,
                                  const std::string& name) -> ::lsp::Range {
  auto [line1, startCol1] = srcMgr->getLineAndColumn(start, bufferId);
  unsigned const line0 = line1 > 0U ? line1 - 1U : 0U;
  unsigned const startChar = startCol1 > 0U ? startCol1 - 1U : 0U;
  return ::lsp::Range{
      .start = ::lsp::Position{.line = line0, .character = startChar},
      .end =
          ::lsp::Position{
              .line = line0,
              .character = startChar + static_cast<unsigned>(name.size()),
          },
  };
}

struct SymbolOccurrence {
  std::string name;
  std::optional<std::string> dotBase;
  ::lsp::Range range;
};

struct EnumMemberOccurrence {
  std::string enumName;
  std::string memberName;
  ::lsp::Range range;
};

struct ActiveCallSite {
  const lesma::FuncCall* call = nullptr;
  const lesma::Expression* receiver = nullptr;
  unsigned spanLength = 0U;
};

auto resolveCanonicalSymbolAtCursor(const AnalysisResult& result, unsigned line, unsigned character,
                                    const CursorIdentifier& id) -> std::optional<ResolvedSymbol>;
auto findEnumMemberDeclarationAtCursor(const lesma::Compound* ast, llvm::SourceMgr* srcMgr,
                                       unsigned bufferId, unsigned line, unsigned character,
                                       const std::string& memberName)
    -> std::optional<std::string>;

auto resolveExpressionTypeAtOffset(const lesma::Expression* expr, lesma::Compound* ast,
                                   lesma::SymbolTable* root, llvm::SourceMgr* srcMgr,
                                   unsigned bufferId, unsigned targetOffset) -> lesma::Type*;

auto resolveMethodReturnType(const lesma::FuncCall* call, const lesma::Expression* receiver,
                             lesma::Compound* ast, lesma::SymbolTable* root,
                             llvm::SourceMgr* srcMgr, unsigned bufferId,
                             unsigned targetOffset) -> lesma::Type* {
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

void findActiveCallInExpr(const lesma::Expression* expr, llvm::SourceMgr* srcMgr, unsigned bufferId,
                          unsigned targetOffset, const lesma::Expression* receiver,
                          ActiveCallSite& best) {
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

void findActiveCallInStmt(const lesma::Statement* stmt, llvm::SourceMgr* srcMgr, unsigned bufferId,
                          unsigned targetOffset, ActiveCallSite& best) {
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
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(text, line, character));
  lesma::SymbolTable* scope =
      activeScopeForOffset(analysis.ast, analysis.rootScope, analysis.sourceMgr, analysis.bufferId,
                           targetOffset);
  if (scope == nullptr) {
    scope = analysis.rootScope;
  }

  std::vector<lesma::Type*> argTypes;
  for (lesma::Expression* arg : activeCall->call->getArguments()) {
    lesma::Type* argType = resolveExpressionTypeAtOffset(arg, analysis.ast, analysis.rootScope,
                                                         analysis.sourceMgr, analysis.bufferId,
                                                         targetOffset);
    if (argType != nullptr) {
      argTypes.push_back(argType);
    }
  }
  lesma::Type* receiverType = nullptr;
  if (activeCall->receiver != nullptr) {
    receiverType = resolveExpressionTypeAtOffset(activeCall->receiver, analysis.ast, analysis.rootScope,
                                                 analysis.sourceMgr, analysis.bufferId,
                                                 targetOffset);
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

auto resolveCallArgumentTypesAtCursor(const AnalysisView& analysis, unsigned line, unsigned character,
                                      const CursorIdentifier& id)
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
    lesma::Type* argType = resolveExpressionTypeAtOffset(arg, analysis.ast, analysis.rootScope,
                                                         analysis.sourceMgr, analysis.bufferId,
                                                         targetOffset);
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
  lesma::Value* resolved =
      lookupFunctionOrSymbol(targetAnalysis->rootScope, symbolName, argTypes.value_or(std::vector<lesma::Type*>{}));
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

  if (id.dotBase.has_value()) {
    if (std::optional<std::string> modulePath = findModuleImportPathByAlias(analysis, *id.dotBase)) {
      if (std::optional<ResolvedSymbol> imported =
              resolveImportedSymbol(result, analysis, *modulePath, id.name, line, character, id)) {
        return imported;
      }
    }
  }

  lesma::Value* local = lookupValueForHover(analysis.ast, analysis.rootScope, analysis.sourceMgr,
                                            analysis.bufferId, line, character, id.name,
                                            id.isTypePosition);
  if (local != nullptr && local->getType() != nullptr && !local->getType()->is(lesma::BaseType::TY_IMPORT)) {
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

void collectSymbolOccurrencesFromTypeExpr(const lesma::TypeExpr* typeExpr, llvm::SourceMgr* srcMgr,
                                          unsigned bufferId, std::vector<SymbolOccurrence>& out) {
  if (typeExpr == nullptr) {
    return;
  }
  if (typeExpr->getType() != lesma::TokenType::PTR_TYPE &&
      typeExpr->getType() != lesma::TokenType::FUNC_TYPE) {
    out.push_back(SymbolOccurrence{
        .name = typeExpr->getName(),
        .dotBase = std::nullopt,
        .range = smRangeToLspRange(srcMgr, bufferId, typeExpr->getSpan()),
    });
  }
  if (typeExpr->getElementType() != nullptr) {
    collectSymbolOccurrencesFromTypeExpr(typeExpr->getElementType(), srcMgr, bufferId, out);
  }
  for (lesma::TypeExpr* param : typeExpr->getParams()) {
    collectSymbolOccurrencesFromTypeExpr(param, srcMgr, bufferId, out);
  }
  if (typeExpr->getReturnType() != nullptr) {
    collectSymbolOccurrencesFromTypeExpr(typeExpr->getReturnType(), srcMgr, bufferId, out);
  }
}

void collectSymbolOccurrencesFromExpr(const lesma::Expression* expr, llvm::SourceMgr* srcMgr,
                                      unsigned bufferId, std::vector<SymbolOccurrence>& out,
                                      std::vector<EnumMemberOccurrence>& enumOut);

void collectSymbolOccurrencesFromStmt(const lesma::Statement* stmt, llvm::SourceMgr* srcMgr,
                                      unsigned bufferId, std::vector<SymbolOccurrence>& out,
                                      std::vector<EnumMemberOccurrence>& enumOut) {
  if (stmt == nullptr) {
    return;
  }
  if (auto const* varDecl = dynamic_cast<const lesma::VarDecl*>(stmt)) {
    if (varDecl->getIdentifier() != nullptr) {
      out.push_back(SymbolOccurrence{
          .name = varDecl->getIdentifier()->getValue(),
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, varDecl->getIdentifier()->getSpan()),
      });
    }
    collectSymbolOccurrencesFromTypeExpr(varDecl->getType(), srcMgr, bufferId, out);
    collectSymbolOccurrencesFromExpr(varDecl->getValue(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* func = dynamic_cast<const lesma::FuncDecl*>(stmt)) {
    out.push_back(SymbolOccurrence{
        .name = func->getName(),
        .dotBase = std::nullopt,
        .range = smRangeToLspRange(srcMgr, bufferId, func->getNameSpan()),
    });
    for (const auto& genericParam : func->getGenericParamDecls()) {
      out.push_back(SymbolOccurrence{
          .name = genericParam.name,
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, genericParam.span),
      });
    }
    for (lesma::Parameter* param : func->getParameters()) {
      if (param != nullptr && param->nameSpan.isValid()) {
        out.push_back(SymbolOccurrence{
            .name = param->name,
            .dotBase = std::nullopt,
            .range = smRangeToLspRange(srcMgr, bufferId, param->nameSpan),
        });
      }
      if (param != nullptr) {
        collectSymbolOccurrencesFromTypeExpr(param->type.get(), srcMgr, bufferId, out);
      }
    }
    collectSymbolOccurrencesFromTypeExpr(func->getReturnType(), srcMgr, bufferId, out);
    collectSymbolOccurrencesFromStmt(func->getBody(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* ext = dynamic_cast<const lesma::ExternFuncDecl*>(stmt)) {
    out.push_back(SymbolOccurrence{
        .name = ext->getName(),
        .dotBase = std::nullopt,
        .range = smRangeToLspRange(srcMgr, bufferId, ext->getNameSpan()),
    });
    for (const auto& genericParam : ext->getGenericParamDecls()) {
      out.push_back(SymbolOccurrence{
          .name = genericParam.name,
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, genericParam.span),
      });
    }
    for (lesma::Parameter* param : ext->getParameters()) {
      if (param != nullptr && param->nameSpan.isValid()) {
        out.push_back(SymbolOccurrence{
            .name = param->name,
            .dotBase = std::nullopt,
            .range = smRangeToLspRange(srcMgr, bufferId, param->nameSpan),
        });
      }
      if (param != nullptr) {
        collectSymbolOccurrencesFromTypeExpr(param->type.get(), srcMgr, bufferId, out);
      }
    }
    collectSymbolOccurrencesFromTypeExpr(ext->getReturnType(), srcMgr, bufferId, out);
    return;
  }
  if (auto const* klass = dynamic_cast<const lesma::Class*>(stmt)) {
    out.push_back(SymbolOccurrence{
        .name = klass->getIdentifier(),
        .dotBase = std::nullopt,
        .range = smRangeToLspRange(srcMgr, bufferId, klass->getNameSpan()),
    });
    for (const auto& genericParam : klass->getGenericParamDecls()) {
      out.push_back(SymbolOccurrence{
          .name = genericParam.name,
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, genericParam.span),
      });
    }
    for (lesma::VarDecl* field : klass->getFields()) {
      collectSymbolOccurrencesFromStmt(field, srcMgr, bufferId, out, enumOut);
    }
    for (lesma::FuncDecl* method : klass->getMethods()) {
      collectSymbolOccurrencesFromStmt(method, srcMgr, bufferId, out, enumOut);
    }
    return;
  }
  if (auto const* enumNode = dynamic_cast<const lesma::Enum*>(stmt)) {
    out.push_back(SymbolOccurrence{
        .name = enumNode->getIdentifier(),
        .dotBase = std::nullopt,
        .range = smRangeToLspRange(srcMgr, bufferId, enumNode->getNameSpan()),
    });
    std::vector<std::string> const& values = enumNode->getValues();
    std::vector<llvm::SMRange> const& spans = enumNode->getValueSpans();
    for (size_t i = 0; i < values.size() && i < spans.size(); ++i) {
      if (spans[i].isValid()) {
        enumOut.push_back(EnumMemberOccurrence{
            .enumName = enumNode->getIdentifier(),
            .memberName = values[i],
            .range = smRangeToLspRange(srcMgr, bufferId, spans[i]),
        });
      }
    }
    return;
  }
  if (auto const* importNode = dynamic_cast<const lesma::Import*>(stmt)) {
    llvm::SMRange aliasSpan = importNode->getAliasSpan();
    if (aliasSpan.isValid()) {
      out.push_back(SymbolOccurrence{
          .name = importNode->getAlias(),
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, aliasSpan),
      });
    }
    for (const lesma::ImportedNameBinding& binding : importNode->getImportedNames()) {
      llvm::SMRange span = binding.aliasSpan.isValid() ? binding.aliasSpan : binding.nameSpan;
      if (!span.isValid()) {
        continue;
      }
      out.push_back(SymbolOccurrence{
          .name = binding.alias.empty() ? binding.name : binding.alias,
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, span),
      });
    }
    return;
  }
  if (auto const* exprStmt = dynamic_cast<const lesma::ExpressionStatement*>(stmt)) {
    collectSymbolOccurrencesFromExpr(exprStmt->getExpression(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* ifNode = dynamic_cast<const lesma::If*>(stmt)) {
    for (lesma::Expression* cond : ifNode->getConds()) {
      collectSymbolOccurrencesFromExpr(cond, srcMgr, bufferId, out, enumOut);
    }
    for (lesma::Compound* block : ifNode->getBlocks()) {
      collectSymbolOccurrencesFromStmt(block, srcMgr, bufferId, out, enumOut);
    }
    return;
  }
  if (auto const* whileNode = dynamic_cast<const lesma::While*>(stmt)) {
    collectSymbolOccurrencesFromExpr(whileNode->getCond(), srcMgr, bufferId, out, enumOut);
    collectSymbolOccurrencesFromStmt(whileNode->getBlock(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* ret = dynamic_cast<const lesma::Return*>(stmt)) {
    collectSymbolOccurrencesFromExpr(ret->getValue(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* defer = dynamic_cast<const lesma::Defer*>(stmt)) {
    collectSymbolOccurrencesFromStmt(defer->getStatement(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* compound = dynamic_cast<const lesma::Compound*>(stmt)) {
    for (lesma::Statement* child : compound->getChildren()) {
      collectSymbolOccurrencesFromStmt(child, srcMgr, bufferId, out, enumOut);
    }
  }
}

void collectSymbolOccurrencesFromExpr(const lesma::Expression* expr, llvm::SourceMgr* srcMgr,
                                      unsigned bufferId, std::vector<SymbolOccurrence>& out,
                                      std::vector<EnumMemberOccurrence>& enumOut) {
  if (expr == nullptr) {
    return;
  }
  if (auto const* lit = dynamic_cast<const lesma::Literal*>(expr)) {
    if (lit->getType() == lesma::TokenType::IDENTIFIER) {
      out.push_back(SymbolOccurrence{
          .name = lit->getValue(),
          .dotBase = std::nullopt,
          .range = smRangeToLspRange(srcMgr, bufferId, lit->getSpan()),
      });
    }
    return;
  }
  if (auto const* call = dynamic_cast<const lesma::FuncCall*>(expr)) {
    out.push_back(SymbolOccurrence{
        .name = call->getName(),
        .dotBase = std::nullopt,
        .range = makeLspRangeFromStartAndName(srcMgr, bufferId, call->getSpan().Start, call->getName()),
    });
    for (lesma::TypeExpr* typeArg : call->getExplicitTypeArgs()) {
      collectSymbolOccurrencesFromTypeExpr(typeArg, srcMgr, bufferId, out);
    }
    for (lesma::Expression* arg : call->getArguments()) {
      collectSymbolOccurrencesFromExpr(arg, srcMgr, bufferId, out, enumOut);
    }
    return;
  }
  if (auto const* dot = dynamic_cast<const lesma::DotOp*>(expr)) {
    collectSymbolOccurrencesFromExpr(dot->getLeft(), srcMgr, bufferId, out, enumOut);
    if (auto const* rightLit = dynamic_cast<const lesma::Literal*>(dot->getRight())) {
      if (rightLit->getType() == lesma::TokenType::IDENTIFIER) {
        auto const* leftLit = dynamic_cast<const lesma::Literal*>(dot->getLeft());
        if (leftLit != nullptr) {
          if (leftLit->getType() == lesma::TokenType::IDENTIFIER) {
            enumOut.push_back(EnumMemberOccurrence{
                .enumName = leftLit->getValue(),
                .memberName = rightLit->getValue(),
                .range = smRangeToLspRange(srcMgr, bufferId, rightLit->getSpan()),
            });
          }
        }
        out.push_back(SymbolOccurrence{
            .name = rightLit->getValue(),
            .dotBase = leftLit != nullptr && leftLit->getType() == lesma::TokenType::IDENTIFIER
                           ? std::optional<std::string>(leftLit->getValue())
                           : std::nullopt,
            .range = smRangeToLspRange(srcMgr, bufferId, rightLit->getSpan()),
        });
      }
    } else {
      collectSymbolOccurrencesFromExpr(dot->getRight(), srcMgr, bufferId, out, enumOut);
    }
    return;
  }
  if (auto const* binary = dynamic_cast<const lesma::BinaryOp*>(expr)) {
    collectSymbolOccurrencesFromExpr(binary->getLeft(), srcMgr, bufferId, out, enumOut);
    collectSymbolOccurrencesFromExpr(binary->getRight(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* unary = dynamic_cast<const lesma::UnaryOp*>(expr)) {
    collectSymbolOccurrencesFromExpr(unary->getExpression(), srcMgr, bufferId, out, enumOut);
    return;
  }
  if (auto const* castOp = dynamic_cast<const lesma::CastOp*>(expr)) {
    collectSymbolOccurrencesFromExpr(castOp->getExpression(), srcMgr, bufferId, out, enumOut);
    collectSymbolOccurrencesFromTypeExpr(castOp->getType(), srcMgr, bufferId, out);
    return;
  }
  if (auto const* isOp = dynamic_cast<const lesma::IsOp*>(expr)) {
    collectSymbolOccurrencesFromExpr(isOp->getLeft(), srcMgr, bufferId, out, enumOut);
    collectSymbolOccurrencesFromTypeExpr(isOp->getRight(), srcMgr, bufferId, out);
  }
}

auto rangeEquals(const ::lsp::Range& lhs, const ::lsp::Range& rhs) -> bool {
  return lhs.start.line == rhs.start.line && lhs.start.character == rhs.start.character &&
         lhs.end.line == rhs.end.line && lhs.end.character == rhs.end.character;
}

auto collectReferences(const AnalysisResult& result, unsigned line, unsigned character,
                       bool includeDeclaration) -> std::vector<::lsp::Location> {
  std::vector<::lsp::Location> locations;
  AnalysisView mainAnalysis = makeAnalysisView(result);
  if (!isUsableAnalysis(mainAnalysis)) {
    return locations;
  }
  std::optional<CursorIdentifier> id =
      findIdentifierAtCursor(mainAnalysis.ast, mainAnalysis.sourceMgr, mainAnalysis.bufferId, line,
                             character);
  if (!id) {
    return locations;
  }

  if (std::optional<ResolvedSymbol> target =
          resolveCanonicalSymbolAtCursor(result, mainAnalysis, line, character, *id)) {
    std::optional<SymbolIdentity> targetIdentity = symbolIdentityForResolved(*target);
    if (!targetIdentity) {
      return locations;
    }
    for (const AnalysisView& analysis : collectAnalysisViews(result)) {
      std::vector<SymbolOccurrence> occurrences;
      std::vector<EnumMemberOccurrence> enumOccurrences;
      for (lesma::Statement* stmt : analysis.ast->getChildren()) {
        collectSymbolOccurrencesFromStmt(stmt, analysis.sourceMgr, analysis.bufferId, occurrences,
                                         enumOccurrences);
      }
      ::lsp::DocumentUri uri =
          uriFromPath(analysis.mainFilePath != nullptr ? *analysis.mainFilePath : std::string{});
      for (const SymbolOccurrence& occurrence : occurrences) {
        std::optional<ResolvedSymbol> resolved =
            resolveCanonicalSymbolAtCursor(result, analysis, occurrence.range.start.line,
                                           occurrence.range.start.character,
                                           CursorIdentifier{occurrence.name, occurrence.dotBase,
                                                            occurrence.range});
        if (!resolved) {
          resolved = resolveCanonicalSymbolAtCursor(
              result, analysis, occurrence.range.end.line, occurrence.range.end.character,
              CursorIdentifier{occurrence.name, occurrence.dotBase, occurrence.range});
        }
        if (!resolved) {
          continue;
        }
        std::optional<SymbolIdentity> occurrenceIdentity = symbolIdentityForResolved(*resolved);
        if (!occurrenceIdentity || occurrenceIdentity->path != targetIdentity->path ||
            occurrenceIdentity->name != targetIdentity->name ||
            !rangeEquals(occurrenceIdentity->range, targetIdentity->range)) {
          continue;
        }
        if (!includeDeclaration && occurrenceIdentity->path == targetIdentity->path &&
            rangeEquals(occurrence.range, targetIdentity->range)) {
          continue;
        }
        locations.push_back(::lsp::Location{.uri = uri, .range = occurrence.range});
      }
    }
    return locations;
  }

  std::vector<SymbolOccurrence> occurrences;
  std::vector<EnumMemberOccurrence> enumOccurrences;
  for (lesma::Statement* stmt : mainAnalysis.ast->getChildren()) {
    collectSymbolOccurrencesFromStmt(stmt, mainAnalysis.sourceMgr, mainAnalysis.bufferId, occurrences,
                                     enumOccurrences);
  }
  ::lsp::DocumentUri uri = uriFromPath(result.mainFilePath);
  std::optional<std::string> enumName = findEnumMemberDeclarationAtCursor(
      mainAnalysis.ast, mainAnalysis.sourceMgr, mainAnalysis.bufferId, line, character, id->name);
  if (id->dotBase || enumName) {
    std::string targetEnum = enumName ? *enumName : *id->dotBase;
    for (const EnumMemberOccurrence& occurrence : enumOccurrences) {
      if (occurrence.enumName == targetEnum && occurrence.memberName == id->name) {
        if (!includeDeclaration && id->range && rangeEquals(occurrence.range, *id->range)) {
          continue;
        }
        locations.push_back(::lsp::Location{.uri = uri, .range = occurrence.range});
      }
    }
  }

  return locations;
}

/** Parser records FuncDecl span only through the return type; outline should cover the whole function. */
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
                        const ::lsp::Range& selectionRange, std::optional<std::string> detail = std::nullopt)
    -> ::lsp::DocumentSymbol {
  auto before = [](const ::lsp::Position& lhs, const ::lsp::Position& rhs) {
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
    symbol.detail = ::lsp::Opt<::lsp::String>(*detail);
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
          value != nullptr ? std::optional<std::string>(
                                 formatTypeName(value->getType(), result.rootScope.get()))
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
    if (auto* enumNode = dynamic_cast<lesma::Enum*>(stmt)) {
      ::lsp::DocumentSymbol symbol = makeDocumentSymbol(
          enumNode->getIdentifier(), ::lsp::SymbolKind::Enum,
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, enumNode->getSpan()),
          smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, enumNode->getNameSpan()));
      std::vector<::lsp::DocumentSymbol> children;
      std::vector<std::string> const& values = enumNode->getValues();
      std::vector<llvm::SMRange> const& spans = enumNode->getValueSpans();
      for (size_t i = 0; i < values.size() && i < spans.size(); ++i) {
        children.push_back(makeDocumentSymbol(
            values[i], ::lsp::SymbolKind::EnumMember,
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, spans[i]),
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, spans[i])));
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
            smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, funcDeclFullSpan(method)),
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
auto findEnumMemberDeclarationAtCursor(const lesma::Compound* ast, llvm::SourceMgr* srcMgr,
                                      unsigned bufferId, unsigned line, unsigned character,
                                      const std::string& memberName)
    -> std::optional<std::string> {
  if (ast == nullptr || srcMgr == nullptr) {
    return std::nullopt;
  }
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return std::nullopt;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
  for (lesma::Statement* stmt : ast->getChildren()) {
    auto const* e = dynamic_cast<const lesma::Enum*>(stmt);
    if (e == nullptr) {
      continue;
    }
    std::vector<std::string> const& values = e->getValues();
    std::vector<llvm::SMRange> const& valueSpans = e->getValueSpans();
    for (size_t i = 0; i < values.size() && i < valueSpans.size(); ++i) {
      if (values[i] != memberName) {
        continue;
      }
      llvm::SMRange span = valueSpans[i];
      if (!span.isValid()) {
        continue;
      }
      unsigned startOff = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
      unsigned endOff = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
      if (targetOffset >= startOff && targetOffset < endOff) {
        return e->getIdentifier();
      }
    }
  }
  return std::nullopt;
}

/** Resolve go-to-definition for an enum member (e.g. OptionA in MyEnum.OptionA). */
auto tryResolveEnumMemberDefinitionLocation(const AnalysisResult& result,
                                           const std::string& dotBase,
                                           const std::string& memberName)
    -> std::optional<::lsp::Location> {
  auto searchAnalysis = [&](const AnalysisView& analysis, const std::string& enumName)
      -> std::optional<::lsp::Location> {
    if (!isUsableAnalysis(analysis) || analysis.mainFilePath == nullptr) {
      return std::nullopt;
    }
    for (lesma::Statement* stmt : analysis.ast->getChildren()) {
      auto const* e = dynamic_cast<const lesma::Enum*>(stmt);
      if (e == nullptr || e->getIdentifier() != enumName) {
        continue;
      }
      std::vector<std::string> const& values = e->getValues();
      std::vector<llvm::SMRange> const& valueSpans = e->getValueSpans();
      for (size_t i = 0; i < values.size() && i < valueSpans.size(); ++i) {
        if (values[i] == memberName && valueSpans[i].isValid()) {
          return ::lsp::Location{
              .uri = uriFromPath(*analysis.mainFilePath),
              .range = smRangeToLspRange(analysis.sourceMgr, analysis.bufferId, valueSpans[i]),
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
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return std::nullopt;
  }
  std::optional<CursorIdentifier> id =
      findIdentifierAtCursor(ast, result.sourceMgr.get(), result.mainBufferId, line, character);
  if (!id) {
    return std::nullopt;
  }
  std::optional<ResolvedSymbol> resolved = resolveCanonicalSymbolAtCursor(result, line, character, *id);
  if (!resolved || resolved->value == nullptr) {
    // Enum member (e.g. MyEnum.OptionA): not a Value, resolve via AST
    if (id->dotBase) {
      auto enumMemberLoc =
          tryResolveEnumMemberDefinitionLocation(result, *id->dotBase, id->name);
      if (enumMemberLoc) {
        return *enumMemberLoc;
      }
    }
    return std::nullopt;
  }
  llvm::SMRange declSpan = resolved->value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    if (!id->dotBase) {
      AnalysisView mainAnalysis = makeAnalysisView(result);
      if (std::optional<std::string> modulePath = findModuleImportPathByAlias(mainAnalysis, id->name)) {
        if (std::optional<AnalysisView> imported = findAnalysisViewForPath(result, *modulePath)) {
          return ::lsp::Location{
              .uri = uriFromPath(*imported->mainFilePath),
              .range = ::lsp::Range{.start = {0U, 0U}, .end = {0U, 0U}},
          };
        }
      }
      if (result.importAliasToPath.contains(id->name)) {
        if (std::optional<AnalysisView> imported =
                findAnalysisViewForPath(result, result.importAliasToPath.at(id->name))) {
          return ::lsp::Location{
              .uri = uriFromPath(*imported->mainFilePath),
              .range = ::lsp::Range{.start = {0U, 0U}, .end = {0U, 0U}},
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

} // namespace

auto main() -> int {
  try {
    auto connection = ::lsp::Connection(::lsp::io::standardIO());
    auto messageHandler = ::lsp::MessageHandler(connection);
    lesma::lsp_srv::DocumentStore docStore;
    AnalysisCache analysisCache;

    std::atomic<bool> running{true};

    messageHandler.add<::lsp::requests::Initialize>([&docStore](
                                                        ::lsp::requests::Initialize::Params&&
                                                            params) {
      if (!params.rootUri.isNull()) {
        std::string path(params.rootUri->path().data(), params.rootUri->path().size());
        docStore.setWorkspaceRoot(std::move(path));
      }
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
      caps.signatureHelpProvider = ::lsp::Opt<::lsp::SignatureHelpOptions>(
          ::lsp::SignatureHelpOptions{
              .triggerCharacters =
                  ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string("("), std::string(",")}),
              .retriggerCharacters =
                  ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string(",")}),
          });
      caps.referencesProvider = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::ReferenceOptions>>(true);
      caps.documentSymbolProvider =
          ::lsp::Opt<::lsp::OneOf<bool, ::lsp::DocumentSymbolOptions>>(true);
      caps.semanticTokensProvider = ::lsp::Opt<
          ::lsp::OneOf<::lsp::SemanticTokensOptions, ::lsp::SemanticTokensRegistrationOptions>>(
          ::lsp::SemanticTokensOptions{
              .legend =
                  ::lsp::SemanticTokensLegend{
                      .tokenTypes = ::lsp::Array<::lsp::String>{"enum", "enumMember", "type"},
                      .tokenModifiers = ::lsp::Array<::lsp::String>(),
                  },
              .full = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::SemanticTokensOptionsFull>>(true),
          });
      caps.inlayHintProvider =
          ::lsp::Opt<::lsp::OneOf<bool, ::lsp::InlayHintOptions, ::lsp::InlayHintRegistrationOptions>>(
              true);
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
        [](::lsp::notifications::Initialized::Params&&) {});

    messageHandler.add<::lsp::notifications::TextDocument_DidOpen>(
        [&messageHandler, &docStore,
         &analysisCache](::lsp::notifications::TextDocument_DidOpen::Params&& params) {
          auto& doc = params.textDocument;
          docStore.open(doc.uri, doc.version, doc.text);
          runAnalyzeAndPublish(doc.uri, docStore, doc.text, doc.version, analysisCache,
                               messageHandler);
        });

    messageHandler.add<::lsp::notifications::TextDocument_DidChange>(
        [&messageHandler, &docStore,
         &analysisCache](::lsp::notifications::TextDocument_DidChange::Params&& params) {
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
         &analysisCache](::lsp::notifications::TextDocument_DidClose::Params&& params) {
          docStore.close(params.textDocument.uri);
          analysisCache.invalidate(params.textDocument.uri, docStore);
          messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
              ::lsp::PublishDiagnosticsParams{.uri = params.textDocument.uri, .diagnostics = {}});
        });

    messageHandler.add<::lsp::requests::TextDocument_Hover>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Hover::Params&& params)
            -> ::lsp::TextDocument_HoverResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_HoverResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          if (result.parser == nullptr) {
            return ::lsp::TextDocument_HoverResult();
          }
          lesma::Compound* ast = result.parser->getAst();
          if (ast == nullptr) {
            return ::lsp::TextDocument_HoverResult();
          }
          unsigned line = params.position.line;
          unsigned character = params.position.character;
          std::optional<CursorIdentifier> id = findIdentifierAtCursor(
              ast, result.sourceMgr.get(), result.mainBufferId, line, character);
          if (!id) {
            return ::lsp::TextDocument_HoverResult();
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
            return ::lsp::TextDocument_HoverResult(std::move(hover));
          }
          // Enum member hover (e.g. MyEnum.OptionA): not a Value, but a field on the enum type
          if (id->dotBase && result.rootScope != nullptr) {
            std::optional<ResolvedSymbol> baseResolved = resolveCanonicalSymbolAtCursor(
                result, line, character,
                CursorIdentifier{*id->dotBase, std::nullopt, std::nullopt});
            lesma::Value* baseVal = baseResolved ? baseResolved->value : result.rootScope->lookup(*id->dotBase);
            if (baseVal != nullptr && baseVal->getType() != nullptr &&
                baseVal->getType()->is(lesma::BaseType::TY_ENUM)) {
              for (lesma::Field* field : baseVal->getType()->getFields()) {
                if (field != nullptr && field->name == id->name) {
                  ::lsp::Hover hover;
                  hover.contents = ::lsp::MarkupContent{
                      .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                      .value = "**" + id->name + "**\n\nmember of enum `" + *id->dotBase + "`",
                  };
                  return ::lsp::TextDocument_HoverResult(std::move(hover));
                }
              }
            }
          }
          // Enum member declaration hover (e.g. Red in "enum Color { Red, Green }")
          std::optional<std::string> enumName = findEnumMemberDeclarationAtCursor(
              ast, result.sourceMgr.get(), result.mainBufferId, line, character, id->name);
          if (enumName) {
            ::lsp::Hover hover;
            hover.contents = ::lsp::MarkupContent{
                .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                .value = "**" + id->name + "**\n\nmember of enum `" + *enumName + "`",
            };
            return ::lsp::TextDocument_HoverResult(std::move(hover));
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
              return ::lsp::TextDocument_HoverResult(std::move(hover));
            }

            auto const* buf = result.sourceMgr->getMemoryBuffer(result.mainBufferId);
            if (buf != nullptr) {
              unsigned targetOffset = static_cast<unsigned>(
                  lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
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
                return ::lsp::TextDocument_HoverResult(std::move(hover));
              }
            }
          }
          return ::lsp::TextDocument_HoverResult();
        });

    messageHandler.add<::lsp::requests::TextDocument_SemanticTokens_Full>(
        [&docStore, &analysisCache](
            ::lsp::requests::TextDocument_SemanticTokens_Full::Params&& params)
            -> ::lsp::TextDocument_SemanticTokens_FullResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_SemanticTokens_FullResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<std::uint32_t> data =
              collectSemanticTokens(result, result.mainBufferId);
          ::lsp::SemanticTokens tokens;
          tokens.data = ::lsp::Array<std::uint32_t>(data.begin(), data.end());
          return ::lsp::TextDocument_SemanticTokens_FullResult(std::move(tokens));
        });

    messageHandler.add<::lsp::requests::TextDocument_InlayHint>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_InlayHint::Params&& params)
            -> ::lsp::TextDocument_InlayHintResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_InlayHintResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<::lsp::InlayHint> hints =
              collectInlayHints(result, result.mainBufferId, params.range);
          return ::lsp::TextDocument_InlayHintResult(
              ::lsp::Array<::lsp::InlayHint>(hints.begin(), hints.end()));
        });

    messageHandler.add<::lsp::requests::TextDocument_SignatureHelp>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_SignatureHelp::Params&& params)
            -> ::lsp::TextDocument_SignatureHelpResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_SignatureHelpResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          auto help = buildSignatureHelp(result, params.position.line, params.position.character);
          if (!help) {
            return ::lsp::TextDocument_SignatureHelpResult();
          }
          return ::lsp::TextDocument_SignatureHelpResult(std::move(*help));
        });

    messageHandler.add<::lsp::requests::TextDocument_Completion>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Completion::Params&& params)
            -> ::lsp::TextDocument_CompletionResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_CompletionResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<::lsp::CompletionItem> items =
              lesma::lsp_srv::completionItems(result, params.position.line, params.position.character);
          if (items.empty()) {
            return ::lsp::TextDocument_CompletionResult();
          }
          return ::lsp::TextDocument_CompletionResult(std::move(items));
        });

    messageHandler.add<::lsp::requests::TextDocument_References>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_References::Params&& params)
            -> ::lsp::TextDocument_ReferencesResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_ReferencesResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<::lsp::Location> refs = collectReferences(
              result, params.position.line, params.position.character,
              params.context.includeDeclaration);
          if (refs.empty()) {
            return ::lsp::TextDocument_ReferencesResult();
          }
          return ::lsp::TextDocument_ReferencesResult(
              ::lsp::Array<::lsp::Location>(refs.begin(), refs.end()));
        });

    messageHandler.add<::lsp::requests::TextDocument_Definition>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Definition::Params&& params)
            -> ::lsp::TextDocument_DefinitionResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          auto loc =
              tryResolveDefinitionLocation(result, params.position.line, params.position.character);
          if (!loc) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          return ::lsp::TextDocument_DefinitionResult(::lsp::Definition(std::move(*loc)));
        });

    messageHandler.add<::lsp::requests::TextDocument_Declaration>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Declaration::Params&& params)
            -> ::lsp::TextDocument_DeclarationResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_DeclarationResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          auto loc =
              tryResolveDefinitionLocation(result, params.position.line, params.position.character);
          if (!loc) {
            return ::lsp::TextDocument_DeclarationResult();
          }
          return ::lsp::TextDocument_DeclarationResult(::lsp::Declaration(std::move(*loc)));
        });

    messageHandler.add<::lsp::requests::TextDocument_DocumentSymbol>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_DocumentSymbol::Params&& params)
            -> ::lsp::TextDocument_DocumentSymbolResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_DocumentSymbolResult();
          }
          DocumentAnalysisSnapshot& snapshot = analysisCache.getOrAnalyze(
              params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<::lsp::DocumentSymbol> symbols = collectDocumentSymbols(result);
          if (symbols.empty()) {
            return ::lsp::TextDocument_DocumentSymbolResult();
          }
          return ::lsp::TextDocument_DocumentSymbolResult(
              ::lsp::Array<::lsp::DocumentSymbol>(symbols.begin(), symbols.end()));
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
