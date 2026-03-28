#include "LspCompletion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SourceMgr.h"

#include "LspAnalysisGraph.h"
#include "LspSourceHelpers.h"
#include "LspTypeFormat.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"

namespace lesma::lsp_srv {
namespace {

/** Inserted after `.` so `holder.` re-parses as member access (see completionItems). */
constexpr std::string_view MEMBER_COMPLETION_PLACEHOLDER = "__cursor__";

using namespace lesma;
using lesma::lsp_srv::formatBufferArrayTypeName;
using lesma::lsp_srv::formatTypeName;

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

using InnermostFunc = InnermostFuncAtOffset<lesma::FuncDecl, lesma::Class>;

auto positionToOffset(llvm::StringRef ref, unsigned line, unsigned character) -> unsigned {
  return static_cast<unsigned>(bufferByteOffsetFromLspUtf8Position(ref, line, character));
}

[[nodiscard]] auto findInnermostFuncContaining(lesma::Compound* ast, unsigned targetOffset,
                                               llvm::SourceMgr* srcMgr, unsigned bufferId)
    -> InnermostFunc {
  return findInnermostFuncContainingAst<lesma::FuncDecl, lesma::Class>(ast, targetOffset, srcMgr,
                                                                       bufferId);
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
      oss << fields[i]->name << ": " << formatTypeName(fields[i]->type, root);
    }
    oss << ")";
    if (type->getReturnType() != nullptr && !type->getReturnType()->is(BaseType::TY_VOID)) {
      oss << " -> " << formatTypeName(type->getReturnType(), root);
    }
    return oss.str();
  }
  return formatTypeName(type, root);
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
  patchedText.insert(static_cast<size_t>(offset), MEMBER_COMPLETION_PLACEHOLDER);
  auto options = std::make_unique<Options>(Options{
      .sourceType = SourceType::STRING,
      .source = std::move(patchedText),
      .debug = Debug::NONE,
      .outputFilename = "output",
      .timer = false,
      .implicitFilePath = result.mainFilePath,
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

/** When the receiver is `self`, resolve the class instance type from the innermost enclosing
 * method. */
auto resolveSelfReceiverType(Compound* ast, unsigned offset, llvm::SourceMgr* srcMgr,
                             unsigned bufferId, SymbolTable* root) -> Type* {
  if (ast == nullptr || root == nullptr) {
    return nullptr;
  }
  InnermostFunc inner = findInnermostFuncContaining(ast, offset, srcMgr, bufferId);
  if (inner.enclosingClass == nullptr) {
    return nullptr;
  }
  Type* classTy = root->lookupType(inner.enclosingClass->getIdentifier());
  if (classTy == nullptr) {
    return nullptr;
  }
  return classTy;
}

void addCandidate(std::vector<CompletionCandidate>& out, std::unordered_set<std::string>& seen,
                  CompletionCandidate candidate);

auto findTraitDeclNamed(AnalysisResult& result, Compound* mainAst, const std::string& name)
    -> TraitDecl* {
  auto scanCompound = [&name](Compound* compound) -> TraitDecl* {
    if (compound == nullptr) {
      return nullptr;
    }
    for (Statement* stmt : compound->getChildren()) {
      if (auto* tr = dynamic_cast<TraitDecl*>(stmt)) {
        if (tr->getIdentifier() == name) {
          return tr;
        }
      }
    }
    return nullptr;
  };
  if (TraitDecl* tr = scanCompound(mainAst); tr != nullptr) {
    return tr;
  }
  for (const auto& entry : result.importedModules) {
    if (entry.second == nullptr || entry.second->parser == nullptr) {
      continue;
    }
    Compound* modAst = entry.second->parser->getAst();
    if (TraitDecl* tr = scanCompound(modAst); tr != nullptr) {
      return tr;
    }
  }
  return nullptr;
}

auto findClassDeclarationInCompound(Compound* compound, llvm::SMRange declarationSpan,
                                    llvm::SourceMgr* srcMgr, unsigned bufferId) -> Class*;

auto findClassDeclarationInStatement(Statement* stmt, llvm::SMRange declarationSpan,
                                     llvm::SourceMgr* srcMgr, unsigned bufferId) -> Class* {
  if (stmt == nullptr) {
    return nullptr;
  }
  if (auto* compound = dynamic_cast<Compound*>(stmt)) {
    return findClassDeclarationInCompound(compound, declarationSpan, srcMgr, bufferId);
  }
  if (auto* klass = dynamic_cast<Class*>(stmt)) {
    if (smRangesEqual(srcMgr, bufferId, klass->getNameSpan(), declarationSpan)) {
      return klass;
    }
    for (FuncDecl* method : klass->getMethods()) {
      if (Class* nested = findClassDeclarationInCompound(method->getBody(), declarationSpan, srcMgr,
                                                         bufferId)) {
        return nested;
      }
    }
    return nullptr;
  }
  if (auto* func = dynamic_cast<FuncDecl*>(stmt)) {
    return findClassDeclarationInCompound(func->getBody(), declarationSpan, srcMgr, bufferId);
  }
  if (auto* ifNode = dynamic_cast<If*>(stmt)) {
    for (Compound* block : ifNode->getBlocks()) {
      if (Class* nested =
              findClassDeclarationInCompound(block, declarationSpan, srcMgr, bufferId)) {
        return nested;
      }
    }
    return nullptr;
  }
  if (auto* whileNode = dynamic_cast<While*>(stmt)) {
    return findClassDeclarationInCompound(whileNode->getBlock(), declarationSpan, srcMgr, bufferId);
  }
  if (auto* defer = dynamic_cast<Defer*>(stmt)) {
    return findClassDeclarationInStatement(defer->getStatement(), declarationSpan, srcMgr,
                                           bufferId);
  }
  return nullptr;
}

auto findClassDeclarationInCompound(Compound* compound, llvm::SMRange declarationSpan,
                                    llvm::SourceMgr* srcMgr, unsigned bufferId) -> Class* {
  if (compound == nullptr) {
    return nullptr;
  }
  for (Statement* child : compound->getChildren()) {
    if (Class* klass = findClassDeclarationInStatement(child, declarationSpan, srcMgr, bufferId)) {
      return klass;
    }
  }
  return nullptr;
}

auto declarationIdentityForType(Type* classType, SymbolTable* root)
    -> std::optional<IndexedDeclarationIdentity> {
  if (classType == nullptr) {
    return std::nullopt;
  }
  if (classType->getDeclarationSpan().isValid() && !classType->getDeclarationFilePath().empty()) {
    return IndexedDeclarationIdentity{
        .filePath = classType->getDeclarationFilePath(),
        .span = classType->getDeclarationSpan(),
    };
  }
  if (root == nullptr) {
    return std::nullopt;
  }
  for (Value* symbol : root->getSymbols()) {
    if (symbol == nullptr || symbol->getCategory() != ValueCategory::TYPE_SYMBOL ||
        symbol->getType() == nullptr || !symbol->getType()->isEqual(classType) ||
        !symbol->getDeclarationSpan().isValid() || symbol->getDeclarationFilePath().empty()) {
      continue;
    }
    return IndexedDeclarationIdentity{
        .filePath = symbol->getDeclarationFilePath(),
        .span = symbol->getDeclarationSpan(),
    };
  }
  return std::nullopt;
}

/** For types like list<int> or Box<int>, returns the template name before '<' (list, Box). */
auto genericSpecializationBaseName(Type* classType) -> std::optional<std::string> {
  if (classType == nullptr || !classType->is(BaseType::TY_CLASS)) {
    return std::nullopt;
  }
  std::string const& dn = classType->getDisplayName();
  if (dn.empty()) {
    return std::nullopt;
  }
  size_t const angle = dn.find('<');
  if (angle == std::string::npos) {
    return std::nullopt;
  }
  return std::string(dn.substr(0U, angle));
}

auto findTopLevelClassByName(Compound* compound, std::string_view name) -> Class* {
  if (compound == nullptr) {
    return nullptr;
  }
  for (Statement* stmt : compound->getChildren()) {
    if (auto* klass = dynamic_cast<Class*>(stmt)) {
      if (klass->getIdentifier() == name) {
        return klass;
      }
    }
  }
  return nullptr;
}

auto findClassDeclarationForType(AnalysisResult& result, Type* classType, Compound* fallbackAst,
                                 SymbolTable* root) -> Class* {
  if (std::optional<IndexedDeclarationIdentity> declaration =
          declarationIdentityForType(classType, root)) {
    if (std::optional<AnalysisView> analysis =
            findAnalysisViewForPath(result, declaration->filePath)) {
      if (Class* klass = findClassDeclarationInCompound(analysis->ast, declaration->span,
                                                        analysis->sourceMgr, analysis->bufferId)) {
        return klass;
      }
      if (std::optional<std::string> baseName = genericSpecializationBaseName(classType)) {
        if (Class* klass = findTopLevelClassByName(analysis->ast, *baseName)) {
          return klass;
        }
      }
    }
  }
  if (fallbackAst == nullptr || root == nullptr) {
    return nullptr;
  }
  for (Statement* stmt : fallbackAst->getChildren()) {
    auto* klass = dynamic_cast<Class*>(stmt);
    if (klass == nullptr) {
      continue;
    }
    Type* fallbackType = root->lookupType(klass->getIdentifier());
    if (fallbackType != nullptr && fallbackType->isEqual(classType)) {
      return klass;
    }
    if (fallbackType != nullptr && fallbackType->is(BaseType::TY_CLASS) &&
        !klass->getGenericParams().empty()) {
      if (std::optional<std::string> baseName = genericSpecializationBaseName(classType);
          baseName.has_value() && *baseName == klass->getIdentifier()) {
        return klass;
      }
    }
  }
  return nullptr;
}

auto isHiddenClassMemberName(std::string_view name) -> bool { return name.starts_with("__"); }

void appendMethodsForClass(Class* klass, SymbolTable* root, std::vector<CompletionCandidate>& out,
                           std::unordered_set<std::string>& seen) {
  if (klass == nullptr) {
    return;
  }
  for (FuncDecl* method : klass->getMethods()) {
    if (method == nullptr || method->getName() == "new" ||
        isHiddenClassMemberName(method->getName())) {
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
}

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

void appendTraitRequirementMethods(AnalysisResult& result, Type* classType, Compound* mainAst,
                                   SymbolTable* root, std::vector<CompletionCandidate>& out,
                                   std::unordered_set<std::string>& seen) {
  if (classType == nullptr || root == nullptr) {
    return;
  }
  // Match appendMembersForType: receivers like `self` are ptr-to-class; impl lists live on the
  // class.
  if (classType->is(BaseType::TY_PTR) && classType->getElementType() != nullptr) {
    classType = classType->getElementType();
  }
  if (!classType->is(BaseType::TY_CLASS)) {
    return;
  }
  for (const std::string& traitName : classType->getImplTraitNames()) {
    TraitDecl* tr = findTraitDeclNamed(result, mainAst, traitName);
    if (tr == nullptr) {
      continue;
    }
    for (FuncDecl* req : tr->getRequirements()) {
      if (req == nullptr || req->getName() == "new" || isHiddenClassMemberName(req->getName())) {
        continue;
      }
      Value* methodValue = req->getResolvedSymbol();
      addCandidate(out, seen,
                   CompletionCandidate{
                       .label = req->getName(),
                       .kind = ::lsp::CompletionItemKind::Method,
                       .detail = symbolDetail(methodValue, root),
                   });
    }
  }
}

/** `__buffer<T>` (TY_ARRAY): same dot-call surface as `list<T>` in codegen (`callListMethodByName`). */
void appendBuiltinBufferListMethodCandidates(Type* bufferType, SymbolTable* root,
                                             std::vector<CompletionCandidate>& out,
                                             std::unordered_set<std::string>& seen) {
  Type* elementType = bufferType != nullptr ? bufferType->getElementType() : nullptr;
  std::string const elemStr =
      elementType != nullptr ? formatTypeName(elementType, root) : std::string{"?"};

  auto detailForName = [&](std::string_view name) -> std::string {
    if (name == "len") {
      return "len() -> int";
    }
    if (name == "clear") {
      return "clear() -> void";
    }
    if (name == "push") {
      return "push(value: " + elemStr + ") -> void";
    }
    if (name == "pop") {
      return "pop() -> " + elemStr;
    }
    if (name == "copy") {
      return "copy() -> " + formatBufferArrayTypeName(bufferType, root);
    }
    return std::string{name};
  };

  for (std::string_view name : OperatorUtils::BUILTIN_LIST_METHOD_NAMES) {
    if (isHiddenClassMemberName(name)) {
      continue;
    }
    addCandidate(out, seen,
                 CompletionCandidate{.label = std::string{name},
                                     .kind = ::lsp::CompletionItemKind::Method,
                                     .detail = detailForName(name)});
  }
}

void appendMembersForType(AnalysisResult& result, Type* baseType, Compound* ast, SymbolTable* root,
                          std::vector<CompletionCandidate>& out,
                          std::unordered_set<std::string>& seen) {
  if (baseType == nullptr) {
    return;
  }
  if (baseType->is(BaseType::TY_PTR) && baseType->getElementType() != nullptr) {
    baseType = baseType->getElementType();
  }
  if (baseType->is(BaseType::TY_ARRAY)) {
    appendBuiltinBufferListMethodCandidates(baseType, root, out, seen);
    return;
  }
  if (!baseType->is(BaseType::TY_CLASS) && !baseType->is(BaseType::TY_ENUM)) {
    return;
  }

  for (Field* field : baseType->getFields()) {
    if (field == nullptr || isHiddenClassMemberName(field->name)) {
      continue;
    }
    addCandidate(out, seen,
                 CompletionCandidate{.label = field->name,
                                     .kind = baseType->is(BaseType::TY_ENUM)
                                                 ? ::lsp::CompletionItemKind::EnumMember
                                                 : ::lsp::CompletionItemKind::Field,
                                     .detail = formatTypeName(field->type, root)});
  }

  if (!baseType->is(BaseType::TY_CLASS) || root == nullptr) {
    return;
  }
  // Walk inheritance chain so subclass completion includes superclass methods (deduped by `seen`).
  for (Type* ty = baseType; ty != nullptr; ty = ty->getClassSuperclass()) {
    if (Class* klass = findClassDeclarationForType(result, ty, ast, root)) {
      appendMethodsForClass(klass, root, out, seen);
    }
  }
}

void appendScopeSymbols(SymbolTable* scope, SymbolTable* root,
                        std::vector<CompletionCandidate>& out,
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
  static constexpr std::array<std::string_view, 26> keywords = {
      "and",    "as",     "break", "class",  "continue", "def",    "defer", "else",  "enum",
      "export", "extern", "for",   "from",   "if",       "import", "in",    "is",    "let",
      "not",    "or",     "pass",  "return", "super",    "this",   "var",   "while",
  };
  static constexpr std::array<std::string_view, 3> literals = {"false", "null", "true"};
  static constexpr std::array<std::string_view, 11> builtinTypes = {
      "bool",  "float", "float32", "float64", "int",  "int8",
      "int16", "int32", "int64",   "str",     "void",
  };
  for (std::string_view keyword : keywords) {
    addCandidate(out, seen,
                 CompletionCandidate{.label = std::string(keyword),
                                     .kind = ::lsp::CompletionItemKind::Keyword,
                                     .detail = {}});
  }
  for (std::string_view literal : literals) {
    addCandidate(out, seen,
                 CompletionCandidate{.label = std::string(literal),
                                     .kind = ::lsp::CompletionItemKind::Value,
                                     .detail = {}});
  }
  for (std::string_view builtinType : builtinTypes) {
    addCandidate(out, seen,
                 CompletionCandidate{.label = std::string(builtinType),
                                     .kind = ::lsp::CompletionItemKind::Class,
                                     .detail = "built-in type"});
  }
}

auto toCompletionItems(const std::vector<CompletionCandidate>& candidates,
                       const std::string& prefix) -> std::vector<::lsp::CompletionItem> {
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
            [](const ::lsp::CompletionItem& lhs, const ::lsp::CompletionItem& rhs) -> bool {
              return lhs.label < rhs.label;
            });
  return items;
}

} // namespace

auto completionItems(AnalysisResult& result, unsigned line, unsigned character)
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
  AnalysisResult* activeResult = &result;
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
    Type* baseType = resolveChainType(*activeResult, ctx.memberChain, activeScope, root);
    if (baseType == nullptr && parts.size() == 1U && parts.front() == "self") {
      baseType = resolveSelfReceiverType(ast, offset, srcMgr, bufferId, root);
    }
    if (parts.size() == 1U && activeResult->importAliasToPath.contains(parts.front())) {
      appendModuleMembersForAlias(*activeResult, parts.front(), candidates, seen);
    }
    appendMembersForType(*activeResult, baseType, ast, root, candidates, seen);
    appendTraitRequirementMethods(*activeResult, baseType, ast, root, candidates, seen);
  } else {
    appendScopeSymbols(activeScope != nullptr ? activeScope : root, root, candidates, seen);
    appendKeywords(candidates, seen);
  }

  return toCompletionItems(candidates, ctx.prefix);
}

} // namespace lesma::lsp_srv
