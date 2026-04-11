#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

#include "Codegen.h"
#include <fmt/format.h>

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

namespace {

[[nodiscard]] auto jitErrorToString(Error err) -> std::string {
  std::string msg;
  handleAllErrors(std::move(err), [&](const ErrorInfoBase& ei) { msg = ei.message(); });
  return msg.empty() ? "unknown error" : msg;
}

} // namespace

auto Codegen::getExportsFromFile(const std::string& filepath, bool isStd,
                                 const std::string& mainFilePath) -> ExportDiscoveryResult {
  return discoverExportedTopLevelNames(filepath, isStd, mainFilePath);
}

auto Codegen::typecheckModule(const Compound* ast, const std::string& modulePath)
    -> std::tuple<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>,
                  std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>,
                  std::unordered_map<lesma::Type*, lesma::Type*>,
                  std::unordered_map<std::string, lesma::Type*>,
                  std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>>> {
  Typechecker typechecker(
      modulePath,
      [this](const std::string& path, bool isStd, const std::string& mainFilePath) {
        return getExportsFromFile(path, isStd, mainFilePath);
      },
      nullptr, nullptr, 0, performanceTimer);
  typechecker.run(ast);
  auto takenTypeCache = typechecker.takeTypeCache();
  auto takenRoot = typechecker.takeRootScope();
  auto takenImportedModules = typechecker.takeImportedModules();
  return {std::move(takenRoot),
          std::move(takenTypeCache),
          typechecker.takeSpecializedTypeEnv(),
          typechecker.takeSpecializedTypeToTemplate(),
          typechecker.takeSpecializedClassTypes(),
          std::move(takenImportedModules)};
}

