#include "Codegen.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Common/ExportDiscovery.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;
using namespace llvm;
using namespace llvm::orc;

auto Codegen::getExportsFromFile(const std::string& filepath, bool isStd,
                                 const std::string& mainFilePath) -> std::vector<std::string> {
  return getExportedTopLevelNamesFromFile(filepath, isStd, mainFilePath);
}

auto Codegen::typecheckModule(const Compound* ast, const std::string& modulePath)
    -> std::tuple<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>,
                  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>> {
  Typechecker typechecker(
      modulePath, [this](const std::string& path, bool isStd, const std::string& mainFilePath) {
        return getExportsFromFile(path, isStd, mainFilePath);
      });
  typechecker.run(ast);
  auto takenTypeCache = typechecker.takeTypeCache();
  auto takenRoot = typechecker.takeRootScope();
  return {std::move(takenRoot), std::move(takenTypeCache), typechecker.takeSpecializedTypeEnv()};
}

auto Codegen::isImported(const std::vector<ImportedNameBinding>& importedNames,
                         const std::string& importName) const -> bool {
  return std::any_of(importedNames.begin(), importedNames.end(),
                     [&importName](const ImportedNameBinding& binding) {
                       return binding.name == importName;
                     });
}

auto Codegen::getImportedLocalName(const std::vector<ImportedNameBinding>& importedNames,
                                   const std::string& importName) const -> std::string {
  for (const ImportedNameBinding& binding : importedNames) {
    if (binding.name == importName) {
      return binding.alias.empty() ? importName : binding.alias;
    }
  }
  return "";
}

auto Codegen::insertImportAlias(const std::string& moduleAlias, bool importToScope) -> void {
  if (importToScope) {
    return;
  }

  auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
  auto* importTypPtr = importTyp.get();
  auto importSym = std::make_unique<Value>(moduleAlias, importTypPtr);
  importSym->setCategory(ValueCategory::MODULE_SYMBOL);
  scope->insertSymbol(std::move(importSym));
  scope->insertType(moduleAlias, std::move(importTyp));
}

auto Codegen::exposeImportedSymbols(llvm::SMRange /*span*/, SymbolTable* importedScope,
                                    bool importAll, bool importToScope,
                                    const std::vector<ImportedNameBinding>& importedNames) -> void {
  for (auto* sym : importedScope->getSymbols()) {
    const bool importedByName = isImported(importedNames, sym->getName());
    const std::string importedLocalName = getImportedLocalName(importedNames, sym->getName());
    const bool exposeClassForModuleImport =
        !importToScope && sym->getType()->is(BaseType::TY_CLASS);
    // Class/enum types from the module are always merged for lookup: exported APIs
    // may reference non-exported helper types in signatures.
    if (sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) &&
        (importAll || importedByName || exposeClassForModuleImport)) {
      llvm::StructType* structType =
          StructType::getTypeByName(theModule->getContext(), sym->getName());
      const std::string localName = importedLocalName.empty() ? sym->getName() : importedLocalName;
      auto structSymbol = std::make_unique<Value>(localName, sym->getType());
      structSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
      structSymbol->getType()->setLlvmType(structType);
      structSymbol->setGenericClassTemplate(sym->getGenericClassTemplate());
      scope->insertTypeRef(sym->getName(), sym->getType());
      scope->insertSymbol(std::move(structSymbol));
      continue;
    }

    if (!sym->getType()->is(BaseType::TY_FUNCTION) || !sym->isExported()) {
      continue;
    }

    if (sym->getType()->getLlvmType() == nullptr) {
      continue;
    }

    auto* fTy = llvm::cast<FunctionType>(sym->getType()->getLlvmType());
    auto name = sym->getName();
    std::vector<lesma::Type*> paramTypes;
    for (auto* field : sym->getType()->getFields()) {
      paramTypes.push_back(field->type);
    }

    Value* funcSymbol = importedScope->lookupFunction(name, paramTypes);
    const bool isMethodSym = MangleUtils::isMethod(sym->getMangledName());
    bool methodClassImported = true;
    if (isMethodSym && !importAll && importToScope) {
      const std::string& mn = sym->getMangledName();
      const size_t colcol = mn.find("::");
      if (colcol != std::string::npos) {
        std::string classPart = mn.substr(0, colcol);
        const size_t arrow = classPart.find("=>");
        if (arrow != std::string::npos) {
          classPart = classPart.substr(arrow + 2);
        }
        methodClassImported = isImported(importedNames, classPart);
      }
    }
    if (funcSymbol == nullptr || !funcSymbol->isExported() ||
        (!importAll && !importedByName &&
         (!isMethodSym || (importToScope && !methodClassImported)))) {
      continue;
    }

    const std::string localName = importedLocalName.empty() ? name : importedLocalName;
    Value* localSymbol = scope->lookupFunction(localName, paramTypes);
    const bool reuseExistingLocal =
        localSymbol != nullptr && localSymbol->getLlvmValue() == nullptr;
    auto symbol =
        reuseExistingLocal ? nullptr : std::make_unique<Value>(localName, funcSymbol->getType());
    Value* targetSymbol = reuseExistingLocal ? localSymbol : symbol.get();
    targetSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
    if (isJit) {
      auto* f = llvm::cast<Function>(
          theModule->getOrInsertFunction(sym->getMangledName(), fTy).getCallee());
      targetSymbol->setType(funcSymbol->getType());
      targetSymbol->getType()->setLlvmType(fTy);
      targetSymbol->setLlvmValue(f);
    } else {
      auto* newFunc =
          Function::Create(fTy, Function::ExternalLinkage, sym->getMangledName(), *theModule);
      targetSymbol->setType(funcSymbol->getType());
      targetSymbol->getType()->setLlvmType(newFunc->getFunctionType());
      targetSymbol->setLlvmValue(newFunc);
    }
    targetSymbol->setExported(false);
    targetSymbol->setMangledName(sym->getMangledName());
    if (!reuseExistingLocal) {
      scope->insertSymbol(std::move(symbol));
    }
  }
}

