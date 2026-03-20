#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include <lsp/fileuri.h>

namespace lesma::lsp_srv {

/** Tracks open documents: URI -> path, version, full text.
 *  (Named lsp_srv to avoid clashing with the global ::lsp namespace from lsp-framework.) */
class DocumentStore {
public:
  struct Document {
    std::string path;
    std::int32_t version = 0;
    std::string text;
  };

  void setWorkspaceRoot(std::string rootPath) { workspaceRoot_ = std::move(rootPath); }

  void open(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text);
  void change(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text);
  void close(const ::lsp::DocumentUri& uri);

  /** Returns path for use as mainFilePath in analysis (resolves relative imports). */
  [[nodiscard]] std::optional<std::string> getPath(const ::lsp::DocumentUri& uri) const;
  /** Decoded, canonical filesystem path for STRING-source analyze() (imports); nullopt if not a
   *  file URI (e.g. untitled). */
  [[nodiscard]] std::optional<std::string> getAnalyzeMainFilePath(const ::lsp::DocumentUri& uri) const;
  [[nodiscard]] std::optional<std::string> getContent(const ::lsp::DocumentUri& uri) const;
  [[nodiscard]] std::optional<Document> getDocument(const ::lsp::DocumentUri& uri) const;

  [[nodiscard]] std::string uriToKey(const ::lsp::DocumentUri& uri) const;

private:
  std::string workspaceRoot_;
  std::unordered_map<std::string, Document> documents_;
};

} // namespace lesma::lsp_srv
