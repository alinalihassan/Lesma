#include "liblesma/Formatter/SourceFormatter.h"

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Formatter/PrettyDoc.h"
#include "liblesma/Token/Token.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

namespace {

using lesma::pretty::Doc;

constexpr int INDENT_WIDTH = 2;
constexpr int DEFAULT_MIN_WIDTH = 20;

[[nodiscard]] auto clampWidth(int width) -> int { return std::max(width, DEFAULT_MIN_WIDTH); }

[[nodiscard]] auto isMajorDeclaration(const Statement* node) -> bool {
  return dynamic_cast<const FuncDecl*>(node) != nullptr ||
         dynamic_cast<const ExternFuncDecl*>(node) != nullptr ||
         dynamic_cast<const Class*>(node) != nullptr ||
         dynamic_cast<const TraitDecl*>(node) != nullptr ||
         dynamic_cast<const Enum*>(node) != nullptr;
}

[[nodiscard]] auto operatorSpelling(TokenType type) -> std::string_view {
  switch (type) {
  case TokenType::PLUS:
    return "+";
  case TokenType::MINUS:
    return "-";
  case TokenType::STAR:
    return "*";
  case TokenType::SLASH:
    return "/";
  case TokenType::MOD:
    return "%";
  case TokenType::POWER:
    return "^";
  case TokenType::AMPERSAND:
    return "&";
  case TokenType::EQUAL_EQUAL:
    return "==";
  case TokenType::BANG_EQUAL:
    return "!=";
  case TokenType::LESS:
    return "<";
  case TokenType::LESS_EQUAL:
    return "<=";
  case TokenType::GREATER:
    return ">";
  case TokenType::GREATER_EQUAL:
    return ">=";
  case TokenType::AND:
    return "and";
  case TokenType::OR:
    return "or";
  case TokenType::NOT:
    return "not";
  case TokenType::BANG:
    return "!";
  case TokenType::IS:
    return "is";
  case TokenType::IS_NOT:
    return "is not";
  case TokenType::EQUAL:
    return "=";
  case TokenType::PLUS_EQUAL:
    return "+=";
  case TokenType::MINUS_EQUAL:
    return "-=";
  case TokenType::STAR_EQUAL:
    return "*=";
  case TokenType::SLASH_EQUAL:
    return "/=";
  case TokenType::MOD_EQUAL:
    return "%=";
  case TokenType::POWER_EQUAL:
    return "^=";
  default:
    return "?";
  }
}

[[nodiscard]] auto unaryNeedsSpace(TokenType type) -> bool { return type == TokenType::NOT; }

[[nodiscard]] auto isVoidType(const TypeExpr* type) -> bool {
  return type != nullptr && type->getType() == TokenType::VOID_TYPE;
}

[[nodiscard]] auto precedence(const Expression* expr) -> int {
  if (dynamic_cast<const DotOp*>(expr) != nullptr ||
      dynamic_cast<const SubscriptOp*>(expr) != nullptr ||
      dynamic_cast<const FuncCall*>(expr) != nullptr) {
    return 90;
  }
  if (dynamic_cast<const UnaryOp*>(expr) != nullptr) {
    return 80;
  }
  if (dynamic_cast<const CastOp*>(expr) != nullptr) {
    return 70;
  }
  if (dynamic_cast<const BinaryOp*>(expr) != nullptr ||
      dynamic_cast<const IsOp*>(expr) != nullptr) {
    if (auto const* bin = dynamic_cast<const BinaryOp*>(expr); bin != nullptr) {
      switch (bin->getOperator()) {
      case TokenType::OR:
        return 10;
      case TokenType::AND:
        return 20;
      case TokenType::EQUAL_EQUAL:
      case TokenType::BANG_EQUAL:
      case TokenType::LESS:
      case TokenType::LESS_EQUAL:
      case TokenType::GREATER:
      case TokenType::GREATER_EQUAL:
        return 30;
      case TokenType::PLUS:
      case TokenType::MINUS:
        return 40;
      case TokenType::STAR:
      case TokenType::SLASH:
      case TokenType::MOD:
        return 50;
      case TokenType::POWER:
        return 60;
      default:
        return 0;
      }
    }
    return 30;
  }
  return 100;
}

[[nodiscard]] auto escapeString(std::string_view value) -> std::string {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\0':
      escaped += "\\0";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '$':
      escaped += "\\$";
      break;
    case static_cast<char>(0x1B):
      escaped += "\\e";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }
  return escaped;
}

[[nodiscard]] auto quoteString(std::string_view value) -> std::string {
  return "\"" + escapeString(value) + "\"";
}

[[nodiscard]] auto formatImportTarget(const Import* node) -> std::string {
  if (node->isStd()) {
    return getBasename(node->getFilePath());
  }
  return quoteString(node->getFilePath());
}

[[nodiscard]] auto defaultImportAlias(const Import* node) -> std::string {
  return getBasename(node->getFilePath());
}

[[nodiscard]] auto docText(std::string value) -> Doc {
  return lesma::pretty::text(std::move(value));
}

[[nodiscard]] auto docs(std::vector<Doc> values) -> Doc {
  return lesma::pretty::concat(std::move(values));
}

[[nodiscard]] auto softLine() -> Doc { return lesma::pretty::line(); }

[[nodiscard]] auto emptySoftLine() -> Doc { return lesma::pretty::line(""); }

[[nodiscard]] auto hardLine() -> Doc { return lesma::pretty::hardLine(); }

[[nodiscard]] auto docGroup(Doc value) -> Doc { return lesma::pretty::group(std::move(value)); }

[[nodiscard]] auto docIfBreak(std::string breakText, std::string flatText = "") -> Doc {
  return lesma::pretty::ifBreak(std::move(breakText), std::move(flatText));
}

