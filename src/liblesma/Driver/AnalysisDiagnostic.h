#pragma once

#include <cstdint>
#include <string>

#include <llvm/Support/SMLoc.h>

namespace lesma {

enum class AnalysisDiagnosticSeverity : std::uint8_t { Error, Warning };

/** Single diagnostic (error/warning) with a source span. */
struct AnalysisDiagnostic {
  std::string message;
  llvm::SMRange span;
  AnalysisDiagnosticSeverity severity = AnalysisDiagnosticSeverity::Error;
};

} // namespace lesma
