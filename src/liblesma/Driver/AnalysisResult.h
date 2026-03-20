#pragma once

#include <memory>
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

/** Single diagnostic (error/warning) with a source span. */
struct AnalysisDiagnostic {
  std::string message;
  llvm::SMRange span;
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
  ImportAliasMap importAliasToPath;
  ImportedNameSourceMap importedNameToSource;
  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> importedModules;

  [[nodiscard]] auto hasErrors() const -> bool { return !diagnostics.empty(); }
};

/** Run lexer, parser, and typechecker. Does not run codegen. */
auto analyze(std::unique_ptr<Options> options) -> AnalysisResult;

} // namespace lesma