[[nodiscard]] auto docNest(int indent, Doc value) -> Doc {
  return lesma::pretty::nest(indent, std::move(value));
}

[[nodiscard]] auto docJoin(const Doc& separator, const std::vector<Doc>& values) -> Doc {
  return lesma::pretty::join(separator, values);
}

[[nodiscard]] auto wrapDelimited(const std::string& open, const std::vector<Doc>& items,
                                 const std::string& close) -> Doc {
  if (items.empty()) {
    return docText(open + close);
  }
  Doc inner = docJoin(docs({docText(","), softLine()}), items);
  return docGroup(docs({
      docText(open),
      docNest(INDENT_WIDTH, docs({emptySoftLine(), inner})),
      docIfBreak(","),
      emptySoftLine(),
      docText(close),
  }));
}

[[nodiscard]] auto isNilDoc(const Doc& doc) -> bool {
  return doc.node == nullptr || doc.node->kind == lesma::pretty::DocKind::Nil;
}

[[nodiscard]] auto repeatHardLines(unsigned count) -> Doc {
  std::vector<Doc> out;
  out.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(hardLine());
  }
  return docs(std::move(out));
}

[[nodiscard]] auto formatGroupedInfix(Doc lhs, std::string op, Doc rhs) -> Doc {
  return docGroup(docs({
      std::move(lhs),
      docNest(INDENT_WIDTH, docs({softLine(), docText(std::move(op)), docText(" "), std::move(rhs)})),
  }));
}

[[nodiscard]] auto trimLeft(std::string_view value) -> std::string_view {
  size_t offset = 0;
  while (offset < value.size() &&
         std::isspace(static_cast<unsigned char>(value[offset])) != 0) {
    ++offset;
  }
  return value.substr(offset);
}

[[nodiscard]] auto trimRight(std::string_view value) -> std::string_view {
  size_t end = value.size();
  while (end > 0 &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(0, end);
}

[[nodiscard]] auto splitLines(std::string_view value) -> std::vector<std::string> {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(value.substr(start));
      break;
    }
    lines.emplace_back(value.substr(start, end - start));
    start = end + 1U;
  }
  return lines;
}

[[nodiscard]] auto formatMultilineBlockComment(std::string_view text) -> Doc {
  std::vector<std::string> lines = splitLines(text);
  if (lines.size() <= 1U) {
    return docText(std::string(text));
  }

  std::vector<Doc> out;
  out.reserve(lines.size() * 2U);
  out.push_back(docText(std::string(trimRight(lines.front()))));

  for (size_t i = 1; i + 1U < lines.size(); ++i) {
    std::string_view content = trimLeft(lines[i]);
    if (!content.empty() && content.front() == '*') {
      content.remove_prefix(1U);
      if (!content.empty() && content.front() == ' ') {
        content.remove_prefix(1U);
      }
    }
    out.push_back(hardLine());
    out.push_back(docText(content.empty() ? " *" : " * " + std::string(content)));
  }

  out.push_back(hardLine());
  out.push_back(docText(" */"));
  return docs(std::move(out));
}

[[nodiscard]] auto formatCommentDocs(const std::vector<CommentTrivia>& comments) -> Doc {
  std::vector<Doc> out;
  bool first = true;
  for (const CommentTrivia& comment : comments) {
    if (!first) {
      out.push_back(hardLine());
      if (comment.blankLinesBefore > 0U) {
        out.push_back(repeatHardLines(comment.blankLinesBefore));
      }
    }
    if (comment.text.starts_with("/*") && comment.text.find('\n') != std::string::npos) {
      out.push_back(formatMultilineBlockComment(comment.text));
    } else {
      out.push_back(docText(comment.text));
    }
    first = false;
  }
  return docs(std::move(out));
}

class SourceFormatter {
public:
  explicit SourceFormatter(const FormattingParseResult& parsed)
      : parsed(parsed), srcMgr(parsed.sourceMgr.get()) {}

  [[nodiscard]] auto format(int width) -> std::string {
    Compound* root = parsed.parser->getAst();
    Doc body = formatStatements(root != nullptr ? root->getChildren() : std::vector<Statement*>{},
                                root, true);
    std::string output = lesma::pretty::layout(body, clampWidth(width));
    if (output.empty() || output.back() != '\n') {
      output.push_back('\n');
    }
    return output;
  }

private:
  const FormattingParseResult& parsed;
  llvm::SourceMgr* srcMgr = nullptr;

  auto collectDotChain(const Expression* expr, const Expression*& base,
                       std::vector<const Expression*>& segments) const -> bool {
    auto const* dot = dynamic_cast<const DotOp*>(expr);
    if (dot == nullptr) {
      base = expr;
      return !segments.empty();
    }
    bool const isChain = collectDotChain(dot->getLeft(), base, segments);
    segments.push_back(dot->getRight());
    return isChain || !segments.empty();
  }

  [[nodiscard]] auto formatDotChain(const Expression* expr, int parentPrecedence) -> Doc {
    const Expression* base = nullptr;
    std::vector<const Expression*> segments;
    collectDotChain(expr, base, segments);

    std::vector<Doc> tailDocs;
    tailDocs.reserve(segments.size() * 2U);
    for (const Expression* segment : segments) {
      tailDocs.push_back(emptySoftLine());
      tailDocs.push_back(
          docs({docText("."), formatExpression(segment, precedence(expr))}));
    }

    return docGroup(docs({
        formatExpression(base, parentPrecedence),
        docNest(INDENT_WIDTH, docs(std::move(tailDocs))),
    }));
  }

