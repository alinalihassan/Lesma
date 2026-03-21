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

  void open(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text);
  void change(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text);
  void close(const ::lsp::DocumentUri& uri);

  /** Returns path for use as mainFilePath in analysis (resolves relative imports). */
  [[nodiscard]] auto getPath(const ::lsp::DocumentUri& uri) const -> std::optional<std::string>;
  /** Decoded, canonical filesystem path for STRING-source analyze() (imports); nullopt if not a
   *  file URI (e.g. untitled). */
  [[nodiscard]] auto getAnalyzeMainFilePath(const ::lsp::DocumentUri& uri) const
      -> std::optional<std::string>;
  [[nodiscard]] auto getContent(const ::lsp::DocumentUri& uri) const -> std::optional<std::string>;
  [[nodiscard]] auto getDocument(const ::lsp::DocumentUri& uri) const -> std::optional<Document>;

  [[nodiscard]] auto uriToKey(const ::lsp::DocumentUri& uri) const -> std::string;

private:
  std::unordered_map<std::string, Document> documents;
};

} // namespace lesma::lsp_srv
