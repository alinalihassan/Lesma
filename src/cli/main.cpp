#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Passes/OptimizationLevel.h>

#include "CLI/CLI.hpp"

#include "liblesma/Common/LesmaVersion.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/Driver.h"

using namespace lesma;

namespace {

[[nodiscard]] auto optimizationLevelFromCli(int level) -> llvm::OptimizationLevel {
  switch (level) {
  case 0:
    return llvm::OptimizationLevel::O0;
  case 1:
    return llvm::OptimizationLevel::O1;
  case 2:
    return llvm::OptimizationLevel::O2;
  default:
    return llvm::OptimizationLevel::O3;
  }
}

auto parseDebugFlags(const std::vector<std::string>& debugOptions) -> Debug {
  Debug flags = Debug::NONE;
  for (const auto& opt : debugOptions) {
    if (opt == "lexer") {
      flags |= Debug::LEXER;
    } else if (opt == "ast") {
      flags |= Debug::AST;
    } else if (opt == "ir") {
      flags |= Debug::IR;
    } else if (opt == "all") {
      flags = Debug::ALL;
    }
  }
  return flags;
}

auto parseCli(int argc, char** argv) -> std::unique_ptr<CLIOptions> {
  std::vector<std::string> debug;
  bool runTimer = false;
  bool compileTimer = false;
  std::string output = "output";
  std::string file;
  int optimizationLevel = 3;
  bool emitDebugInfo = false;

  CLI::App app{"Lesma programming language", "lesma"};
  app.set_version_flag("-v,--version", LESMA_VERSION, "Print the Lesma version");
  app.set_help_all_flag("-s,--subcommands", "Expand help to show subcommand flags and options");

  CLI::App* run = app.add_subcommand("run", "Run source code");
  CLI::App* compile = app.add_subcommand("compile", "Compile source code");
  app.require_subcommand();

  auto addDebugOption = [&](CLI::App* sub) -> CLI::Option* {
    return sub->add_option("-d,--debug", debug, "Debug options: lexer, ast, ir, all (default: all)")
        ->expected(0, -1)
        ->check(CLI::IsMember({"lexer", "ast", "ir", "all"}));
  };
  run->add_option("file", file, "Lesma source filename")->required();
  compile->add_option("file", file, "Lesma source filename")->required();
  compile->add_option("-o,--output", output, "Output filename");
  run->add_option("-O,--opt", optimizationLevel, "Optimization level (0–3)")
      ->check(CLI::Range(0, 3));
  compile->add_option("-O,--opt", optimizationLevel, "Optimization level (0–3)")
      ->check(CLI::Range(0, 3));
  auto addDebugInfoFlag = [&](CLI::App* sub) -> void {
    sub->add_flag_callback(
        "-g,--debug-info",
        [&emitDebugInfo]() -> void { emitDebugInfo = true; },
        "Emit LLVM debug metadata (use with -d ir to print; compile also emits DWARF, best with -O0)");
  };
  addDebugInfoFlag(compile);
  addDebugInfoFlag(run);
  CLI::Option* const runDebugOpt = addDebugOption(run);
  // add_flag(bool&) uses lexical_cast on flag values; it fails with "--timer = true" on CLI11 2.6.
  run->add_flag_callback(
      "-t,--timer", [&runTimer]() -> void { runTimer = true; }, "Enable compiler timer");
  CLI::Option* const compileDebugOpt = addDebugOption(compile);
  compile->add_flag_callback(
      "-t,--timer", [&compileTimer]() -> void { compileTimer = true; }, "Enable compiler timer");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    if ((app.get_subcommands().empty() && argc == 1) ||
        (!app.get_subcommands().empty() && argc == 2)) {
      lesma::print(app.help());
      std::exit(0);
    } else {
      std::exit(app.exit(e));
    }
  }

  // Default to "all" when -d is specified without arguments
  if ((runDebugOpt->count() > 0 || compileDebugOpt->count() > 0) && debug.empty()) {
    debug.emplace_back("all");
  }

  bool const timer = runTimer || compileTimer;
  return std::make_unique<CLIOptions>(CLIOptions{.file = std::filesystem::absolute(file),
                                                 .output = output,
                                                 .debug = debug,
                                                 .timer = timer,
                                                 .jit = run->parsed(),
                                                 .optimizationLevel = optimizationLevel,
                                                 .emitDebugInfo = emitDebugInfo});
}

} // namespace

auto main(int argc, char** argv) -> int {
  // CLI Parsing
  auto options = parseCli(argc, argv);
  auto debugFlags = parseDebugFlags(options->debug);
  auto driverOptions = std::make_unique<Options>(Options{
      .sourceType = SourceType::FILE,
      .source = options->file,
      .debug = debugFlags,
      .outputFilename = options->output,
      .timer = options->timer,
      .implicitFilePath = "",
      .optimizationLevel = optimizationLevelFromCli(options->optimizationLevel),
      .emitDebugInfo = options->emitDebugInfo,
  });
  int const exitCode = options->jit ? Driver::run(std::move(driverOptions))
                                    : Driver::compile(std::move(driverOptions));
  // Driver calls llvm::llvm_shutdown() before returning. A normal return from main would still
  // run libc/loader finalization for libLLVM.so; on Linux that can double-free after llvm_shutdown.
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exitCode);
}