  auto collectRepeatedBinaryOperands(const Expression* expr, TokenType op,
                                     std::vector<const Expression*>& operands) const -> bool {
    auto const* binary = dynamic_cast<const BinaryOp*>(expr);
    if (binary == nullptr || binary->getOperator() != op || op == TokenType::POWER) {
      operands.push_back(expr);
      return false;
    }

    bool flattenedLeft = collectRepeatedBinaryOperands(binary->getLeft(), op, operands);
    bool flattenedRight = collectRepeatedBinaryOperands(binary->getRight(), op, operands);
    static_cast<void>(flattenedLeft);
    static_cast<void>(flattenedRight);
    return true;
  }

  [[nodiscard]] auto formatRepeatedBinaryChain(const BinaryOp* binary, int currentPrecedence) -> Doc {
    std::vector<const Expression*> operands;
    collectRepeatedBinaryOperands(binary, binary->getOperator(), operands);

    std::vector<Doc> parts;
    parts.reserve(operands.size() * 2U);
    parts.push_back(formatExpression(operands.front(), currentPrecedence));
    for (size_t i = 1; i < operands.size(); ++i) {
      parts.push_back(docNest(
          INDENT_WIDTH,
          docs({softLine(), docText(std::string(operatorSpelling(binary->getOperator()))),
                docText(" "), formatExpression(operands[i], currentPrecedence + 1)})));
    }
    return docGroup(docs(std::move(parts)));
  }

  [[nodiscard]] auto formatCommaSeparated(const std::vector<Doc>& items) -> Doc {
    if (items.empty()) {
      return lesma::pretty::nil();
    }
    return docGroup(docJoin(docs({docText(","), softLine()}), items));
  }

  [[nodiscard]] auto formatClassOrTraitHead(std::vector<Doc> head,
                                            std::optional<Doc> wrappedSuffix = std::nullopt) -> Doc {
    if (wrappedSuffix.has_value()) {
      head.push_back(docNest(INDENT_WIDTH, docs({softLine(), *wrappedSuffix})));
    }
    return docGroup(docs(std::move(head)));
  }

  [[nodiscard]] auto formatLeadingComments(const Statement* node) -> Doc {
    return node == nullptr ? lesma::pretty::nil() : formatCommentDocs(node->getLeadingComments());
  }

  [[nodiscard]] auto formatTrailingComment(const Statement* node) -> Doc {
    if (node == nullptr || !node->getTrailingComment().has_value()) {
      return lesma::pretty::nil();
    }
    return docText(" " + node->getTrailingComment()->text);
  }

  [[nodiscard]] auto formatTrailingDetachedComments(const Statement* node) -> Doc {
    if (node == nullptr || node->getTrailingDetachedComments().empty()) {
      return lesma::pretty::nil();
    }
    std::vector<Doc> out;
    if (node->getExtraBlankLinesBeforeTrailingDetachedComments() > 0U) {
      out.push_back(repeatHardLines(node->getExtraBlankLinesBeforeTrailingDetachedComments()));
    }
    out.push_back(formatCommentDocs(node->getTrailingDetachedComments()));
    return docs(std::move(out));
  }

  // Canonical statement spacing:
  // - one newline between statements
  // - top-level major declarations keep at least one separating blank line
  // - user-authored blank lines may request one extra blank line, but spacing is capped
  // - comments stay attached to the following statement or trailing container position
  [[nodiscard]] auto formatStatements(const std::vector<Statement*>& statements,
                                      const Statement* container, bool topLevel) -> Doc {
    std::vector<Doc> out;
    Statement const* previous = nullptr;
    for (Statement* statement : statements) {
      if (statement == nullptr) {
        continue;
      }
      if (previous != nullptr) {
        unsigned structuralExtraBlankLines =
            topLevel && isMajorDeclaration(previous) && isMajorDeclaration(statement) ? 1U : 0U;
        if (dynamic_cast<const VarDecl*>(previous) != nullptr &&
            dynamic_cast<const FuncDecl*>(statement) != nullptr) {
          structuralExtraBlankLines = std::max(structuralExtraBlankLines, 1U);
        }
        unsigned const totalHardLines =
            1U + std::max(structuralExtraBlankLines, statement->getExtraBlankLinesBefore());
        out.push_back(repeatHardLines(totalHardLines));
      }
      Doc leadingComments = formatLeadingComments(statement);
      if (!isNilDoc(leadingComments)) {
        out.push_back(leadingComments);
        out.push_back(hardLine());
      }
      out.push_back(formatStatement(statement) + formatTrailingComment(statement));
      previous = statement;
    }
    Doc trailingComments = formatTrailingDetachedComments(container);
    if (!isNilDoc(trailingComments)) {
      if (previous != nullptr) {
        out.push_back(hardLine());
      }
      out.push_back(trailingComments);
    }
    return docs(std::move(out));
  }

