#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lesma::pretty {

struct DocNode;

struct Doc {
  std::shared_ptr<DocNode> node;
};

enum class DocKind : std::uint8_t {
  Nil,
  Text,
  Line,
  IfBreak,
  HardLine,
  Concat,
  Nest,
  Group,
};

struct DocNode {
  DocKind kind = DocKind::Nil;
  std::string text;
  std::string altText;
  int indent = 0;
  std::vector<Doc> children;
};

[[nodiscard]] auto nil() -> Doc;
[[nodiscard]] auto text(std::string value) -> Doc;
[[nodiscard]] auto line(std::string flatText = " ") -> Doc;
[[nodiscard]] auto ifBreak(std::string breakText, std::string flatText = "") -> Doc;
[[nodiscard]] auto hardLine() -> Doc;
[[nodiscard]] auto concat(std::vector<Doc> docs) -> Doc;
[[nodiscard]] auto nest(int indent, Doc doc) -> Doc;
[[nodiscard]] auto group(Doc doc) -> Doc;
[[nodiscard]] auto join(const Doc& separator, const std::vector<Doc>& docs) -> Doc;
[[nodiscard]] auto layout(const Doc& doc, int width) -> std::string;
[[nodiscard]] auto append(const Doc& lhs, const Doc& rhs) -> Doc;

inline auto operator+(const Doc& lhs, const Doc& rhs) -> Doc { return append(lhs, rhs); }

} // namespace lesma::pretty
