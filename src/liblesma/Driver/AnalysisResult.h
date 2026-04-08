#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "liblesma/Driver/AnalysisDiagnostic.h"
#include "liblesma/Driver/Driver.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"

namespace lesma {
class Compound;
class Timer;

struct IndexedDeclarationIdentity {
  std::string filePath;
  llvm::SMRange span;
};

enum class IndexedTokenKind : std::uint8_t {
  Namespace = 0,
  Class = 1,
  Enum = 2,
  EnumMember = 3,
  Type = 4,
  TypeParameter = 5,
  Function = 6,
  Method = 7,
  Parameter = 8,
  Variable = 9,
  Property = 10,
};

namespace analysis_index_modifier {
constexpr unsigned DECLARATION = 1U << 0U;
}

struct IndexedSymbolOccurrence {
  std::string name;
  std::optional<std::string> dotBase;
  llvm::SMRange span;
  /** Non-owning; if set, hover uses this type instead of the resolved symbol's declared type. */
  Type* flowSensitiveType = nullptr;
  /** Non-owning resolved/declared type for this occurrence (used by LSP when no symbol lookup fits). */
  Type* resolvedType = nullptr;
  /** When set, semantic tokens use this range instead of \c span (e.g. operator glyphs only). */
  std::optional<llvm::SMRange> semanticHighlightSpan;
  std::optional<IndexedDeclarationIdentity> declaration;
  bool isTypePosition = false;
  bool isMemberAccess = false;
  unsigned modifiers = 0U;
  std::optional<IndexedTokenKind> fallbackTokenKind;
};

struct AnalysisIndex {
  std::vector<IndexedSymbolOccurrence> symbolOccurrences;
};

using ImportAliasMap = std::unordered_map<std::string, std::string>;
using ImportedNameSourceMap = std::unordered_map<std::string, std::pair<std::string, std::string>>;

struct ImportedModuleAnalysis {
  std::shared_ptr<llvm::SourceMgr> sourceMgr;
  unsigned mainBufferId = 0;
  std::string mainFilePath;

  std::unique_ptr<Parser> parser;
  /** Types must outlive \c rootScope: symbols hold raw \c Type* into this cache. */
  std::vector<std::unique_ptr<Type>> typeCache;
  std::unique_ptr<SymbolTable> rootScope;
  AnalysisIndex index;

  ImportAliasMap importAliasToPath;
  ImportedNameSourceMap importedNameToSource;
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModules;
};

/**
 * Result of running the compiler pipeline up to (and including) typecheck,
 * without codegen. Used by the LSP and by Driver for the full compile path.
 */
struct AnalysisResult {
  std::shared_ptr<llvm::SourceMgr> sourceMgr;
  unsigned mainBufferId = 0;
  std::string mainFilePath;

  /** Errors stop analysis; warnings may appear on success. */
  std::vector<AnalysisDiagnostic> diagnostics;
  /** Copied from Options for Driver printing (warnings are still stored in \c diagnostics). */
  bool suppressWarnings = false;

  /** Set when parse succeeded (and possibly typecheck). */
  std::unique_ptr<Parser> parser;
  /** Types must outlive \c rootScope: symbols hold raw \c Type* into this cache. */
  std::vector<std::unique_ptr<Type>> typeCache;
  std::unique_ptr<SymbolTable> rootScope;
  /** Generic bindings for specialized classes (e.g. list<int>); keys align with \p typeCache. */
  std::unordered_map<Type*, std::unordered_map<std::string, Type*>> specializedTypeEnv;
  /** Specialized class → template class type (same keys as specialized-type entries in
   * \p specializedTypeEnv). */
  std::unordered_map<Type*, Type*> specializedTypeToTemplate;
  /** Stable registry key -> canonical specialized class type from typecheck. */
  std::unordered_map<std::string, Type*> specializedClassTypes;
  AnalysisIndex index;
  ImportAliasMap importAliasToPath;
  ImportedNameSourceMap importedNameToSource;
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModules;

  [[nodiscard]] auto hasErrors() const -> bool {
    for (const auto& d : diagnostics) {
      if (d.severity == AnalysisDiagnosticSeverity::Error) {
        return true;
      }
    }
    return false;
  }
};

/** Run lexer, parser, and typechecker. Does not run codegen.
 *  When \p phaseTimer is non-null, the driver passes an enabled Timer so phases are recorded
 *  (Reading source, Lexing, Parsing, Typecheck). */
auto analyze(std::unique_ptr<Options> options, Timer* phaseTimer = nullptr) -> AnalysisResult;
auto buildAnalysisIndex(const Compound* ast, llvm::SourceMgr* srcMgr, unsigned bufferId,
                        const std::string& mainFilePath)
    -> AnalysisIndex;

[[nodiscard]] auto indexedTokenKindFromResolvedSymbol(const Value* resolvedSymbol,
                                                      bool isTypePosition, bool isMemberAccess,
                                                      IndexedTokenKind fallbackKind) -> IndexedTokenKind;

} // namespace lesma
