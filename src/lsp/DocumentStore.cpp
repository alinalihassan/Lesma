#include "DocumentStore.h"

#include <filesystem>
#include <string>

#include <lsp/uri.h>

namespace lesma::lsp_srv {
namespace {

[[nodiscard]] auto canonicalFilePath(const ::lsp::DocumentUri& uri) -> std::optional<std::string> {
  std::string const full = uri.toString();
  if (full.rfind("file:", 0) != 0) {
    return std::nullopt;
  }
  std::string rawPath(uri.path().data(), uri.path().size());
  std::string decoded = ::lsp::Uri::decode(rawPath);
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

[[nodiscard]] auto stableFileUriKey(const ::lsp::DocumentUri& uri) -> std::string {
  // Clients may send file: URIs that differ by casing or encoding from didOpen; map to a
  // canonical path so hover/definition/completion find the same document as diagnostics.
  if (std::optional<std::string> canon = canonicalFilePath(uri)) {
    return *canon;
  }
  return uri.toString();
}

[[nodiscard]] auto documentPathForUri(const ::lsp::DocumentUri& uri) -> std::string {
  if (std::optional<std::string> canon = canonicalFilePath(uri)) {
    return *canon;
  }
  std::string rawPath(uri.path().data(), uri.path().size());
  return ::lsp::Uri::decode(rawPath);
}

} // namespace

void DocumentStore::open(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text) {
  std::string key = uriToKey(uri);
  documents[key] =
      Document{.path = documentPathForUri(uri), .version = version, .text = std::move(text)};
}

void DocumentStore::change(const ::lsp::DocumentUri& uri, std::int32_t version, std::string text) {
  std::string key = uriToKey(uri);
  auto it = documents.find(key);
  if (it != documents.end()) {
    it->second.version = version;
    it->second.text = std::move(text);
  }
}

void DocumentStore::close(const ::lsp::DocumentUri& uri) { documents.erase(uriToKey(uri)); }

auto DocumentStore::getPath(const ::lsp::DocumentUri& uri) const -> std::optional<std::string> {
  auto it = documents.find(uriToKey(uri));
  if (it == documents.end()) {
    return std::nullopt;
  }
  return it->second.path;
}

auto DocumentStore::getAnalyzeMainFilePath(const ::lsp::DocumentUri& uri) const
    -> std::optional<std::string> {
  if (!getDocument(uri)) {
    return std::nullopt;
  }
  return canonicalFilePath(uri);
}

auto DocumentStore::getContent(const ::lsp::DocumentUri& uri) const -> std::optional<std::string> {
  auto it = documents.find(uriToKey(uri));
  if (it == documents.end()) {
    return std::nullopt;
  }
  return it->second.text;
}

auto DocumentStore::getDocument(const ::lsp::DocumentUri& uri) const
    -> std::optional<DocumentStore::Document> {
  auto it = documents.find(uriToKey(uri));
  if (it == documents.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto DocumentStore::uriToKey(const ::lsp::DocumentUri& uri) const -> std::string {
  return stableFileUriKey(uri);
}

} // namespace lesma::lsp_srv
