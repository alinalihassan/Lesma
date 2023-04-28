#include "DebugInfo.h"

llvm::DIFile *DebugInfo::getFile(const std::string &path) const {
    std::string filename;
    std::string directory;
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        filename = path.substr(pos + 1);
        directory = path.substr(0, pos);
    } else {
        filename = path;
        directory = ".";
    }
    return Builder->createFile(filename, directory);
}