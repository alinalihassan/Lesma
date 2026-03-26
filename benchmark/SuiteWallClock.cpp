#include "SuiteWallClock.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CLI/CLI.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include "fmt/format.h"

namespace fs = std::filesystem;

namespace lesma {
namespace {

enum class ExpectExit : std::uint8_t { Zero, NonZero };

[[nodiscard]] auto roundDigits(double x, int digits) -> double {
  const double m = std::pow(10.0, static_cast<double>(digits));
  return std::round(x * m) / m;
}

[[nodiscard]] auto chartTestLabel(std::string_view filePath) -> std::string {
  auto const slash = filePath.find_last_of('/');
  std::string base =
      slash == std::string_view::npos ? std::string(filePath)
                                      : std::string(filePath.substr(slash + 1));
  if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".les") == 0) {
    base.resize(base.size() - 4);
  }
  return base;
}

void appendLesFiles(fs::path const& dir, std::vector<fs::path>& out) {
  if (!fs::exists(dir)) {
    return;
  }
  for (auto const& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) {
      continue;
    }
    if (e.path().extension() == ".les") {
      out.push_back(e.path());
    }
  }
  std::sort(out.begin(), out.end());
}

[[nodiscard]] auto buildVegaLiteSpec(std::string const& suite, llvm::json::Object const& aggregate,
                                     llvm::json::Array const& testRows) -> llvm::json::Object {
  llvm::json::Array values;
  for (llvm::json::Value const& el : testRows) {
    auto const* obj = el.getAsObject();
    if (obj == nullptr) {
      continue;
    }
    if (!obj->getNumber("seconds").has_value()) {
      continue;
    }
    llvm::json::Object copy;
    if (auto fp = obj->getString("file")) {
      copy["file"] = chartTestLabel(*fp);
    }
    copy["seconds"] = *obj->getNumber("seconds");
    if (auto c = obj->getInteger("exit_code")) {
      copy["exit_code"] = *c;
    }
    if (auto b = obj->getBoolean("unexpected")) {
      copy["unexpected"] = *b;
    }
    if (auto er = obj->getString("error")) {
      copy["error"] = er->str();
    }
    values.push_back(llvm::json::Value(std::move(copy)));
  }

  std::string meanPart = "n/a";
  if (auto m = aggregate.getNumber("mean_seconds_per_test")) {
    meanPart = fmt::format("{}s/test", *m);
  }
  double const total = aggregate.getNumber("total_wall_seconds").value_or(0.0);
  std::string const description =
      fmt::format("suite total: {}s, mean: {}", total, meanPart);
  std::string const title = suite == "success"
                                ? "Lesma run() wall time per success test"
                                : fmt::format("Lesma run() wall time ({} suite)", suite);

  llvm::json::Object encX;
  encX["field"] = "seconds";
  encX["type"] = "quantitative";
  encX["title"] = "Wall time (s)";
  llvm::json::Object encY;
  encY["field"] = "file";
  encY["type"] = "nominal";
  encY["sort"] = "-x";
  encY["title"] = nullptr;
  llvm::json::Object tipA;
  tipA["field"] = "file";
  tipA["type"] = "nominal";
  tipA["title"] = "test";
  llvm::json::Object tipB;
  tipB["field"] = "seconds";
  tipB["type"] = "quantitative";
  tipB["title"] = "seconds";
  tipB["format"] = ".4f";
  llvm::json::Array tooltips;
  tooltips.push_back(llvm::json::Value(std::move(tipA)));
  tooltips.push_back(llvm::json::Value(std::move(tipB)));
  llvm::json::Object encoding;
  encoding["x"] = llvm::json::Value(std::move(encX));
  encoding["y"] = llvm::json::Value(std::move(encY));
  encoding["tooltip"] = llvm::json::Value(std::move(tooltips));
  llvm::json::Object mark;
  mark["type"] = "bar";
  mark["tooltip"] = true;
  llvm::json::Object data;
  data["values"] = llvm::json::Value(std::move(values));
  llvm::json::Object height;
  height["step"] = 12;

  llvm::json::Object spec;
  spec["$schema"] = "https://vega.github.io/schema/vega-lite/v5.json";
  spec["title"] = title;
  spec["description"] = description;
  spec["data"] = llvm::json::Value(std::move(data));
  spec["mark"] = llvm::json::Value(std::move(mark));
  spec["encoding"] = llvm::json::Value(std::move(encoding));
  spec["width"] = 640;
  spec["height"] = llvm::json::Value(std::move(height));
  return spec;
}

