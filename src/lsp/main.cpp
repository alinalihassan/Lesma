#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include <llvm/Support/SourceMgr.h>

#include "DocumentStore.h"
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/AnalysisResult.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"

namespace {

using namespace lesma;

::lsp::Range smRangeToLspRange(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange span) {
  if (!span.isValid()) {
    return ::lsp::Range{.start = {0U, 0U}, .end = {0U, 0U}};
  }
  auto startLc = srcMgr->getLineAndColumn(span.Start, bufferId);
  auto endLc = srcMgr->getLineAndColumn(span.End, bufferId);
  return ::lsp::Range{
      .start =
          ::lsp::Position{
              .line = startLc.first > 0U ? startLc.first - 1U : 0U,
              .character = startLc.second,
          },
      .end =
          ::lsp::Position{
              .line = endLc.first > 0U ? endLc.first - 1U : 0U,
              .character = endLc.second,
          },
  };
}

void runAnalyzeAndPublish(const ::lsp::DocumentUri& uri, const std::string& /*path*/,
                          const std::string& content, ::lsp::MessageHandler& messageHandler) {
  auto options = std::make_unique<Options>(Options{
      SourceType::STRING,
      content,
      Debug::NONE,
      "output",
      false,
  });
  AnalysisResult result = lesma::analyze(std::move(options));

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

std::unordered_map<std::string, std::pair<std::string, llvm::SMRange>>
buildDeclarationIndex(const lesma::Compound* ast, const std::string& path) {
  std::unordered_map<std::string, std::pair<std::string, llvm::SMRange>> index;
  for (lesma::Statement* stmt : ast->getChildren()) {
    if (auto* v = dynamic_cast<lesma::VarDecl*>(stmt)) {
      if (v->getIdentifier() != nullptr) {
        std::string name = v->getIdentifier()->getValue();
        index[name] = {path, v->getSpan()};
      }
    } else if (auto* f = dynamic_cast<lesma::FuncDecl*>(stmt)) {
      index[f->getName()] = {path, f->getSpan()};
    } else if (auto* c = dynamic_cast<lesma::Class*>(stmt)) {
      index[c->getIdentifier()] = {path, c->getSpan()};
    } else if (auto* e = dynamic_cast<lesma::Enum*>(stmt)) {
      index[e->getIdentifier()] = {path, e->getSpan()};
    }
  }
  return index;
}

unsigned getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (!buf) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

std::optional<std::string> findIdentifierAtSimple(const lesma::Compound* ast,
                                                  llvm::SourceMgr* srcMgr, unsigned bufferId,
                                                  unsigned line, unsigned character) {
  unsigned targetOffset = 0;
  {
    auto const* buf = srcMgr->getMemoryBuffer(bufferId);
    if (!buf) {
      return std::nullopt;
    }
    llvm::StringRef ref = buf->getBuffer();
    unsigned currentLine = 0;
    unsigned currentChar = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
      if (currentLine == line && currentChar == character) {
        targetOffset = static_cast<unsigned>(i);
        break;
      }
      if (ref[i] == '\n') {
        ++currentLine;
        currentChar = 0;
      } else {
        ++currentChar;
      }
    }
    if (currentLine != line || currentChar != character) {
      targetOffset = static_cast<unsigned>(ref.size());
    }
  }

  std::optional<std::string> result;
  std::function<void(const lesma::Statement*)> visit = [&](const lesma::Statement* node) {
    if (!node || result) {
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
        if (v->getIdentifier() && v->getIdentifier()->getType() == lesma::TokenType::IDENTIFIER) {
          result = v->getIdentifier()->getValue();
        }
      }
      if (auto const* compound = dynamic_cast<const lesma::Compound*>(node)) {
        for (lesma::Statement* s : compound->getChildren()) {
          visit(s);
        }
      }
    }
  };
  std::function<void(const lesma::Expression*)> visitExpr = [&](const lesma::Expression* node) {
    if (!node || result) {
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
        result = fc->getName();
      }
      if (auto const* dot = dynamic_cast<const lesma::DotOp*>(node)) {
        visitExpr(dot->getLeft());
        if (!result && dot->getRight()) {
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
  for (lesma::Statement* stmt : ast->getChildren()) {
    visit(stmt);
    if (result) {
      return result;
    }
    if (auto const* es = dynamic_cast<const lesma::ExpressionStatement*>(stmt)) {
      if (es->getExpression()) {
        visitExpr(es->getExpression());
      }
    }
  }
  return result;
}

} // namespace

auto main() -> int {
  try {
    auto connection = ::lsp::Connection(::lsp::io::standardIO());
    auto messageHandler = ::lsp::MessageHandler(connection);
    lesma::lsp_srv::DocumentStore docStore;

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
        [&messageHandler, &docStore](::lsp::notifications::TextDocument_DidOpen::Params&& params) {
          auto& doc = params.textDocument;
          docStore.open(doc.uri, doc.version, doc.text);
          std::string path(doc.uri.path().data(), doc.uri.path().size());
          runAnalyzeAndPublish(doc.uri, path, doc.text, messageHandler);
        });

    messageHandler.add<::lsp::notifications::TextDocument_DidChange>(
        [&messageHandler,
         &docStore](::lsp::notifications::TextDocument_DidChange::Params&& params) {
          auto uri = params.textDocument.uri;
          std::optional<std::string> pathOpt = docStore.getPath(uri);
          if (!pathOpt) {
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
            auto doc = docStore.getDocument(uri);
            if (doc) {
              text = doc->text;
            }
          }
          if (!text.empty()) {
            docStore.change(uri, params.textDocument.version, text);
            runAnalyzeAndPublish(uri, *pathOpt, text, messageHandler);
          }
        });

    messageHandler.add<::lsp::notifications::TextDocument_DidClose>(
        [&messageHandler, &docStore](::lsp::notifications::TextDocument_DidClose::Params&& params) {
          docStore.close(params.textDocument.uri);
          messageHandler.sendNotification<::lsp::notifications::TextDocument_PublishDiagnostics>(
              ::lsp::PublishDiagnosticsParams{.uri = params.textDocument.uri, .diagnostics = {}});
        });

    messageHandler.add<::lsp::requests::TextDocument_Hover>(
        [&docStore](::lsp::requests::TextDocument_Hover::Params&& params)
            -> ::lsp::TextDocument_HoverResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_HoverResult();
          }
          auto options = std::make_unique<lesma::Options>(lesma::Options{
              lesma::SourceType::STRING,
              doc->text,
              lesma::Debug::NONE,
              "output",
              false,
          });
          AnalysisResult result = lesma::analyze(std::move(options));
          if (result.hasErrors() || !result.parser || !result.rootScope) {
            return ::lsp::TextDocument_HoverResult();
          }
          lesma::Compound* ast = result.parser->getAst();
          if (!ast) {
            return ::lsp::TextDocument_HoverResult();
          }
          unsigned line = params.position.line;
          unsigned character = params.position.character;
          std::optional<std::string> name = findIdentifierAtSimple(
              ast, result.sourceMgr.get(), result.mainBufferId, line, character);
          if (!name) {
            return ::lsp::TextDocument_HoverResult();
          }
          lesma::Value* value = result.rootScope->lookup(*name);
          if (!value || !value->getType()) {
            return ::lsp::TextDocument_HoverResult();
          }
          std::string typeStr = value->getType()->toString();
          ::lsp::Hover hover;
          hover.contents = ::lsp::MarkupContent{
              .kind = ::lsp::MarkupKindEnum(::lsp::MarkupKind::PlainText),
              .value = typeStr,
          };
          return ::lsp::TextDocument_HoverResult(std::move(hover));
        });

