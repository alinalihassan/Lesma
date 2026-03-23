#include "Codegen.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include <lld/Common/Driver.h>

// Declare LLD driver functions using the macro from Driver.h
#ifdef __APPLE__
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif
#include <nameof.hpp>

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenTypeUtils.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr,
                 const std::string& filename, std::vector<std::string> imports, bool jit, bool main,
                 std::string alias, const std::shared_ptr<ThreadSafeContext>& context,
                 std::shared_ptr<std::vector<std::string>> sharedModules,
                 std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>> sharedScopes,
                 std::unique_ptr<SymbolTable> preScope,
                 std::vector<std::unique_ptr<lesma::Type>> preTypeCache) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  theContext = context == nullptr
                   ? std::make_shared<ThreadSafeContext>(std::make_unique<LLVMContext>())
                   : context;
  targetMachine = initializeTargetMachine();
  theModule = initializeModule();
  if (jit) {
    theJit = initializeJit();
  }

  builder = std::make_unique<IRBuilder<>>(theModule->getContext());
  this->parser = std::move(parser);
  sourceManager = std::move(srcMgr);
  if (preScope == nullptr) {
    throw CodegenError({}, "Codegen requires a precomputed typecheck scope");
  }
  rootScope = std::move(preScope);
  scope = rootScope.get();
  for (auto& t : preTypeCache) {
    typeCache.push_back(std::move(t));
  }

  this->alias = std::move(alias);
  this->filename = filename;
  isMain = main;
  isJit = jit;

  if (sharedModules && sharedScopes) {
    importedModules = std::move(sharedModules);
    importedScopes = std::move(sharedScopes);
  } else {
    importedModules = std::make_shared<std::vector<std::string>>(std::move(imports));
    importedScopes = std::make_shared<std::vector<std::unique_ptr<SymbolTable>>>();
  }
  topLevelFunc = initializeTopLevel();
  // base.les is loaded at the start of run() so we don't load it during
  // constructor re-entrancy when creating Codegens for imported modules.
}

auto Codegen::initializeModule() -> std::unique_ptr<Module> {
  std::unique_ptr<Module> mod;
#if LLVM_VERSION_MAJOR >= 21
  theContext->withContextDo([&mod](LLVMContext* ctx) { mod = std::make_unique<Module>("Lesma", *ctx); });
#else
  {
    auto lock = theContext->getLock();
    LLVMContext* ctx = theContext->getContext();
    mod = std::make_unique<Module>("Lesma", *ctx);
  }
#endif
#if LLVM_VERSION_MAJOR >= 21
  mod->setTargetTriple(targetMachine->getTargetTriple());
#else
  mod->setTargetTriple(targetMachine->getTargetTriple().str());
#endif
  mod->setDataLayout(targetMachine->createDataLayout());
  mod->setSourceFileName(filename);

  return mod;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto Codegen::initializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine> {
  // Configure output target
  auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());

  // Search after selected target
  std::string error;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple.getTriple(), error);
  if (target == nullptr) {
    throw CodegenError({}, "Target not available:\n{}", error);
  }

  llvm::TargetOptions const opt;
  llvm::Reloc::Model rm = llvm::Reloc::Model();
  std::unique_ptr<llvm::TargetMachine> targetMachine(
#if LLVM_VERSION_MAJOR >= 21
      target->createTargetMachine(targetTriple, "generic", "", opt, rm));
#else
      target->createTargetMachine(targetTriple.str(), "generic", "", opt, rm));
#endif
  return targetMachine;
}

auto Codegen::initializeJit() -> std::unique_ptr<LLJIT> {
  llvm::orc::LLJITBuilder jitBuilder{};
  jitBuilder.setDataLayout(theModule->getDataLayout());
  jitBuilder.setJITTargetMachineBuilder(
      llvm::orc::JITTargetMachineBuilder(targetMachine->getTargetTriple()));
  auto jit = llvm::cantFail(jitBuilder.create());
  if (!jit) {
    throw CodegenError({}, "Couldn't initialize JIT\n");
  }

  // Add support for C native functions
  auto& mainJd = jit->getMainJITDylib();
  auto generator = cantFail(
      DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix()));
  mainJd.addGenerator(std::move(generator));

  return jit;
}

auto Codegen::initializeTopLevel() -> llvm::Function* {
  std::vector<llvm::Type*> const paramTypes = {};

  FunctionType* ft = FunctionType::get(builder->getInt64Ty(), paramTypes, false);
  Function* f = Function::Create(ft, isMain ? Function::ExternalLinkage : Function::InternalLinkage,
                                 "main", *theModule);

  auto* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  return f;
}

auto Codegen::defineFunction(lesma::Value* value, const FuncDecl* node, Value* clsSymbol) -> void {
  if (clsSymbol == nullptr) {
    auto fields = value->getType()->getFields();
    if (!fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_PTR) &&
        fields.front()->type->getElementType() != nullptr) {
      auto* selfClassType = fields.front()->type->getElementType();
      if (auto it = specializedClassSymbolsByType.find(selfClassType);
          it != specializedClassSymbolsByType.end()) {
        clsSymbol = it->second;
      }
    }
  }
  SymbolTable* savedScope = scope;
  scope = value->getBodyScope();
  if (scope == nullptr) {
    scope = savedScope->createChildBlock(node->getName());
  }
  auto lookupInCurrentScope = [this](const std::string& name) -> Value* {
    for (auto* symbol : scope->getSymbols()) {
      if (symbol->getName() == name) {
        return symbol;
      }
    }
    return nullptr;
  };
  currentFunction = value;
  deferStack.emplace();

  if (value->getLlvmValue() == nullptr) {
    throw CodegenError(node->getSpan(), "Function {} is declared but has no LLVM body",
                       node->getName());
  }
  auto* f = llvm::cast<Function>(value->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  int fieldIndex = 0;
  for (const auto& field : value->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);
    std::string paramName;

    if (field->name == "self") {
      paramName = "self";
    } else {
      size_t const paramIndex = param->getArgNo() - (clsSymbol != nullptr ? 1U : 0U);
      if (paramIndex < node->getParameters().size()) {
        paramName = node->getParameters()[paramIndex]->name;
      } else {
        paramName = field->name;
      }
    }
    param->setName(paramName);

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
    builder->CreateStore(param, ptr);

    if (auto* existingParam = lookupInCurrentScope(paramName);
        existingParam != nullptr && existingParam->getLlvmValue() == nullptr) {
      existingParam->setLlvmValue(ptr);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    } else {
      auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
      symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(symbol));
    }

    fieldIndex++;
  }

  node->getBody()->accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  if (!isReturn) {
    for (auto* inst : instrs) {
      inst->accept(*this);
    }
  }

  // Check for well-formness of all BBs. In particular, look for
  // any unterminated BB and try to add a Return to it.
  for (BasicBlock& bb : *f) {
    Instruction* terminator = bb.getTerminator();
    if (terminator != nullptr) {
      continue; // Well-formed
    }

    if (value->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      // Make implicit return of void Function explicit.
      builder->SetInsertPoint(&bb);
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->getSpan(), "Function {} does not always return a result",
                         node->getName());
    }
  }

  isReturn = false;

  // Verify only this module's function (not cross-module declarations).
  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(node->getSpan(), "Invalid function {}\n{}", node->getName(), verifyOutput);
  }

  // Insert Function to Symbol Table
  scope = savedScope;

  currentFunction = nullptr;

  // Reset Insert Point to Top Level
  builder->SetInsertPoint(&topLevelFunc->back());
}

auto Codegen::getExportsFromFile(const std::string& filepath, bool isStd,
                                 const std::string& mainFilePath) -> std::vector<std::string> {
  std::string absolutePath =
      isStd ? filepath
            : fmt::format("{}/{}", std::filesystem::absolute(mainFilePath).parent_path().string(),
                          filepath);
  auto buffer = llvm::MemoryBuffer::getFile(absolutePath);
  if (!buffer) {
    return {};
  }

  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  auto lexer = std::make_unique<Lexer>(srcMgr);
  lexer->scanAll();
  auto pars = std::make_unique<Parser>(lexer->getTokens());
  pars->parse();
  Compound* ast = pars->getAst();
  if (ast == nullptr) {
    return {};
  }

  std::vector<std::string> out;
  for (Statement* stmt : ast->getChildren()) {
    if (auto* f = dynamic_cast<FuncDecl*>(stmt)) {
      if (f->isExported()) {
        out.push_back(f->getName());
      }
    } else if (auto* c = dynamic_cast<Class*>(stmt)) {
      if (c->isExported()) {
        out.push_back(c->getIdentifier());
      }
    } else if (auto* e = dynamic_cast<Enum*>(stmt)) {
      if (e->isExported()) {
        out.push_back(e->getIdentifier());
      }
    } else if (auto* ef = dynamic_cast<ExternFuncDecl*>(stmt)) {
      if (ef->isExported()) {
        out.push_back(ef->getName());
      }
    }
  }

  return out;
}

auto Codegen::typecheckModule(const Compound* ast, const std::string& modulePath)
    -> std::pair<std::unique_ptr<SymbolTable>, std::vector<std::unique_ptr<lesma::Type>>> {
  Typechecker typechecker(
      modulePath, [this](const std::string& path, bool isStd, const std::string& mainFilePath) {
        return getExportsFromFile(path, isStd, mainFilePath);
      });
  typechecker.run(ast);
  return {typechecker.takeRootScope(), typechecker.takeTypeCache()};
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
  auto findImportedAlias = [&importedNames](const std::string& import) -> std::string {
    for (const ImportedNameBinding& binding : importedNames) {
      if (binding.name == import) {
        return binding.alias;
      }
    }
    return "";
  };

  for (auto* sym : importedScope->getSymbols()) {
    auto impAlias = findImportedAlias(sym->getName());
    const bool exposeClassForModuleImport =
        !importToScope && sym->getType()->is(BaseType::TY_CLASS);
    if (sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) && sym->isExported() &&
        (importAll || !impAlias.empty() || exposeClassForModuleImport)) {
      llvm::StructType* structType =
          StructType::getTypeByName(theModule->getContext(), sym->getName());
      auto structSymbol =
          std::make_unique<Value>(impAlias.empty() ? sym->getName() : impAlias, sym->getType());
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
    impAlias = findImportedAlias(name);
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
        methodClassImported = !findImportedAlias(classPart).empty();
      }
    }
    if (funcSymbol == nullptr || !funcSymbol->isExported() ||
        (!importAll && impAlias.empty() &&
         (!isMethodSym || (importToScope && !methodClassImported)))) {
      continue;
    }

    const std::string localName =
        impAlias.empty() ? name : std::regex_replace(name, std::regex(name), impAlias);
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
  std::filesystem::path mainPath = filename;
  // Read source
  auto absolutePath =
      isStd ? filepath
            : fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(),
                          filepath);

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
  // auto sourceStr =
  // sourceManager->getMemoryBuffer(fileId)->getBuffer().str();
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

    auto [preScope, preTypeCache] = typecheckModule(ast, absolutePath);

    auto codegen = std::make_unique<Codegen>(
        std::move(parser), sourceManager, absolutePath, std::vector<std::string>{}, isJit, false,
        !importToScope ? moduleAlias : "", theContext, importedModules, importedScopes,
        std::move(preScope), std::move(preTypeCache));
    codegen->run();

    // Optimize
    codegen->optimize(OptimizationLevel::O3);
    codegen->theModule->setModuleIdentifier(filepath);

    insertImportAlias(moduleAlias, importToScope);
    exposeImportedSymbols(span, codegen->rootScope.get(), importAll, importToScope, importedNames);

    importedScopes->push_back(std::move(codegen->rootScope));
    codegen->scope = nullptr; // Clear navigation pointer (rootscope now moved)

    for (auto& type : codegen->typeCache) {
      typeCache.push_back(std::move(type));
    }

    if (isJit) {
      // Add the module to JIT (after importing symbols; theModule still valid)
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

auto Codegen::optimize(OptimizationLevel opt) -> void {
  if (opt == OptimizationLevel::O0) {
    return;
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb(&*targetMachine);

  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  // Add custom passes to LoopPassManager
  llvm::LoopPassManager lpm;
  lpm.addPass(llvm::LoopFullUnrollPass());

  // Add custom passes to FunctionPassManager
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::ADCEPass());
  fpm.addPass(llvm::GVNPass());
  fpm.addPass(llvm::DSEPass());
  fpm.addPass(llvm::LoopVectorizePass());
  fpm.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(lpm)));

  // Add custom passes to CGSCCPassManager
  llvm::CGSCCPassManager cgpm;
  cgpm.addPass(llvm::InlinerPass());

  // Add custom pass managers to ModulePassManager
  llvm::ModulePassManager mpm =
      pb.buildModuleOptimizationPipeline(opt, ThinOrFullLTOPhase::FullLTOPreLink);
  mpm.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(std::move(cgpm)));
  mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
  mpm.addPass(llvm::GlobalDCEPass());

  mpm.run(*theModule, mam);
}

auto Codegen::writeToObjectFile(const std::string& output) -> void {
  std::error_code err;
  llvm::raw_fd_ostream out(output + ".o", err);

  if (err) {
    throw CodegenError({}, "Error opening file {} for writing: {}", output, err.message());
  }

  llvm::legacy::PassManager passManager;
  if (targetMachine->addPassesToEmitFile(passManager, out, nullptr,
                                         llvm::CodeGenFileType::ObjectFile)) {
    throw CodegenError({}, "Target Machine can't emit an object file");
  }
  // Emit object file
  passManager.run(*theModule);

  // Flush and close the file
  out.flush();
  out.close();
}

void Codegen::linkObjectFileWithLld(const std::string& objFilename) {
  std::string output = getBasename(objFilename);

  std::vector<const char*> args;

  // First arg determines linker flavor: ld.lld (ELF), ld64.lld (MachO),
  // lld-link (COFF)
#ifdef __APPLE__
  args.push_back("ld64.lld");
#elif defined(_WIN32)
  args.push_back("lld-link");
#else
  args.push_back("ld.lld");
#endif

  // Suppress linker warnings
  args.push_back("-w");
  // Files
  args.push_back("-o");
  args.push_back(output.c_str());
  args.push_back(objFilename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }

#ifdef __APPLE__
  // Add macOS-specific linker arguments
  args.push_back("-arch");
  args.push_back("arm64");
  args.push_back("-platform_version");
  args.push_back("macos"); // platform
  args.push_back("11.0");  // min version
  args.push_back("11.0");  // sdk version
  args.push_back("-L");
  args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
  args.push_back("-lSystem");
#endif

  // Run the LLD linker using lldMain
#ifdef __APPLE__
  lld::Result result =
      lld::lldMain(args, llvm::outs(), llvm::errs(), {{.f = lld::Darwin, .d = &lld::macho::link}});
#elif defined(_WIN32)
  lld::Result result =
      lld::lldMain(args, llvm::outs(), llvm::errs(), {{.f = lld::WinLink, .d = &lld::coff::link}});
#else
  lld::Result result =
      lld::lldMain(args, llvm::outs(), llvm::errs(), {{.f = lld::Gnu, .d = &lld::elf::link}});
#endif

  if (result.retCode != 0) {
    throw CodegenError({}, "Linking Failed");
  }

  // Remove object files (ignore errors as they're temporary)
  std::ignore = llvm::sys::fs::remove(objFilename);
  for (const auto& obj : objectFiles) {
    std::ignore = llvm::sys::fs::remove(obj);
  }
}