[[nodiscard]] auto runLesmaOnTest(fs::path const& lesmaExe, fs::path const& repoRoot,
                                  fs::path const& testFile, double timeoutSec, double& outSeconds,
                                  int& outExitCode, std::string& outError) -> bool {
  outError.clear();
  std::error_code ec;
  fs::path const prevCwd = fs::current_path(ec);
  if (ec) {
    outError = "getcwd_failed";
    return false;
  }
  fs::current_path(repoRoot, ec);
  if (ec) {
    outError = "chdir_failed";
    return false;
  }
  std::error_code relEc;
  auto const rel = fs::relative(testFile, repoRoot, relEc);
  std::string const testArg = relEc ? testFile.lexically_normal().generic_string()
                                    : rel.generic_string();
  std::string const exeStr = lesmaExe.generic_string();

  std::vector<std::string> storage;
  storage.push_back(exeStr);
  storage.emplace_back("run");
  storage.push_back(testArg);

  llvm::SmallVector<llvm::StringRef, 4> args;
  for (auto& s : storage) {
    args.push_back(s);
  }

  std::array<std::optional<llvm::StringRef>, 3> const redirects = {
      std::nullopt,
      std::optional<llvm::StringRef>(llvm::StringRef("")),
      std::optional<llvm::StringRef>(llvm::StringRef("")),
  };

  auto const t0 = std::chrono::steady_clock::now();
  unsigned const wait =
      timeoutSec > 0.0 ? static_cast<unsigned>(std::ceil(timeoutSec)) : 0U;
  std::string errMsg;
  int const ret = llvm::sys::ExecuteAndWait(
      exeStr, args, std::nullopt, llvm::ArrayRef<std::optional<llvm::StringRef>>(redirects), wait,
      0, &errMsg, nullptr, nullptr, nullptr);
  auto const t1 = std::chrono::steady_clock::now();
  outSeconds = std::chrono::duration<double>(t1 - t0).count();

  std::error_code ecRestore;
  fs::current_path(prevCwd, ecRestore);
  (void)ecRestore;

  if (ret == -1) {
    outError = errMsg.empty() ? "spawn_failed" : errMsg;
    return false;
  }
  if (ret == -2) {
    outError = "timeout";
    return false;
  }
  outExitCode = ret;
  return true;
}

[[nodiscard]] auto argvStrings(int argc, char** argv) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (argc <= 0 || argv == nullptr) {
    return out;
  }
  out.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    // argv is OS-supplied; copy to owned strings for CLI11.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char* p = argv[i];
    out.emplace_back(p != nullptr ? p : "");
  }
  return out;
}

} // namespace

