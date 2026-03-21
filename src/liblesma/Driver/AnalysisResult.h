#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "liblesma/Driver/Driver.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"

namespace lesma {
class Compound;

/** Single diagnostic (error/warning) with a source span. */
struct AnalysisDiagnostic {
  std::string message;
  llvm::SMRange span;
};

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
  std::optional<IndexedDeclarationIdentity> declaration;
  bool isTypePosition = false;
  bool isMemberAccess = false;
  unsigned modifiers = 0U;
  std::optional<IndexedTokenKind> fallbackTokenKind;
};

struct IndexedEnumMemberOccurrence {
  std::string enumName;
  std::string memberName;
  llvm::SMRange span;
};

struct AnalysisIndex {
  std::vector<IndexedSymbolOccurrence> symbolOccurrences;
  std::vector<IndexedEnumMemberOccurrence> enumMemberOccurrences;
};

using ImportAliasMap = std::unordered_map<std::string, std::string>;
using ImportedNameSourceMap =
    std::unordered_map<std::string, std::pair<std::string, std::string>>;

struct ImportedModuleAnalysis {
  std::shared_ptr<llvm::SourceMgr> sourceMgr;
  unsigned mainBufferId = 0;
  std::string mainFilePath;

  std::unique_ptr<Parser> parser;
  std::unique_ptr<SymbolTable> rootScope;
  std::vector<std::unique_ptr<Type>> typeCache;
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

  /** Non-empty when lex/parse/typecheck failed. First error only (fail-fast). */
  std::vector<AnalysisDiagnostic> diagnostics;

  /** Set when parse succeeded (and possibly typecheck). */
  std::unique_ptr<Parser> parser;
  std::unique_ptr<SymbolTable> rootScope;
  std::vector<std::unique_ptr<Type>> typeCache;
  AnalysisIndex index;
  ImportAliasMap importAliasToPath;
  ImportedNameSourceMap importedNameToSource;
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModules;

  [[nodiscard]] auto hasErrors() const -> bool { return !diagnostics.empty(); }
};

/** Run lexer, parser, and typechecker. Does not run codegen. */
auto analyze(std::unique_ptr<Options> options) -> AnalysisResult;
auto buildAnalysisIndex(const Compound* ast, llvm::SourceMgr* srcMgr, unsigned bufferId)
    -> AnalysisIndex;

} // namespace lesma
