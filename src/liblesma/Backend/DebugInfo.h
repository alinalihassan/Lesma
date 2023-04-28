#pragma once

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfo.h>


struct DebugInfo {
    /// LLVM debug info builder
    std::unique_ptr<llvm::DIBuilder> Builder;
    /// Current compilation unit
    llvm::DICompileUnit *Unit;

    DebugInfo()
        : Builder(), Unit(nullptr) {}

    llvm::DIFile *getFile(const std::string &path) const;
};