auto runSuiteWallClock(int argc, char** argv) -> int {
  CLI::App app{"Lesma benchmark: integration wall-clock suite"};
  std::string lesmaStr;
  std::string repoRootStr;
  std::string suite = "success";
  double timeoutSec = 120.0;
  std::string jsonOut;
  std::string ghaOut;
  bool quiet = false;

  app.add_option("lesma", lesmaStr)->required()->description("Path to lesma executable");
  app.add_option("--repo-root", repoRootStr)->description("Repository root (default: cwd)");
  app.add_option("--suite", suite)->check(CLI::IsMember({"success", "failure", "both"}));
  app.add_option("--timeout", timeoutSec);
  app.add_option("--json-out", jsonOut)->description("Write full results JSON (includes vegaLite)");
  app.add_option("--gha-benchmark-json", ghaOut)
      ->description("Write github-action-benchmark JSON");
  app.add_flag_callback("--quiet", [&quiet]() -> void { quiet = true; },
                        "Suppress per-test stderr lines");

  std::vector<std::string> const argvOwned = argvStrings(argc, argv);
  std::vector<std::string> ownedArgv;
  if (!argvOwned.empty()) {
    ownedArgv.push_back(argvOwned.front());
  } else {
    ownedArgv.emplace_back("");
  }
  for (std::size_t i = 2; i < argvOwned.size(); ++i) {
    ownedArgv.push_back(argvOwned[i]);
  }
  std::vector<char*> parseArgv;
  parseArgv.reserve(ownedArgv.size());
  for (auto& s : ownedArgv) {
    parseArgv.push_back(s.data());
  }

  try {
    app.parse(static_cast<int>(parseArgv.size()), parseArgv.data());
  } catch (CLI::ParseError const& e) {
    return app.exit(e);
  }

  fs::path repoRoot =
      repoRootStr.empty() ? fs::current_path() : fs::absolute(repoRootStr);
  fs::path lesmaPath = fs::absolute(lesmaStr);
  if (!fs::is_regular_file(lesmaPath)) {
    fmt::print(stderr, "error: lesma binary not found: {}\n", lesmaPath.string());
    return 2;
  }

  std::vector<std::pair<fs::path, ExpectExit>> tests;
  if (suite == "success" || suite == "both") {
    std::vector<fs::path> paths;
    appendLesFiles(repoRoot / "tests" / "lesma" / "success", paths);
    for (auto const& p : paths) {
      tests.emplace_back(p, ExpectExit::Zero);
    }
  }
  if (suite == "failure" || suite == "both") {
    std::vector<fs::path> paths;
    appendLesFiles(repoRoot / "tests" / "lesma" / "failure", paths);
    for (auto const& p : paths) {
      tests.emplace_back(p, ExpectExit::NonZero);
    }
  }

  llvm::json::Array testRows;
  int failures = 0;
  double totalWall = 0.0;

  for (auto const& [path, expect] : tests) {
    std::error_code ec;
    auto const rel = fs::relative(path, repoRoot, ec);
    std::string const relStr = ec ? path.generic_string() : rel.generic_string();

    double elapsed = 0.0;
    int code = 0;
    std::string err;
    bool const ran =
        runLesmaOnTest(lesmaPath, repoRoot, path, timeoutSec, elapsed, code, err);

    if (!ran) {
      if (err == "timeout") {
        ++failures;
        if (!quiet) {
          fmt::print(stderr, "TIMEOUT {}\n", relStr);
        }
        llvm::json::Object row;
        row["file"] = relStr;
        row["seconds"] = nullptr;
        row["exit_code"] = nullptr;
        row["error"] = "timeout";
        testRows.push_back(llvm::json::Value(std::move(row)));
      } else {
        ++failures;
        if (!quiet) {
          fmt::print(stderr, "ERROR {} ({})\n", relStr, err);
        }
        llvm::json::Object row;
        row["file"] = relStr;
        row["seconds"] = nullptr;
        row["exit_code"] = nullptr;
        row["error"] = err;
        testRows.push_back(llvm::json::Value(std::move(row)));
      }
      continue;
    }

    totalWall += elapsed;
    bool const okExit = expect == ExpectExit::Zero ? code == 0 : code != 0;
    bool const bad = !okExit;
    if (bad) {
      ++failures;
    }
    llvm::json::Object row;
    row["file"] = relStr;
    row["seconds"] = roundDigits(elapsed, 6);
    row["exit_code"] = code;
    row["unexpected"] = bad;
    testRows.push_back(llvm::json::Value(std::move(row)));
    if (bad && !quiet) {
      char const* want = expect == ExpectExit::Zero ? "0" : "non-zero";
      fmt::print(stderr, "UNEXPECTED exit {} (want {}) {}\n", code, want, relStr);
    }
  }

  std::vector<double> okTimes;
  for (llvm::json::Value const& el : testRows) {
    auto const* o = el.getAsObject();
    if (o == nullptr) {
      continue;
    }
    if (o->getBoolean("unexpected").value_or(false)) {
      continue;
    }
    if (std::optional<double> s = o->getNumber("seconds")) {
      okTimes.push_back(*s);
    }
  }

  double meanOk = 0.0;
  if (!okTimes.empty()) {
    double sum = 0.0;
    for (double t : okTimes) {
      sum += t;
    }
    meanOk = sum / static_cast<double>(okTimes.size());
  }

  llvm::json::Object aggregate;
  aggregate["count"] = static_cast<int64_t>(tests.size());
  aggregate["ok_count"] = static_cast<int64_t>(okTimes.size());
  aggregate["failures"] = failures;
  aggregate["total_wall_seconds"] = roundDigits(totalWall, 4);
  if (okTimes.empty()) {
    aggregate["mean_seconds_per_test"] = nullptr;
  } else {
    aggregate["mean_seconds_per_test"] = roundDigits(meanOk, 6);
  }

  double const totalPrint = roundDigits(totalWall, 4);
  std::optional<double> const meanPrint =
      okTimes.empty() ? std::nullopt : std::optional<double>(roundDigits(meanOk, 6));

  if (!jsonOut.empty()) {
    llvm::json::Object vega =
        buildVegaLiteSpec(suite, aggregate, testRows);
    llvm::json::Object payload;
    payload["lesma"] = lesmaPath.generic_string();
    payload["repo_root"] = repoRoot.generic_string();
    payload["suite"] = suite;
    payload["aggregate"] = llvm::json::Value(std::move(aggregate));
    payload["tests"] = llvm::json::Value(std::move(testRows));
    payload["vegaLite"] = llvm::json::Value(std::move(vega));

    llvm::json::Value root(std::move(payload));
    std::string buffer;
    llvm::raw_string_ostream os(buffer);
    os << llvm::formatv("{0:2}", root);
    os.flush();

    fs::path const outPath = fs::absolute(jsonOut);
    fs::create_directories(outPath.parent_path());
    std::ofstream f(outPath);
    if (!f) {
      fmt::print(stderr, "error: could not write {}\n", outPath.string());
      return 2;
    }
    f << buffer;
    if (!f) {
      fmt::print(stderr, "error: write failed {}\n", outPath.string());
      return 2;
    }
  }

  if (!ghaOut.empty()) {
    llvm::json::Object bench;
    bench["tool"] = "customSmallerIsBetter";
    bench["output_file"] = fs::path(ghaOut).filename().string();
    llvm::json::Array benches;
    llvm::json::Object b1;
    b1["name"] = fmt::format("lesma_suite_{}_wall_total_s", suite);
    b1["unit"] = "second";
    b1["value"] = roundDigits(totalWall, 4);
    llvm::json::Object b2;
    b2["name"] = fmt::format("lesma_suite_{}_mean_run_s", suite);
    b2["unit"] = "second";
    b2["value"] = okTimes.empty() ? 0.0 : roundDigits(meanOk, 6);
    benches.push_back(llvm::json::Value(std::move(b1)));
    benches.push_back(llvm::json::Value(std::move(b2)));
    bench["benches"] = llvm::json::Value(std::move(benches));
    llvm::json::Value ghRoot(std::move(bench));
    std::string ghBuf;
    llvm::raw_string_ostream ghOs(ghBuf);
    ghOs << llvm::formatv("{0:2}", ghRoot);
    ghOs.flush();
    fs::path const ghPath = fs::absolute(ghaOut);
    fs::create_directories(ghPath.parent_path());
    std::ofstream gf(ghPath);
    if (!gf) {
      fmt::print(stderr, "error: could not write {}\n", ghPath.string());
      return 2;
    }
    gf << ghBuf;
  }

  fmt::print("Lesma benchmark ({}): {}/{} ok, total wall {} s, mean ", suite,
             static_cast<int>(okTimes.size()), static_cast<int>(tests.size()), totalPrint);
  if (!meanPrint.has_value()) {
    fmt::print("{}\n", "None");
  } else {
    fmt::print("{} s/test\n", *meanPrint);
  }
  if (failures != 0 && !quiet) {
    fmt::print(stderr, "({} test(s) failed or timed out)\n", failures);
  }

  return failures != 0 ? 1 : 0;
}

} // namespace lesma