auto Codegen::isImported(const std::vector<ImportedNameBinding>& importedNames,
                         const std::string& importName) const -> bool {
  return std::ranges::any_of(importedNames, [&importName](const ImportedNameBinding& binding) {
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

auto Codegen::insertImportAlias(const std::string& moduleAlias, bool importToScope,
                                const std::string& importedModuleAbsolutePath) -> void {
  if (importToScope) {
    return;
  }

  auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
  importTyp->setDeclarationFilePath(importedModuleAbsolutePath);
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
      structSymbol->setDeclarationKind(sym->getDeclarationKind());
      structSymbol->setDeclarationSpan(sym->getDeclarationSpan());
      structSymbol->setDeclarationFilePath(sym->getDeclarationFilePath());
      structSymbol->setExported(sym->isExported());
      structSymbol->getType()->setLlvmType(structType);
      structSymbol->setGenericClassTemplate(sym->getGenericClassTemplate());
      structSymbol->setConstructor(sym->getConstructor());
      scope->insertTypeRef(sym->getName(), sym->getType());
      scope->insertSymbol(std::move(structSymbol));
      continue;
    }

    if (sym->getDeclarationKind() == ValueDeclarationKind::VARIABLE && sym->isExported()) {
      if (!importAll && !importedByName) {
        continue;
      }
      lesma::Type* memTy = sym->getType();
      getOrCreateLlvmType(memTy);
      llvm::Type* storageTy = memTy->getLlvmType();
      if (memTy->is(BaseType::TY_CLASS)) {
        storageTy = builder->getPtrTy();
      } else if (memTy->is(BaseType::TY_PTR) && memTy->getElementType() != nullptr &&
                 memTy->getElementType()->is(BaseType::TY_CLASS)) {
        storageTy = builder->getPtrTy();
      }
      std::string const mangled = MangleUtils::getGlobalVariableSymbolName(
          normalizeResolvedFilesystemPath(sym->getDeclarationFilePath()), sym->getName());
      llvm::GlobalVariable* gv = theModule->getGlobalVariable(mangled, true);
      if (gv == nullptr) {
        gv = new llvm::GlobalVariable(*theModule, storageTy, false,
                                      llvm::GlobalValue::ExternalLinkage, nullptr, mangled);
      }
      const std::string localName = importedLocalName.empty() ? sym->getName() : importedLocalName;
      auto vs = std::make_unique<Value>(localName, memTy);
      vs->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      vs->setDeclarationKind(ValueDeclarationKind::VARIABLE);
      vs->setMutable(sym->getMutability());
      vs->setLlvmValue(gv);
      vs->setMangledName(mangled);
      vs->setDeclarationFilePath(sym->getDeclarationFilePath());
      scope->insertSymbol(std::move(vs));
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

    Value* funcSymbol =
        importedScope->lookupFunction(name, paramTypes, FunctionLookupKind::VALUE, nullptr, nullptr,
                                      sym->getType()->getGenericParams().size());
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
    Value* localSymbol =
        scope->lookupFunction(localName, paramTypes, FunctionLookupKind::VALUE, nullptr, nullptr,
                              sym->getType()->getGenericParams().size());
    // Reuse only when we are updating the same mangled symbol; lookupFunction(name, params) alone
    // can match the wrong overload when many symbols share a name (e.g. `new`).
    const bool reuseExistingLocal =
        localSymbol != nullptr && localSymbol->getLlvmValue() == nullptr &&
        localSymbol->getMangledName() == sym->getMangledName();
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
    targetSymbol->setDeclarationKind(funcSymbol->getDeclarationKind());
    targetSymbol->setDeclarationSpan(funcSymbol->getDeclarationSpan());
    targetSymbol->setDeclarationFilePath(funcSymbol->getDeclarationFilePath());
    targetSymbol->setStaticMethod(funcSymbol->isStaticMethod());
    targetSymbol->setMemberDeclaredInClass(funcSymbol->getMemberDeclaredInClass());
    targetSymbol->setPrivateMember(funcSymbol->isPrivateMember());
    targetSymbol->setGenericClassTemplate(funcSymbol->getGenericClassTemplate());
    if (!reuseExistingLocal) {
      scope->insertSymbol(std::move(symbol));
    }
  }
}

auto Codegen::compileModule(llvm::SMRange span, const std::string& filepath, bool isStd,
                            const std::string& moduleAlias, bool importAll, bool importToScope,
                            const std::vector<ImportedNameBinding>& importedNames) -> void {
  (void) isStd;
  const std::string resolvedPath = normalizeModuleImportPath(filename, filepath);
  const std::string canonicalPath = normalizeResolvedFilesystemPath(resolvedPath);

  static thread_local std::unordered_set<std::string> compiling;
  if (compiling.contains(canonicalPath)) {
    throw CodegenError(span, "Circular import detected: {}", filepath);
  }

  // If this codegen already compiled this module, only merge its symbols (no
  // recompilation). Only skip when we have the scope (same codegen compiled
  // it); otherwise a child may see the path in the list but not have the scope.
  auto it = std::find(importedModules->begin(), importedModules->end(), canonicalPath);
  if (it != importedModules->end()) {
    auto existingIdx = static_cast<size_t>(it - importedModules->begin());
    if (existingIdx >= importedScopes->size() || !importedScopes->at(existingIdx)) {
      throw CodegenError(span, "Circular import detected: {}", filepath);
    }
  }
  if (it != importedModules->end()) {
    auto existingIdx = static_cast<size_t>(it - importedModules->begin());
    SymbolTable* existingScope = importedScopes->at(existingIdx).get();
    if (existingIdx >= importedSpecializationStates->size()) {
      throw CodegenError(span, "Missing specialization metadata for import {}", filepath);
    }
    const ImportedSpecializationState& importedState =
        importedSpecializationStates->at(existingIdx);
    mergeImportedSpecializationState(importedState);
    mergeImportedTraitMetadata(importedState);
    insertImportAlias(moduleAlias, importToScope, canonicalPath);
    if (!importToScope && !moduleAlias.empty()) {
      importAliasToModulePath[moduleAlias] = canonicalPath;
    }
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
  compiling.insert(canonicalPath);

  auto buffer = performanceTimer != nullptr
                    ? performanceTimer->measureFile(
                          "Reading", canonicalPath,
                          [&]() -> llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> {
                            return MemoryBuffer::getFile(canonicalPath);
                          })
                    : MemoryBuffer::getFile(canonicalPath);
  if (std::error_code ec = buffer.getError()) {
    compiling.erase(canonicalPath);
    throw LesmaError(llvm::SMRange(), "Could not read file: {}", canonicalPath);
  }

  auto fileId = sourceManager->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  importedModules->push_back(canonicalPath);
  auto importIdx = importedModules->size() - 1U;
  if (importedScopes->size() <= importIdx) {
    importedScopes->resize(importIdx + 1U);
  }
  if (importedSpecializationStates->size() <= importIdx) {
    importedSpecializationStates->resize(importIdx + 1U);
  }

  try {
    // Lexer
    auto lexer = std::make_unique<Lexer>(sourceManager);
    if (performanceTimer != nullptr) {
      performanceTimer->measureFile("Lexing", canonicalPath, [&]() -> void { lexer->scanAll(); });
    } else {
      lexer->scanAll();
    }

    // Parser
    auto parser =
        std::make_unique<Parser>(lexer->getTokens(), nullptr, sourceManager, fileId, canonicalPath);
    if (performanceTimer != nullptr) {
      performanceTimer->measureFile("Parsing", canonicalPath, [&]() -> void { parser->parse(); });
    } else {
      parser->parse();
    }
    Compound* ast = parser->getAst();
    if (ast == nullptr) {
      throw CodegenError(span, "Unable to parse imported module {}", filepath);
    }

    auto [preScope, preTypeCache, preSpecEnv, preTemplateOf, preSpecializedClassTypes,
          preImportedModuleAnalyses] =
        performanceTimer != nullptr
            ? performanceTimer->measureFile(
                  "Typecheck", canonicalPath,
                  [&]() -> std::tuple<
                            std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>,
                            std::unordered_map<lesma::Type*,
                                               std::unordered_map<std::string, lesma::Type*>>,
                            std::unordered_map<lesma::Type*, lesma::Type*>,
                            std::unordered_map<std::string, lesma::Type*>,
                            std::unordered_map<std::string,
                                               std::shared_ptr<ImportedModuleAnalysis>>> {
                    return typecheckModule(ast, canonicalPath);
                  })
            : typecheckModule(ast, canonicalPath);

    auto codegen = std::make_unique<Codegen>(
        std::move(parser), sourceManager, canonicalPath, std::vector<std::string>{}, isJit, false,
        !importToScope ? moduleAlias : "", theContext, theJit, importedModules, importedScopes,
        importedSpecializationStates, std::move(preScope), std::move(preTypeCache),
        std::move(preSpecEnv), std::move(preTemplateOf), std::move(preSpecializedClassTypes),
        std::move(preImportedModuleAnalyses), performanceTimer, emitDebugInfo, emitArcDebug,
        emitArcTrace, OptimizationLevel::O0, pendingJitModuleInits, pendingJitModuleFinis);
    if (performanceTimer != nullptr) {
      performanceTimer->measureFile("Compiling", canonicalPath, [&]() -> void { codegen->run(); });
    } else {
      codegen->run();
    }
    ImportedSpecializationState importedState = codegen->captureImportedSpecializationState();
    importedSpecializationStates->at(importIdx) = importedState;
    mergeImportedTraitMetadata(importedState);
    for (const auto& [key, fn] : codegen->genericFunctions) {
      if (!key.empty() && fn != nullptr) {
        genericFunctions[key] = fn;
      }
    }
    for (const auto& [owner, methods] : codegen->genericMethods) {
      auto& dest = genericMethods[owner];
      for (const auto& [methodKey, methodAst] : methods) {
        if (!methodKey.empty() && methodAst != nullptr) {
          dest[methodKey] = methodAst;
        }
      }
    }

    // Imported modules run optimize(O0) (no-op). For JIT, promote PrivateLinkage so Mach-O
    // JITLink can resolve symbols across ORC modules at -O0 (see prepareJit / addIRModule).
    codegen->optimize(OptimizationLevel::O0);
    codegen->theModule->setModuleIdentifier(filepath);

    insertImportAlias(moduleAlias, importToScope, canonicalPath);
    if (!importToScope && !moduleAlias.empty()) {
      importAliasToModulePath[moduleAlias] = canonicalPath;
    }
    exposeImportedSymbols(span, codegen->rootScope.get(), importAll, importToScope, importedNames);

    importedScopes->at(importIdx) = std::move(codegen->rootScope);
    codegen->scope = nullptr; // Clear navigation pointer (rootscope now moved)

    for (auto& type : codegen->typeCache) {
      typeCache.push_back(std::move(type));
    }
    mergeImportedSpecializationState(importedState);

    std::string jitModuleInitSymbol;
    std::string jitModuleFiniSymbol;
    auto addImportToBackend = [&]() -> void {
      if (isJit) {
        codegen->verifyIrModuleOrThrow(fmt::format("import {}", filepath));
        if (llvm::Function* importMain = codegen->theModule->getFunction("main");
            importMain != nullptr && importMain->hasInternalLinkage()) {
          jitModuleInitSymbol = MangleUtils::getImportedModuleInitSymbolName(canonicalPath);
          importMain->setName(jitModuleInitSymbol);
          importMain->setLinkage(llvm::GlobalValue::ExternalLinkage);
          importMain->setVisibility(llvm::GlobalValue::HiddenVisibility);
        }
        jitModuleFiniSymbol = MangleUtils::getImportedModuleFiniSymbolName(canonicalPath);
        if (llvm::Function* importFini = codegen->theModule->getFunction(jitModuleFiniSymbol);
            importFini != nullptr) {
          importFini->setLinkage(llvm::GlobalValue::ExternalLinkage);
          importFini->setVisibility(llvm::GlobalValue::HiddenVisibility);
        } else {
          jitModuleFiniSymbol.clear();
        }
        for (llvm::Function& fn : *codegen->theModule) {
          if (fn.hasPrivateLinkage()) {
            fn.setLinkage(llvm::GlobalValue::ExternalLinkage);
            fn.setVisibility(llvm::GlobalValue::HiddenVisibility);
          }
        }
        // Do not promote private GlobalVariables (string literals, etc.): Mach-O JITLink reports
        // "Unexpected definitions" for anonymous ___unnamed_* symbols when they become external.
        llvm::Error jitErr =
            theJit->addIRModule(ThreadSafeModule(std::move(codegen->theModule), *theContext));
        if (jitErr) {
          throw CodegenError(span, std::string("Failed adding import to JIT: ") + canonicalPath +
                                       ": " + jitErrorToString(std::move(jitErr)));
        }
        if (!jitModuleInitSymbol.empty() && pendingJitModuleInits != nullptr) {
          pendingJitModuleInits->push_back(jitModuleInitSymbol);
        }
        if (!jitModuleFiniSymbol.empty() && pendingJitModuleFinis != nullptr) {
          pendingJitModuleFinis->push_back(jitModuleFiniSymbol);
        }
        codegen->theModule = codegen->initializeModule();
      } else {
        std::string objFile = fmt::format("tmp{}", objectFiles.size());
        codegen->writeToObjectFile(objFile);
        objectFiles.push_back(fmt::format("{}.o", objFile));
      }
    };

    if (performanceTimer != nullptr) {
      performanceTimer->measureFile(isJit ? "JIT" : "Writing Object File", canonicalPath,
                                    addImportToBackend);
    } else {
      addImportToBackend();
    }
    // Keep the imported codegen alive so Class* stored in copied symbols stay
    // valid
    importedCodegens.push_back(std::move(codegen));
  } catch (const LesmaError& err) {
    compiling.erase(canonicalPath);
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, err.what());
    } else {
      showInline(sourceManager.get(), fileId, err.getSpan(), canonicalPath, true, err.what());
    }

    throw CodegenError(span, "Unable to import {} due to errors", filepath);
  }
  compiling.erase(canonicalPath);
}
