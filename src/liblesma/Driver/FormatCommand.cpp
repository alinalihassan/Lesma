#include "liblesma/Driver/Driver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "liblesma/Common/Utils.h"
#include "liblesma/Formatter/SourceFormatter.h"

namespace fs = std::filesystem;

using namespace lesma;

namespace {

[[nodiscard]] auto normalizeNewlines(const std::string& input) -> std::string {
  std::string normalized;
  normalized.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\r') {
      if (i + 1U < input.size() && input[i + 1U] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(input[i]);
  }
  return normalized;
}

[[nodiscard]] auto isHiddenDirectory(const fs::path& path) -> bool {
  std::string const name = path.filename().string();
  return !name.empty() && name.starts_with(".");
}

[[nodiscard]] auto isLesmaFile(const fs::path& path) -> bool {
  return path.has_extension() && path.extension() == ".les";
}

auto collectFiles(const std::vector<fs::path>& inputPaths, std::vector<fs::path>& files) -> bool {
  bool ok = true;
  for (const fs::path& inputPath : inputPaths) {
    std::error_code errorCode;
    fs::path const path = fs::weakly_canonical(inputPath, errorCode);
    if (errorCode) {
      lesma::print(LogType::ERROR, "Path not found: {}\n", inputPath.string());
      ok = false;
      continue;
    }
    if (fs::is_regular_file(path)) {
      if (!isLesmaFile(path)) {
        lesma::print(LogType::ERROR, "Not a .les file: {}\n", path.string());
        ok = false;
        continue;
      }
      files.push_back(path);
      continue;
    }
    if (!fs::is_directory(path)) {
      lesma::print(LogType::ERROR, "Unsupported path: {}\n", path.string());
      ok = false;
      continue;
    }

    fs::recursive_directory_iterator iter(path, fs::directory_options::none, errorCode);
    fs::recursive_directory_iterator end;
    if (errorCode) {
      lesma::print(LogType::ERROR, "Could not read directory: {}\n", path.string());
      ok = false;
      continue;
    }
    for (; iter != end; iter.increment(errorCode)) {
      if (errorCode) {
        lesma::print(LogType::ERROR, "Could not walk directory: {}\n", path.string());
        ok = false;
        break;
      }
      fs::path const current = iter->path();
      if (iter->is_symlink()) {
        continue;
      }
      if (iter->is_directory() && isHiddenDirectory(current)) {
        iter.disable_recursion_pending();
        continue;
      }
      if (iter->is_regular_file() && isLesmaFile(current)) {
        files.push_back(fs::weakly_canonical(current));
      }
    }
  }
  return ok;
}

} // namespace

auto Driver::formatPaths(const std::vector<fs::path>& paths, int width) -> int {
  std::vector<fs::path> files;
  bool ok = collectFiles(paths, files);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  for (const fs::path& path : files) {
    auto formatted = formatFile(path, width);
    if (!formatted.has_value()) {
      FormattingError const& error = formatted.error();
      if (error.span.isValid() && error.sourceMgr != nullptr && error.bufferId != 0U) {
        showInline(error.sourceMgr.get(), error.bufferId, error.span, error.filePath, true, error.message);
      } else {
        lesma::print(LogType::ERROR, "{}\n", error.message);
      }
      ok = false;
      continue;
    }

    std::ifstream ifs(path, std::ios::binary);
    std::string current((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::string const normalizedCurrent = normalizeNewlines(current);
    std::string const normalizedFormatted = normalizeNewlines(*formatted);
    if (normalizedCurrent == normalizedFormatted) {
      continue;
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      lesma::print(LogType::ERROR, "Could not write file: {}\n", path.string());
      ok = false;
      continue;
    }
    ofs << *formatted;
    if (!ofs) {
      lesma::print(LogType::ERROR, "Could not write file: {}\n", path.string());
      ok = false;
      continue;
    }
    lesma::print("{}\n", path.string());
  }

  return ok ? 0 : 1;
}