  [[nodiscard]] auto formatStatement(const Statement* node) -> Doc {
    if (auto const* import = dynamic_cast<const Import*>(node); import != nullptr) {
      return formatImport(import);
    }
    if (auto const* enumDecl = dynamic_cast<const Enum*>(node); enumDecl != nullptr) {
      return formatEnum(enumDecl);
    }
    if (auto const* classDecl = dynamic_cast<const Class*>(node); classDecl != nullptr) {
      return formatClass(classDecl);
    }
    if (auto const* traitDecl = dynamic_cast<const TraitDecl*>(node); traitDecl != nullptr) {
      return formatTrait(traitDecl);
    }
    if (auto const* varDecl = dynamic_cast<const VarDecl*>(node); varDecl != nullptr) {
      return formatVarDecl(varDecl);
    }
    if (auto const* ifStmt = dynamic_cast<const If*>(node); ifStmt != nullptr) {
      return formatIf(ifStmt);
    }
    if (auto const* whileStmt = dynamic_cast<const While*>(node); whileStmt != nullptr) {
      return formatWhile(whileStmt);
    }
    if (auto const* forStmt = dynamic_cast<const ForIn*>(node); forStmt != nullptr) {
      return formatForIn(forStmt);
    }
    if (auto const* funcDecl = dynamic_cast<const FuncDecl*>(node); funcDecl != nullptr) {
      return formatFuncDecl(funcDecl);
    }
    if (auto const* externDecl = dynamic_cast<const ExternFuncDecl*>(node); externDecl != nullptr) {
      return formatExternFuncDecl(externDecl);
    }
    if (auto const* assignment = dynamic_cast<const Assignment*>(node); assignment != nullptr) {
      return docs({formatExpression(assignment->getLeftHandSide()), docText(" "),
                   docText(std::string(operatorSpelling(assignment->getOperator()))), docText(" "),
                   formatExpression(assignment->getRightHandSide())});
    }
    if (auto const* exprStmt = dynamic_cast<const ExpressionStatement*>(node);
        exprStmt != nullptr) {
      return formatExpression(exprStmt->getExpression());
    }
    if (dynamic_cast<const Break*>(node) != nullptr) {
      return docText("break");
    }
    if (dynamic_cast<const Continue*>(node) != nullptr) {
      return docText("continue");
    }
    if (dynamic_cast<const Pass*>(node) != nullptr) {
      return docText("pass");
    }
    if (auto const* returnStmt = dynamic_cast<const Return*>(node); returnStmt != nullptr) {
      if (returnStmt->getValue() == nullptr) {
        return docText("return");
      }
      return docs({docText("return "), formatExpression(returnStmt->getValue())});
    }
    if (auto const* deferStmt = dynamic_cast<const Defer*>(node); deferStmt != nullptr) {
      return docs({docText("defer "), formatStatement(deferStmt->getStatement())});
    }
    if (auto const* unimplemented = dynamic_cast<const UnimplementedStatement*>(node);
        unimplemented != nullptr) {
      return docText(unimplemented->getMessage());
    }
    llvm::report_fatal_error("SourceFormatter: unhandled Statement subclass");
  }

  [[nodiscard]] auto formatBlock(const Compound* block) -> Doc {
    std::vector<Statement*> children =
        block != nullptr ? block->getChildren() : std::vector<Statement*>{};
    Doc body = formatStatements(children, block, false);
    if (isNilDoc(body)) {
      return docText("{}");
    }
    return docs({
        docText("{"),
        docNest(INDENT_WIDTH, docs({hardLine(), body})),
        hardLine(),
        docText("}"),
    });
  }

  [[nodiscard]] auto formatImport(const Import* node) -> Doc {
    std::string const target = formatImportTarget(node);
    if (node->getImportAll() && !node->getImportScope()) {
      std::vector<Doc> parts{docText("import "), docText(target)};
      std::string const alias = node->getAlias();
      if (!alias.empty() && alias != defaultImportAlias(node)) {
        parts.push_back(docText(" as "));
        parts.push_back(docText(alias));
      }
      return docs(std::move(parts));
    }
    if (node->getImportAll() && node->getImportScope()) {
      return docs({docText("from "), docText(target), docText(" import *")});
    }
    std::vector<Doc> names;
    for (const ImportedNameBinding& binding : node->getImportedNames()) {
      if (binding.alias.empty() || binding.alias == binding.name) {
        names.push_back(docText(binding.name));
      } else {
        names.push_back(docs({docText(binding.name), docText(" as "), docText(binding.alias)}));
      }
    }
    return docs({docText("from "), docText(target), docText(" import "),
                 docJoin(docs({docText(","), softLine()}), names)});
  }

  [[nodiscard]] auto formatEnum(const Enum* node) -> Doc {
    std::vector<Doc> head;
    if (node->isExported()) {
      head.push_back(docText("export "));
    }
    head.push_back(docText("enum "));
    head.push_back(docText(node->getIdentifier()));

    std::vector<Doc> bodyDocs;
    std::vector<EnumValueDecl> const& values = node->getValueDecls();
    for (size_t i = 0; i < values.size(); ++i) {
      if (i > 0U) {
        bodyDocs.push_back(hardLine());
        if (values[i].extraBlankLinesBefore > 0U) {
          bodyDocs.push_back(repeatHardLines(values[i].extraBlankLinesBefore));
        }
      }
      Doc leading = formatCommentDocs(values[i].leadingComments);
      if (!isNilDoc(leading)) {
        bodyDocs.push_back(leading);
        bodyDocs.push_back(hardLine());
      }
      bodyDocs.push_back(docText(values[i].name));
      if (values[i].trailingComment.has_value()) {
        bodyDocs.push_back(docText(" " + values[i].trailingComment->text));
      }
    }
    Doc detached = formatTrailingDetachedComments(node);
    if (!isNilDoc(detached)) {
      if (!bodyDocs.empty()) {
        bodyDocs.push_back(hardLine());
      }
      bodyDocs.push_back(detached);
    }
    Doc body = docs(std::move(bodyDocs));
    if (isNilDoc(body)) {
      return docs({docs(std::move(head)), docText(" {}")});
    }
    return docs({
        docs(std::move(head)),
        docText(" {"),
        docNest(INDENT_WIDTH, docs({hardLine(), body})),
        hardLine(),
        docText("}"),
    });
  }