auto Codegen::linkObjectFile(const std::string& objFilename) -> void {
  linkObjectFileWithLld(objFilename);
}

auto Codegen::prepareJit() -> void {
  auto jitError = theJit->addIRModule(ThreadSafeModule(std::move(theModule), *theContext));
  if (jitError) {
    throw CodegenError({}, "JIT Error:\n{}");
  }
  auto mainFunc = theJit->lookup(topLevelFunc->getName());
  if (!mainFunc) {
    throw CodegenError({}, "Couldn't find top level function\n");
  }
  mainFuncAddress = mainFunc->toPtr<MainFnTy>();
}

auto Codegen::executeJit() -> int {
  if (mainFuncAddress == nullptr) {
    throw CodegenError({}, "Main function address not found, did you prepare JIT?\n");
  }

  return mainFuncAddress();
}

auto Codegen::run() -> void {
  // Load implicit stdlib modules once so every module has base functions and
  // list helpers. Done here (not in constructor) to avoid re-entrancy when
  // creating Codegens for imported modules.
  std::vector<std::string> const implicitStdlibModules = {"base.les", "list.les"};
  auto const currentPath = std::filesystem::absolute(std::filesystem::path(filename)).lexically_normal();
  auto const basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  auto const listPath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "list.les").lexically_normal();
  if (currentPath != basePath && currentPath != listPath) {
    for (const auto& moduleName : implicitStdlibModules) {
      auto const modulePath =
          std::filesystem::absolute(std::filesystem::path(getStdDir()) / moduleName).lexically_normal();
      if (currentPath == modulePath) {
        continue;
      }
      compileModule(llvm::SMRange(), getStdDir() + moduleName, true, moduleName, true, true, {});
    }
  }

  deferStack.emplace();
  parser->getAst()->accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  // Visit all statements
  for (auto* inst : instrs) {
    inst->accept(*this);
  }

  // Define the function bodies (index-based: specializeFunction may append
  // new prototypes while we iterate, e.g. when combine<int> triggers add<int>)
  for (size_t pi = 0; pi < prototypes.size(); ++pi) {
    auto* fn = std::get<0>(prototypes[pi]);
    auto savedGenerics = currentGenericTypes;
    if (auto env = specializationEnvs.find(fn); env != specializationEnvs.end()) {
      currentGenericTypes = env->second;
    } else if (auto* cls = std::get<2>(prototypes[pi]); cls != nullptr &&
               cls->getType() != nullptr && cls->getType()->is(BaseType::TY_PTR) &&
               cls->getType()->getElementType() != nullptr) {
      if (auto clsEnv = specializedClassTypeEnvs.find(cls->getType()->getElementType());
          clsEnv != specializedClassTypeEnvs.end()) {
        currentGenericTypes = clsEnv->second;
      }
    } else {
      auto fields = fn->getType()->getFields();
      if (!fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_PTR) &&
          fields.front()->type->getElementType() != nullptr) {
        if (auto clsEnv = specializedClassTypeEnvs.find(fields.front()->type->getElementType());
            clsEnv != specializedClassTypeEnvs.end()) {
          currentGenericTypes = clsEnv->second;
        }
      }
    }
    defineFunction(fn, std::get<1>(prototypes[pi]), std::get<2>(prototypes[pi]));
    currentGenericTypes = std::move(savedGenerics);
  }

  // Return 0 for top-level function
  builder->CreateRet(ConstantInt::getSigned(builder->getInt64Ty(), 0));
}

auto Codegen::dump() -> void { theModule->print(outs(), nullptr); }

auto Codegen::visit(const Statement* node) -> void {
  lesma::print("Visited a blank statement\n{}", node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const Expression* node) -> void {
  lesma::print("Visited a blank expression\n{}", node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const TypeExpr* node) -> void {
  // For primitive types, cache them so they survive beyond result's lifetime
  // This is needed because setReturnType and similar store raw Type* pointers
  if (node->getType() == TokenType::INT_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT8_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt8Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT16_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt16Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT32_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt32Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT32_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, builder->getFloatTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::BOOL_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::STRING_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::VOID_TYPE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::PTR_TYPE) {
    node->getElementType()->accept(*this);
    // Function type is already a pointer at LLVM level; parser uses *func for
    // consistency, so do not add another pointer layer.
    if (!result->getType()->is(BaseType::TY_FUNCTION)) {
      auto* type = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), result->getType()));
      result = std::make_unique<Value>(type);
    }
  } else if (node->getType() == TokenType::FUNC_TYPE) {
    node->getReturnType()->accept(*this);
    auto retType = std::move(result);
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<lesma::Type*> paramTypes;
    std::vector<llvm::Type*> paramLLVMTypes;
    for (auto* paramType : node->getParams()) {
      paramType->accept(*this);
      paramLLVMTypes.push_back(result->getType()->getLlvmType());
      paramTypes.push_back(result->getType());
      fields.push_back(std::make_unique<Field>(result->getName(), result->getType()));
    }

    // With opaque pointers, function pointer types are just `ptr`
    // The actual function signature is tracked in Lesma's Type system via
    // fields
    auto funcType =
        std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
    funcType->setReturnType(retType->getType());
    result = std::make_unique<Value>(cacheType(std::move(funcType)));
  } else if (node->getType() == TokenType::CUSTOM_TYPE) {
    auto git = currentGenericTypes.find(node->getName());
    if (git != currentGenericTypes.end()) {
      result = std::make_unique<Value>(git->second);
      return;
    }
    std::vector<lesma::Type*> explicitTypeArgs;
    for (auto* typeArg : node->getTypeArgs()) {
      typeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
    }
    if (node->getName() == "__buffer") {
      if (explicitTypeArgs.size() != 1U) {
        throw CodegenError(node->getSpan(), "__buffer<T> expects exactly one type argument");
      }
      auto* type =
          cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, explicitTypeArgs.front()));
      type->setDisplayName(node->getName());
      getOrCreateLlvmType(type);
      result = std::make_unique<Value>(type);
      return;
    }
    auto* typ = scope->lookupType(node->getName());
    auto* sym = scope->lookupStruct(node->getName());
    if (typ == nullptr && sym == nullptr) {
      throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());
    }
    if (!explicitTypeArgs.empty()) {
      const Class* templateClass = nullptr;
      if (auto gitClass = genericClasses.find(node->getName()); gitClass != genericClasses.end()) {
        templateClass = gitClass->second;
      } else if (sym != nullptr && sym->getGenericClassTemplate() != nullptr) {
        templateClass = static_cast<const Class*>(sym->getGenericClassTemplate());
      }
      if (templateClass == nullptr) {
        throw CodegenError(node->getSpan(), "Type {} is not a generic class", node->getName());
      }
      sym = specializeClass(templateClass, {}, explicitTypeArgs);
      typ = sym->getType();
    }
    if (sym->getType()->getLlvmType() == nullptr) {
      getOrCreateLlvmType(sym->getType());
    }

    result = std::make_unique<Value>(*sym);
  } else {
    throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
  }
}

auto Codegen::getOrCreateListStructType(lesma::Type* listType) -> llvm::StructType* {
  std::string typeName = "lesma.list";
  if (listType != nullptr && listType->getElementType() != nullptr) {
    getOrCreateLlvmType(listType->getElementType());
    typeName += "." + MangleUtils::getTypeMangledName({}, listType->getElementType());
  }
  auto existing = listStructTypes.find(typeName);
  if (existing != listStructTypes.end()) {
    return existing->second;
  }
  auto* structType = llvm::StructType::create(
      theModule->getContext(),
      {builder->getPtrTy(), builder->getInt64Ty(), builder->getInt64Ty()},
      typeName, false);
  listStructTypes[typeName] = structType;
  return structType;
}

auto Codegen::getListStoredElementType(lesma::Type* listType) -> llvm::Type* {
  auto* elementType = listType->getElementType();
  if (elementType != nullptr && elementType->is(BaseType::TY_CLASS)) {
    return builder->getPtrTy();
  }
  return getOrCreateLlvmType(elementType);
}

auto Codegen::getListStoredElementValue(llvm::SMRange span, lesma::Value* value,
                                        lesma::Type* elementType) -> llvm::Value* {
  if (elementType != nullptr && elementType->is(BaseType::TY_CLASS)) {
    return value->getLlvmValue();
  }
  auto castVal = cast(span, value, elementType);
  return castVal->getLlvmValue();
}

auto Codegen::emitCalloc(llvm::Value* count, llvm::Value* size, const llvm::Twine& name)
    -> llvm::Value* {
  auto callocFn = theModule->getOrInsertFunction(
      "calloc",
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty(), builder->getInt64Ty()},
                              false));
  return builder->CreateCall(callocFn, {count, size}, name);
}

auto Codegen::emitMalloc(llvm::Value* size, const llvm::Twine& name) -> llvm::Value* {
  auto mallocFn = theModule->getOrInsertFunction(
      "malloc", llvm::FunctionType::get(builder->getPtrTy(), {builder->getInt64Ty()}, false));
  return builder->CreateCall(mallocFn, {size}, name);
}

auto Codegen::emitRealloc(llvm::Value* ptr, llvm::Value* size, const llvm::Twine& name)
    -> llvm::Value* {
  auto reallocFn = theModule->getOrInsertFunction(
      "realloc",
      llvm::FunctionType::get(builder->getPtrTy(), {builder->getPtrTy(), builder->getInt64Ty()},
                              false));
  return builder->CreateCall(reallocFn, {ptr, size}, name);
}

auto Codegen::emitFree(llvm::Value* ptr) -> void {
  auto freeFn = theModule->getOrInsertFunction(
      "free", llvm::FunctionType::get(builder->getVoidTy(), {builder->getPtrTy()}, false));
  builder->CreateCall(freeFn, {ptr});
}

auto Codegen::emitExit(int code) -> void {
  auto exitFn = theModule->getOrInsertFunction(
      "exit", llvm::FunctionType::get(builder->getVoidTy(), {builder->getInt64Ty()}, false));
  builder->CreateCall(exitFn, {builder->getInt64(code)});
}

auto Codegen::emitListLength(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* lenPtr = builder->CreateStructGEP(structType, listHandle, 1, "list.len.ptr");
  return builder->CreateLoad(builder->getInt64Ty(), lenPtr, "list.len");
}

auto Codegen::emitListCapacity(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* capPtr = builder->CreateStructGEP(structType, listHandle, 2, "list.cap.ptr");
  return builder->CreateLoad(builder->getInt64Ty(), capPtr, "list.cap");
}

auto Codegen::emitListDataPtr(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* dataPtrPtr = builder->CreateStructGEP(structType, listHandle, 0, "list.data.ptr.ptr");
  return builder->CreateLoad(builder->getPtrTy(), dataPtrPtr, "list.data.ptr");
}

auto Codegen::emitStoreListDataPtr(lesma::Type* listType, llvm::Value* listHandle,
                                   llvm::Value* dataValue)
    -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 0, "list.data.slot");
  builder->CreateStore(dataValue, slot);
}

auto Codegen::emitStoreListLength(lesma::Type* listType, llvm::Value* listHandle, llvm::Value* length)
    -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 1, "list.len.slot");
  builder->CreateStore(length, slot);
}

auto Codegen::emitStoreListCapacity(lesma::Type* listType, llvm::Value* listHandle,
                                    llvm::Value* capacity) -> void {
  auto* structType = getOrCreateListStructType(listType);
  auto* slot = builder->CreateStructGEP(structType, listHandle, 2, "list.cap.slot");
  builder->CreateStore(capacity, slot);
}

auto Codegen::emitListBoundsCheck(llvm::SMRange span, lesma::Type* listType, llvm::Value* listHandle,
                                  llvm::Value* index) -> void {
  (void) span;
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* okBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.bounds.ok", parentFunction);
  auto* failBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.bounds.fail", parentFunction);
  auto* length = emitListLength(listType, listHandle);
  auto* nonNegative = builder->CreateICmpSGE(index, builder->getInt64(0));
  auto* inRange = builder->CreateICmpSLT(index, length);
  builder->CreateCondBr(builder->CreateLogicalAnd(nonNegative, inRange), okBlock, failBlock);

  builder->SetInsertPoint(failBlock);
  emitExit(1);
  builder->CreateUnreachable();

  builder->SetInsertPoint(okBlock);
}

auto Codegen::emitListElementPointer(llvm::SMRange span, lesma::Type* listType, llvm::Value* listHandle,
                                     llvm::Value* index) -> llvm::Value* {
  emitListBoundsCheck(span, listType, listHandle, index);
  auto* dataPtr = emitListDataPtr(listType, listHandle);
  return builder->CreateGEP(getListStoredElementType(listType), dataPtr, index, "list.elem.ptr");
}

auto Codegen::emitListEnsureCapacity(lesma::Type* listType, llvm::Value* listHandle,
                                     llvm::Value* minCapacity) -> void {
  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* currentBlock = builder->GetInsertBlock();
  auto* growBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.grow", parentFunction);
  auto* doneBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.done", parentFunction);
  auto* capacity = emitListCapacity(listType, listHandle);
  auto* currentData = emitListDataPtr(listType, listHandle);
  builder->CreateCondBr(builder->CreateICmpSGE(capacity, minCapacity), doneBlock, growBlock);

  builder->SetInsertPoint(growBlock);
  auto* doubledCap = builder->CreateMul(capacity, builder->getInt64(2), "list.cap.doubled");
  auto* baseCap = builder->CreateSelect(builder->CreateICmpEQ(capacity, builder->getInt64(0)),
                                        builder->getInt64(1), doubledCap);
  auto* newCap = builder->CreateSelect(builder->CreateICmpSGE(baseCap, minCapacity), baseCap,
                                       minCapacity, "list.cap.new");
  auto* elementSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(getListStoredElementType(listType))
                            .getFixedValue());
  auto* newBytes = builder->CreateMul(newCap, elementSize, "list.grow.bytes");
  auto* allocBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.alloc", parentFunction);
  auto* reallocBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.grow.realloc", parentFunction);
  builder->CreateCondBr(
      builder->CreateICmpEQ(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
      allocBlock, reallocBlock);

  builder->SetInsertPoint(allocBlock);
  auto* allocatedData = emitMalloc(newBytes, "list.grow.alloc");
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(reallocBlock);
  auto* reallocatedData = emitRealloc(currentData, newBytes, "list.grow.realloc");
  builder->CreateBr(doneBlock);

  builder->SetInsertPoint(doneBlock);
  auto* newData = builder->CreatePHI(builder->getPtrTy(), 3, "list.grow.result");
  auto* finalCapacity = builder->CreatePHI(builder->getInt64Ty(), 3, "list.grow.capacity");
  newData->addIncoming(currentData, currentBlock);
  newData->addIncoming(allocatedData, allocBlock);
  newData->addIncoming(reallocatedData, reallocBlock);
  finalCapacity->addIncoming(capacity, currentBlock);
  finalCapacity->addIncoming(newCap, allocBlock);
  finalCapacity->addIncoming(newCap, reallocBlock);
  emitStoreListDataPtr(listType, listHandle, newData);
  emitStoreListCapacity(listType, listHandle, finalCapacity);
}

