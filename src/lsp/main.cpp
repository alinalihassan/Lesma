#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include <llvm/Support/SourceMgr.h>

#include "DocumentStore.h"
#include "LspCompletion.h"
#include "LspUtf16.h"
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
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
                          const lesma::lsp_srv::DocumentStore& docStore,
                          const std::string& content, int version,
                          AnalysisCache& analysisCache, ::lsp::MessageHandler& messageHandler) {
  DocumentAnalysisSnapshot& snapshot =
      analysisCache.getOrAnalyze(uri, docStore, content, version);
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
        if (sym->getCategory() == lesma::ValueCategory::TYPE_SYMBOL &&
            sym->getType() == type) {
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
  case lesma::ValueCategory::TYPE_SYMBOL:
    return "**" + name + "**\n\n" + typeStr;
  case lesma::ValueCategory::MODULE_SYMBOL:
    return "**" + name + "**\n\n(import)";
  case lesma::ValueCategory::ADDRESSABLE_STORAGE:
  case lesma::ValueCategory::DIRECT_VALUE:
    return "**" + name + "**\n\nType: `" + typeStr + "`";
  }
  return "**" + name + "**\n\nType: `" + typeStr + "`";
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

void considerFunc(lesma::FuncDecl* f, lesma::Class* cls, unsigned targetOffset,
                  llvm::SourceMgr* sm, unsigned bid, InnermostFunc& best, unsigned& bestLen) {
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

InnermostFunc findInnermostFuncContaining(lesma::Compound* ast, unsigned targetOffset,
                                          llvm::SourceMgr* sm, unsigned bid) {
  InnermostFunc best;
  unsigned bestLen = 0U;
  scanCompoundForFuncs(ast, nullptr, targetOffset, sm, bid, best, bestLen);
  return best;
}

lesma::Value* lookupValueForHover(lesma::Compound* ast, lesma::SymbolTable* root,
                                llvm::SourceMgr* srcMgr, unsigned bufferId, unsigned line,
                                unsigned character, const std::string& name) {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return nullptr;
  }
  unsigned const targetOffset = static_cast<unsigned>(
      lesma::lsp_srv::bufferByteOffsetFromLspPosition(buf->getBuffer(), line, character));
  InnermostFunc const inner =
      findInnermostFuncContaining(ast, targetOffset, srcMgr, bufferId);
  if (inner.func != nullptr && inner.enclosingClass == nullptr) {
    lesma::Value* const funcSym = root->lookup(inner.func->getName());
    if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
      if (lesma::Value* v = funcSym->getBodyScope()->lookup(name)) {
        return v;
      }
    }
  }
  return root->lookup(name);
}

