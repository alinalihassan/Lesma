#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CLI/CLI.hpp"

#include "liblesma/Common/LesmaVersion.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Driver/Driver.h"

using namespace lesma;

namespace {

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
  bool timer = false;
  std::string output = "output";
  std::string file;

  CLI::App app{"Lesma programming language", "lesma"};
  app.set_version_flag("-v,--version", LESMA_VERSION, "Print the Lesma version");
  app.set_help_all_flag("-s,--subcommands", "Expand help to show subcommand flags and options");
  auto* debugOpt =
      app.add_option("-d,--debug", debug, "Debug options: lexer, ast, ir, all (default: all)")
          ->expected(0, -1)
          ->check(CLI::IsMember({"lexer", "ast", "ir", "all"}));
  app.add_flag("-t,--timer", timer, "Enable compiler timer");

  CLI::App* run = app.add_subcommand("run", "Run source code");
  CLI::App* compile = app.add_subcommand("compile", "Compile source code");
  app.require_subcommand();

  run->add_option("file", file, "Lesma source filename")->required();
  compile->add_option("file", file, "Lesma source filename")->required();
  compile->add_option("-o,--output", output, "Output filename");

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
  if (debugOpt->count() > 0 && debug.empty()) {
    debug.emplace_back("all");
  }

  return std::make_unique<CLIOptions>(
      CLIOptions{std::filesystem::absolute(file), output, debug, timer, run->parsed()});
}

} // namespace

auto main(int argc, char** argv) -> int {
  // CLI Parsing
  auto options = parseCli(argc, argv);
  auto debugFlags = parseDebugFlags(options->debug);
  auto driverOptions = std::make_unique<Options>(Options{
      SourceType::FILE,
      options->file,
      debugFlags,
      options->output,
      options->timer,
      "",
  });
  return options->jit ? Driver::run(std::move(driverOptions))
                      : Driver::compile(std::move(driverOptions));
}