auto Codegen::emitListDeepCopy(lesma::Type* listType, llvm::Value* listHandle) -> llvm::Value* {
  auto* structType = getOrCreateListStructType(listType);
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* newHandle = emitMalloc(headerSize, "list.copy.header");
  auto* length = emitListLength(listType, listHandle);
  auto* capacity = emitListCapacity(listType, listHandle);
  emitStoreListLength(listType, newHandle, length);
  emitStoreListCapacity(listType, newHandle, capacity);
  auto* elementType = listType->getElementType();
  auto* elementLlvmType = getListStoredElementType(listType);
  auto* elementSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue());
  auto* hasCapacity = builder->CreateICmpSGT(capacity, builder->getInt64(0));
  auto* dataSlot = builder->CreateAlloca(builder->getPtrTy(), nullptr, "list.copy.data.slot");
  builder->CreateStore(llvm::ConstantPointerNull::get(builder->getPtrTy()), dataSlot);

  llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
  auto* copyElementsBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.copy.elements", parentFunction);
  auto* doneBlock =
      llvm::BasicBlock::Create(theModule->getContext(), "list.copy.done", parentFunction);
  builder->CreateCondBr(hasCapacity, copyElementsBlock, doneBlock);

  builder->SetInsertPoint(copyElementsBlock);
  auto* newBytes = builder->CreateMul(capacity, elementSize, "list.copy.bytes");
  auto* newData = emitMalloc(newBytes, "list.copy.data");
  builder->CreateStore(newData, dataSlot);
  auto isNestedListLike = [elementType]() -> bool {
    if (elementType == nullptr || !elementType->is(BaseType::TY_CLASS)) {
      return false;
    }
    auto fields = elementType->getFields();
    return !fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_ARRAY);
  };
  if (elementType->is(BaseType::TY_ARRAY) || isNestedListLike()) {
    auto* indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "list.copy.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
    auto* loopCond =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.cond", parentFunction);
    auto* loopBody =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.body", parentFunction);
    auto* loopInc =
        llvm::BasicBlock::Create(theModule->getContext(), "list.copy.loop.inc", parentFunction);
    builder->CreateBr(loopCond);

    builder->SetInsertPoint(loopCond);
    auto* index = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    builder->CreateCondBr(builder->CreateICmpSLT(index, length), loopBody, doneBlock);

    builder->SetInsertPoint(loopBody);
    auto* oldElementPtr = emitListElementPointer({}, listType, listHandle, index);
    auto* oldElement = builder->CreateLoad(elementLlvmType, oldElementPtr);
    llvm::Value* copiedElement = nullptr;
    if (elementType->is(BaseType::TY_ARRAY)) {
      copiedElement = emitListDeepCopy(elementType, oldElement);
    } else {
      lesma::Value oldValue("", elementType, oldElement);
      auto copiedValue = callMethodByName({}, &oldValue, "copy");
      copiedElement = copiedValue->getLlvmValue();
    }
    auto* newElementPtr = builder->CreateGEP(elementLlvmType, newData, index, "list.copy.elem.ptr");
    builder->CreateStore(copiedElement, newElementPtr);
    builder->CreateBr(loopInc);

    builder->SetInsertPoint(loopInc);
    auto* nextIndex = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                         builder->getInt64(1));
    builder->CreateStore(nextIndex, indexPtr);
    builder->CreateBr(loopCond);
  } else {
    auto memcpyFn = theModule->getOrInsertFunction(
        "memcpy",
        llvm::FunctionType::get(builder->getPtrTy(),
                                {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()},
                                false));
    builder->CreateCall(memcpyFn, {newData, emitListDataPtr(listType, listHandle), newBytes});
    builder->CreateBr(doneBlock);
  }

  builder->SetInsertPoint(doneBlock);
  emitStoreListDataPtr(listType, newHandle, builder->CreateLoad(builder->getPtrTy(), dataSlot));
  return newHandle;
}

auto Codegen::isListIntrinsicName(const std::string& functionName) const -> bool {
  return functionName == "__list_len" || functionName == "__list_push" ||
         functionName == "__list_pop" || functionName == "__list_clear" ||
         functionName == "__list_copy" || functionName == "__list_get" ||
         functionName == "__list_set" || functionName == "__buffer_new" ||
         functionName == "__buffer_len" || functionName == "__buffer_push" ||
         functionName == "__buffer_pop" || functionName == "__buffer_clear" ||
         functionName == "__buffer_copy" || functionName == "__buffer_get" ||
         functionName == "__buffer_set";
}

auto Codegen::genListIntrinsicCall(const FuncCall* node, const std::vector<lesma::Type*>& paramTypes,
                                   const std::vector<llvm::Value*>& paramsLLVM)
    -> std::unique_ptr<lesma::Value> {
  if (node->getName() == "__buffer_new") {
    auto explicitTypeArgs = node->getExplicitTypeArgs();
    if (explicitTypeArgs.size() != 1U) {
      throw CodegenError(node->getSpan(), "__buffer_new<T>() expects exactly one type argument");
    }
    explicitTypeArgs.front()->accept(*this);
    auto* listType = cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, result->getType()));
    listType->setDisplayName("__buffer");
    getOrCreateLlvmType(listType);
    auto* listStructTy = getOrCreateListStructType(listType);
    auto* headerSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
    auto* listHandle = emitMalloc(headerSize, "buffer.header");
    emitStoreListDataPtr(listType, listHandle, llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(listType, listHandle, builder->getInt64(0));
    emitStoreListCapacity(listType, listHandle, builder->getInt64(0));
    return std::make_unique<Value>("", listType, listHandle);
  }
  if (paramTypes.empty() || paramsLLVM.empty()) {
    throw CodegenError(node->getSpan(), "List intrinsic {} requires a list argument", node->getName());
  }
  auto* listType = paramTypes.front();
  auto* listHandle = paramsLLVM.front();
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) || listType->getElementType() == nullptr) {
    throw CodegenError(node->getSpan(), "List intrinsic {} requires list<T>", node->getName());
  }

  if (node->getName() == "__list_len" || node->getName() == "__buffer_len") {
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty())),
        emitListLength(listType, listHandle));
  }
  if (node->getName() == "__list_copy" || node->getName() == "__buffer_copy") {
    return std::make_unique<Value>("", listType, emitListDeepCopy(listType, listHandle));
  }
  if (node->getName() == "__list_clear" || node->getName() == "__buffer_clear") {
    auto* currentData = emitListDataPtr(listType, listHandle);
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* freeBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.clear.free", parentFunction);
    auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.clear.done", parentFunction);
    builder->CreateCondBr(
        builder->CreateICmpNE(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
        freeBlock, doneBlock);
    builder->SetInsertPoint(freeBlock);
    emitFree(currentData);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    emitStoreListDataPtr(listType, listHandle, llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(listType, listHandle, builder->getInt64(0));
    emitStoreListCapacity(listType, listHandle, builder->getInt64(0));
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }
  if (node->getName() == "__list_push" || node->getName() == "__buffer_push") {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__list_push expects list and value");
    }
    auto* length = emitListLength(listType, listHandle);
    auto* nextLength = builder->CreateAdd(length, builder->getInt64(1));
    emitListEnsureCapacity(listType, listHandle, nextLength);
    auto* elementPtr = builder->CreateGEP(getListStoredElementType(listType),
                                          emitListDataPtr(listType, listHandle), length,
                                          "list.push.ptr");
    builder->CreateStore(paramsLLVM[1], elementPtr);
    emitStoreListLength(listType, listHandle, nextLength);
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }
  if (node->getName() == "__list_get" || node->getName() == "__buffer_get") {
    if (paramTypes.size() != 2U || paramsLLVM.size() != 2U) {
      throw CodegenError(node->getSpan(), "__list_get expects list and index");
    }
    auto* elementPtr = emitListElementPointer(node->getSpan(), listType, listHandle, paramsLLVM[1]);
    return std::make_unique<Value>("", listType->getElementType(),
                                   builder->CreateLoad(getListStoredElementType(listType), elementPtr));
  }
  if (node->getName() == "__list_set" || node->getName() == "__buffer_set") {
    if (paramTypes.size() != 3U || paramsLLVM.size() != 3U) {
      throw CodegenError(node->getSpan(), "__list_set expects list, index, and value");
    }
    auto* elementPtr = emitListElementPointer(node->getSpan(), listType, listHandle, paramsLLVM[1]);
    builder->CreateStore(paramsLLVM[2], elementPtr);
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }
  if (node->getName() == "__list_pop" || node->getName() == "__buffer_pop") {
    auto* length = emitListLength(listType, listHandle);
    emitListBoundsCheck(node->getSpan(), listType, listHandle,
                        builder->CreateSub(length, builder->getInt64(1)));
    auto* newLength = builder->CreateSub(length, builder->getInt64(1), "list.pop.len");
    auto* elementPtr = builder->CreateGEP(getListStoredElementType(listType),
                                          emitListDataPtr(listType, listHandle), newLength,
                                          "list.pop.ptr");
    auto* poppedValue = builder->CreateLoad(getListStoredElementType(listType), elementPtr);
    emitStoreListLength(listType, listHandle, newLength);
    return std::make_unique<Value>("", listType->getElementType(), poppedValue);
  }

  throw CodegenError(node->getSpan(), "Unknown list intrinsic {}", node->getName());
}

auto Codegen::getOrCreateLlvmType(lesma::Type* type) -> llvm::Type* {
  if (type->getLlvmType() != nullptr) {
    return type->getLlvmType();
  }
  switch (type->getBaseType()) {
  case BaseType::TY_GENERIC: {
    auto it = currentGenericTypes.find(type->getGenericName());
    if (it == currentGenericTypes.end()) {
      throw CodegenError({}, "Unknown generic type {}", type->getGenericName());
    }
    return getOrCreateLlvmType(it->second);
  }
  case BaseType::TY_INT:
    type->setLlvmType(builder->getInt64Ty());
    break;
  case BaseType::TY_FLOAT:
    type->setLlvmType(builder->getDoubleTy());
    break;
  case BaseType::TY_BOOL:
    type->setLlvmType(builder->getInt1Ty());
    break;
  case BaseType::TY_STRING:
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_VOID:
    type->setLlvmType(builder->getVoidTy());
    break;
  case BaseType::TY_PTR:
    if (type->getElementType() != nullptr) {
      getOrCreateLlvmType(type->getElementType());
    }
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_ARRAY: {
    if (type->getElementType() != nullptr) {
      getOrCreateLlvmType(type->getElementType());
    }
    getOrCreateListStructType(type);
    type->setLlvmType(builder->getPtrTy());
    break;
  }
  case BaseType::TY_FUNCTION:
    getOrCreateLlvmType(type->getReturnType());
    for (auto* f : type->getFields()) {
      getOrCreateLlvmType(f->type);
    }
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_CLASS:
  case BaseType::TY_ENUM: {
    // Create opaque struct first to break recursion (e.g. class with field
    // *Self)
    llvm::StructType* st = llvm::StructType::create(theModule->getContext());
    type->setLlvmType(st);
    std::vector<llvm::Type*> elementTypes;
    for (auto* f : type->getFields()) {
      elementTypes.push_back(getOrCreateLlvmType(f->type));
    }
    if (elementTypes.empty()) {
      elementTypes.push_back(builder->getInt8Ty());
    }
    st->setBody(elementTypes);
    break;
  }
  default:
    type->setLlvmType(builder->getPtrTy());
    break;
  }
  return type->getLlvmType();
}

void Codegen::bindGenericsFromTypePair(const TypeExpr* declared, lesma::Type* actual,
                                       const std::unordered_set<std::string>& genericNameSet,
                                       std::unordered_map<std::string, lesma::Type*>& env) {
  if (declared == nullptr || actual == nullptr) {
    return;
  }
  if (declared->getType() == TokenType::CUSTOM_TYPE) {
    const std::string& name = declared->getName();
    if (genericNameSet.contains(name) && !env.contains(name)) {
      env[name] = actual;
      return;
    }
    if (name == "__buffer" && actual->is(BaseType::TY_ARRAY) && actual->getElementType() != nullptr) {
      auto typeArgs = declared->getTypeArgs();
      if (typeArgs.size() == 1U) {
        bindGenericsFromTypePair(typeArgs.front(), actual->getElementType(), genericNameSet, env);
      }
    }
    return;
  }
  if (declared->getType() == TokenType::PTR_TYPE && actual->is(BaseType::TY_PTR) &&
      declared->getElementType() != nullptr && actual->getElementType() != nullptr) {
    bindGenericsFromTypePair(declared->getElementType(), actual->getElementType(), genericNameSet,
                             env);
    return;
  }
  if (declared->getType() == TokenType::FUNC_TYPE && actual->is(BaseType::TY_FUNCTION)) {
    if (declared->getReturnType() != nullptr && actual->getReturnType() != nullptr) {
      bindGenericsFromTypePair(declared->getReturnType(), actual->getReturnType(), genericNameSet,
                               env);
    }
    auto declParams = declared->getParams();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < declParams.size() && i < actualFields.size(); ++i) {
      bindGenericsFromTypePair(declParams[i], actualFields[i]->type, genericNameSet, env);
    }
  }
}

