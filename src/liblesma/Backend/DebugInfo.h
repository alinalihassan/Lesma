#pragma once

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DIBuilder.h>

#include <map>


namespace lesma {
    class DebugInfo {
    private:
        std::map<std::string, llvm::DIType*> types;

        void insertType(llvm::DIType *type);
        llvm::DIType *createNewType(llvm::Type *type);
    public:
        llvm::DIBuilder *Builder;
        llvm::DICompileUnit *CU;

        DebugInfo(llvm::Module &module, const std::string &path);

        llvm::DIType *getType(llvm::Type *type);
        llvm::DISubroutineType *getType(llvm::FunctionType *FT);
    };
}