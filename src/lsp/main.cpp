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
#include <lsp/types.h>

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
      if (inSpan(f->getNameSpan())) {
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

lesma::Value* lookupValueForHover(lesma::Compound* ast, lesma::SymbolTable* root,
                                  llvm::SourceMgr* srcMgr, unsigned bufferId, unsigned line,
                                  unsigned character, const std::string& name) {
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
    // Otherwise, look up in the function's body scope (for parameters, locals)
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
    // Otherwise, look up in the function's body scope (for locals)
    if (funcSym != nullptr && funcSym->getBodyScope() != nullptr) {
      if (lesma::Value* v = funcSym->getBodyScope()->lookup(name)) {
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
        if (!result && v->getValue() != nullptr) {
          visitExpr(v->getValue());
        }
      }
      if (auto const* f = dynamic_cast<const lesma::FuncDecl*>(node)) {
        // Check parameter names first so the first parameter shows param hover, not function
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
      if (auto const* c = dynamic_cast<const lesma::Class*>(node)) {
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
};

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
            llvm::SMRange span = rightLit->getSpan();
            if (span.isValid()) {
              auto [line1, startCol1] = srcMgr->getLineAndColumn(span.Start, bufferId);
              auto [endLine1, endCol1] = srcMgr->getLineAndColumn(span.End, bufferId);
              unsigned line0 = line1 > 0U ? line1 - 1U : 0U;
              unsigned startChar = startCol1 > 0U ? startCol1 - 1U : 0U;
              unsigned length = (endLine1 == line1 && endCol1 >= startCol1)
                                   ? (endCol1 - startCol1)
                                   : 0U;
              rawTokens.emplace_back(line0, startChar, length,
                                     static_cast<unsigned>(SemanticTokenType::EnumMember));
            }
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
  };

  std::function<void(const lesma::Statement*)> visitStmt = [&](const lesma::Statement* node) {
    if (node == nullptr) {
      return;
    }
    // Enum member declarations (e.g. OptionA, OptionB in "enum E { OptionA, OptionB }")
    if (auto const* e = dynamic_cast<const lesma::Enum*>(node)) {
      for (llvm::SMRange span : e->getValueSpans()) {
        if (span.isValid()) {
          auto [line1, startCol1] = srcMgr->getLineAndColumn(span.Start, bufferId);
          auto [endLine1, endCol1] = srcMgr->getLineAndColumn(span.End, bufferId);
          unsigned line0 = line1 > 0U ? line1 - 1U : 0U;
          unsigned startChar = startCol1 > 0U ? startCol1 - 1U : 0U;
          unsigned length = (endLine1 == line1 && endCol1 >= startCol1)
                               ? (endCol1 - startCol1)
                               : 0U;
          rawTokens.emplace_back(line0, startChar, length,
                                 static_cast<unsigned>(SemanticTokenType::EnumMember));
        }
      }
    }
    if (auto const* es = dynamic_cast<const lesma::ExpressionStatement*>(node)) {
      if (es->getExpression() != nullptr) {
        visitExpr(es->getExpression());
      }
      return;
    }
    if (auto const* v = dynamic_cast<const lesma::VarDecl*>(node)) {
      if (v->getValue() != nullptr) {
        visitExpr(v->getValue());
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

/** Resolve symbol at cursor using compiler metadata. Returns the Value* if found. */
lesma::Value* resolveSymbolAtCursor(const AnalysisResult& result, unsigned line, unsigned character,
                                    const std::string& name) {
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
  lesma::Compound* ast = result.parser ? result.parser->getAst() : nullptr;
  if (ast == nullptr || result.sourceMgr == nullptr || result.mainFilePath.empty()) {
    return std::nullopt;
  }
  for (lesma::Statement* stmt : ast->getChildren()) {
    auto const* e = dynamic_cast<const lesma::Enum*>(stmt);
    if (e == nullptr || e->getIdentifier() != dotBase) {
      continue;
    }
    std::vector<std::string> const& values = e->getValues();
    std::vector<llvm::SMRange> const& valueSpans = e->getValueSpans();
    for (size_t i = 0; i < values.size() && i < valueSpans.size(); ++i) {
      if (values[i] == memberName) {
        llvm::SMRange span = valueSpans[i];
        if (!span.isValid()) {
          return std::nullopt;
        }
        ::lsp::Location loc;
        std::filesystem::path const p(result.mainFilePath);
        std::error_code ec;
        std::filesystem::path absPath = std::filesystem::absolute(p, ec);
        if (!ec) {
          loc.uri = ::lsp::FileUri::fromPath(absPath.string());
        } else {
          loc.uri = ::lsp::FileUri::fromPath(result.mainFilePath);
        }
        loc.range = smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, span);
        return loc;
      }
    }
    return std::nullopt;
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
  // Resolve symbol using compiler metadata
  lesma::Value* value = resolveSymbolAtCursor(result, line, character, id->name);
  if (value == nullptr) {
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
      caps.declarationProvider = ::lsp::Opt<
          ::lsp::OneOf<bool, ::lsp::DeclarationOptions, ::lsp::DeclarationRegistrationOptions>>(
          ::lsp::OneOf<bool, ::lsp::DeclarationOptions, ::lsp::DeclarationRegistrationOptions>{
              true});
      caps.completionProvider = ::lsp::Opt<::lsp::CompletionOptions>(::lsp::CompletionOptions{
          .triggerCharacters = ::lsp::Opt<::lsp::Array<::lsp::String>>({std::string(".")}),
      });
      caps.semanticTokensProvider = ::lsp::Opt<
          ::lsp::OneOf<::lsp::SemanticTokensOptions, ::lsp::SemanticTokensRegistrationOptions>>(
          ::lsp::SemanticTokensOptions{
              .legend =
                  ::lsp::SemanticTokensLegend{
                      .tokenTypes = ::lsp::Array<::lsp::String>{"enum", "enumMember"},
                      .tokenModifiers = ::lsp::Array<::lsp::String>(),
                  },
              .full = ::lsp::Opt<::lsp::OneOf<bool, ::lsp::SemanticTokensOptionsFull>>(true),
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
          // Resolve symbol using compiler metadata
          lesma::Value* value = resolveSymbolAtCursor(result, line, character, id->name);
          if (value != nullptr && value->getType() != nullptr) {
            std::string hoverText = formatHoverContent(value, result.rootScope.get());
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
            lesma::Value* baseVal = result.rootScope->lookup(*id->dotBase);
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

    while (running) {
      messageHandler.processIncomingMessages();
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "lesma-lsp: " << e.what() << '\n';
    return 1;
  }
}