auto Codegen::specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                                 const std::vector<std::string>& genericNames,
                                 const std::vector<lesma::Type*>& explicitTypeArgs)
    -> lesma::Value* {
  for (auto* paramType : paramTypes) {
    if (paramType != nullptr) {
      getOrCreateLlvmType(paramType);
    }
  }
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }
  std::string key =
      getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
  if (auto it = specializedFunctions.find(key); it != specializedFunctions.end()) {
    return it->second;
  }

  auto saved = currentGenericTypes;
  std::unordered_map<std::string, lesma::Type*> env = saved;
  if (!explicitTypeArgs.empty()) {
    if (explicitTypeArgs.size() != genericNames.size()) {
      throw CodegenError(
          node->getSpan(),
          "Explicit type argument count {} does not match generic parameter count {}",
          explicitTypeArgs.size(), genericNames.size());
    }
    for (size_t i = 0; i < genericNames.size(); ++i) {
      env[genericNames[i]] = explicitTypeArgs[i];
    }
  }
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  auto templateParams = node->getParameters();
  size_t offset = (selfSymbol != nullptr) ? 1U : 0U;
  for (size_t i = 0; i < templateParams.size() && (i + offset) < paramTypes.size(); ++i) {
    TypeExpr* declType = templateParams[i]->type.get();
    if (declType != nullptr) {
      bindGenericsFromTypePair(declType, paramTypes[i + offset], genericNameSet, env);
    }
  }
  currentGenericTypes = env;

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> concreteParamTypes;
  if (selfSymbol != nullptr) {
    fields.push_back(std::make_unique<Field>("self", selfSymbol->getType()));
    concreteParamTypes.push_back(selfSymbol->getType());
  }
  for (auto* param : node->getParameters()) {
    param->type->accept(*this);
    fields.push_back(std::make_unique<Field>(param->name, result->getType()));
    concreteParamTypes.push_back(result->getType());
  }
  node->getReturnType()->accept(*this);
  auto* returnType = result->getType();
  std::vector<llvm::Type*> paramLLVMTypes;
  for (auto* t : concreteParamTypes) {
    getOrCreateLlvmType(t);
    paramLLVMTypes.push_back(t->getLlvmType());
  }
  getOrCreateLlvmType(returnType);
  auto funcType =
      std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  auto* typePtr = cacheType(std::move(funcType));
  auto mangledName =
      getMangledName(node->getSpan(), node->getName(), concreteParamTypes, selfSymbol != nullptr);
  auto func = std::make_unique<Value>(node->getName(), typePtr);
  func->setCategory(ValueCategory::CALLABLE_SYMBOL);
  func->setMangledName(mangledName);
  func->setExported(node->isExported());
  auto linkage = node->isExported() ? Function::ExternalLinkage : Function::PrivateLinkage;
  llvm::Type* llvmReturnType =
      returnType->is(BaseType::TY_CLASS) ? builder->getPtrTy() : returnType->getLlvmType();
  auto* llvmFuncType = FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
  auto* llvmFunc = Function::Create(llvmFuncType, linkage, mangledName, *theModule);
  typePtr->setLlvmType(llvmFuncType);
  func->setLlvmValue(llvmFunc);
  auto* funcPtr = func.get();
  scope->insertSymbol(std::move(func));
  prototypes.emplace_back(funcPtr, node, selfSymbol);
  specializedFunctions.emplace(std::move(key), funcPtr);
  specializationEnvs.emplace(funcPtr, currentGenericTypes);
  currentGenericTypes = std::move(saved);
  return funcPtr;
}

auto Codegen::specializeClass(const Class* node,
                              const std::vector<lesma::Type*>& constructorArgTypes,
                              const std::vector<lesma::Type*>& explicitTypeArgs) -> lesma::Value* {
  auto genericNames = node->getGenericParams();
  for (auto* constructorArgType : constructorArgTypes) {
    if (constructorArgType != nullptr) {
      getOrCreateLlvmType(constructorArgType);
    }
  }
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }

  const FuncDecl* constructorDecl = nullptr;
  for (auto* method : node->getMethods()) {
    if (method->getName() == "new") {
      constructorDecl = method;
      break;
    }
  }

  std::unordered_map<std::string, lesma::Type*> env;
  if (!explicitTypeArgs.empty()) {
    if (explicitTypeArgs.size() != genericNames.size()) {
      throw CodegenError(node->getSpan(),
                         "Explicit type argument count {} does not match generic class parameter "
                         "count {}",
                         explicitTypeArgs.size(), genericNames.size());
    }
    for (size_t i = 0; i < genericNames.size(); ++i) {
      env[genericNames[i]] = explicitTypeArgs[i];
    }
  }
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  if (constructorDecl != nullptr) {
    auto params = constructorDecl->getParameters();
    for (size_t i = 0; i < params.size() && i < constructorArgTypes.size(); ++i) {
      TypeExpr* declType = params[i]->type.get();
      if (declType != nullptr) {
        bindGenericsFromTypePair(declType, constructorArgTypes[i], genericNameSet, env);
      }
    }
  }

  for (const auto& gn : genericNames) {
    if (!env.contains(gn)) {
      throw CodegenError(node->getSpan(),
                         "Generic class {} requires type arguments for all parameters; "
                         "could not infer {} from constructor",
                         node->getIdentifier(), gn);
    }
  }

  std::string key = (alias.empty() ? "" : "&" + alias + "=>") + node->getIdentifier();
  for (const auto& gn : genericNames) {
    key += "|" + MangleUtils::getTypeMangledName(node->getSpan(), env[gn]);
  }
  if (auto it = specializedClasses.find(key); it != specializedClasses.end()) {
    return it->second;
  }

  auto saved = currentGenericTypes;
  auto* savedSelfSymbol = selfSymbol;
  currentGenericTypes = env;

  std::string concreteName = (alias.empty() ? "" : alias + "_") + node->getIdentifier();
  for (const auto& gn : genericNames) {
    concreteName += "_" + MangleUtils::getTypeMangledName(node->getSpan(), env[gn]);
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<llvm::Type*> elementLLVMTypes;
  for (auto* field : node->getFields()) {
    if (field->getType() != nullptr) {
      field->getType()->accept(*this);
    } else {
      field->getValue()->accept(*this);
    }
    getOrCreateLlvmType(result->getType());
    elementLLVMTypes.push_back(result->getType()->getLlvmType());
    std::unique_ptr<Value> defaultVal;
    if (field->getValue() != nullptr) {
      defaultVal = std::move(result);
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
      } else {
        result = std::make_unique<Value>(*defaultVal);
      }
    }
    fields.push_back(std::make_unique<Field>(field->getIdentifier()->getValue(), result->getType(),
                                             std::move(defaultVal)));
  }

  auto* structType =
      llvm::StructType::create(theModule->getContext(), elementLLVMTypes, concreteName);
  auto type = std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
  std::string displayName = node->getIdentifier() + "<";
  for (size_t i = 0; i < genericNames.size(); ++i) {
    if (i > 0U) {
      displayName += ", ";
    }
    displayName += env[genericNames[i]]->toString();
  }
  displayName += ">";
  type->setDisplayName(displayName);
  auto* typePtr = type.get();
  scope->insertType(concreteName, std::move(type));

  auto structSymbol = std::make_unique<Value>(concreteName, typePtr);
  structSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  structSymbol->setExported(node->isExported());
  auto* structSymbolPtr = structSymbol.get();
  scope->insertSymbol(std::move(structSymbol));
  specializedClasses.emplace(key, structSymbolPtr);
  specializedClassSymbolsByType[typePtr] = structSymbolPtr;
  specializedClassTypeEnvs[typePtr] = env;

  auto* selfType =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  methodSelfSymbols.push_back(std::make_unique<Value>(concreteName, selfType));
  methodSelfSymbols.back()->setExported(node->isExported());
  selfSymbol = methodSelfSymbols.back().get();

  auto hasConstructor = false;
  for (auto* method : node->getMethods()) {
    method->accept(*this);
    if (method->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = {selfSymbol->getType()};
      auto* constructor = scope->lookupFunction("new", constructorParams);
      structSymbolPtr->setConstructor(constructor);
    }
  }
  selfSymbol = nullptr;

  if (!hasConstructor) {
    throw CodegenError(node->getSpan(), "Generic class {} has no constructors",
                       node->getIdentifier());
  }
  selfSymbol = savedSelfSymbol;
  currentGenericTypes = std::move(saved);
  return structSymbolPtr;
}

auto Codegen::visit(const Compound* node) -> void {
  for (auto* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

auto Codegen::visit(const VarDecl* node) -> void {
  const std::string name = node->getIdentifier()->getValue();
  auto* existing = [&]() -> Value* {
    for (auto* symbol : scope->getSymbols()) {
      if (symbol->getName() == name) {
        return symbol;
      }
    }
    return nullptr;
  }();

  if (existing != nullptr && existing->getLlvmValue() == nullptr) {
    // Reuse symbol from typecheck; fill in LLVM value
    lesma::Type* type = existing->getType();
    std::unique_ptr<lesma::Value> valueResult;
    if (node->getValue() != nullptr) {
      node->getValue()->accept(*this);
      valueResult = std::move(result);
      if (type->is(BaseType::TY_CLASS) && valueResult->getType() != nullptr &&
          valueResult->getType()->is(BaseType::TY_CLASS)) {
        type = valueResult->getType();
      } else if (type->is(BaseType::TY_CLASS) && valueResult->getType() != nullptr &&
                 valueResult->getType()->is(BaseType::TY_PTR) &&
                 valueResult->getType()->getElementType() != nullptr &&
                 valueResult->getType()->getElementType()->is(BaseType::TY_CLASS)) {
        type = valueResult->getType()->getElementType();
      }
    }
    getOrCreateLlvmType(type);
    if (type->is(BaseType::TY_CLASS)) {
      lesma::Type* ptrType =
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
      existing->setType(ptrType);
    }
    lesma::Type* storedType = existing->getType();
    const bool isPtrToClass = storedType->is(BaseType::TY_PTR) &&
                              storedType->getElementType() != nullptr &&
                              storedType->getElementType()->is(BaseType::TY_CLASS);
    llvm::Type* allocaTy = (storedType->is(BaseType::TY_CLASS) || isPtrToClass)
                               ? builder->getPtrTy()
                               : storedType->getLlvmType();
    auto* ptr = builder->CreateAlloca(allocaTy, nullptr, name);
    existing->setLlvmValue(ptr);
    existing->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    existing->setMutable(node->getMutability());
    if (valueResult != nullptr) {
      if (isPtrToClass && valueResult->getType() != nullptr && valueResult->getType()->is(BaseType::TY_PTR)) {
        builder->CreateStore(valueResult->getLlvmValue(), ptr);
      } else {
        lesma::Type* castTarget = isPtrToClass ? storedType->getElementType() : storedType;
        auto castVal = cast(node->getSpan(), valueResult.get(), castTarget);
        builder->CreateStore(castVal->getLlvmValue(), ptr);
      }
    }
    return;
  }

  lesma::Type* type = nullptr;
  std::unique_ptr<lesma::Value> val;

  if (node->getValue() != nullptr) {
    node->getValue()->accept(*this);
    val = std::move(result);
    type = val->getType();
  }

  if (node->getType() != nullptr) {
    node->getType()->accept(*this);
    type = result->getType();
  }

  getOrCreateLlvmType(type);
  auto* ptr = builder->CreateAlloca(type->getLlvmType(), nullptr, name);

  if (type->is(BaseType::TY_CLASS)) {
    type = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  }
  const bool isPtrToClass =
      type->is(BaseType::TY_PTR) && type->getElementType()->is(BaseType::TY_CLASS);
  auto symbol = std::make_unique<Value>(
      name, type, node->getType() != nullptr ? SymbolState::INITIALIZED : SymbolState::DECLARED);
  symbol->setLlvmValue(ptr);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));

  if (node->getValue() != nullptr) {
    if (isPtrToClass && val->getType() != nullptr && val->getType()->is(BaseType::TY_PTR)) {
      builder->CreateStore(val->getLlvmValue(), ptr);
    } else {
      lesma::Type* castTarget = isPtrToClass ? type->getElementType() : type;
      auto castVal = cast(node->getSpan(), val.get(), castTarget);
      builder->CreateStore(castVal->getLlvmValue(), ptr);
    }
  }
}

auto Codegen::visit(const If* node) -> void {
  auto* parentFct = builder->GetInsertBlock()->getParent();
  auto* bStart = llvm::BasicBlock::Create(theModule->getContext(), "if.start");
  auto* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "if.end");

  builder->CreateBr(bStart);
  bStart->insertInto(parentFct);
  builder->SetInsertPoint(bStart);

  for (unsigned long i = 0; i < node->getConds().size(); i++) {
    auto* bIfTrue = llvm::BasicBlock::Create(theModule->getContext(), "if.true");
    bIfTrue->insertInto(parentFct);
    auto* bIfFalse = bEnd;
    if (i + 1 < node->getConds().size()) {
      bIfFalse = llvm::BasicBlock::Create(theModule->getContext(), "if.false");
      bIfFalse->insertInto(parentFct);
    }

    node->getConds().at(i)->accept(*this);
    builder->CreateCondBr(result->getLlvmValue(), bIfTrue, bIfFalse);
    builder->SetInsertPoint(bIfTrue);

    node->getBlocks().at(i)->accept(*this);

    if (!isBreak && builder->GetInsertBlock()->getTerminator() == nullptr) {
      builder->CreateBr(bEnd);
    }

    builder->SetInsertPoint(bIfFalse);
  }

  bEnd->insertInto(parentFct);

  if (!isBreak) {
    builder->SetInsertPoint(bEnd);
  } else {
    isBreak = false;
  }
}