  [[nodiscard]] auto formatTrait(const TraitDecl* node) -> Doc {
    std::vector<Doc> head;
    if (node->isExported()) {
      head.push_back(docText("export "));
    }
    head.push_back(docText("trait "));
    head.push_back(docText(node->getIdentifier()));
    head.push_back(formatGenericParams(node->getGenericParamDecls()));
    Doc headDoc = formatClassOrTraitHead(std::move(head));
    std::vector<Statement*> requirements;
    for (FuncDecl* requirement : node->getRequirements()) {
      requirements.push_back(requirement);
    }
    Doc body = formatStatements(requirements, node, false);
    if (isNilDoc(body)) {
      return docs({headDoc, docText(" {}")});
    }
    return docs({
        headDoc,
        docText(" {"),
        docNest(INDENT_WIDTH, docs({hardLine(), body})),
        hardLine(),
        docText("}"),
    });
  }

  [[nodiscard]] auto formatClass(const Class* node) -> Doc {
    std::vector<Doc> head;
    if (node->isExported()) {
      head.push_back(docText("export "));
    }
    head.push_back(docText("class "));
    head.push_back(docText(node->getIdentifier()));
    head.push_back(formatGenericParams(node->getGenericParamDecls()));
    if (node->getBaseType() != nullptr) {
      head.push_back(docText(" : "));
      head.push_back(formatType(node->getBaseType()));
    }
    std::optional<Doc> implSuffix;
    if (!node->getImplTraitNames().empty()) {
      std::vector<Doc> implDocs;
      auto const& traitNames = node->getImplTraitNames();
      auto const& traitTypeArgs = node->getImplTraitTypeArgs();
      for (size_t i = 0; i < traitNames.size(); ++i) {
        std::vector<Doc> traitDoc{docText(traitNames[i])};
        if (i < traitTypeArgs.size() && !traitTypeArgs[i].empty()) {
          std::vector<Doc> args;
          for (const auto& typeArg : traitTypeArgs[i]) {
            args.push_back(formatType(typeArg.get()));
          }
          traitDoc.push_back(wrapDelimited("<", args, ">"));
        }
        implDocs.push_back(docs(std::move(traitDoc)));
      }
      implSuffix = docs({docText("impl "), formatCommaSeparated(implDocs)});
    }
    Doc headDoc = formatClassOrTraitHead(std::move(head), std::move(implSuffix));

    std::vector<Statement*> members;
    for (VarDecl* field : node->getFields()) {
      members.push_back(field);
    }
    for (FuncDecl* method : node->getMethods()) {
      members.push_back(method);
    }
    std::sort(members.begin(), members.end(), [](const Statement* lhs, const Statement* rhs) {
      return lhs->getStart().getPointer() < rhs->getStart().getPointer();
    });
    Doc body = formatStatements(members, node, false);
    if (isNilDoc(body)) {
      return docs({headDoc, docText(" {}")});
    }
    return docs({
        headDoc,
        docText(" {"),
        docNest(INDENT_WIDTH, docs({hardLine(), body})),
        hardLine(),
        docText("}"),
    });
  }

  [[nodiscard]] auto formatVarDecl(const VarDecl* node) -> Doc {
    std::vector<Doc> parts;
    if (node->isExported()) {
      parts.push_back(docText("export "));
    }
    if (node->getIsPrivate()) {
      parts.push_back(docText("private "));
    }
    if (node->getIsStatic()) {
      parts.push_back(docText("static "));
    }
    parts.push_back(docText(node->getMutability() ? "var " : "let "));
    std::vector<Doc> vars;
    for (Literal* literal : node->getVarLiterals()) {
      vars.push_back(docText(literal->getValue()));
    }
    parts.push_back(docJoin(docs({docText(","), softLine()}), vars));
    if (node->getType() != nullptr) {
      parts.push_back(docText(": "));
      parts.push_back(formatType(node->getType()));
    }
    if (node->getValue() != nullptr) {
      parts.push_back(docText(" = "));
      parts.push_back(formatExpression(node->getValue()));
    }
    return docs(std::move(parts));
  }

  [[nodiscard]] auto formatIf(const If* node) -> Doc {
    std::vector<Expression*> conds = node->getConds();
    std::vector<Compound*> blocks = node->getBlocks();
    if (conds.empty() || blocks.empty()) {
      return docText("if true {}");
    }
    std::vector<Doc> parts{
        docText("if "),
        formatExpression(conds[0]),
        docText(" "),
        formatBlock(blocks[0]),
    };
    for (size_t i = 1; i < conds.size() && i < blocks.size(); ++i) {
      Doc leading = formatCommentDocs(node->getBranchLeadingComments(i));
      bool const breakBeforeElse =
          !isNilDoc(leading) || node->getBranchExtraBlankLinesBefore(i) > 0U;
      if (!isNilDoc(leading)) {
        parts.push_back(hardLine());
        if (node->getBranchExtraBlankLinesBefore(i) > 0U) {
          parts.push_back(repeatHardLines(node->getBranchExtraBlankLinesBefore(i)));
        }
        parts.push_back(leading);
        parts.push_back(hardLine());
      } else if (node->getBranchExtraBlankLinesBefore(i) > 0U) {
        parts.push_back(hardLine());
        parts.push_back(repeatHardLines(node->getBranchExtraBlankLinesBefore(i)));
      }
      parts.push_back(docText(breakBeforeElse ? "else " : " else "));
      if (dynamic_cast<Else*>(conds[i]) != nullptr) {
        parts.push_back(formatBlock(blocks[i]));
      } else {
        parts.push_back(docText("if "));
        parts.push_back(formatExpression(conds[i]));
        parts.push_back(docText(" "));
        parts.push_back(formatBlock(blocks[i]));
      }
    }
    return docs(std::move(parts));
  }