    messageHandler.add<::lsp::requests::TextDocument_Definition>(
        [&docStore](::lsp::requests::TextDocument_Definition::Params&& params)
            -> ::lsp::TextDocument_DefinitionResult {
          auto doc = docStore.getDocument(params.textDocument.uri);
          if (!doc) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          auto options = std::make_unique<lesma::Options>(lesma::Options{
              lesma::SourceType::STRING,
              doc->text,
              lesma::Debug::NONE,
              "output",
              false,
          });
          AnalysisResult result = lesma::analyze(std::move(options));
          if (result.hasErrors() || !result.parser) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          lesma::Compound* ast = result.parser->getAst();
          if (!ast) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          unsigned line = params.position.line;
          unsigned character = params.position.character;
          std::optional<std::string> name = findIdentifierAtSimple(
              ast, result.sourceMgr.get(), result.mainBufferId, line, character);
          if (!name) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          auto index = buildDeclarationIndex(ast, doc->path);
          auto it = index.find(*name);
          if (it == index.end()) {
            return ::lsp::TextDocument_DefinitionResult();
          }
          ::lsp::Location loc;
          loc.uri = ::lsp::FileUri::fromPath(it->second.first);
          loc.range =
              smRangeToLspRange(result.sourceMgr.get(), result.mainBufferId, it->second.second);
          return ::lsp::TextDocument_DefinitionResult(::lsp::Definition(std::move(loc)));
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