auto Codegen::visit(const While* node) -> void {
  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();

  // Create blocks
  llvm::BasicBlock* bCond = llvm::BasicBlock::Create(theModule->getContext(), "while.cond");
  llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(theModule->getContext(), "while");
  llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "while.end");

  breakBlocks.push(bEnd);
  continueBlocks.push(bCond);

  // Jump into condition block
  builder->CreateBr(bCond);

  // Fill condition block
  bCond->insertInto(parentFct);
  builder->SetInsertPoint(bCond);
  node->getCond()->accept(*this);
  builder->CreateCondBr(result->getLlvmValue(), bLoop, bEnd);

  // Fill while body block
  bLoop->insertInto(parentFct);
  builder->SetInsertPoint(bLoop);
  node->getBlock()->accept(*this);

  if (!isBreak) {
    builder->CreateBr(bCond);
  } else {
    isBreak = false;
  }

  // Fill loop end block
  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);

  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::visit(const ForIn* node) -> void {
  node->getIterable()->accept(*this);
  std::unique_ptr<lesma::Value> iterable = std::move(result);
  lesma::Type* listType = iterable->getType();
  llvm::Value* listHandle = iterable->getLlvmValue();
  if (listType != nullptr && listType->is(BaseType::TY_PTR) && listType->getElementType() != nullptr &&
      listType->getElementType()->is(BaseType::TY_CLASS)) {
    listType = listType->getElementType();
  }
  if (listType != nullptr && listType->is(BaseType::TY_CLASS)) {
    auto fields = listType->getFields();
    if (!fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_ARRAY)) {
      auto* storagePtr =
          builder->CreateStructGEP(cast<llvm::StructType>(getOrCreateLlvmType(listType)),
                                   iterable->getLlvmValue(), 0, "list.storage.ptr");
      listHandle = builder->CreateLoad(getOrCreateLlvmType(fields.front()->type), storagePtr,
                                       "list.storage");
      listType = fields.front()->type;
    }
  }
  SymbolTable* savedScope = scope;
  scope = node->getBodyScope() != nullptr ? node->getBodyScope() : savedScope->createChildBlock("for");
  Value* loopVar = scope->lookup(node->getIdentifier()->getValue());
  if (loopVar == nullptr) {
    throw CodegenError(node->getSpan(), "Missing loop variable symbol for for-in");
  }
  getOrCreateLlvmType(loopVar->getType());
  if (loopVar->getLlvmValue() == nullptr) {
    auto* elemPtr = builder->CreateAlloca(loopVar->getType()->getLlvmType(), nullptr, loopVar->getName());
    loopVar->setLlvmValue(elemPtr);
    loopVar->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  }
  scope = savedScope;

  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock* bCond = llvm::BasicBlock::Create(theModule->getContext(), "for.cond");
  llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(theModule->getContext(), "for");
  llvm::BasicBlock* bInc = llvm::BasicBlock::Create(theModule->getContext(), "for.inc");
  llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "for.end");
  std::unique_ptr<lesma::Value> iteratorValue;
  llvm::AllocaInst* indexPtr = nullptr;
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) || listType->getElementType() == nullptr) {
    iteratorValue = callMethodByName(node->getSpan(), iterable.get(), "__iter");
  } else {
    getOrCreateLlvmType(listType);
    indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "for.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
  }

  breakBlocks.push(bEnd);
  continueBlocks.push(bInc);
  builder->CreateBr(bCond);

  if (listType != nullptr && listType->is(BaseType::TY_ARRAY) && listType->getElementType() != nullptr) {
    bCond->insertInto(parentFct);
    builder->SetInsertPoint(bCond);
    auto* idxVal = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    auto* lenVal = emitListLength(listType, listHandle);
    builder->CreateCondBr(builder->CreateICmpSLT(idxVal, lenVal), bLoop, bEnd);

    bLoop->insertInto(parentFct);
    builder->SetInsertPoint(bLoop);
    auto* elemPtr = emitListElementPointer(node->getSpan(), listType, listHandle, idxVal);
    auto* elemVal = builder->CreateLoad(getListStoredElementType(listType), elemPtr);
    builder->CreateStore(elemVal, loopVar->getLlvmValue());
    scope = node->getBodyScope() != nullptr ? node->getBodyScope() : scope;
    node->getBlock()->accept(*this);
    scope = savedScope;

    if (!isBreak) {
      builder->CreateBr(bInc);
    } else {
      isBreak = false;
    }

    bInc->insertInto(parentFct);
    builder->SetInsertPoint(bInc);
    auto* nextIdx = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                       builder->getInt64(1));
    builder->CreateStore(nextIdx, indexPtr);
    builder->CreateBr(bCond);
  } else {
    bCond->insertInto(parentFct);
    builder->SetInsertPoint(bCond);
    auto hasNextValue = callMethodByName(node->getSpan(), iteratorValue.get(), "__has_next");
    builder->CreateCondBr(hasNextValue->getLlvmValue(), bLoop, bEnd);

    bLoop->insertInto(parentFct);
    builder->SetInsertPoint(bLoop);
    auto nextValue = callMethodByName(node->getSpan(), iteratorValue.get(), "__next");
    builder->CreateStore(nextValue->getLlvmValue(), loopVar->getLlvmValue());
    scope = node->getBodyScope() != nullptr ? node->getBodyScope() : scope;
    node->getBlock()->accept(*this);
    scope = savedScope;

    if (!isBreak) {
      builder->CreateBr(bInc);
    } else {
      isBreak = false;
    }

    bInc->insertInto(parentFct);
    builder->SetInsertPoint(bInc);
    builder->CreateBr(bCond);
  }

  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);
  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::visit(const FuncDecl* node) -> void {
  if (!node->getGenericParams().empty()) {
    auto savedGenerics = currentGenericTypes;
    for (const auto& name : node->getGenericParams()) {
      currentGenericTypes[name] = cacheType(std::make_unique<Type>(name));
    }
    if (selfSymbol != nullptr) {
      genericMethods[selfSymbol->getName()][node->getName()] = node;
    } else {
      genericFunctions[node->getName()] = node;
    }
    currentGenericTypes = std::move(savedGenerics);
    return;
  }
  if (selfSymbol != nullptr && node->getName() == "new" &&
      node->getReturnType()->getType() != TokenType::VOID_TYPE) {
    throw CodegenError(node->getSpan(), "Cannot create class method new with return type {}",
                       node->getReturnType()->getName());
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;
  bool shouldExport = node->isExported();

  if (selfSymbol != nullptr) {
    paramTypes.push_back(selfSymbol->getType());
    paramLLVMTypes.push_back(builder->getPtrTy());
    fields.push_back(std::make_unique<Field>("self", selfSymbol->getType()));
    shouldExport = selfSymbol->isExported();
  }

  for (auto* param : node->getParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->getType()->is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(node->getSpan(),
                         "Declared parameter type and default value do not match for {}",
                         param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType, std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* returnType = result->getType();
  getOrCreateLlvmType(returnType);

  lesma::Value* existingFunc = scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    auto normalizeFunctionParamType = [](lesma::Type* type) -> lesma::Type* {
      if (type != nullptr && type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
          type->getElementType()->is(BaseType::TY_CLASS)) {
        return type->getElementType();
      }
      return type;
    };
    for (auto* candidate : scope->getSymbols()) {
      if (candidate == nullptr || candidate->getName() != node->getName() ||
          !candidate->getType()->is(BaseType::TY_FUNCTION) || candidate->getLlvmValue() != nullptr) {
        continue;
      }
      auto candidateFields = candidate->getType()->getFields();
      if (candidateFields.size() != paramTypes.size()) {
        continue;
      }
      bool compatible = true;
      for (size_t i = 0; i < candidateFields.size(); ++i) {
        lesma::Type* formalType = normalizeFunctionParamType(candidateFields[i]->type);
        lesma::Type* actualType = normalizeFunctionParamType(paramTypes[i]);
        if ((formalType == nullptr) != (actualType == nullptr) ||
            (formalType != nullptr && !formalType->isEqual(actualType))) {
          compatible = false;
          break;
        }
      }
      if (compatible) {
        existingFunc = candidate;
        break;
      }
    }
  }
  if (!currentGenericTypes.empty() && (existingFunc == nullptr || existingFunc->getLlvmValue() == nullptr)) {
    existingFunc = nullptr;
  }
  if (existingFunc != nullptr && existingFunc->getLlvmValue() != nullptr) {
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto mangledName =
      getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
  auto makeCallableSignatureKey = [](const std::string& name,
                                     const std::vector<lesma::Type*>& types) -> std::string {
    std::string key = name;
    for (auto* type : types) {
      key += "|" + (type != nullptr ? type->toString() : "?");
    }
    return key;
  };
  std::string const signatureKey = makeCallableSignatureKey(node->getName(), paramTypes);
  auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  llvm::Type* llvmReturnType =
      returnType->is(BaseType::TY_CLASS) ? builder->getPtrTy() : returnType->getLlvmType();
  llvm::FunctionType* funcType = FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);
  auto loweredType = std::make_unique<Type>(BaseType::TY_FUNCTION, funcType, std::move(fields));
  loweredType->setReturnType(returnType);
  loweredType->setGenericParams(node->getGenericParams());
  loweredType->setVarArgs(node->getVarArgs());
  if (existingFunc != nullptr) {
    existingFunc->setType(cacheType(std::move(loweredType)));
    existingFunc->setLlvmValue(f);
    existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
    existingFunc->setMangledName(mangledName);
    specializedFunctions[mangledName] = existingFunc;
    specializedFunctions[signatureKey] = existingFunc;
    prototypes.emplace_back(existingFunc, node, selfSymbol);
    if (!currentGenericTypes.empty()) {
      specializationEnvs[existingFunc] = currentGenericTypes;
    }
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto funcSymbol = std::make_unique<Value>(node->getName(), cacheType(std::move(loweredType)), f);
  funcSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(mangledName);
  auto* funcSymbolPtr = funcSymbol.get();
  scope->insertSymbol(std::move(funcSymbol));
  specializedFunctions[mangledName] = funcSymbolPtr;
  specializedFunctions[signatureKey] = funcSymbolPtr;
  prototypes.emplace_back(funcSymbolPtr, node, selfSymbol);
  if (!currentGenericTypes.empty()) {
    specializationEnvs[funcSymbolPtr] = currentGenericTypes;
  }
  result = std::make_unique<Value>(*funcSymbolPtr);
}

auto Codegen::visit(const ExternFuncDecl* node) -> void {
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;

  for (auto* param : node->getParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->getType()->is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(node->getSpan(),
                         "Declared parameter type and default value do not match for {}",
                         param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType, std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* retType = nullptr;
  if (!result) {
    retType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  } else {
    retType = result->getType();
    getOrCreateLlvmType(retType);
  }

  lesma::Value* existingFunc = scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked extern symbol for {}",
                       node->getName());
  }
  if (existingFunc->getLlvmValue() != nullptr) {
    return;
  }

  Function* f = nullptr;
  if (theModule->getFunction(node->getName()) != nullptr) {
    f = theModule->getFunction(node->getName());
  } else {
    llvm::Type* llvmReturnType =
        retType->is(BaseType::TY_CLASS) ? builder->getPtrTy() : retType->getLlvmType();
    FunctionType* ft = FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
    f = llvm::cast<Function>(theModule->getOrInsertFunction(node->getName(), ft).getCallee());
    if (node->isExported()) {
      f->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }

  auto loweredType =
      std::make_unique<Type>(BaseType::TY_FUNCTION, f->getFunctionType(), std::move(fields));
  loweredType->setReturnType(retType);
  loweredType->setGenericParams(node->getGenericParams());
  loweredType->setVarArgs(node->getVarArgs());
  existingFunc->setType(cacheType(std::move(loweredType)));
  existingFunc->setLlvmValue(f);
  existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
  existingFunc->setMangledName(node->getName());
}

auto Codegen::visit(const Assignment* node) -> void {
  if (auto* subscript = dynamic_cast<SubscriptOp*>(node->getLeftHandSide())) {
    subscript->getLeft()->accept(*this);
    auto baseValue = std::move(result);
    if (baseValue != nullptr && baseValue->getType() != nullptr && node->getOperator() == TokenType::EQUAL) {
      subscript->getIndex()->accept(*this);
      auto indexValue = std::move(result);
      node->getRightHandSide()->accept(*this);
      auto rhsValue = std::move(result);
      result = callMethodByName(node->getSpan(), baseValue.get(),
                                std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                {indexValue.get(), rhsValue.get()});
      return;
    }
  }

  lesma::Value* lhs = nullptr;
  std::unique_ptr<lesma::Value> lhsOwner; // Holds owned lhs if from result
  isAssignment = true;
  bool isPtr = false;
  if (dynamic_cast<Literal*>(node->getLeftHandSide()) != nullptr) {
    auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide());
    auto* symbol = scope->lookup(lit->getValue());
    if (symbol == nullptr) {
      throw CodegenError(node->getSpan(), "Variable not found: {}", lit->getValue());
    }
    if (!symbol->getMutability()) {
      throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");
    }

    lhs = symbol;
  } else if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr ||
             dynamic_cast<SubscriptOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    lhsOwner = std::move(result);
    lhs = lhsOwner.get();
    isPtr = true;
  } else {
    throw CodegenError(node->getSpan(), "Unable to assign {} to {}",
                       node->getRightHandSide()->toString(sourceManager.get(), "", true),
                       node->getLeftHandSide()->toString(sourceManager.get(), "", true));
  }
  isAssignment = false;

  node->getRightHandSide()->accept(*this);
  auto value = cast(node->getSpan(), result.get(),
                    isPtr ? lhs->getType()->getElementType() : lhs->getType());

  switch (node->getOperator()) {
  case TokenType::EQUAL:
    builder->CreateStore(value->getLlvmValue(), lhs->getLlvmValue());
    break;
  case TokenType::PLUS_EQUAL:
  case TokenType::MINUS_EQUAL:
  case TokenType::SLASH_EQUAL:
  case TokenType::STAR_EQUAL:
  case TokenType::MOD_EQUAL:
    emitCompoundAssign(node->getSpan(), node->getOperator(), lhs, value.get());
    break;
  case TokenType::POWER_EQUAL:
    throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
  default:
    throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
  }
}

auto Codegen::visit(const Break* node) -> void {
  if (breakBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot break without being in a loop");
  }

  auto* block = breakBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::visit(const Continue* node) -> void {
  if (continueBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");
  }

  auto* block = continueBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::visit(const Return* node) -> void {
  // Check if it's top-level
  if (builder->GetInsertBlock()->getParent() == topLevelFunc) {
    throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");
  }

  // Execute all deferred statements
  for (auto* inst : deferStack.top()) {
    inst->accept(*this);
  }

  isReturn = true;

  if (node->getValue() == nullptr) {
    if (currentFunction->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->getSpan(),
                         "Return type does not match the function return type, expected {}, "
                         "actual void",
                         currentFunction->getType()->getReturnType()->toString());
    }
  } else {
    node->getValue()->accept(*this);
    getOrCreateLlvmType(result->getType());
    lesma::Type* actualType = result->getType();
    lesma::Type* declaredReturnType = currentFunction->getType()->getReturnType();
    if (actualType != nullptr && actualType->is(BaseType::TY_PTR) && actualType->getElementType() != nullptr &&
        actualType->getElementType()->is(BaseType::TY_CLASS)) {
      actualType = actualType->getElementType();
    }
    llvm::Type* actualReturnType =
        actualType != nullptr && actualType->is(BaseType::TY_CLASS) ? builder->getPtrTy()
                                                                    : result->getType()->getLlvmType();
    llvm::Type* expectedReturnType =
        declaredReturnType != nullptr && declaredReturnType->is(BaseType::TY_CLASS)
            ? builder->getPtrTy()
            : builder->getCurrentFunctionReturnType();
    if (actualReturnType == expectedReturnType) {
      builder->CreateRet(result->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(),
                         "Return type does not match the function return type, expected {}, "
                         "actual {}",
                         currentFunction->getType()->getReturnType()->toString(),
                         result->getType()->toString());
    }
  }
}

auto Codegen::visit(const Defer* node) -> void { deferStack.top().push_back(node->getStatement()); }

auto Codegen::visit(const UnimplementedStatement* node) -> void {
  throw CodegenError(node->getSpan(), "{}", node->getMessage());
}

auto Codegen::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Codegen::visit(const Import* node) -> void {
  compileModule(node->getSpan(), node->getFilePath(), node->isStd(), node->getAlias(),
                node->getImportAll(), node->getImportScope(), node->getImportedNames());
}

auto Codegen::visit(const Class* node) -> void {
  if (!node->getGenericParams().empty()) {
    genericClasses[node->getIdentifier()] = node;
    auto* genericSymbol = scope->lookupStruct(node->getIdentifier());
    if (genericSymbol != nullptr) {
      genericSymbol->setGenericClassTemplate(node);
    }
    return;
  }

  lesma::Value* existingStruct = scope->lookupStruct(node->getIdentifier());
  if (existingStruct == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked class symbol for {}",
                       node->getIdentifier());
  }

  lesma::Type* type = existingStruct->getType();
  if (type->getLlvmType() == nullptr) {
    std::vector<llvm::Type*> elementLLVMTypes;
    for (auto* f : type->getFields()) {
      elementLLVMTypes.push_back(getOrCreateLlvmType(f->type));
    }
    auto* structType =
        llvm::StructType::create(theModule->getContext(), elementLLVMTypes, node->getIdentifier());
    type->setLlvmType(structType);
  }

  auto* selfType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  methodSelfSymbols.push_back(std::make_unique<Value>(node->getIdentifier(), selfType));
  methodSelfSymbols.back()->setExported(node->isExported());
  selfSymbol = methodSelfSymbols.back().get();
  auto hasConstructor = false;
  for (auto* func : node->getMethods()) {
    func->accept(*this);
    if (func->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = {selfSymbol->getType()};
      auto* constructor = scope->lookupFunction("new", constructorParams);
      existingStruct->setConstructor(constructor);
    }
  }

  if (!hasConstructor) {
    throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());
  }

  selfSymbol = nullptr;
}

auto Codegen::visit(const Enum* node) -> void {
  lesma::Value* existingEnum = scope->lookupStruct(node->getIdentifier());
  if (existingEnum == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked enum symbol for {}",
                       node->getIdentifier());
  }

  if (existingEnum->getType()->getLlvmType() == nullptr) {
    std::vector<llvm::Type*> elementTypes = {builder->getInt8Ty()};
    auto* structType =
        llvm::StructType::create(theModule->getContext(), elementTypes, node->getIdentifier());
    existingEnum->getType()->setLlvmType(structType);
  }
}