  [[nodiscard]] auto formatWhile(const While* node) -> Doc {
    return docs({docText("while "), formatExpression(node->getCond()), docText(" "),
                 formatBlock(node->getBlock())});
  }

  [[nodiscard]] auto formatForIn(const ForIn* node) -> Doc {
    return docs({docText("for "), docText(node->getIdentifier()->getValue()), docText(" in "),
                 formatExpression(node->getIterable()), docText(" "),
                 formatBlock(node->getBlock())});
  }

  [[nodiscard]] auto formatFuncDecl(const FuncDecl* node) -> Doc {
    std::vector<Doc> parts;
    if (node->isExported()) {
      parts.push_back(docText("export "));
    }
    if (node->getIsPrivate()) {
      parts.push_back(docText("private "));
    }
    if (node->getDeclaresOverload()) {
      parts.push_back(docText("overload "));
    }
    if (node->getIsStatic()) {
      parts.push_back(docText("static "));
    }
    parts.push_back(docText("func "));
    parts.push_back(formatFunctionName(node->getName(), node->getOverloadGlyphSpan().isValid()));
    parts.push_back(formatGenericParams(node->getGenericParamDecls()));
    parts.push_back(formatParameters(node->getParameters(), node->getVarArgs()));
    if (!isVoidType(node->getReturnType())) {
      parts.push_back(docText(" -> "));
      parts.push_back(formatType(node->getReturnType()));
    }
    if (node->getBody() != nullptr) {
      parts.push_back(docText(" "));
      parts.push_back(formatBlock(node->getBody()));
    }
    return docs(std::move(parts));
  }

  [[nodiscard]] auto formatExternFuncDecl(const ExternFuncDecl* node) -> Doc {
    std::vector<Doc> parts;
    if (node->isExported()) {
      parts.push_back(docText("export "));
    }
    parts.push_back(docText("func extern "));
    parts.push_back(docText(node->getName()));
    parts.push_back(formatGenericParams(node->getGenericParamDecls()));
    parts.push_back(formatParameters(node->getParameters(), node->getVarArgs()));
    if (!isVoidType(node->getReturnType())) {
      parts.push_back(docText(" -> "));
      parts.push_back(formatType(node->getReturnType()));
    }
    return docs(std::move(parts));
  }

  [[nodiscard]] auto formatFunctionName(const std::string& name, bool overloadSpanValid) -> Doc {
    if (overloadSpanValid) {
      if (auto surface = OperatorUtils::surfaceSpellingForMangledOperator(name);
          surface.has_value()) {
        return docText(std::string(*surface));
      }
    }
    return docText(name);
  }

  [[nodiscard]] auto formatGenericParams(const std::vector<GenericParamDecl>& genericParams)
      -> Doc {
    if (genericParams.empty()) {
      return lesma::pretty::nil();
    }
    std::vector<Doc> docsOut;
    for (const GenericParamDecl& param : genericParams) {
      std::vector<Doc> paramDocs{docText(param.name)};
      if (!param.traitBounds.empty()) {
        std::vector<Doc> bounds;
        bounds.reserve(param.traitBounds.size());
        for (const std::string& bound : param.traitBounds) {
          bounds.push_back(docText(bound));
        }
        paramDocs.push_back(docText(": "));
        paramDocs.push_back(docJoin(docText(" & "), bounds));
      }
      docsOut.push_back(docs(std::move(paramDocs)));
    }
    return wrapDelimited("<", docsOut, ">");
  }

  [[nodiscard]] auto formatParameters(const std::vector<Parameter*>& parameters, bool varargs)
      -> Doc {
    std::vector<Doc> docsOut;
    for (const Parameter* parameter : parameters) {
      std::vector<Doc> parts{docText(parameter->name)};
      if (parameter->type != nullptr) {
        parts.push_back(docText(": "));
        parts.push_back(formatType(parameter->type.get()));
      }
      if (parameter->defaultVal != nullptr) {
        parts.push_back(docText(" = "));
        parts.push_back(formatExpression(parameter->defaultVal.get()));
      }
      docsOut.push_back(docs(std::move(parts)));
    }
    if (varargs) {
      docsOut.push_back(docText("..."));
    }
    return wrapDelimited("(", docsOut, ")");
  }

  [[nodiscard]] auto formatType(const TypeExpr* type) -> Doc {
    if (type == nullptr) {
      return docText("void");
    }
    switch (type->getType()) {
    case TokenType::PTR_TYPE:
      return docs({docText("*"), formatType(type->getElementType())});
    case TokenType::CUSTOM_TYPE: {
      std::vector<TypeExpr*> typeArgs = type->getTypeArgs();
      if (typeArgs.empty()) {
        return docText(type->getName());
      }
      std::vector<Doc> argDocs;
      argDocs.reserve(typeArgs.size());
      for (TypeExpr* typeArg : typeArgs) {
        argDocs.push_back(formatType(typeArg));
      }
      return docs({docText(type->getLookupName()), wrapDelimited("<", argDocs, ">")});
    }
    case TokenType::FUNC_TYPE: {
      std::vector<Doc> params;
      for (TypeExpr* param : type->getParams()) {
        params.push_back(formatType(param));
      }
      std::vector<Doc> parts{docText("func"), wrapDelimited("(", params, ")")};
      if (!isVoidType(type->getReturnType())) {
        parts.push_back(docText(" -> "));
        parts.push_back(formatType(type->getReturnType()));
      }
      return docs(std::move(parts));
    }
    case TokenType::TUPLE_TYPE: {
      std::vector<Doc> elements;
      for (TypeExpr* element : type->getParams()) {
        elements.push_back(formatType(element));
      }
      if (elements.size() == 1U) {
        return docs({docText("("), elements.front(), docText(",)")});
      }
      return wrapDelimited("(", elements, ")");
    }
    case TokenType::UNION_TYPE: {
      std::vector<Doc> arms;
      for (TypeExpr* arm : type->getParams()) {
        arms.push_back(formatType(arm));
      }
      return docJoin(docText(" | "), arms);
    }
    default:
      return docText(type->getName());
    }
  }

