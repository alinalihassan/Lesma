#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

namespace lesma {

enum class AnalysisDiagnosticSeverity : std::uint8_t { Error, Warning };

/** Single diagnostic (error/warning) with a source span. */
struct AnalysisDiagnostic {
  std::string message;
  llvm::SMRange span;
  AnalysisDiagnosticSeverity severity = AnalysisDiagnosticSeverity::Error;
  /** When non-null, \p span is in this manager at \p spanBufferId; otherwise use
   *  \c AnalysisResult::sourceMgr and \c AnalysisResult::mainBufferId. */
  std::shared_ptr<llvm::SourceMgr> spanSourceMgr;
  unsigned spanBufferId = 0;
  /** Absolute or logical path for display and LSP routing; if empty with null \p spanSourceMgr,
   *  use \c AnalysisResult::mainFilePath. */
  std::string spanDisplayPath;
};

} // namespace lesma
