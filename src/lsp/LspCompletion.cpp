#include "LspCompletion.h"

#include "LspUtf16.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SourceMgr.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

namespace lesma::lsp_srv {
namespace {

using namespace lesma;

struct CompletionContext {
  bool isMember = false;
  std::string prefix;
  std::string memberChain;
};

struct CompletionCandidate {
  std::string label;
  ::lsp::CompletionItemKind kind = ::lsp::CompletionItemKind::Text;
  std::string detail;
};

struct InnermostFunc {
  lesma::FuncDecl* func = nullptr;
  lesma::Class* enclosingClass = nullptr;
};

auto positionToOffset(llvm::StringRef ref, unsigned line, unsigned character) -> unsigned {
  return static_cast<unsigned>(bufferByteOffsetFromLspPosition(ref, line, character));
}

auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) -> unsigned {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

auto isIdentChar(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

auto startsWith(const std::string& text, const std::string& prefix) -> bool {
  return prefix.empty() || text.rfind(prefix, 0) == 0;
}

auto splitChain(const std::string& chain) -> std::vector<std::string> {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : chain) {
    if (ch == '.') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

auto typeDisplayName(Type* type, SymbolTable* root) -> std::string {
  if (type == nullptr) {
    return "?";
  }
  if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr) {
    return typeDisplayName(type->getElementType(), root) + "*";
  }
  if (type->is(BaseType::TY_ARRAY) && type->getElementType() != nullptr) {
    return typeDisplayName(type->getElementType(), root) + "[]";
  }
  if (type->is(BaseType::TY_INT)) {
    return type->isSigned() ? "int" : "uint";
  }
  if (type->is(BaseType::TY_FLOAT)) {
    return "float";
  }
  if (type->is(BaseType::TY_STRING)) {
    return "string";
  }
  if (type->is(BaseType::TY_BOOL)) {
    return "bool";
  }
  if (type->is(BaseType::TY_VOID)) {
    return "void";
  }
  if ((type->is(BaseType::TY_CLASS) || type->is(BaseType::TY_ENUM)) && root != nullptr) {
    for (Value* sym : root->getSymbols()) {
      if (sym != nullptr && sym->getCategory() == ValueCategory::TYPE_SYMBOL &&
          sym->getType() == type) {
        return sym->getName();
      }
    }
  }
  return type->toString();
}

auto symbolDetail(Value* value, SymbolTable* root) -> std::string {
  if (value == nullptr || value->getType() == nullptr) {
    return {};
  }
  Type* type = value->getType();
  if (type->is(BaseType::TY_FUNCTION)) {
    std::ostringstream oss;
    oss << value->getName() << "(";
    auto fields = type->getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (i > 0U) {
        oss << ", ";
      }
      oss << fields[i]->name << ": " << typeDisplayName(fields[i]->type, root);
    }
    oss << ")";
    if (type->getReturnType() != nullptr && !type->getReturnType()->is(BaseType::TY_VOID)) {
      oss << " -> " << typeDisplayName(type->getReturnType(), root);
    }
    return oss.str();
  }
  return typeDisplayName(type, root);
}

auto candidateKindForValue(Value* value) -> ::lsp::CompletionItemKind {
  if (value == nullptr || value->getType() == nullptr) {
    return ::lsp::CompletionItemKind::Text;
  }
  switch (value->getCategory()) {
  case ValueCategory::CALLABLE_SYMBOL:
    return ::lsp::CompletionItemKind::Function;
  case ValueCategory::TYPE_SYMBOL:
    return value->getType()->is(BaseType::TY_ENUM) ? ::lsp::CompletionItemKind::Enum
                                                   : ::lsp::CompletionItemKind::Class;
  case ValueCategory::MODULE_SYMBOL:
    return ::lsp::CompletionItemKind::Module;
  case ValueCategory::ADDRESSABLE_STORAGE:
  case ValueCategory::DIRECT_VALUE:
    return ::lsp::CompletionItemKind::Variable;
  }
  return ::lsp::CompletionItemKind::Text;
}

void considerFunc(lesma::FuncDecl* func, lesma::Class* cls, unsigned targetOffset,
                  llvm::SourceMgr* srcMgr, unsigned bufferId, InnermostFunc& best,
                  unsigned& bestLen) {
  if (func == nullptr || func->getBody() == nullptr) {
    return;
  }
  llvm::SMRange span = func->getBody()->getSpan();
  if (!span.isValid()) {
    return;
  }
  unsigned const start = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
  unsigned const end = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
  if (targetOffset < start || targetOffset >= end) {
    return;
  }
  unsigned const len = end - start;
  if (best.func == nullptr || len < bestLen) {
    best.func = func;
    best.enclosingClass = cls;
    bestLen = len;
  }
}

void scanCompoundForFuncs(lesma::Compound* compound, lesma::Class* cls, unsigned targetOffset,
                          llvm::SourceMgr* srcMgr, unsigned bufferId, InnermostFunc& best,
                          unsigned& bestLen) {
  if (compound == nullptr) {
    return;
  }
  for (lesma::Statement* stmt : compound->getChildren()) {
    if (auto* func = dynamic_cast<lesma::FuncDecl*>(stmt)) {
      considerFunc(func, cls, targetOffset, srcMgr, bufferId, best, bestLen);
      scanCompoundForFuncs(func->getBody(), cls, targetOffset, srcMgr, bufferId, best, bestLen);
    } else if (auto* klass = dynamic_cast<lesma::Class*>(stmt)) {
      for (lesma::FuncDecl* method : klass->getMethods()) {
        considerFunc(method, klass, targetOffset, srcMgr, bufferId, best, bestLen);
        scanCompoundForFuncs(method->getBody(), klass, targetOffset, srcMgr, bufferId, best,
                             bestLen);
      }
    }
  }
}

auto findInnermostFuncContaining(lesma::Compound* ast, unsigned targetOffset,
                                 llvm::SourceMgr* srcMgr, unsigned bufferId) -> InnermostFunc {
  InnermostFunc best;
  unsigned bestLen = 0U;
  scanCompoundForFuncs(ast, nullptr, targetOffset, srcMgr, bufferId, best, bestLen);
  return best;
}

auto activeScopeForOffset(const AnalysisResult& result, unsigned offset) -> SymbolTable* {
  if (result.rootScope == nullptr || result.parser == nullptr || result.sourceMgr == nullptr) {
    return nullptr;
  }
  lesma::Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return result.rootScope.get();
  }
  InnermostFunc inner =
      findInnermostFuncContaining(ast, offset, result.sourceMgr.get(), result.mainBufferId);
  if (inner.func != nullptr) {
    if (Value* funcSym = inner.func->getResolvedSymbol()) {
      if (funcSym->getBodyScope() != nullptr) {
        return funcSym->getBodyScope();
      }
    }
  }
  return result.rootScope.get();
}

auto extractCompletionContext(llvm::StringRef text, unsigned offset) -> CompletionContext {
  CompletionContext ctx;
  size_t cursor = static_cast<size_t>(offset);
  size_t prefixStart = cursor;
  while (prefixStart > 0U && isIdentChar(text[prefixStart - 1U])) {
    --prefixStart;
  }
  ctx.prefix = std::string(text.slice(prefixStart, cursor));
  if (prefixStart == 0U || text[prefixStart - 1U] != '.') {
    return ctx;
  }

  ctx.isMember = true;
  size_t chainEnd = prefixStart - 1U;
  size_t chainStart = chainEnd;
  while (chainStart > 0U && (isIdentChar(text[chainStart - 1U]) || text[chainStart - 1U] == '.')) {
    --chainStart;
  }
  ctx.memberChain = std::string(text.slice(chainStart, chainEnd + 1U));
  return ctx;
}

auto reanalyzeWithCompletionPlaceholder(const AnalysisResult& result, llvm::StringRef text,
                                        unsigned offset) -> AnalysisResult {
  std::string patchedText(text.data(), text.size());
  patchedText.insert(static_cast<size_t>(offset), "__cursor__");
  auto options = std::make_unique<Options>(Options{
      SourceType::STRING,
      std::move(patchedText),
      Debug::NONE,
      "output",
      false,
      result.mainFilePath,
  });
  return analyze(std::move(options));
}

auto lookupName(SymbolTable* scope, SymbolTable* root, const std::string& name) -> Value* {
  if (scope != nullptr) {
    if (Value* value = scope->lookup(name)) {
      return value;
    }
  }
  return root != nullptr ? root->lookup(name) : nullptr;
}

auto lookupImportedModuleSymbol(const AnalysisResult& result, const std::string& alias,
                                const std::string& symbolName) -> Value* {
  auto pathIt = result.importAliasToPath.find(alias);
  if (pathIt == result.importAliasToPath.end()) {
    return nullptr;
  }
  auto moduleIt = result.importedModules.find(pathIt->second);
  if (moduleIt == result.importedModules.end() || moduleIt->second == nullptr ||
      moduleIt->second->rootScope == nullptr) {
    return nullptr;
  }
  Value* value = moduleIt->second->rootScope->lookup(symbolName);
  if (value == nullptr || !value->isExported()) {
    return nullptr;
  }
  return value;
}

auto resolveMemberFieldType(Type* baseType, const std::string& name) -> Type* {
  if (baseType == nullptr) {
    return nullptr;
  }
  if (baseType->is(BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
    baseType = baseType->getElementType();
  }
  if (!baseType->is(BaseType::TY_CLASS) && !baseType->is(BaseType::TY_ENUM)) {
    return nullptr;
  }
  return TypeUtils::findTypeInFields(baseType, name);
}

auto resolveChainType(const AnalysisResult& result, const std::string& chain, SymbolTable* scope,
                      SymbolTable* root) -> Type* {
  std::vector<std::string> parts = splitChain(chain);
  if (parts.empty()) {
    return nullptr;
  }
  Value* current = lookupName(scope, root, parts.front());
  size_t nextPartIdx = 1U;
  if (current == nullptr) {
    current = lookupImportedModuleSymbol(result, parts.front(),
                                         parts.size() > 1U ? parts[1U] : std::string{});
    nextPartIdx = current != nullptr ? 2U : 1U;
  }
  if (current == nullptr) {
    return nullptr;
  }
  Type* type = current->getType();
  for (size_t i = nextPartIdx; i < parts.size(); ++i) {
    type = resolveMemberFieldType(type, parts[i]);
    if (type == nullptr) {
      return nullptr;
    }
  }
  return type;
}

void addCandidate(std::vector<CompletionCandidate>& out, std::unordered_set<std::string>& seen,
                  CompletionCandidate candidate);

void appendModuleMembersForAlias(const AnalysisResult& result, const std::string& alias,
                                 std::vector<CompletionCandidate>& out,
                                 std::unordered_set<std::string>& seen) {
  auto pathIt = result.importAliasToPath.find(alias);
  if (pathIt == result.importAliasToPath.end()) {
    return;
  }
  auto moduleIt = result.importedModules.find(pathIt->second);
  if (moduleIt == result.importedModules.end() || moduleIt->second == nullptr ||
      moduleIt->second->rootScope == nullptr) {
    return;
  }
  SymbolTable* moduleScope = moduleIt->second->rootScope.get();
  for (Value* value : moduleScope->getSymbols()) {
    if (value == nullptr || !value->isExported()) {
      continue;
    }
    addCandidate(out, seen,
                 CompletionCandidate{
                     .label = value->getName(),
                     .kind = candidateKindForValue(value),
                     .detail = symbolDetail(value, moduleScope),
                 });
  }
}

void addCandidate(std::vector<CompletionCandidate>& out, std::unordered_set<std::string>& seen,
                  CompletionCandidate candidate) {
  if (candidate.label.empty() || !seen.insert(candidate.label).second) {
    return;
  }
  out.push_back(std::move(candidate));
}

void appendMembersForType(Type* baseType, Compound* ast, SymbolTable* root,
                          std::vector<CompletionCandidate>& out,
                          std::unordered_set<std::string>& seen) {
  if (baseType == nullptr) {
    return;
  }
  if (baseType->is(BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
    baseType = baseType->getElementType();
  }
  if (!baseType->is(BaseType::TY_CLASS) && !baseType->is(BaseType::TY_ENUM)) {
    return;
  }

  for (Field* field : baseType->getFields()) {
    if (field == nullptr) {
      continue;
    }
    addCandidate(out, seen,
                 CompletionCandidate{.label = field->name,
                                     .kind = baseType->is(BaseType::TY_ENUM)
                                                 ? ::lsp::CompletionItemKind::EnumMember
                                                 : ::lsp::CompletionItemKind::Field,
                                     .detail = typeDisplayName(field->type, root)});
  }

  if (!baseType->is(BaseType::TY_CLASS) || ast == nullptr || root == nullptr) {
    return;
  }
  for (Statement* stmt : ast->getChildren()) {
    auto* klass = dynamic_cast<Class*>(stmt);
    if (klass == nullptr) {
      continue;
    }
    Type* classType = root->lookupType(klass->getIdentifier());
    if (classType == nullptr || !classType->isEqual(baseType)) {
      continue;
    }
    for (FuncDecl* method : klass->getMethods()) {
      if (method == nullptr) {
        continue;
      }
      if (method->getName() == "new") {
        continue;
      }
      Value* methodValue = method->getResolvedSymbol();
      addCandidate(out, seen,
                   CompletionCandidate{
                       .label = method->getName(),
                       .kind = ::lsp::CompletionItemKind::Method,
                       .detail = symbolDetail(methodValue, root),
                   });
    }
    break;
  }
}

void appendScopeSymbols(SymbolTable* scope, SymbolTable* root, std::vector<CompletionCandidate>& out,
                        std::unordered_set<std::string>& seen) {
  for (SymbolTable* current = scope; current != nullptr; current = current->getParent()) {
    for (Value* value : current->getSymbols()) {
      if (value == nullptr) {
        continue;
      }
      addCandidate(out, seen,
                   CompletionCandidate{
                       .label = value->getName(),
                       .kind = candidateKindForValue(value),
                       .detail = symbolDetail(value, root),
                   });
    }
  }
}

void appendKeywords(std::vector<CompletionCandidate>& out, std::unordered_set<std::string>& seen) {
  static constexpr std::array<std::string_view, 25> keywords = {
      "and",      "as",   "break",  "class", "continue", "def", "defer", "else",   "enum",
      "export",   "extern", "for",  "from",  "if",       "import", "in", "is",     "let",
      "not",      "or",   "return", "super", "this",     "var", "while",
  };
  static constexpr std::array<std::string_view, 3> literals = {"false", "null", "true"};
  static constexpr std::array<std::string_view, 11> builtinTypes = {
      "bool", "float", "float32", "float64", "int", "int8",
      "int16", "int32", "int64", "str", "void",
  };
  for (std::string_view keyword : keywords) {
    addCandidate(out, seen, CompletionCandidate{.label = std::string(keyword),
                                                .kind = ::lsp::CompletionItemKind::Keyword,
                                                .detail = {}});
  }
  for (std::string_view literal : literals) {
    addCandidate(out, seen, CompletionCandidate{.label = std::string(literal),
                                                .kind = ::lsp::CompletionItemKind::Value,
                                                .detail = {}});
  }
  for (std::string_view builtinType : builtinTypes) {
    addCandidate(out, seen, CompletionCandidate{.label = std::string(builtinType),
                                                .kind = ::lsp::CompletionItemKind::Class,
                                                .detail = "built-in type"});
  }
}

auto toCompletionItems(const std::vector<CompletionCandidate>& candidates, const std::string& prefix)
    -> std::vector<::lsp::CompletionItem> {
  std::vector<::lsp::CompletionItem> items;
  for (const CompletionCandidate& candidate : candidates) {
    if (!startsWith(candidate.label, prefix)) {
      continue;
    }
    ::lsp::CompletionItem item;
    item.label = candidate.label;
    item.kind = ::lsp::Opt<::lsp::CompletionItemKindEnum>(candidate.kind);
    if (!candidate.detail.empty()) {
      item.detail = ::lsp::Opt<::lsp::String>(candidate.detail);
    }
    items.push_back(std::move(item));
  }
  std::sort(items.begin(), items.end(),
            [](const ::lsp::CompletionItem& lhs,
               const ::lsp::CompletionItem& rhs) -> bool {
              return lhs.label < rhs.label;
            });
  return items;
}

} // namespace

auto completionItems(const AnalysisResult& result, unsigned line, unsigned character)
    -> std::vector<::lsp::CompletionItem> {
  std::vector<::lsp::CompletionItem> items;
  if (result.sourceMgr == nullptr) {
    return items;
  }
  llvm::SourceMgr* srcMgr = result.sourceMgr.get();
  unsigned const bufferId = result.mainBufferId;
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return items;
  }

  llvm::StringRef text = buf->getBuffer();
  unsigned const offset = positionToOffset(text, line, character);
  CompletionContext ctx = extractCompletionContext(text, offset);

  AnalysisResult patchedResult;
  const AnalysisResult* activeResult = &result;
  if (ctx.isMember && (result.parser == nullptr || result.rootScope == nullptr)) {
    // `holder.` is syntactically incomplete, so the normal analysis may fail to parse.
    // Re-analyze with a temporary identifier after the dot so we can still resolve members.
    patchedResult = reanalyzeWithCompletionPlaceholder(result, text, offset);
    activeResult = &patchedResult;
  }

  if (activeResult->parser == nullptr || activeResult->rootScope == nullptr) {
    return items;
  }
  Compound* ast = activeResult->parser->getAst();
  if (ast == nullptr) {
    return items;
  }

  SymbolTable* activeScope = activeScopeForOffset(*activeResult, offset);
  SymbolTable* root = activeResult->rootScope.get();

  std::vector<CompletionCandidate> candidates;
  std::unordered_set<std::string> seen;

  if (ctx.isMember) {
    std::vector<std::string> parts = splitChain(ctx.memberChain);
    if (parts.size() == 1U) {
      appendModuleMembersForAlias(*activeResult, parts.front(), candidates, seen);
    }
    Type* baseType = resolveChainType(*activeResult, ctx.memberChain, activeScope, root);
    appendMembersForType(baseType, ast, root, candidates, seen);
    if (candidates.empty()) {
      appendScopeSymbols(activeScope != nullptr ? activeScope : root, root, candidates, seen);
    }
  } else {
    appendScopeSymbols(activeScope != nullptr ? activeScope : root, root, candidates, seen);
    appendKeywords(candidates, seen);
  }

  return toCompletionItems(candidates, ctx.prefix);
}

} // namespace lesma::lsp_srv