auto Codegen::compileModule(llvm::SMRange span, const std::string& filepath, bool isStd,
                            const std::string& moduleAlias, bool importAll, bool importToScope,
                            const std::vector<ImportedNameBinding>& importedNames) -> void {
  (void)isStd;
  const std::string absolutePath = normalizeModuleImportPath(filename, filepath);

  static thread_local std::unordered_set<std::string> compiling;
  if (compiling.contains(absolutePath)) {
    throw CodegenError(span, "Circular import detected: {}", filepath);
  }

  // If this codegen already compiled this module, only merge its symbols (no
  // recompilation). Only skip when we have the scope (same codegen compiled
  // it); otherwise a child may see the path in the list but not have the scope.
  auto it = std::find(importedModules->begin(), importedModules->end(), absolutePath);
  if (it != importedModules->end()) {
    auto existingIdx = static_cast<size_t>(it - importedModules->begin());
    if (existingIdx >= importedScopes->size()) {
      throw CodegenError(span, "Circular import detected: {}", filepath);
    }
  }
  if (it != importedModules->end()) {
    auto existingIdx = static_cast<size_t>(it - importedModules->begin());
    SymbolTable* existingScope = importedScopes->at(existingIdx).get();
    if (existingIdx < importedCodegens.size() && importedCodegens[existingIdx] != nullptr) {
      mergeImportedTraitMetadata(*importedCodegens[existingIdx]);
      mergeImportedSpecializationState(*importedCodegens[existingIdx]);
    }
    insertImportAlias(moduleAlias, importToScope);
    exposeImportedSymbols(span, existingScope, importAll, importToScope, importedNames);
    return;
  }

  // Guard against circular import: if this module is already being compiled
  // (path in importedModules but scope not yet in importedScopes), we would
  // fall through and re-compile it → infinite recursion. Detect and error.
  if (compiling.size() >= 32) {
    throw CodegenError(span,
                       "Import recursion depth exceeded (possible circular "
                       "import): {}",
                       filepath);
  }
  compiling.insert(absolutePath);

  auto buffer = MemoryBuffer::getFile(absolutePath);
  if (std::error_code ec = buffer.getError()) {
    compiling.erase(absolutePath);
    throw LesmaError(llvm::SMRange(), "Could not read file: {}", absolutePath);
  }

  auto fileId = sourceManager->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  importedModules->push_back(absolutePath);

  try {
    // Lexer
    auto lexer = std::make_unique<Lexer>(sourceManager);
    lexer->scanAll();

    // Parser
    auto parser = std::make_unique<Parser>(lexer->getTokens());
    parser->parse();
    Compound* ast = parser->getAst();
    if (ast == nullptr) {
      throw CodegenError(span, "Unable to parse imported module {}", filepath);
    }

    auto [preScope, preTypeCache, preSpecEnv] = typecheckModule(ast, absolutePath);

    auto codegen = std::make_unique<Codegen>(
        std::move(parser), sourceManager, absolutePath, std::vector<std::string>{}, isJit, false,
        !importToScope ? moduleAlias : "", theContext, importedModules, importedScopes,
        std::move(preScope), std::move(preTypeCache), std::move(preSpecEnv));
    codegen->run();
    mergeImportedTraitMetadata(*codegen);

    // O0: imported modules are separate LLVM modules; O3+ADCE can drop defs only referenced
    // from another module (e.g. list methods). The main module is optimized in Driver.
    codegen->optimize(OptimizationLevel::O0);
    codegen->theModule->setModuleIdentifier(filepath);

    insertImportAlias(moduleAlias, importToScope);
    exposeImportedSymbols(span, codegen->rootScope.get(), importAll, importToScope, importedNames);

    importedScopes->push_back(std::move(codegen->rootScope));
    codegen->scope = nullptr; // Clear navigation pointer (rootscope now moved)

    for (auto& type : codegen->typeCache) {
      typeCache.push_back(std::move(type));
    }
    mergeImportedSpecializationState(*codegen);

    if (isJit) {
      // Add the module to the JIT (after importing symbols; theModule still valid)
      cantFail(theJit->addIRModule(ThreadSafeModule(std::move(codegen->theModule), *theContext)),
               fmt::format("Failed adding import {} to JIT", filename).c_str());
    } else {
      // Create object file to be linked
      std::string objFile = fmt::format("tmp{}", objectFiles.size());
      codegen->writeToObjectFile(objFile);
      objectFiles.push_back(fmt::format("{}.o", objFile));
    }
    // Keep the imported codegen alive so Class* stored in copied symbols stay
    // valid
    importedCodegens.push_back(std::move(codegen));
  } catch (const LesmaError& err) {
    compiling.erase(absolutePath);
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, err.what());
    } else {
      showInline(sourceManager.get(), fileId, err.getSpan(), absolutePath, true, err.what());
    }

    throw CodegenError(span, "Unable to import {} due to errors", filepath);
  }
  compiling.erase(absolutePath);
}