auto Codegen::visit(const FuncCall* node) -> void { result = genFuncCall(node, {}); }

auto Codegen::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  auto left = std::move(result);
  node->getRight()->accept(*this);
  auto right = std::move(result);
  lesma::Type* finalType = CodegenTypeUtils::getExtendedType(left->getType(), right->getType());
  if (finalType == nullptr && left->getType()->is(BaseType::TY_ENUM) &&
      right->getType()->is(BaseType::TY_ENUM) && left->getType()->isEqual(right->getType())) {
    finalType = left->getType();
  }

  switch (node->getOperator()) {
  case TokenType::MINUS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFSub(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSub(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::PLUS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFAdd(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateAdd(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::STAR:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFMul(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateMul(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::SLASH:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFDiv(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSDiv(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::MOD:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::POWER:
    if (finalType == nullptr) {
      break;
    }

    if (!right->getType()->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw CodegenError(node->getSpan(), "Cannot use non-numbers for power coefficient: {}",
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
  case TokenType::EQUAL_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      throw CodegenError(node->getSpan(), "Operator {} is not supported for types {} and {}",
                         NAMEOF_ENUM(node->getOperator()), left->getType()->toString(),
                         right->getType()->toString());
    }

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                           leftName, rightName);
      }

      llvm::Value* leftVal = builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal = builder->CreateExtractValue(right->getLlvmValue(), {0});
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::BANG_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      throw CodegenError(node->getSpan(), "Operator {} is not supported for types {} and {}",
                         NAMEOF_ENUM(node->getOperator()), left->getType()->toString(),
                         right->getType()->toString());
    }

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                           leftName, rightName);
      }

      llvm::Value* leftVal = builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal = builder->CreateExtractValue(right->getLlvmValue(), {0});
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpONE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }
    break;
  case TokenType::GREATER:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::GREATER_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::LESS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::LESS_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::AND:
    if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                         node->getLeft()->toString(sourceManager.get(), "", true),
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalAnd(left->getLlvmValue(), right->getLlvmValue()));
    return;
  case TokenType::OR:
    if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                         node->getLeft()->toString(sourceManager.get(), "", true),
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalOr(left->getLlvmValue(), right->getLlvmValue()));
    return;
  default:
    throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}",
                       NAMEOF_ENUM(node->getOperator()));
  }

  if (auto operatorName = OperatorUtils::getBinaryOperatorName(node->getOperator());
      operatorName.has_value()) {
    result = callMethodByName(node->getSpan(), left.get(), std::string{*operatorName}, {right.get()});
    return;
  }

  throw CodegenError(node->getSpan(), "Operator {} is not supported for types {} and {}",
                     NAMEOF_ENUM(node->getOperator()), left->getType()->toString(),
                     right->getType()->toString());
}

auto Codegen::visit(const SubscriptOp* node) -> void {
  node->getLeft()->accept(*this);
  auto listValue = std::move(result);
  node->getIndex()->accept(*this);
  auto indexValue = std::move(result);
  auto getStdListBuffer = [this](lesma::Value* value) -> std::pair<lesma::Type*, llvm::Value*> {
    if (value == nullptr || value->getType() == nullptr) {
      return {nullptr, nullptr};
    }
    lesma::Type* type = value->getType();
    llvm::Value* handle = value->getLlvmValue();
    if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
        type->getElementType()->is(BaseType::TY_CLASS)) {
      type = type->getElementType();
    }
    if (!type->is(BaseType::TY_CLASS)) {
      return {nullptr, nullptr};
    }
    auto fields = type->getFields();
    if (fields.empty() || fields.front()->type == nullptr || !fields.front()->type->is(BaseType::TY_ARRAY)) {
      return {nullptr, nullptr};
    }
    auto* storagePtr =
        builder->CreateStructGEP(cast<llvm::StructType>(getOrCreateLlvmType(type)), value->getLlvmValue(), 0,
                                 "list.storage.ptr");
    handle = builder->CreateLoad(getOrCreateLlvmType(fields.front()->type), storagePtr, "list.storage");
    return {fields.front()->type, handle};
  };
  if (listValue != nullptr && listValue->getType() != nullptr && listValue->getType()->is(BaseType::TY_ARRAY)) {
    if (isAssignment) {
      throw CodegenError(node->getSpan(), "Operator [] assignment requires operator []=");
    }
    result =
        callMethodByName(node->getSpan(), listValue.get(), std::string{OperatorUtils::SUBSCRIPT_GET_NAME},
                         {indexValue.get()});
    return;
  }
  if (auto [bufferType, bufferHandle] = getStdListBuffer(listValue.get());
      bufferType != nullptr && bufferType->getElementType() != nullptr) {
    if (isAssignment) {
      throw CodegenError(node->getSpan(), "Operator [] assignment requires operator []=");
    }
    auto* elemPtr = emitListElementPointer(node->getSpan(), bufferType, bufferHandle,
                                           indexValue->getLlvmValue());
    result = std::make_unique<Value>(
        "", bufferType->getElementType(),
        builder->CreateLoad(getListStoredElementType(bufferType), elemPtr));
    return;
  }
  if (isAssignment) {
    throw CodegenError(node->getSpan(), "Operator [] assignment requires operator []=");
  }
  result =
      callMethodByName(node->getSpan(), listValue.get(), std::string{OperatorUtils::SUBSCRIPT_GET_NAME},
                       {indexValue.get()});
}

auto Codegen::visit(const DotOp* node) -> void {
  node->getLeft()->accept(*this);
  auto leftValue = std::move(result);
  if (leftValue != nullptr && leftValue->getType() != nullptr &&
      leftValue->getType()->is(BaseType::TY_ARRAY)) {
    auto* call = dynamic_cast<FuncCall*>(node->getRight());
    if (call == nullptr) {
      throw CodegenError(node->getSpan(), "Expected list method call after dot");
    }
    std::vector<std::unique_ptr<lesma::Value>> argStorage;
    std::vector<lesma::Value*> args;
    for (auto* arg : call->getArguments()) {
      arg->accept(*this);
      argStorage.push_back(std::move(result));
      args.push_back(argStorage.back().get());
    }
    std::vector<lesma::Type*> explicitTypeArgs;
    for (auto* explicitTypeArg : call->getExplicitTypeArgs()) {
      explicitTypeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
    }
    result = callMethodByName(node->getSpan(), leftValue.get(), call->getName(), args, explicitTypeArgs);
    return;
  }

  if (leftValue != nullptr && leftValue->getType() != nullptr) {
    lesma::Type* receiverType = leftValue->getType();
    if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr &&
        receiverType->getElementType()->is(BaseType::TY_CLASS)) {
      receiverType = receiverType->getElementType();
    }
    if (receiverType->is(BaseType::TY_CLASS)) {
      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
        throw CodegenError(node->getRight()->getSpan(),
                           "Expected identifier or method call right-hand of dot operator, "
                           "found {}",
                           node->getRight()->toString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
          TokenType::IDENTIFIER == dynamic_cast<Literal*>(node->getRight())->getType()) {
        field = dynamic_cast<Literal*>(node->getRight())->getValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->getRight());
      }

      auto* cls = scope->lookupStruct(receiverType->getLlvmType()->getStructName().str());
      if (cls == nullptr) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}",
                           receiverType->getLlvmType()->getStructName().str());
      }
      cls->setName(receiverType->getLlvmType()->getStructName().str());

      if (!field.empty()) {
        auto index = TypeUtils::findIndexInFields(cls->getType(), field);
        auto* type = TypeUtils::findTypeInFields(cls->getType(), field);
        if (index == -1) {
          throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field,
                             receiverType->getLlvmType()->getStructName().str());
        }

        auto* ptr =
            builder->CreateStructGEP(cls->getType()->getLlvmType(), leftValue->getLlvmValue(), index);
        if (isAssignment) {
          result = std::make_unique<Value>(
              "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type)), ptr);
          return;
        }
        result = std::make_unique<Value>("", type, builder->CreateLoad(type->getLlvmType(), ptr));
        return;
      }
      if (method != nullptr) {
        auto receiverValue = std::move(leftValue);
        std::vector<std::unique_ptr<lesma::Value>> argStorage;
        std::vector<lesma::Value*> args;
        for (auto* arg : method->getArguments()) {
          arg->accept(*this);
          argStorage.push_back(std::move(result));
          args.push_back(argStorage.back().get());
        }
        std::vector<lesma::Type*> explicitTypeArgs;
        for (auto* explicitTypeArg : method->getExplicitTypeArgs()) {
          explicitTypeArg->accept(*this);
          explicitTypeArgs.push_back(result->getType());
        }
        result = callMethodByName(node->getSpan(), receiverValue.get(), method->getName(), args,
                                  explicitTypeArgs);
        return;
      }
    }
  }

  if (auto* left = dynamic_cast<Literal*>(node->getLeft())) {
    if (left->getType() != TokenType::IDENTIFIER) {
      throw CodegenError(node->getLeft()->getSpan(),
                         "Expected identifier left-hand of dot operator, found {}",
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    auto* typeSym = scope->lookupType(left->getValue());
    if (typeSym != nullptr) {
      // Assuming it's an enum or statically accessed class
      if (!typeSym->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS, BaseType::TY_IMPORT})) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}",
                           left->getValue());
      }

      auto* right = dynamic_cast<Literal*>(node->getRight());
      if (typeSym->is(BaseType::TY_ENUM)) {
        // Check if right-hand expression is an identifier expression
        if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier right-hand of dot operator, found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        if (right->getType() != TokenType::IDENTIFIER) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier right-hand of dot operator, found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        // Setting value to the enum
        auto val = TypeUtils::findIndexInFields(typeSym, right->getValue());
        // Field not found in enum
        if (val == -1) {
          throw CodegenError(node->getLeft()->getSpan(), "Identifier {} not in {}",
                             right->getValue(), left->getValue());
        }

        auto* structVal = scope->lookupStruct(left->getValue());
        auto* enumPtr = builder->CreateAlloca(structVal->getType()->getLlvmType());
        auto* field = builder->CreateStructGEP(structVal->getType()->getLlvmType(), enumPtr, 0);
        builder->CreateStore(builder->getInt8(val), field);
        // Enum variant literals are returned by value (not pointer).
        auto* enumVal = builder->CreateLoad(structVal->getType()->getLlvmType(), enumPtr);

        result = std::make_unique<Value>("", structVal->getType(), enumVal);
        return;
      }

      if (typeSym->is(BaseType::TY_IMPORT)) {
        std::string field;
        FuncCall const* method = nullptr;

        if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
            (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier or method call right-hand of dot operator, "
                             "found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
            dynamic_cast<Literal*>(node->getRight())->getType() == TokenType::IDENTIFIER) {
          field = dynamic_cast<Literal*>(node->getRight())->getValue();
        } else {
          method = dynamic_cast<FuncCall*>(node->getRight());
        }

        if (method != nullptr) {
          auto tmpAlias = alias;
          alias = left->getValue();
          result = genFuncCall(method, {});
          alias = tmpAlias;
          return;
        }
      }
    } else {
      // Assuming it's a class instance
      left->accept(*this);
      // We refer to the class type, if it's a pointer, we get the result
      lesma::Type* lesmaType = result->getType();
      if (result->getType()->is(BaseType::TY_PTR) &&
          result->getType()->getElementType()->is(BaseType::TY_CLASS)) {
        lesmaType = result->getType()->getElementType();
      }

      if (!lesmaType->is(BaseType::TY_CLASS)) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}",
                           left->getValue());
      }

      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
        throw CodegenError(node->getRight()->getSpan(),
                           "Expected identifier or method call right-hand of dot operator, "
                           "found {}",
                           node->getRight()->toString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
          TokenType::IDENTIFIER == dynamic_cast<Literal*>(node->getRight())->getType()) {
        field = dynamic_cast<Literal*>(node->getRight())->getValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->getRight());
      }

      // lookupStruct returns by LLVM struct name; ensure display name matches.
      auto* cls = scope->lookupStruct(lesmaType->getLlvmType()->getStructName().str());
      cls->setName(lesmaType->getLlvmType()->getStructName().str());

      if (cls->getType()->is(BaseType::TY_CLASS)) {
        if (!field.empty()) {
          auto index = TypeUtils::findIndexInFields(cls->getType(), field);
          auto* type = TypeUtils::findTypeInFields(cls->getType(), field);
          if (index == -1) {
            throw CodegenError(
                node->getRight()->getSpan(), "Could not find field {} in {}", field,
                result->getType()->getElementType()->getLlvmType()->getStructName().str());
          }

          auto* ptr = builder->CreateStructGEP(cls->getType()->getLlvmType(),
                                               result->getLlvmValue(), index);
          if (isAssignment) {
            result = std::make_unique<Value>(
                "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type)),
                ptr);
            return;
          }
          //                    auto &x = cls->GetType()->GetFields()[index];
          result = std::make_unique<Value>("", type, builder->CreateLoad(type->getLlvmType(), ptr));
          return;
        }
        if (method != nullptr) {
          auto receiverValue = std::move(result);
          std::vector<std::unique_ptr<lesma::Value>> argStorage;
          std::vector<lesma::Value*> args;
          for (auto* arg : method->getArguments()) {
            arg->accept(*this);
            argStorage.push_back(std::move(result));
            args.push_back(argStorage.back().get());
          }
          std::vector<lesma::Type*> explicitTypeArgs;
          for (auto* explicitTypeArg : method->getExplicitTypeArgs()) {
            explicitTypeArg->accept(*this);
            explicitTypeArgs.push_back(result->getType());
          }
          result =
              callMethodByName(node->getSpan(), receiverValue.get(), method->getName(), args, explicitTypeArgs);
          return;
        }
      } else {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}",
                           lesmaType->getLlvmType()->getStructName().str());
      }
    }
  }
  throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}",
                     node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  auto expr = std::move(result);
  node->getType()->accept(*this);
  auto* castType = result->getType();
  result = cast(node->getSpan(), expr.get(), castType);
}

