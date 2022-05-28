#include "DebugInfo.h"

using namespace lesma;

DebugInfo::DebugInfo(llvm::Module &module, const std::string& path) {
    Builder = new llvm::DIBuilder(module);
    auto filename = path.substr(path.find_last_of("/\\") + 1);
    auto directory = path.substr(0, path.find_last_of("/\\"));
    CU = Builder->createCompileUnit(llvm::dwarf::DW_LANG_C, Builder->createFile(filename, directory),
                                    "Lesma Compiler", true, "", 0);

    insertType(Builder->createBasicType("void", 0, llvm::dwarf::DW_ATE_address));
    insertType(Builder->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean));
    insertType(Builder->createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char));
    insertType(Builder->createBasicType("int", 64, llvm::dwarf::DW_ATE_signed));
    insertType(Builder->createBasicType("float", 64, llvm::dwarf::DW_ATE_float));
}

std::string getTypeName(llvm::Type *type) {
    std::string type_str;
    llvm::raw_string_ostream rso(type_str);
    type->print(rso);

    return type_str;
}

void DebugInfo::insertType(llvm::DIType *type) {
    types.insert(std::pair<std::string, llvm::DIType*>(type->getName(), type));
}

llvm::DIType *DebugInfo::createNewType(llvm::Type *type) {
    if (type->isPointerTy())
        return Builder->createPointerType(getType(type->getPointerElementType()), type->getScalarSizeInBits());

    return Builder->createNullPtrType();
}

llvm::DIType *DebugInfo::getType(llvm::Type *type) {
    if (type->isIntegerTy(1))
        return types["bool"];
    else if (type->isIntegerTy(8))
        return types["char"];
    else if (type->isIntegerTy())
        return types["int"];
    else if (type->isFloatingPointTy())
        return types["float"];
    else if (type->isVoidTy())
        return types["void"];
    else if (types.find(getTypeName(type)) != types.end())
        return types[getTypeName(type)];

    return createNewType(type);
}

llvm::DISubroutineType *DebugInfo::getType(llvm::FunctionType *FT) {
    std::vector<llvm::Metadata *> param_types;

    // Add the result type.
    param_types.push_back(getType(FT->getReturnType()));

    // Add the parameters
    for (auto type: FT->params())
        param_types.push_back(getType(type));

    return Builder->createSubroutineType(Builder->getOrCreateTypeArray(param_types));
}
