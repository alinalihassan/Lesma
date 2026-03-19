#include "DocumentStore.h"

#include <lsp/uri.h>

namespace lesma::lsp_srv {

void DocumentStore::open(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text) {
  std::string key = uriToKey(uri);
  std::string pathStr(uri.path().data(), uri.path().size());
  documents_[key] =
      Document{.path = std::move(pathStr), .version = version, .text = std::move(text)};
}

void DocumentStore::change(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text) {
  std::string key = uriToKey(uri);
  auto it = documents_.find(key);
  if (it != documents_.end()) {
    it->second.version = version;
    it->second.text = std::move(text);
  }
}

void DocumentStore::close(const ::lsp::DocumentUri& uri) { documents_.erase(uriToKey(uri)); }

std::optional<std::string> DocumentStore::getPath(const ::lsp::DocumentUri& uri) const {
  auto it = documents_.find(uriToKey(uri));
  if (it == documents_.end()) {
    return std::nullopt;
  }
  return it->second.path;
}

std::optional<std::string> DocumentStore::getContent(const ::lsp::DocumentUri& uri) const {
  auto it = documents_.find(uriToKey(uri));
  if (it == documents_.end()) {
    return std::nullopt;
  }
  return it->second.text;
}

std::optional<DocumentStore::Document>
DocumentStore::getDocument(const ::lsp::DocumentUri& uri) const {
  auto it = documents_.find(uriToKey(uri));
  if (it == documents_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string DocumentStore::uriToKey(const ::lsp::DocumentUri& uri) const { return uri.toString(); }

} // namespace lesma::lsp_srv