auto Codegen::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  auto* leftType = result->getType();
  node->getRight()->accept(*this);
  auto* rightType = result->getType();
  if (leftType != nullptr && leftType->is(BaseType::TY_PTR) && leftType->getElementType() != nullptr &&
      leftType->getElementType()->is(BaseType::TY_CLASS)) {
    leftType = leftType->getElementType();
  }
  if (rightType != nullptr && rightType->is(BaseType::TY_PTR) && rightType->getElementType() != nullptr &&
      rightType->getElementType()->is(BaseType::TY_CLASS)) {
    rightType = rightType->getElementType();
  }

  llvm::Value* val = nullptr;
  bool typesEqual = leftType->isEqual(rightType);
  if (!typesEqual && leftType != nullptr && rightType != nullptr &&
      leftType->getBaseType() == rightType->getBaseType() &&
      leftType->isOneOf({BaseType::TY_CLASS, BaseType::TY_ARRAY, BaseType::TY_ENUM})) {
    typesEqual = leftType->toString() == rightType->toString();
  }

  if (node->getOperator() == TokenType::IS) {
    val = typesEqual ? builder->getTrue() : builder->getFalse();
  } else {
    val = typesEqual ? builder->getFalse() : builder->getTrue();
  }

  result = std::make_unique<Value>(
      "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), val);
}

auto Codegen::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);
  auto operand = std::move(result);

  llvm::Value* val = nullptr;
  lesma::Type* type = operand->getType();
  std::unique_ptr<lesma::Type> ptrTypeHolder; // Keep owned type alive if created

  if (node->getOperator() == TokenType::MINUS) {
    if (operand->getType()->is(BaseType::TY_INT)) {
      val = builder->CreateNeg(operand->getLlvmValue());
    } else if (operand->getType()->is(BaseType::TY_FLOAT)) {
      val = builder->CreateFNeg(operand->getLlvmValue());
    } else {
      result = callMethodByName(node->getSpan(), operand.get(),
                                std::string{*OperatorUtils::getUnaryOperatorName(node->getOperator())});
      return;
    }
  } else if (node->getOperator() == TokenType::NOT || node->getOperator() == TokenType::BANG) {
    if (operand->getType()->is(BaseType::TY_BOOL)) {
      val = builder->CreateNot(operand->getLlvmValue());
    } else {
      result = callMethodByName(node->getSpan(), operand.get(),
                                std::string{*OperatorUtils::getUnaryOperatorName(node->getOperator())});
      return;
    }
  } else if (node->getOperator() == TokenType::STAR) {
    if (operand->getType()->is(BaseType::TY_PTR)) {
      val = builder->CreateLoad(operand->getType()->getElementType()->getLlvmType(),
                                operand->getLlvmValue());
      type = operand->getType()->getElementType();
    } else {
      throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()),
                         node->getExpression()->toString(sourceManager.get(), "", true));
    }
  } else if (node->getOperator() == TokenType::AMPERSAND) {
    val = builder->CreateAlloca(operand->getType()->getLlvmType());
    ptrTypeHolder =
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), operand->getType());
    type = ptrTypeHolder.get();
    builder->CreateStore(operand->getLlvmValue(), val);
  } else {
    throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getExpression()->toString(sourceManager.get(), "", true));
  }

  // For AMPERSAND case, Value needs to own the Type since ptrTypeHolder will go
  // out of scope
  if (ptrTypeHolder) {
    result = std::make_unique<Value>(std::move(ptrTypeHolder));
    result->setLlvmValue(val);
  } else {
    result = std::make_unique<Value>("", type, val);
  }
}

auto Codegen::visit(const ListLiteral* node) -> void {
  lesma::Type* listType = node->getResolvedType();
  if (listType != nullptr && listType->is(BaseType::TY_CLASS)) {
    auto fields = listType->getFields();
    if (fields.empty() || fields.front()->type == nullptr || !fields.front()->type->is(BaseType::TY_ARRAY) ||
        fields.front()->type->getElementType() == nullptr) {
      throw CodegenError(node->getSpan(), "List literal resolved to invalid stdlib list class");
    }
    auto* bufferType = fields.front()->type;
    auto* structType = cast<llvm::StructType>(getOrCreateLlvmType(listType));
    auto* classSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
    auto* classHandle = emitMalloc(classSize, "list.obj");
    auto* storagePtr = builder->CreateStructGEP(structType, classHandle, 0, "list.storage.ptr");

    auto* listStructTy = getOrCreateListStructType(bufferType);
    auto* headerSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
    auto* bufferHandle = emitMalloc(headerSize, "list.header");
    llvm::Value* dataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
    auto elements = node->getElements();
    if (!elements.empty()) {
      auto* elementLlvmType = getListStoredElementType(bufferType);
      auto* byteSize = builder->getInt64(
          theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue() * elements.size());
      dataPtr = emitMalloc(byteSize, "list.data");
      for (size_t i = 0; i < elements.size(); ++i) {
        elements[i]->accept(*this);
        auto* elementPtr =
            builder->CreateGEP(elementLlvmType, dataPtr, builder->getInt64(i), "list.elem.ptr");
        builder->CreateStore(getListStoredElementValue(node->getSpan(), result.get(),
                                                       bufferType->getElementType()),
                             elementPtr);
      }
    }
    auto* count = builder->getInt64(elements.size());
    emitStoreListDataPtr(bufferType, bufferHandle, dataPtr);
    emitStoreListLength(bufferType, bufferHandle, count);
    emitStoreListCapacity(bufferType, bufferHandle, count);
    builder->CreateStore(bufferHandle, storagePtr);
    result = std::make_unique<Value>("", listType, classHandle);
    return;
  }
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) ||
      listType->getElementType() == nullptr) {
    throw CodegenError(node->getSpan(), "List literal has no resolved list/buffer type");
  }

  lesma::Type* elementType = listType->getElementType();
  llvm::Type* elementLlvmType = getListStoredElementType(listType);
  getOrCreateLlvmType(listType);
  auto* listStructTy = getOrCreateListStructType(listType);
  std::vector<Expression*> elements = node->getElements();
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
  auto* listHandle = emitMalloc(headerSize, "list.header");

  llvm::Value* dataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
  if (!elements.empty()) {
    uint64_t elementSize =
        theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue();
    auto* byteSize = builder->getInt64(elementSize * elements.size());
    dataPtr = emitMalloc(byteSize, "list.data");

    for (size_t i = 0; i < elements.size(); ++i) {
      elements[i]->accept(*this);
      auto* elementPtr =
          builder->CreateGEP(elementLlvmType, dataPtr, builder->getInt64(i), "list.elem.ptr");
      builder->CreateStore(getListStoredElementValue(elements[i]->getSpan(), result.get(), elementType),
                           elementPtr);
    }
  }

  auto* count = builder->getInt64(elements.size());
  emitStoreListDataPtr(listType, listHandle, dataPtr);
  emitStoreListLength(listType, listHandle, count);
  emitStoreListCapacity(listType, listHandle, count);
  result = std::make_unique<Value>("", listType, listHandle);
}

auto Codegen::visit(const Literal* node) -> void {
  // Cache Types to prevent dangling pointers when result is reassigned
  if (node->getType() == TokenType::DOUBLE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(
        "", type, ConstantFP::get(theModule->getContext(), APFloat(std::stod(node->getValue()))));
  } else if (node->getType() == TokenType::INTEGER) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(
        "", type, ConstantInt::getSigned(builder->getInt64Ty(), std::stoi(node->getValue())));
  } else if (node->getType() == TokenType::BOOL) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(
        "", type, node->getValue() == "true" ? builder->getTrue() : builder->getFalse());
  } else if (node->getType() == TokenType::STRING) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>("", type, builder->CreateGlobalString(node->getValue()));
  } else if (node->getType() == TokenType::NIL) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result =
        std::make_unique<Value>("", type, ConstantPointerNull::getNullValue(builder->getPtrTy()));
  } else if (node->getType() == TokenType::IDENTIFIER) {
    // Look this variable up in the function.
    auto* val = scope->lookup(node->getValue());
    if (val == nullptr) {
      throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());
    }
    result = materializeSymbolValue(val);
  } else {
    throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
  }
}

auto Codegen::visit(const Else* /*node*/) -> void {
  auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
  result = std::make_unique<Value>("", type, llvm::ConstantInt::getTrue(theModule->getContext()));
}

auto Codegen::cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
    -> std::unique_ptr<lesma::Value> {
  return CodegenTypeUtils::cast(span, val, type, builder.get());
}

auto Codegen::symbolUsesDirectLlvmValue(const lesma::Value* symbol) const -> bool {
  return symbol != nullptr && symbol->usesDirectLlvmValue();
}

auto Codegen::materializeSymbolValue(lesma::Value* symbol) -> std::unique_ptr<lesma::Value> {
  if (symbol == nullptr) {
    return nullptr;
  }

  getOrCreateLlvmType(symbol->getType());
  if (symbolUsesDirectLlvmValue(symbol)) {
    return std::make_unique<Value>(*symbol);
  }

  llvm::Value* llvmVal =
      builder->CreateLoad(symbol->getType()->getLlvmType(), symbol->getLlvmValue());
  return std::make_unique<Value>("", symbol->getType(), llvmVal);
}

auto Codegen::getMangledName(llvm::SMRange span, std::string funcName,
                             const std::vector<lesma::Type*>& paramTypes, bool isMethodFlag,
                             std::string alias) -> std::string {
  alias = alias.empty() ? this->alias : alias;
  std::string name = (alias.empty() ? "" : "&" + alias + "=>") +
                     (selfSymbol != nullptr && isMethodFlag
                          ? selfSymbol->getName() + "::" + std::move(funcName) + ":"
                          : "." + std::move(funcName) + ":");
  bool first = true;

  for (auto* paramType : paramTypes) {
    if (!first) {
      name += ",";
    } else {
      first = false;
    }
    name += MangleUtils::getTypeMangledName(span, paramType);
  }

  return name;
}

auto Codegen::emitCompoundAssign(llvm::SMRange span, TokenType op, lesma::Value* lhs,
                                 lesma::Value* value) -> void {
  lesma::Type* targetType = lhs->getType();
  if (targetType != nullptr && targetType->is(BaseType::TY_PTR) &&
      targetType->getElementType() != nullptr) {
    targetType = targetType->getElementType();
  }
  if (targetType == nullptr || (!targetType->is(BaseType::TY_FLOAT) && !targetType->is(BaseType::TY_INT))) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(op));
  }
  const bool isFloat = targetType->is(BaseType::TY_FLOAT);
  auto* varVal = builder->CreateLoad(targetType->getLlvmType(), lhs->getLlvmValue());
  llvm::Value* newVal = nullptr;

  switch (op) {
  case TokenType::PLUS_EQUAL:
    newVal = isFloat ? builder->CreateFAdd(value->getLlvmValue(), varVal)
                     : builder->CreateAdd(value->getLlvmValue(), varVal);
    break;
  case TokenType::MINUS_EQUAL:
    newVal = isFloat ? builder->CreateFSub(varVal, value->getLlvmValue())
                     : builder->CreateSub(varVal, value->getLlvmValue());
    break;
  case TokenType::SLASH_EQUAL:
    newVal = isFloat ? builder->CreateFDiv(varVal, value->getLlvmValue())
                     : builder->CreateSDiv(varVal, value->getLlvmValue());
    break;
  case TokenType::STAR_EQUAL:
    newVal = isFloat ? builder->CreateFMul(value->getLlvmValue(), varVal)
                     : builder->CreateMul(value->getLlvmValue(), varVal);
    break;
  case TokenType::MOD_EQUAL:
    newVal = isFloat ? builder->CreateFRem(varVal, value->getLlvmValue())
                     : builder->CreateSRem(varVal, value->getLlvmValue());
    break;
  default:
    throw CodegenError(span, "Invalid compound operator: {}", NAMEOF_ENUM(op));
  }
  builder->CreateStore(newVal, lhs->getLlvmValue());
}

auto Codegen::appendCallableArgument(lesma::Value* arg, std::vector<lesma::Type*>& paramTypes,
                                     std::vector<llvm::Value*>& paramsLLVM) -> void {
  lesma::Type* argType = arg->getType();
  if (argType->is(BaseType::TY_CLASS)) {
    lesma::Type* ptrType = nullptr;
    for (auto& t : typeCache) {
      if (t->is(BaseType::TY_PTR) && t->getElementType() == argType) {
        ptrType = t.get();
        break;
      }
    }
    if (ptrType != nullptr) {
      argType = ptrType;
    } else {
      argType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), argType));
    }
  }
  paramTypes.push_back(argType);
  paramsLLVM.push_back(arg->getLlvmValue());
}

