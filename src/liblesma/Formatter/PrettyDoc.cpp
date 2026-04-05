#include "liblesma/Formatter/PrettyDoc.h"

#include <algorithm>
#include <utility>

namespace lesma::pretty {

namespace {

enum class LayoutMode : std::uint8_t {
  Flat,
  Break,
};

struct LayoutFrame {
  LayoutMode mode = LayoutMode::Break;
  int indent = 0;
  Doc doc;
};

auto makeNode(DocKind kind) -> Doc {
  return Doc{
      .node = std::make_shared<DocNode>(DocNode{.kind = kind, .text = "", .indent = 0, .children = {}})};
}

auto flattenConcat(std::vector<Doc>& out, const Doc& doc) -> void {
  if (doc.node == nullptr || doc.node->kind == DocKind::Nil) {
    return;
  }
  if (doc.node->kind == DocKind::Concat) {
    for (const Doc& child : doc.node->children) {
      flattenConcat(out, child);
    }
    return;
  }
  out.push_back(doc);
}

auto trimWhitespaceOnlyCurrentLine(std::string& output) -> void {
  size_t const lineStart = output.find_last_of('\n');
  size_t const contentStart = lineStart == std::string::npos ? 0U : lineStart + 1U;
  if (contentStart >= output.size()) {
    return;
  }
  bool const onlySpaces = std::all_of(output.begin() + static_cast<std::ptrdiff_t>(contentStart),
                                      output.end(), [](char ch) { return ch == ' '; });
  if (onlySpaces) {
    output.erase(contentStart);
  }
}

auto fits(int remaining, std::vector<LayoutFrame> stack) -> bool {
  while (remaining >= 0 && !stack.empty()) {
    LayoutFrame frame = std::move(stack.back());
    stack.pop_back();
    if (frame.doc.node == nullptr) {
      continue;
    }
    DocNode const& node = *frame.doc.node;
    switch (node.kind) {
    case DocKind::Nil:
      break;
    case DocKind::Text:
      remaining -= static_cast<int>(node.text.size());
      break;
    case DocKind::Line:
      if (frame.mode == LayoutMode::Flat) {
        remaining -= static_cast<int>(node.text.size());
      } else {
        return true;
      }
      break;
    case DocKind::HardLine:
      return true;
    case DocKind::Concat:
      for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        stack.push_back(LayoutFrame{.mode = frame.mode, .indent = frame.indent, .doc = *it});
      }
      break;
    case DocKind::Nest:
      if (!node.children.empty()) {
        stack.push_back(LayoutFrame{
            .mode = frame.mode,
            .indent = frame.indent + node.indent,
            .doc = node.children.front(),
        });
      }
      break;
    case DocKind::Group:
      if (!node.children.empty()) {
        stack.push_back(LayoutFrame{
            .mode = LayoutMode::Flat,
            .indent = frame.indent,
            .doc = node.children.front(),
        });
      }
      break;
    }
  }
  return remaining >= 0;
}

} // namespace

auto nil() -> Doc { return makeNode(DocKind::Nil); }

auto text(std::string value) -> Doc {
  Doc doc = makeNode(DocKind::Text);
  doc.node->text = std::move(value);
  return doc;
}

auto line(std::string flatText) -> Doc {
  Doc doc = makeNode(DocKind::Line);
  doc.node->text = std::move(flatText);
  return doc;
}

auto hardLine() -> Doc { return makeNode(DocKind::HardLine); }

auto concat(std::vector<Doc> docs) -> Doc {
  std::vector<Doc> flattened;
  flattened.reserve(docs.size());
  for (const Doc& doc : docs) {
    flattenConcat(flattened, doc);
  }
  if (flattened.empty()) {
    return nil();
  }
  if (flattened.size() == 1U) {
    return flattened.front();
  }
  Doc doc = makeNode(DocKind::Concat);
  doc.node->children = std::move(flattened);
  return doc;
}

auto nest(int indent, Doc doc) -> Doc {
  if (doc.node == nullptr || doc.node->kind == DocKind::Nil || indent == 0) {
    return doc;
  }
  Doc nested = makeNode(DocKind::Nest);
  nested.node->indent = indent;
  nested.node->children.push_back(std::move(doc));
  return nested;
}

auto group(Doc doc) -> Doc {
  if (doc.node == nullptr || doc.node->kind == DocKind::Nil) {
    return doc;
  }
  Doc grouped = makeNode(DocKind::Group);
  grouped.node->children.push_back(std::move(doc));
  return grouped;
}

auto join(const Doc& separator, const std::vector<Doc>& docs) -> Doc {
  if (docs.empty()) {
    return nil();
  }
  std::vector<Doc> joined;
  joined.reserve(docs.size() * 2U);
  for (size_t i = 0; i < docs.size(); ++i) {
    if (i > 0U) {
      joined.push_back(separator);
    }
    joined.push_back(docs[i]);
  }
  return concat(std::move(joined));
}

auto append(const Doc& lhs, const Doc& rhs) -> Doc { return concat({lhs, rhs}); }

auto layout(const Doc& doc, int width) -> std::string {
  std::string output;
  int column = 0;
  std::vector<LayoutFrame> stack;
  stack.push_back(LayoutFrame{.mode = LayoutMode::Break, .indent = 0, .doc = doc});

  while (!stack.empty()) {
    LayoutFrame frame = std::move(stack.back());
    stack.pop_back();
    if (frame.doc.node == nullptr) {
      continue;
    }
    DocNode const& node = *frame.doc.node;
    switch (node.kind) {
    case DocKind::Nil:
      break;
    case DocKind::Text:
      output += node.text;
      column += static_cast<int>(node.text.size());
      break;
    case DocKind::Line:
      if (frame.mode == LayoutMode::Flat) {
        output += node.text;
        column += static_cast<int>(node.text.size());
      } else {
        trimWhitespaceOnlyCurrentLine(output);
        output.push_back('\n');
        output.append(static_cast<size_t>(frame.indent), ' ');
        column = frame.indent;
      }
      break;
    case DocKind::HardLine:
      trimWhitespaceOnlyCurrentLine(output);
      output.push_back('\n');
      output.append(static_cast<size_t>(frame.indent), ' ');
      column = frame.indent;
      break;
    case DocKind::Concat:
      for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        stack.push_back(LayoutFrame{.mode = frame.mode, .indent = frame.indent, .doc = *it});
      }
      break;
    case DocKind::Nest:
      if (!node.children.empty()) {
        stack.push_back(LayoutFrame{
            .mode = frame.mode,
            .indent = frame.indent + node.indent,
            .doc = node.children.front(),
        });
      }
      break;
    case DocKind::Group: {
      if (node.children.empty()) {
        break;
      }
      std::vector<LayoutFrame> probeStack = stack;
      probeStack.push_back(
          LayoutFrame{.mode = LayoutMode::Flat, .indent = frame.indent, .doc = node.children.front()});
      LayoutMode const mode = fits(width - column, std::move(probeStack)) ? LayoutMode::Flat
                                                                          : LayoutMode::Break;
      stack.push_back(LayoutFrame{.mode = mode, .indent = frame.indent, .doc = node.children.front()});
      break;
    }
    }
  }

  return output;
}

} // namespace lesma::pretty