/** Find identifier at cursor position by walking AST to find the identifier token. */
std::optional<std::string> findIdentifierAtCursor(const lesma::Compound* ast,
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

  std::optional<std::string> result;
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
      if (auto const* lit = dynamic_cast<const lesma::Literal*>(node)) {
        if (lit->getType() == lesma::TokenType::IDENTIFIER) {
          result = lit->getValue();
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
            result = fc->getName();
            if (result) {
              return;
            }
          }
        }
        // Visit arguments to find identifiers in them (e.g., holder.callback)
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
              visitExpr(dot->getRight());
              if (result) {
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
    if (targetOffset >= startOff && targetOffset < endOff) {
      if (auto const* v = dynamic_cast<const lesma::VarDecl*>(node)) {
        // Check if cursor is on the identifier itself (not just anywhere in VarDecl)
        if (v->getIdentifier() != nullptr &&
            v->getIdentifier()->getType() == lesma::TokenType::IDENTIFIER) {
          llvm::SMRange idSpan = v->getIdentifier()->getSpan();
          if (idSpan.isValid()) {
            unsigned idStart = getOffsetFromSMLoc(srcMgr, bufferId, idSpan.Start);
            unsigned idEnd = getOffsetFromSMLoc(srcMgr, bufferId, idSpan.End);
            if (targetOffset >= idStart && targetOffset < idEnd) {
              result = v->getIdentifier()->getValue();
            }
          }
        }
        // Also visit the expression (initializer) to find identifiers there
        if (!result && v->getValue() != nullptr) {
          visitExpr(v->getValue());
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
    }
  };
  for (lesma::Statement* stmt : ast->getChildren()) {
    visit(stmt);
    if (result) {
      return result;
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

/** Resolve symbol at cursor using compiler metadata. Returns the Value* if found. */
lesma::Value* resolveSymbolAtCursor(const AnalysisResult& result, unsigned line,
                                    unsigned character, const std::string& name) {
  if (result.rootScope == nullptr || result.sourceMgr == nullptr) {
    return nullptr;
  }
  lesma::Compound* ast = result.parser ? result.parser->getAst() : nullptr;
  if (ast == nullptr) {
    return nullptr;
  }
  // Use the improved lookup that considers function scopes
  return lookupValueForHover(ast, result.rootScope.get(), result.sourceMgr.get(),
                             result.mainBufferId, line, character, name);
}

/** Resolve definition location using compiler metadata from Value. */
auto tryResolveDefinitionLocation(const AnalysisResult& result, unsigned line,
                                   unsigned character) -> std::optional<::lsp::Location> {
  if (result.parser == nullptr || result.sourceMgr == nullptr) {
    return std::nullopt;
  }
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> name =
      findIdentifierAtCursor(ast, result.sourceMgr.get(), result.mainBufferId, line, character);
  if (!name) {
    return std::nullopt;
  }
  // Resolve symbol using compiler metadata
  lesma::Value* value = resolveSymbolAtCursor(result, line, character, *name);
  if (value == nullptr) {
    return std::nullopt;
  }
  llvm::SMRange declSpan = value->getDeclarationSpan();
  if (!declSpan.isValid()) {
    return std::nullopt;
  }
  std::string declPath = value->getDeclarationFilePath();
  if (declPath.empty()) {
    declPath = result.mainFilePath;
  }
  if (declPath.empty()) {
    return std::nullopt;
  }
  ::lsp::Location loc;
  std::filesystem::path const p(declPath);
  std::error_code ec;
  std::filesystem::path absPath = std::filesystem::absolute(p, ec);
  if (!ec) {
    loc.uri = ::lsp::FileUri::fromPath(absPath.string());
  } else {
    loc.uri = ::lsp::FileUri::fromPath(declPath);
  }
  // Determine buffer ID for the declaration file
  unsigned declBufferId = result.mainBufferId;
  if (declPath != result.mainFilePath && result.sourceMgr != nullptr) {
    // For now, assume same buffer; cross-file support would need buffer tracking
    declBufferId = result.mainBufferId;
  }
  loc.range = smRangeToLspRange(result.sourceMgr.get(), declBufferId, declSpan);
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
      caps.declarationProvider = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::DeclarationOptions,
                                                         ::lsp::DeclarationRegistrationOptions>>(
          ::lsp::OneOf<bool, ::lsp::DeclarationOptions, ::lsp::DeclarationRegistrationOptions>{true});
      caps.completionProvider = ::lsp::Opt<::lsp::CompletionOptions>(::lsp::CompletionOptions{
          .triggerCharacters = ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string(".")}),
      });
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
          DocumentAnalysisSnapshot& snapshot =
              analysisCache.getOrAnalyze(params.textDocument.uri, docStore, doc->text, doc->version);
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
          std::optional<std::string> name = findIdentifierAtCursor(
              ast, result.sourceMgr.get(), result.mainBufferId, line, character);
          if (!name) {
            return ::lsp::TextDocument_HoverResult();
          }
          // Resolve symbol using compiler metadata
          lesma::Value* value = resolveSymbolAtCursor(result, line, character, *name);
          if (value != nullptr && value->getType() != nullptr) {
            std::string hoverText = formatHoverContent(value, result.rootScope.get());
            ::lsp::Hover hover;
            hover.contents = ::lsp::MarkupContent{
                .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::Markdown),
                .value = std::move(hoverText),
            };
            return ::lsp::TextDocument_HoverResult(std::move(hover));
          }
          return ::lsp::TextDocument_HoverResult();
        });

    messageHandler.add<::lsp::requests::TextDocument_Completion>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Completion::Params&& params)
            -> ::lsp::TextDocument_CompletionResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_CompletionResult();
          }
          DocumentAnalysisSnapshot& snapshot =
              analysisCache.getOrAnalyze(params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          std::vector<std::string> names = lesma::lsp_srv::memberCompletionNames(
              result, params.position.line, params.position.character);
          if (names.empty()) {
            return ::lsp::TextDocument_CompletionResult();
          }
          std::vector<::lsp::CompletionItem> items;
          items.reserve(names.size());
          for (std::string const& n : names) {
            ::lsp::CompletionItem item;
            item.label = n;
            item.kind = ::lsp::Opt<::lsp::CompletionItemKindEnum>(
                ::lsp::CompletionItemKindEnum(::lsp::CompletionItemKind::Field));
            items.push_back(std::move(item));
          }
          return ::lsp::TextDocument_CompletionResult(std::move(items));
        });

    messageHandler.add<::lsp::requests::TextDocument_Definition>(
        [&docStore, &analysisCache](::lsp::requests::TextDocument_Definition::Params&& params)
            -> ::lsp::TextDocument_DefinitionResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          DocumentAnalysisSnapshot& snapshot =
              analysisCache.getOrAnalyze(params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          auto loc = tryResolveDefinitionLocation(result, params.position.line,
                                                  params.position.character);
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
          DocumentAnalysisSnapshot& snapshot =
              analysisCache.getOrAnalyze(params.textDocument.uri, docStore, doc->text, doc->version);
          AnalysisResult& result = snapshot.result;
          auto loc = tryResolveDefinitionLocation(result, params.position.line,
                                                  params.position.character);
          if (!loc) {
            return ::lsp::TextDocument_DeclarationResult();
          }
          return ::lsp::TextDocument_DeclarationResult(::lsp::Declaration(std::move(*loc)));
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