  [[nodiscard]] auto formatExpression(const Expression* expr, int parentPrecedence = 0) -> Doc {
    if (expr == nullptr) {
      return docText("nil");
    }
    Doc result;
    if (auto const* literal = dynamic_cast<const Literal*>(expr); literal != nullptr) {
      result = formatLiteral(literal);
    } else if (auto const* stringInterpolation = dynamic_cast<const StringInterpolation*>(expr);
               stringInterpolation != nullptr) {
      result = formatStringInterpolation(stringInterpolation);
    } else if (auto const* lambda = dynamic_cast<const LambdaExpr*>(expr); lambda != nullptr) {
      result = formatLambda(lambda);
    } else if (auto const* funcCall = dynamic_cast<const FuncCall*>(expr); funcCall != nullptr) {
      result = formatFuncCall(funcCall);
    } else if (dynamic_cast<const SuperExpr*>(expr) != nullptr) {
      result = docText("super");
    } else if (auto const* binary = dynamic_cast<const BinaryOp*>(expr); binary != nullptr) {
      int const currentPrecedence = precedence(expr);
      std::vector<const Expression*> operands;
      bool const repeatedChain =
          collectRepeatedBinaryOperands(binary, binary->getOperator(), operands);
      if (repeatedChain && operands.size() > 2U) {
        result = formatRepeatedBinaryChain(binary, currentPrecedence);
      } else {
        result = formatGroupedInfix(formatExpression(binary->getLeft(), currentPrecedence),
                                    std::string(operatorSpelling(binary->getOperator())),
                                    formatExpression(binary->getRight(), currentPrecedence + 1));
      }
    } else if (auto const* subscript = dynamic_cast<const SubscriptOp*>(expr);
               subscript != nullptr) {
      result = docs({formatExpression(subscript->getLeft(), precedence(expr)), docText("["),
                     formatExpression(subscript->getIndex()), docText("]")});
    } else if (auto const* dot = dynamic_cast<const DotOp*>(expr); dot != nullptr) {
      result = formatDotChain(dot, parentPrecedence);
    } else if (auto const* cast = dynamic_cast<const CastOp*>(expr); cast != nullptr) {
      int const currentPrecedence = precedence(expr);
      result = formatGroupedInfix(formatExpression(cast->getExpression(), currentPrecedence), "as",
                                  formatType(cast->getType()));
    } else if (auto const* isOp = dynamic_cast<const IsOp*>(expr); isOp != nullptr) {
      int const currentPrecedence = precedence(expr);
      result = formatGroupedInfix(formatExpression(isOp->getLeft(), currentPrecedence),
                                  std::string(operatorSpelling(isOp->getOperator())),
                                  formatType(isOp->getRight()));
    } else if (auto const* unary = dynamic_cast<const UnaryOp*>(expr); unary != nullptr) {
      int const currentPrecedence = precedence(expr);
      std::string const op = std::string(operatorSpelling(unary->getOperator()));
      result = docs({docText(op),
                     unaryNeedsSpace(unary->getOperator()) ? docText(" ") : lesma::pretty::nil(),
                     formatExpression(unary->getExpression(), currentPrecedence)});
    } else if (auto const* list = dynamic_cast<const ListLiteral*>(expr); list != nullptr) {
      std::vector<Doc> elements;
      for (Expression* element : list->getElements()) {
        elements.push_back(formatExpression(element));
      }
      result = wrapDelimited("[", elements, "]");
    } else if (auto const* dict = dynamic_cast<const DictLiteral*>(expr); dict != nullptr) {
      std::vector<Doc> entries;
      std::vector<Expression*> keys = dict->getKeys();
      std::vector<Expression*> values = dict->getValues();
      for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        entries.push_back(
            docs({formatExpression(keys[i]), docText(": "), formatExpression(values[i])}));
      }
      result = wrapDelimited("{", entries, "}");
    } else if (auto const* tuple = dynamic_cast<const TupleLiteral*>(expr); tuple != nullptr) {
      std::vector<Doc> elements;
      for (Expression* element : tuple->getElements()) {
        elements.push_back(formatExpression(element));
      }
      if (elements.size() == 1U) {
        result = docs({docText("("), elements.front(), docText(",)")});
      } else {
        result = wrapDelimited("(", elements, ")");
      }
    } else if (dynamic_cast<const Else*>(expr) != nullptr) {
      result = docText("else");
    } else {
      llvm::report_fatal_error("SourceFormatter: unhandled Expression subclass");
    }

