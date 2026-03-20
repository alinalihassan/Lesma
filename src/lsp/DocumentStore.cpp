#include "DocumentStore.h"

#include <filesystem>
#include <string>

#include <lsp/uri.h>

namespace lesma::lsp_srv {
namespace {

[[nodiscard]] auto stableFileUriKey(const ::lsp::DocumentUri& uri) -> std::string {
  // Clients may send file: URIs that differ by casing or encoding from didOpen; map to a
  // canonical path so hover/definition/completion find the same document as diagnostics.
  std::string const full = uri.toString();
  if (full.rfind("file:", 0) != 0) {
    return full;
  }
  std::string rawPath(uri.path().data(), uri.path().size());
  std::string const decoded = ::lsp::Uri::decode(rawPath);
  std::filesystem::path const p(decoded);
  std::error_code ec;
  std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
  if (!ec) {
    return canon.make_preferred().string();
  }
  std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (!ec) {
    abs = abs.lexically_normal();
    return abs.make_preferred().string();
  }
  return decoded;
}

} // namespace

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

std::optional<std::string> DocumentStore::getAnalyzeMainFilePath(const ::lsp::DocumentUri& uri) const {
  auto doc = getDocument(uri);
  if (!doc) {
    return std::nullopt;
  }
  std::string const full = uri.toString();
  if (full.rfind("file:", 0) != 0) {
    return std::nullopt;
  }
  std::string decoded = ::lsp::Uri::decode(doc->path);
  std::filesystem::path const p(decoded);
  std::error_code ec;
  std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
  if (!ec) {
    return canon.make_preferred().string();
  }
  std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (!ec) {
    return abs.lexically_normal().make_preferred().string();
  }
  return decoded;
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

std::string DocumentStore::uriToKey(const ::lsp::DocumentUri& uri) const {
  return stableFileUriKey(uri);
}

} // namespace lesma::lsp_srv