auto Codegen::callNamedFunction(llvm::SMRange span, const std::string& functionName,
                                const std::vector<lesma::Type*>& paramTypes,
                                const std::vector<llvm::Value*>& paramsLLVM,
                                const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> localParamTypes = paramTypes;
  std::vector<llvm::Value*> localParamsLLVM = paramsLLVM;
  auto makeCallableSignatureKey = [](const std::string& name,
                                     const std::vector<lesma::Type*>& types) -> std::string {
    std::string key = name;
    for (auto* type : types) {
      key += "|" + (type != nullptr ? type->toString() : "?");
    }
    return key;
  };
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }
  Value* symbol = nullptr;
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->lookupStruct(functionName);
  llvm::Value* classPtr = nullptr;
  std::unique_ptr<Type> selfParamType;

  if (classSym == nullptr || (classSym->getType()->is(BaseType::TY_CLASS) &&
                              classSym->getType()->getLlvmType() == nullptr)) {
    const Class* templateClass = nullptr;
    if (auto git = genericClasses.find(functionName); git != genericClasses.end()) {
      templateClass = git->second;
    } else if (classSym != nullptr && classSym->getGenericClassTemplate() != nullptr) {
      templateClass = static_cast<const Class*>(classSym->getGenericClassTemplate());
    }
    if (templateClass != nullptr) {
      classSym = specializeClass(templateClass, localParamTypes, explicitTypeArgs);
    }
  }

  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    auto* classLlvmType = getOrCreateLlvmType(classSym->getType());
    auto* classSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(classLlvmType).getFixedValue());
    classPtr = emitMalloc(classSize, functionName + ".obj");
    localParamsLLVM.insert(localParamsLLVM.begin(), classPtr);
    selfParamType = std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), classSym->getType());
    localParamTypes.insert(localParamTypes.begin(), selfParamType.get());

    selfSymbol = classSym;
    symbol = scope->lookupFunction("new", localParamTypes);
  } else {
    auto directSignatureKey = makeCallableSignatureKey(functionName, localParamTypes);
    if (auto directIt = specializedFunctions.find(directSignatureKey);
        directIt != specializedFunctions.end()) {
      symbol = directIt->second;
    }
    auto normalizeFunctionParamType = [](lesma::Type* type) -> lesma::Type* {
      if (type != nullptr && type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
          type->getElementType()->is(BaseType::TY_CLASS)) {
        return type->getElementType();
      }
      return type;
    };
    for (auto* candidate : scope->getSymbols()) {
      if (symbol != nullptr) {
        break;
      }
      if (candidate == nullptr || candidate->getName() != functionName ||
          !candidate->getType()->is(BaseType::TY_FUNCTION) || candidate->getLlvmValue() == nullptr) {
        continue;
      }
      auto candidateFields = candidate->getType()->getFields();
      if (candidateFields.size() != localParamTypes.size()) {
        continue;
      }
      bool compatible = true;
      for (size_t i = 0; i < candidateFields.size(); ++i) {
        lesma::Type* formalType = normalizeFunctionParamType(candidateFields[i]->type);
        lesma::Type* actualType = normalizeFunctionParamType(localParamTypes[i]);
        if ((formalType == nullptr) != (actualType == nullptr) ||
            (formalType != nullptr && !formalType->isEqual(actualType))) {
          compatible = false;
          break;
        }
      }
      if (compatible) {
        symbol = candidate;
        break;
      }
    }
    if (symbol == nullptr) {
      auto directMangledName = getMangledName(span, functionName, localParamTypes, false);
      if (auto directIt = specializedFunctions.find(directMangledName);
          directIt != specializedFunctions.end()) {
        symbol = directIt->second;
      } else {
        symbol = scope->lookupFunction(functionName, localParamTypes);
      }
    }
  }

  if (symbol == nullptr) {
    throw CodegenError(span, "{} {} not in current scope.",
                       classSym != nullptr ? "Constructor for" : "Function", functionName);
  }
  if (symbol->getLlvmValue() == nullptr) {
    auto directMangledName = getMangledName(span, functionName, localParamTypes, selfSymbol != nullptr);
    if (auto* directFunction = theModule->getFunction(directMangledName); directFunction != nullptr) {
      symbol->setLlvmValue(directFunction);
    }
  }

  if (symbol->getType()->getFields().size() == localParamTypes.size() &&
      symbol->getLlvmValue() == nullptr) {
    const FuncDecl* templateDecl = nullptr;
    if (selfSymbol != nullptr) {
      auto cit = genericMethods.find(selfSymbol->getName());
      if (cit != genericMethods.end()) {
        auto mit = cit->second.find(functionName);
        if (mit != cit->second.end()) {
          templateDecl = mit->second;
        }
      }
    }
    if (templateDecl == nullptr) {
      auto git = genericFunctions.find(functionName);
      if (git != genericFunctions.end()) {
        templateDecl = git->second;
      }
    }
    if (templateDecl != nullptr) {
      std::vector<std::string> genericNames = templateDecl->getGenericParams();
      symbol = specializeFunction(templateDecl, localParamTypes, genericNames, explicitTypeArgs);
    }
  }

  if (!symbol->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
    throw CodegenError(span, "Symbol {} is not a function or constructor.", functionName);
  }

  if (symbol->getType()->getFields().size() > localParamsLLVM.size()) {
    auto fields = symbol->getType()->getFields();
    for (auto* field : fields) {
      if (field->defaultValue == nullptr) {
        throw CodegenError(span,
                           "Something bad happened, lookup found a function with incorrect defaults",
                           functionName);
      }
      localParamsLLVM.push_back(field->defaultValue->getLlvmValue());
    }
  }

  llvm::Value* callableValue = nullptr;
  if (symbol->getType()->is(BaseType::TY_CLASS)) {
    if (symbol->getConstructor() == nullptr || symbol->getConstructor()->getLlvmValue() == nullptr) {
      throw CodegenError(span, "Constructor for {} is declared but not defined", functionName);
    }
    callableValue = symbol->getConstructor()->getLlvmValue();
  } else {
    if (symbol->getLlvmValue() == nullptr) {
      throw CodegenError(span, "Function {} is declared but not defined", functionName);
    }
    callableValue = symbol->getLlvmValue();
  }
  auto* func = llvm::cast<Function>(callableValue);
  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    builder->CreateCall(func, localParamsLLVM);
    selfSymbol = selfSymbolTmp;
    return std::make_unique<Value>("", classSym->getType(), classPtr);
  }

  selfSymbol = selfSymbolTmp;
  return std::make_unique<Value>("", symbol->getType()->getReturnType(),
                                 builder->CreateCall(func, localParamsLLVM));
}

auto Codegen::callListMethodByName(llvm::SMRange span, lesma::Value* receiver,
                                   const std::string& methodName,
                                   const std::vector<lesma::Value*>& args,
                                   const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  (void) explicitTypeArgs;
  lesma::Type* receiverType = receiver->getType();
  lesma::Type* listClassType = nullptr;
  lesma::Type* bufferType = nullptr;
  llvm::Value* bufferHandle = receiver->getLlvmValue();

  if (receiverType->is(BaseType::TY_ARRAY)) {
    bufferType = receiverType;
  } else {
    if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr) {
      receiverType = receiverType->getElementType();
    }
    if (receiverType->is(BaseType::TY_CLASS)) {
      auto fields = receiverType->getFields();
      if (!fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_ARRAY)) {
        listClassType = receiverType;
        bufferType = fields.front()->type;
        auto* storagePtr = builder->CreateStructGEP(cast<llvm::StructType>(getOrCreateLlvmType(listClassType)),
                                                    receiver->getLlvmValue(), 0, "list.storage.ptr");
        bufferHandle = builder->CreateLoad(getOrCreateLlvmType(bufferType), storagePtr, "list.storage");
      }
    }
  }
  if (bufferType == nullptr || bufferHandle == nullptr) {
    throw CodegenError(span, "Function {} not in current scope.", methodName);
  }

  if (methodName == "len") {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    return std::make_unique<Value>("", type, emitListLength(bufferType, bufferHandle));
  }
  if (methodName == "clear") {
    auto* currentData = emitListDataPtr(bufferType, bufferHandle);
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* freeBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.clear.free", parentFunction);
    auto* doneBlock = llvm::BasicBlock::Create(theModule->getContext(), "list.clear.done", parentFunction);
    builder->CreateCondBr(
        builder->CreateICmpNE(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())), freeBlock,
        doneBlock);
    builder->SetInsertPoint(freeBlock);
    emitFree(currentData);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    emitStoreListDataPtr(bufferType, bufferHandle, llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(bufferType, bufferHandle, builder->getInt64(0));
    emitStoreListCapacity(bufferType, bufferHandle, builder->getInt64(0));
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }
  if (methodName == "push") {
    if (args.size() != 1U) {
      throw CodegenError(span, "push expects one argument");
    }
    auto* length = emitListLength(bufferType, bufferHandle);
    auto* nextLength = builder->CreateAdd(length, builder->getInt64(1));
    emitListEnsureCapacity(bufferType, bufferHandle, nextLength);
    auto* elementPtr =
        builder->CreateGEP(getListStoredElementType(bufferType), emitListDataPtr(bufferType, bufferHandle),
                           length, "list.push.ptr");
    builder->CreateStore(getListStoredElementValue(span, args[0], bufferType->getElementType()), elementPtr);
    emitStoreListLength(bufferType, bufferHandle, nextLength);
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }
  if (methodName == "pop") {
    auto* length = emitListLength(bufferType, bufferHandle);
    emitListBoundsCheck(span, bufferType, bufferHandle, builder->CreateSub(length, builder->getInt64(1)));
    auto* newLength = builder->CreateSub(length, builder->getInt64(1), "list.pop.len");
    auto* elementPtr = builder->CreateGEP(getListStoredElementType(bufferType),
                                          emitListDataPtr(bufferType, bufferHandle), newLength,
                                          "list.pop.ptr");
    auto* poppedValue = builder->CreateLoad(getListStoredElementType(bufferType), elementPtr);
    emitStoreListLength(bufferType, bufferHandle, newLength);
    return std::make_unique<Value>("", bufferType->getElementType(), poppedValue);
  }
  if (methodName == "copy") {
    auto* copiedBuffer = emitListDeepCopy(bufferType, bufferHandle);
    if (listClassType == nullptr) {
      return std::make_unique<Value>("", bufferType, copiedBuffer);
    }
    auto* classLlvmType = getOrCreateLlvmType(listClassType);
    auto* classSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(classLlvmType).getFixedValue());
    auto* classHandle = emitMalloc(classSize, "list.copy.obj");
    auto* storagePtr =
        builder->CreateStructGEP(cast<llvm::StructType>(classLlvmType), classHandle, 0, "list.copy.storage.ptr");
    builder->CreateStore(copiedBuffer, storagePtr);
    return std::make_unique<Value>("", listClassType, classHandle);
  }
  if (methodName == std::string{OperatorUtils::SUBSCRIPT_GET_NAME}) {
    if (args.size() != 1U) {
      throw CodegenError(span, "operator [] expects one argument");
    }
    auto* elementPtr = emitListElementPointer(span, bufferType, bufferHandle, args[0]->getLlvmValue());
    return std::make_unique<Value>("", bufferType->getElementType(),
                                   builder->CreateLoad(getListStoredElementType(bufferType), elementPtr));
  }
  if (methodName == std::string{OperatorUtils::SUBSCRIPT_SET_NAME}) {
    if (args.size() != 2U) {
      throw CodegenError(span, "operator []= expects two arguments");
    }
    auto* elementPtr = emitListElementPointer(span, bufferType, bufferHandle, args[0]->getLlvmValue());
    builder->CreateStore(getListStoredElementValue(span, args[1], bufferType->getElementType()), elementPtr);
    return std::make_unique<Value>("",
                                   cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())),
                                   nullptr);
  }

  throw CodegenError(span, "Function {} not in current scope.", methodName);
}

auto Codegen::callMethodByName(llvm::SMRange span, lesma::Value* receiver, const std::string& methodName,
                               const std::vector<lesma::Value*>& args,
                               const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  if (receiver->getType()->is(BaseType::TY_ARRAY)) {
    return callListMethodByName(span, receiver, methodName, args, explicitTypeArgs);
  }

  lesma::Type* receiverType = receiver->getType();
  if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr) {
    receiverType = receiverType->getElementType();
  }
  if (receiverType->is(BaseType::TY_CLASS)) {
    auto fields = receiverType->getFields();
    if (!fields.empty() && fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_ARRAY)) {
      return callListMethodByName(span, receiver, methodName, args, explicitTypeArgs);
    }
  }
  if (!receiverType->is(BaseType::TY_CLASS)) {
    throw CodegenError(span, "Method {} requires class receiver", methodName);
  }

  auto* cls = scope->lookupStruct(receiverType->getLlvmType()->getStructName().str());
  if (cls == nullptr) {
    throw CodegenError(span, "Cannot find related class {}",
                       receiverType->getLlvmType()->getStructName().str());
  }
  cls->setName(receiverType->getLlvmType()->getStructName().str());

  auto* savedSelfSymbol = selfSymbol;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  appendCallableArgument(receiver, paramTypes, paramsLLVM);
  for (auto* arg : args) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }
  lesma::Value* directMethod = scope->lookupFunction(methodName, paramTypes);
  if (directMethod == nullptr) {
    for (const auto& importedScope : *importedScopes) {
      if (importedScope == nullptr) {
        continue;
      }
      directMethod = importedScope->lookupFunction(methodName, paramTypes);
      if (directMethod != nullptr) {
        break;
      }
    }
  }
  if (directMethod != nullptr && directMethod->getLlvmValue() != nullptr) {
    std::vector<llvm::Value*> finalParams;
    auto fields = directMethod->getType()->getFields();
    finalParams.reserve(paramsLLVM.size());
    for (size_t i = 0; i < paramsLLVM.size(); ++i) {
      auto paramVal = std::make_unique<Value>("", paramTypes[i], paramsLLVM[i]);
      auto castVal = cast(span, paramVal.get(), fields[i]->type);
      finalParams.push_back(castVal->getLlvmValue());
    }
    selfSymbol = savedSelfSymbol;
    return std::make_unique<Value>("", directMethod->getType()->getReturnType(),
                                   builder->CreateCall(llvm::cast<Function>(directMethod->getLlvmValue()),
                                                       finalParams));
  }
  selfSymbol = cls;
  auto resultValue = callNamedFunction(span, methodName, paramTypes, paramsLLVM, explicitTypeArgs);
  selfSymbol = savedSelfSymbol;
  return resultValue;
}

auto Codegen::genFuncCall(const FuncCall* node, const std::vector<lesma::Value*>& extraParams = {})
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  std::vector<lesma::Type*> explicitTypeArgs;

  for (auto* arg : extraParams) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }

  for (auto* arg : node->getArguments()) {
    arg->accept(*this);
    appendCallableArgument(result.get(), paramTypes, paramsLLVM);
  }

  for (auto* explicitTypeArg : node->getExplicitTypeArgs()) {
    explicitTypeArg->accept(*this);
    explicitTypeArgs.push_back(result->getType());
  }

  if (isListIntrinsicName(node->getName())) {
    return genListIntrinsicCall(node, paramTypes, paramsLLVM);
  }
  return callNamedFunction(node->getSpan(), node->getName(), paramTypes, paramsLLVM, explicitTypeArgs);
}
