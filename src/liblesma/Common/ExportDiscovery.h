#pragma once

#include <string>
#include <vector>

namespace lesma {

/** Parse a file and return exported top-level names (for `import *`). */
auto getExportedTopLevelNamesFromFile(const std::string& filepath, bool isStd,
                                      const std::string& mainFilePath) -> std::vector<std::string>;

} // namespace lesma
