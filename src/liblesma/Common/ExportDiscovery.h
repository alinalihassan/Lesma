#pragma once

#include <string>
#include <vector>

namespace lesma {

/** Result of scanning a module for `export` top-level names (`import *`). */
struct ExportDiscoveryResult {
  std::vector<std::string> names;
  /** Non-empty if the file could not be read or parsed; \p names may be empty. */
  std::string failureMessage;
};

/** Parse a file and return exported top-level names, or a failure message. */
auto discoverExportedTopLevelNames(const std::string& filepath, bool isStd,
                                     const std::string& mainFilePath) -> ExportDiscoveryResult;

/** Returns \p names only; on failure returns an empty vector (legacy / codegen helper). */
auto getExportedTopLevelNamesFromFile(const std::string& filepath, bool isStd,
                                      const std::string& mainFilePath) -> std::vector<std::string>;

} // namespace lesma
