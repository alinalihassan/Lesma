#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <llvm/Passes/OptimizationLevel.h>

namespace lesma {
enum class SourceType : std::uint8_t {
  FILE,
  STRING,
};

/** Final executable link mode for \c lesma compile (in-process LLD). */
enum class LinkMode : std::uint8_t {
  /** Platform default (typically dynamic libc on macOS/ELF; see AGENTS.md). */
  Default,
  /** Static where supported (ELF: \c LESMA_MUSL_RUNTIME or \c -static; macOS still dynamic). */
  Static,
  /** Prefer dynamic linking on ELF (no \c -static / musl bundle). */
  Dynamic,
};

enum Debug : std::uint8_t {
  NONE = 0x00,  // 00000000
  LEXER = 0x01, // 00000001
  AST = 0x02,   // 00000010
  IR = 0x04,    // 00000100
  ALL = LEXER | AST | IR,
};

// Bitwise operators for Debug flags
inline auto operator|(Debug a, Debug b) -> Debug {
  return static_cast<Debug>(static_cast<int>(a) | static_cast<int>(b));
}

inline auto operator&(Debug a, Debug b) -> Debug {
  return static_cast<Debug>(static_cast<int>(a) & static_cast<int>(b));
}

inline auto operator|=(Debug& a, Debug b) -> Debug& { return a = a | b; }

struct Options {
  SourceType sourceType;
  std::string source;
  Debug debug = Debug::NONE;
  std::string outputFilename = "output";
  bool timer = false;
  /** When sourceType is STRING, used as the logical file path (imports, diagnostics). LSP sets
   *  this to the open document's filesystem path. */
  std::string implicitFilePath;
  /** IR optimization for codegen (compile and JIT run). Defaults to O3. */
  llvm::OptimizationLevel optimizationLevel = llvm::OptimizationLevel::O3;
  /** Emit DWARF debug info in object files (compile subcommand). */
  bool emitDebugInfo = false;
  /** When true, Driver does not print warnings to stderr (diagnostics still collected). */
  bool suppressWarnings = false;
  /** Used when emitting a native executable (non-JIT). */
  LinkMode linkMode = LinkMode::Default;
};

class Driver {
private:
  static auto baseCompile(std::unique_ptr<lesma::Options> options, bool jit) -> int;

public:
  static auto run(std::unique_ptr<lesma::Options> options) -> int;
  static auto compile(std::unique_ptr<lesma::Options> options) -> int;
};
} // namespace lesma