    if (precedence(expr) < parentPrecedence) {
      return docs({docText("("), result, docText(")")});
    }
    return result;
  }

  [[nodiscard]] auto formatLiteral(const Literal* literal) -> Doc {
    if (literal->getType() == TokenType::STRING) {
      return docText(quoteString(literal->getValue()));
    }
    return docText(literal->getValue());
  }

  [[nodiscard]] auto formatStringInterpolation(const StringInterpolation* interpolation) -> Doc {
    std::string rendered = "\"";
    std::vector<std::string> const& chunks = interpolation->getChunks();
    std::vector<Expression*> exprs = interpolation->getExprs();
    for (size_t i = 0; i < exprs.size(); ++i) {
      rendered += escapeString(chunks[i]);
      rendered += "${";
      rendered += lesma::pretty::layout(formatExpression(exprs[i]), clampWidth(1000));
      while (!rendered.empty() && rendered.back() == '\n') {
        rendered.pop_back();
      }
      rendered += "}";
    }
    rendered += escapeString(chunks.back());
    rendered += "\"";
    return docText(rendered);
  }

  [[nodiscard]] auto formatLambda(const LambdaExpr* node) -> Doc {
    std::vector<Doc> parts{docText("func"), formatGenericParams(node->getGenericParamDecls()),
                           formatParameters(node->getParameters(), false)};
    if (node->getReturnType() != nullptr && !isVoidType(node->getReturnType())) {
      parts.push_back(docText(" -> "));
      parts.push_back(formatType(node->getReturnType()));
    }
    if (node->isExpressionBody()) {
      parts.push_back(docText(" => "));
      parts.push_back(formatExpression(node->getExpressionBody()));
    } else {
      parts.push_back(docText(" "));
      parts.push_back(formatBlock(node->getBlockBody()));
    }
    return docs(std::move(parts));
  }

  [[nodiscard]] auto formatFuncCall(const FuncCall* node) -> Doc {
    std::vector<Doc> parts{docText(node->getName())};
    std::vector<TypeExpr*> explicitTypeArgs = node->getExplicitTypeArgs();
    if (!explicitTypeArgs.empty()) {
      std::vector<Doc> typeDocs;
      typeDocs.reserve(explicitTypeArgs.size());
      for (TypeExpr* typeArg : explicitTypeArgs) {
        typeDocs.push_back(formatType(typeArg));
      }
      parts.push_back(wrapDelimited("<", typeDocs, ">"));
    }
    std::vector<Doc> argDocs;
    for (Expression* arg : node->getArguments()) {
      argDocs.push_back(formatExpression(arg));
    }
    parts.push_back(wrapDelimited("(", argDocs, ")"));
    return docs(std::move(parts));
  }
};

} // namespace

auto lesma::parseFileForFormatting(const std::filesystem::path& path)
    -> std::expected<FormattingParseResult, FormattingError> {
  auto sourceMgr = std::make_shared<llvm::SourceMgr>();
  unsigned mainBufferId = 0;
  std::string filePath = std::filesystem::absolute(path).string();

  try {
    auto buffer = llvm::MemoryBuffer::getFileAsStream(filePath);
    if (!buffer) {
      return std::unexpected(FormattingError{
          .message = "Could not read file: " + filePath,
          .span = llvm::SMRange(),
          .sourceMgr = nullptr,
          .bufferId = 0,
          .filePath = filePath,
      });
    }
    mainBufferId = sourceMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  } catch (const LesmaError& err) {
    return std::unexpected(FormattingError{
        .message = err.what(),
        .span = err.getSpan(),
        .sourceMgr = sourceMgr,
        .bufferId = mainBufferId,
        .filePath = filePath,
    });
  }

  try {
    auto lexer = std::make_unique<Lexer>(sourceMgr, nullptr, filePath);
    lexer->scanAll();

    auto parser =
        std::make_unique<Parser>(lexer->getTokens(), nullptr, sourceMgr, mainBufferId, filePath);
    parser->parse();
    return FormattingParseResult{
        .sourceMgr = std::move(sourceMgr),
        .mainBufferId = mainBufferId,
        .filePath = std::move(filePath),
        .lexer = std::move(lexer),
        .parser = std::move(parser),
    };
  } catch (const LesmaError& err) {
    return std::unexpected(FormattingError{
        .message = err.what(),
        .span = err.getSpan(),
        .sourceMgr = sourceMgr,
        .bufferId = mainBufferId,
        .filePath = filePath,
    });
  }
}

auto lesma::parseSourceForFormatting(std::string source, std::string logicalPath)
    -> std::expected<FormattingParseResult, FormattingError> {
  auto sourceMgr = std::make_shared<llvm::SourceMgr>();
  unsigned mainBufferId = 0;

  try {
    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(source, logicalPath);
    mainBufferId = sourceMgr->AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  } catch (const LesmaError& err) {
    return std::unexpected(FormattingError{
        .message = err.what(),
        .span = err.getSpan(),
        .sourceMgr = sourceMgr,
        .bufferId = mainBufferId,
        .filePath = std::move(logicalPath),
    });
  }

  try {
    auto lexer = std::make_unique<Lexer>(sourceMgr, nullptr, logicalPath);
    lexer->scanAll();

    auto parser =
        std::make_unique<Parser>(lexer->getTokens(), nullptr, sourceMgr, mainBufferId, logicalPath);
    parser->parse();
    return FormattingParseResult{
        .sourceMgr = std::move(sourceMgr),
        .mainBufferId = mainBufferId,
        .filePath = std::move(logicalPath),
        .lexer = std::move(lexer),
        .parser = std::move(parser),
    };
  } catch (const LesmaError& err) {
    return std::unexpected(FormattingError{
        .message = err.what(),
        .span = err.getSpan(),
        .sourceMgr = sourceMgr,
        .bufferId = mainBufferId,
        .filePath = std::move(logicalPath),
    });
  }
}

auto lesma::formatParsedFile(const FormattingParseResult& parsed, int width) -> std::string {
  SourceFormatter formatter(parsed);
  return formatter.format(width);
}

auto lesma::formatFile(const std::filesystem::path& path, int width)
    -> std::expected<std::string, FormattingError> {
  auto parsed = parseFileForFormatting(path);
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error());
  }
  return formatParsedFile(*parsed, width);
}

auto lesma::formatSource(std::string source, std::string logicalPath, int width)
    -> std::expected<std::string, FormattingError> {
  auto parsed = parseSourceForFormatting(std::move(source), std::move(logicalPath));
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error());
  }
  return formatParsedFile(*parsed, width);
}
