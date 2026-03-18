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

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
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
#include <llvm/Support/Program.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
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
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser,
                 std::shared_ptr<SourceMgr> srcMgr, const std::string& filename,
                 std::vector<std::string> imports, bool jit, bool main,
                 std::string alias,
                 const std::shared_ptr<ThreadSafeContext>& context,
                 std::shared_ptr<std::vector<std::string>> sharedModules,
                 std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>>
                     sharedScopes,
                 std::optional<std::unique_ptr<SymbolTable>> preScope,
                 std::optional<std::vector<std::unique_ptr<lesma::Type>>>
                     preTypeCache) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  theContext =
      context == nullptr
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
  if (preScope && *preScope) {
    rootScope = std::move(*preScope);
  } else {
    rootScope = std::make_unique<SymbolTable>(nullptr);
  }
  scope = rootScope.get();
  if (preTypeCache && !preTypeCache->empty()) {
    for (auto& t : *preTypeCache) {
      typeCache.push_back(std::move(t));
    }
  }

  this->alias = std::move(alias);
  this->filename = filename;
  isMain = main;
  isJit = jit;

  if (sharedModules && sharedScopes) {
    importedModules = std::move(sharedModules);
    importedScopes = std::move(sharedScopes);
  } else {
    importedModules =
        std::make_shared<std::vector<std::string>>(std::move(imports));
    importedScopes =
        std::make_shared<std::vector<std::unique_ptr<SymbolTable>>>();
  }
  topLevelFunc = initializeTopLevel();
  // base.les is loaded at the start of run() so we don't load it during
  // constructor re-entrancy when creating Codegens for imported modules.
}

auto Codegen::initializeModule() -> std::unique_ptr<Module> {
  std::unique_ptr<Module> mod;
  theContext->withContextDo([&](LLVMContext* ctx) -> void {
    mod = std::make_unique<Module>("Lesma", *ctx);
  });
  mod->setTargetTriple(targetMachine->getTargetTriple());
  mod->setDataLayout(targetMachine->createDataLayout());
  mod->setSourceFileName(filename);

  return mod;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto Codegen::initializeTargetMachine()
    -> std::unique_ptr<llvm::TargetMachine> {
  // Configure output target
  auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());

  // Search after selected target
  std::string error;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(targetTriple.getTriple(), error);
  if (target == nullptr) {
    throw CodegenError({}, "Target not available:\n{}", error);
  }

  llvm::TargetOptions const opt;
  llvm::Reloc::Model rm = llvm::Reloc::Model();
  std::unique_ptr<llvm::TargetMachine> targetMachine(
      target->createTargetMachine(targetTriple, "generic", "", opt, rm));
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
  auto generator = cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
      jit->getDataLayout().getGlobalPrefix()));
  mainJd.addGenerator(std::move(generator));

  return jit;
}

auto Codegen::initializeTopLevel() -> llvm::Function* {
  std::vector<llvm::Type*> const paramTypes = {};

  FunctionType* ft =
      FunctionType::get(builder->getInt64Ty(), paramTypes, false);
  Function* f = Function::Create(
      ft, isMain ? Function::ExternalLinkage : Function::InternalLinkage,
      "main", *theModule);

  auto* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  return f;
}

auto Codegen::defineFunction(lesma::Value* value, const FuncDecl* node,
                             Value* clsSymbol) -> void {
  scope = scope->createChildBlock(node->getName());
  currentFunction = value;
  deferStack.emplace();

  auto* f = llvm::cast<Function>(value->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  int fieldIndex = 0;
  for (const auto& field : value->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);

    if (clsSymbol != nullptr && param->getArgNo() == 0) {
      param->setName("self");
    } else {
      param->setName(node->getParameters()[param->getArgNo() -
                                           (clsSymbol != nullptr ? 1 : 0)]
                         ->name);
    }

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr,
                                             param->getName() + "_ptr");
    builder->CreateStore(param, ptr);

    auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
    scope->insertSymbol(std::move(symbol));

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
      throw CodegenError(node->getSpan(),
                         "Function {} does not always return a result",
                         node->getName());
    }
  }

  isReturn = false;

  // Verify only this module's function (not cross-module declarations).
  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(node->getSpan(), "Invalid function {}\n{}", node->getName(),
                       verifyOutput);
  }

  // Insert Function to Symbol Table
  scope = scope->getParent();

  currentFunction = nullptr;

  // Reset Insert Point to Top Level
  builder->SetInsertPoint(&topLevelFunc->back());
}

auto Codegen::compileModule(
    llvm::SMRange span, const std::string& filepath, bool isStd,
    const std::string& moduleAlias, bool importAll, bool importToScope,
    const std::vector<std::pair<std::string, std::string> /*unused*/>&
        importedNames) -> void {
  std::filesystem::path mainPath = filename;
  // Read source
  auto absolutePath =
      isStd ? filepath
            : fmt::format(
                  "{}/{}",
                  std::filesystem::absolute(mainPath).parent_path().c_str(),
                  filepath);

  // If this codegen already compiled this module, only merge its symbols (no
  // recompilation). Only skip when we have the scope (same codegen compiled it);
  // otherwise a child may see the path in the list but not have the scope.
  auto it = std::find(importedModules->begin(), importedModules->end(),
                      absolutePath);
  if (it != importedModules->end()) {
    auto existingIdx =
        static_cast<size_t>(it - importedModules->begin());
    if (existingIdx >= importedScopes->size()) {
      it = importedModules->end();
    }
  }
  if (it != importedModules->end()) {
    auto existingIdx =
        static_cast<size_t>(it - importedModules->begin());
    SymbolTable* existingScope = importedScopes->at(existingIdx).get();

    if (!importToScope) {
      auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
      auto* importTypPtr = importTyp.get();
      auto importSym =
          std::make_unique<Value>(moduleAlias, importTypPtr);
      scope->insertSymbol(std::move(importSym));
      scope->insertType(moduleAlias, std::move(importTyp));
    }

    auto findInImports =
        [importedNames](const std::string& import) -> std::string {
      for (const auto& impPair : importedNames) {
        if (impPair.first == import) {
          return impPair.second;
        }
      }
      return "";
    };

    for (auto* sym : existingScope->getSymbols()) {
      auto impAlias = findInImports(sym->getName());
      if (sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) &&
          sym->isExported() && (importAll || !impAlias.empty())) {
        llvm::StructType* structType =
            StructType::getTypeByName(theModule->getContext(), sym->getName());
        auto structSymbol = std::make_unique<Value>(
            impAlias.empty() ? sym->getName() : impAlias, sym->getType());
        structSymbol->getType()->setLlvmType(structType);
        scope->insertTypeRef(sym->getName(), sym->getType());
        scope->insertSymbol(std::move(structSymbol));
      } else if (sym->getType()->is(BaseType::TY_FUNCTION) &&
                 sym->isExported()) {
        auto* fTy = llvm::cast<FunctionType>(sym->getType()->getLlvmType());
        llvm::Function* f = nullptr;
        if (isJit) {
          f = llvm::cast<Function>(
              theModule->getOrInsertFunction(sym->getMangledName(), fTy)
                  .getCallee());
        }
        auto name = sym->getName();
        std::vector<lesma::Type*> paramTypes;
        for (auto* field : sym->getType()->getFields()) {
          paramTypes.push_back(field->type);
        }
        Value* funcSymbol = existingScope->lookupFunction(name, paramTypes);
        impAlias = findInImports(name);
        const bool isMethodSym = MangleUtils::isMethod(sym->getMangledName());
        bool methodClassImported = true;
        if (isMethodSym && !importAll) {
          const std::string& mn = sym->getMangledName();
          const size_t colcol = mn.find("::");
          if (colcol != std::string::npos) {
            std::string classPart = mn.substr(0, colcol);
            const size_t arrow = classPart.find("=>");
            if (arrow != std::string::npos) {
              classPart = classPart.substr(arrow + 2);
            }
            methodClassImported = !findInImports(classPart).empty();
          }
        }
        if (funcSymbol != nullptr && funcSymbol->isExported() &&
            (importAll || !impAlias.empty() ||
             (isMethodSym && methodClassImported))) {
          auto symbol = std::make_unique<Value>(
              impAlias.empty()
                  ? name
                  : std::regex_replace(name, std::regex(name), impAlias),
              funcSymbol->getType());
          if (isJit) {
            symbol->getType()->setLlvmType(fTy);
            symbol->setLlvmValue(f);
            symbol->setExported(false);
            symbol->setMangledName(sym->getMangledName());
          } else {
            auto* newFunc = Function::Create(
                fTy, Function::ExternalLinkage, sym->getMangledName(),
                *theModule);
            symbol->getType()->setLlvmType(newFunc->getFunctionType());
            symbol->setLlvmValue(newFunc);
            symbol->setExported(false);
            symbol->setMangledName(sym->getMangledName());
          }
          scope->insertSymbol(std::move(symbol));
        }
      }
    }
    return;
  }

  // Guard against circular import: if this module is already being compiled
  // (path in importedModules but scope not yet in importedScopes), we would
  // fall through and re-compile it → infinite recursion. Detect and error.
  static thread_local std::unordered_set<std::string> compiling;
  if (compiling.count(absolutePath)) {
    throw CodegenError(span, "Circular import detected: {}", filepath);
  }
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

  auto fileId =
      sourceManager->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
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

    // Per-module Codegen; we transfer rootScope and typeCache into
    // importedScopes/typeCache below so symbols and types stay alive.
    auto codegen = std::make_unique<Codegen>(
        std::move(parser), sourceManager, absolutePath, std::vector<std::string>{},
        isJit, false, !importToScope ? moduleAlias : "", theContext,
        importedModules, importedScopes);
    codegen->run();

    // Optimize
    codegen->optimize(OptimizationLevel::O3);
    codegen->theModule->setModuleIdentifier(filepath);

    if (!importToScope) {
      auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
      auto* importTypPtr = importTyp.get();
      auto importSym = std::make_unique<Value>(moduleAlias, importTypPtr);
      scope->insertSymbol(std::move(importSym));
      scope->insertType(moduleAlias, std::move(importTyp));
    }

    auto findInImports =
        [importedNames](const std::string& import) -> std::string {
      for (const auto& impPair : importedNames) {
        if (impPair.first == import) {
          return impPair.second;
        }
      }

      return "";
    };

    // Import Symbols (must run before moving codegen->theModule into JIT or
    // writing object file). Ownership: codegen->rootScope is moved into
    // importedScopes below so the imported scope and its Types/Values stay
    // alive; typeCache is merged so cached types are retained.
    for (auto* sym : codegen->scope->getSymbols()) {
      auto impAlias = findInImports(sym->getName());
      if (sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) &&
          sym->isExported() && (importAll || !impAlias.empty())) {
        llvm::StructType* structType =
            StructType::getTypeByName(theModule->getContext(), sym->getName());

        auto structSymbol = std::make_unique<Value>(
            impAlias.empty() ? sym->getName() : impAlias, sym->getType());
        structSymbol->getType()->setLlvmType(structType);
        // Insert non-owning reference to imported Type
        scope->insertTypeRef(sym->getName(), sym->getType());
        scope->insertSymbol(std::move(structSymbol));
      } else if (sym->getType()->is(BaseType::TY_FUNCTION) &&
                 sym->isExported()) {
        // After optimize(), GlobalDCE may have removed the function; do not
        // use sym->getLlvmValue() (use-after-free). Look up by name; if DCE
        // removed it we still add the symbol so the importer can call it.
        auto* f = codegen->theModule->getFunction(sym->getMangledName());
        auto* fTy = llvm::cast<FunctionType>(sym->getType()->getLlvmType());

        if (isJit) {
          // Use declaration in current module (needed if DCE removed f, or for
          // JIT linking)
          f = llvm::cast<Function>(
              theModule->getOrInsertFunction(sym->getMangledName(), fTy)
                  .getCallee());
        }

        auto name = sym->getName();
        std::vector<lesma::Type*> paramTypes;
        for (auto* field : sym->getType()->getFields()) {
          paramTypes.push_back(field->type);
        }

        Value* funcSymbol = codegen->scope->lookupFunction(name, paramTypes);

        // Only import if it's exported
        impAlias = findInImports(name);
        // TODO: methods should only be imported if they class is in the imports
        // specified
        if (funcSymbol != nullptr && funcSymbol->isExported() &&
            (importAll || !impAlias.empty() ||
             MangleUtils::isMethod(sym->getMangledName()))) {
          auto symbol = std::make_unique<Value>(
              impAlias.empty()
                  ? name
                  : std::regex_replace(name, std::regex(name), impAlias),
              funcSymbol->getType());

          if (isJit) {
            symbol->getType()->setLlvmType(fTy);
            symbol->setLlvmValue(f);
            symbol->setExported(false);
            symbol->setMangledName(sym->getMangledName());
          } else {
            // If it's compiled, we need to make a new Function declaration in
            // the importing file
            auto* newFunc = Function::Create(fTy, Function::ExternalLinkage,
                                             sym->getMangledName(), *theModule);
            symbol->getType()->setLlvmType(newFunc->getFunctionType());
            symbol->setLlvmValue(newFunc);
            symbol->setExported(false);
            symbol->setMangledName(sym->getMangledName());
          }

          scope->insertSymbol(std::move(symbol));
        }
      }
    }

    // Transfer ownership of imported Scope and type cache to keep Types/Values
    // alive (Types/Values in imported scope are referenced by newly created
    // symbols)
    importedScopes->push_back(std::move(codegen->rootScope));
    codegen->scope = nullptr; // Clear navigation pointer (rootscope now moved)

    // Transfer type cache to keep cached types alive
    for (auto& type : codegen->typeCache) {
      typeCache.push_back(std::move(type));
    }

    if (isJit) {
      // Add the module to JIT (after importing symbols; theModule still valid)
      cantFail(theJit->addIRModule(ThreadSafeModule(
                   std::move(codegen->theModule), *theContext)),
               fmt::format("Failed adding import {} to JIT", filename).c_str());
    } else {
      // Create object file to be linked
      std::string objFile = fmt::format("tmp{}", objectFiles.size());
      codegen->writeToObjectFile(objFile);
      objectFiles.push_back(fmt::format("{}.o", objFile));
    }
  } catch (const LesmaError& err) {
    compiling.erase(absolutePath);
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, err.what());
    } else {
      showInline(sourceManager.get(), fileId, err.getSpan(), absolutePath, true,
                 err.what());
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
  llvm::ModulePassManager mpm = pb.buildModuleOptimizationPipeline(
      opt, ThinOrFullLTOPhase::FullLTOPreLink);
  mpm.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(std::move(cgpm)));
  mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
  mpm.addPass(llvm::GlobalDCEPass());

  mpm.run(*theModule, mam);
}

auto Codegen::writeToObjectFile(const std::string& output) -> void {
  std::error_code err;
  llvm::raw_fd_ostream out(output + ".o", err);

  if (err) {
    throw CodegenError({}, "Error opening file {} for writing: {}", output,
                       err.message());
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
      lld::lldMain(args, llvm::outs(), llvm::errs(),
                   {{.f = lld::Darwin, .d = &lld::macho::link}});
#elif defined(_WIN32)
  lld::Result result =
      lld::lldMain(args, llvm::outs(), llvm::errs(),
                   {{.f = lld::WinLink, .d = &lld::coff::link}});
#else
  lld::Result result = lld::lldMain(args, llvm::outs(), llvm::errs(),
                                    {{.f = lld::Gnu, .d = &lld::elf::link}});
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

[[maybe_unused]] auto
Codegen::linkObjectFileWithClang(const std::string& objFilename) -> void {
  auto clangPath = llvm::sys::findProgramByName("clang");
  if (clangPath.getError()) {
    throw CodegenError({}, "Unable to find clang path");
  }

  std::string output = getBasename(objFilename);

  llvm::SmallVector<const char*, 32> args;
  args.push_back(clangPath.get().c_str());
  args.push_back("-o");
  args.push_back(output.c_str());
  args.push_back(objFilename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }

// Add the standard library path for Apple
#ifdef __APPLE__
  args.push_back("-L");
  args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
#endif

  // Set up the diagnostic engine
  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(
      new clang::DiagnosticIDs());
  clang::DiagnosticOptions diagOpts;
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - DiagnosticsEngine owns it
  auto* diagClient = new clang::TextDiagnosticPrinter(llvm::errs(), diagOpts);
  clang::DiagnosticsEngine diags(diagIDs, diagOpts, diagClient);

  // Create a compilation using Clang's driver
  clang::driver::Driver theDriver(args[0], theModule->getTargetTriple().str(),
                                  diags, "Lesma Compiler",
                                  llvm::vfs::getRealFileSystem());
  std::unique_ptr<clang::driver::Compilation> c(
      theDriver.BuildCompilation(args));

  if (!c) {
    throw CodegenError({}, "Failed to create clang driver compilation");
  }

  // Run the driver
  llvm::SmallVector<std::pair<int, const clang::driver::Command*>, 8>
      failingCommands;
  int res = theDriver.ExecuteCompilation(*c, failingCommands);

  if (res != 0) {
    throw CodegenError({}, "Linking failed");
  }

  // Remove object files (ignore errors - cleanup is best-effort)
  std::ignore = llvm::sys::fs::remove(objFilename);
  for (const auto& obj : objectFiles) {
    std::ignore = llvm::sys::fs::remove(obj);
  }
}

auto Codegen::linkObjectFile(const std::string& objFilename) -> void {
  linkObjectFileWithLld(objFilename);
  // linkObjectFileWithClang(objFilename);
}

auto Codegen::prepareJit() -> void {
  auto jitError =
      theJit->addIRModule(ThreadSafeModule(std::move(theModule), *theContext));
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
    throw CodegenError(
        {}, "Main function address not found, did you prepare JIT?\n");
  }

  return mainFuncAddress();
}

auto Codegen::run() -> void {
  // Load base stdlib once so every module has exit/print/etc. Done here
  // (not in constructor) to avoid re-entrancy when creating Codegens for
  // imported modules.
  if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
    compileModule(llvm::SMRange(), getStdDir() + "base.les", true, "base", true,
                  true, {});
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
    }
    defineFunction(fn, std::get<1>(prototypes[pi]), std::get<2>(prototypes[pi]));
    currentGenericTypes = std::move(savedGenerics);
  }

  // Return 0 for top-level function
  builder->CreateRet(ConstantInt::getSigned(builder->getInt64Ty(), 0));
}

auto Codegen::dump() -> void { theModule->print(outs(), nullptr); }

auto Codegen::visit(const Statement* node) -> void {
  lesma::print("Visited a blank statement\n{}",
               node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const Expression* node) -> void {
  lesma::print("Visited a blank expression\n{}",
               node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const TypeExpr* node) -> void {
  // For primitive types, cache them so they survive beyond result's lifetime
  // This is needed because setReturnType and similar store raw Type* pointers
  if (node->getType() == TokenType::INT_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT8_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt8Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT16_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt16Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::INT32_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt32Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::FLOAT32_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getFloatTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::BOOL_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::STRING_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::VOID_TYPE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(type);
  } else if (node->getType() == TokenType::PTR_TYPE) {
    node->getElementType()->accept(*this);
    // Function type is already a pointer at LLVM level; parser uses *func for
    // consistency, so do not add another pointer layer.
    if (!result->getType()->is(BaseType::TY_FUNCTION)) {
      auto* type = cacheType(std::make_unique<Type>(
          BaseType::TY_PTR, builder->getPtrTy(), result->getType()));
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
      fields.push_back(
          std::make_unique<Field>(result->getName(), result->getType()));
    }

    // With opaque pointers, function pointer types are just `ptr`
    // The actual function signature is tracked in Lesma's Type system via
    // fields
    auto funcType = std::make_unique<Type>(
        BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
    funcType->setReturnType(retType->getType());
    result = std::make_unique<Value>(cacheType(std::move(funcType)));
  } else if (node->getType() == TokenType::CUSTOM_TYPE) {
    auto git = currentGenericTypes.find(node->getName());
    if (git != currentGenericTypes.end()) {
      result = std::make_unique<Value>(git->second);
      return;
    }
    auto* typ = scope->lookupType(node->getName());
    auto* sym = scope->lookupStruct(node->getName());
    if (typ == nullptr && sym == nullptr) {
      throw CodegenError(node->getSpan(), "Type not found: {}",
                         node->getName());
    }
    if (sym->getType()->getLlvmType() == nullptr) {
      getOrCreateLlvmType(sym->getType());
    }

    result = std::make_unique<Value>(*sym);
  } else {
    throw CodegenError(node->getSpan(), "Unimplemented type {}",
                       NAMEOF_ENUM(node->getType()));
  }
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
  case BaseType::TY_FUNCTION:
    getOrCreateLlvmType(type->getReturnType());
    for (auto* f : type->getFields()) {
      getOrCreateLlvmType(f->type);
    }
    type->setLlvmType(builder->getPtrTy());
    break;
  case BaseType::TY_CLASS:
  case BaseType::TY_ENUM: {
    // Create opaque struct first to break recursion (e.g. class with field *Self)
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

auto Codegen::specializeFunction(const FuncDecl* node, const std::vector<lesma::Type*>& paramTypes,
                                 const std::vector<std::string>& genericNames) -> lesma::Value* {
  std::string key = node->getName();
  for (auto* t : paramTypes) {
    key += "|" + t->toString();
  }
  if (auto it = specializedFunctions.find(key); it != specializedFunctions.end()) {
    return it->second;
  }

  std::unordered_map<std::string, lesma::Type*> env;
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  auto templateParams = node->getParameters();
  size_t offset = (selfSymbol != nullptr) ? 1U : 0U;
  for (size_t i = 0; i < templateParams.size() && (i + offset) < paramTypes.size(); ++i) {
    const std::string& typeName = templateParams[i]->type->getName();
    if (genericNameSet.contains(typeName) && !env.contains(typeName)) {
      env[typeName] = paramTypes[i + offset];
    }
  }
  auto saved = currentGenericTypes;
  currentGenericTypes = std::move(env);

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
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
  funcType->setReturnType(returnType);
  auto* typePtr = cacheType(std::move(funcType));
  auto mangledName = getMangledName(node->getSpan(), node->getName(), concreteParamTypes, selfSymbol != nullptr);
  auto func = std::make_unique<Value>(node->getName(), typePtr);
  func->setMangledName(mangledName);
  func->setExported(node->isExported());
  auto linkage = node->isExported() ? Function::ExternalLinkage : Function::PrivateLinkage;
  auto* llvmFuncType = FunctionType::get(returnType->getLlvmType(), paramLLVMTypes, node->getVarArgs());
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
                              const std::vector<lesma::Type*>& constructorArgTypes) -> lesma::Value* {
  auto genericNames = node->getGenericParams();

  const FuncDecl* constructorDecl = nullptr;
  for (auto* method : node->getMethods()) {
    if (method->getName() == "new") {
      constructorDecl = method;
      break;
    }
  }

  std::unordered_map<std::string, lesma::Type*> env;
  std::unordered_set<std::string> genericNameSet(genericNames.begin(), genericNames.end());
  if (constructorDecl != nullptr) {
    auto params = constructorDecl->getParameters();
    for (size_t i = 0; i < params.size() && i < constructorArgTypes.size(); ++i) {
      const std::string& typeName = params[i]->type->getName();
      if (genericNameSet.contains(typeName) && !env.contains(typeName)) {
        env[typeName] = constructorArgTypes[i];
      }
    }
  }

  std::string key = node->getIdentifier();
  for (const auto& gn : genericNames) {
    if (env.contains(gn)) {
      key += "|" + env[gn]->toString();
    }
  }
  if (auto it = specializedClasses.find(key); it != specializedClasses.end()) {
    return it->second;
  }

  auto saved = currentGenericTypes;
  currentGenericTypes = env;

  std::string concreteName = node->getIdentifier();
  for (const auto& gn : genericNames) {
    if (env.contains(gn)) {
      concreteName += "_" + env[gn]->toString();
    }
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
    fields.push_back(std::make_unique<Field>(field->getIdentifier()->getValue(),
                                             result->getType(),
                                             std::move(defaultVal)));
  }

  auto* structType = llvm::StructType::create(
      theModule->getContext(), elementLLVMTypes, concreteName);
  auto type = std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
  auto* typePtr = type.get();
  scope->insertType(concreteName, std::move(type));

  auto structSymbol = std::make_unique<Value>(concreteName, typePtr);
  structSymbol->setExported(node->isExported());
  auto* structSymbolPtr = structSymbol.get();

  auto* selfType = cacheType(
      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  methodSelfSymbols.push_back(
      std::make_unique<Value>(concreteName, selfType));
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

  scope->insertSymbol(std::move(structSymbol));
  specializedClasses.emplace(key, structSymbolPtr);
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
  lesma::Value* existing = scope->lookup(name);

  if (existing != nullptr && existing->getLlvmValue() == nullptr) {
    // Reuse symbol from typecheck; fill in LLVM value
    lesma::Type* type = existing->getType();
    getOrCreateLlvmType(type);
    llvm::Type* allocaTy = type->is(BaseType::TY_CLASS)
                               ? builder->getPtrTy()
                               : type->getLlvmType();
    auto* ptr = builder->CreateAlloca(allocaTy, nullptr, name);
    existing->setLlvmValue(ptr);
    existing->setMutable(node->getMutability());
    const bool isPtrToClass = type->is(BaseType::TY_PTR) &&
                              type->getElementType() != nullptr &&
                              type->getElementType()->is(BaseType::TY_CLASS);
    if (node->getValue() != nullptr) {
      node->getValue()->accept(*this);
      auto val = std::move(result);
      lesma::Type* castTarget =
          (type->is(BaseType::TY_CLASS) || isPtrToClass)
              ? (type->is(BaseType::TY_CLASS) ? type : type->getElementType())
              : type;
      auto castVal = cast(node->getSpan(), val.get(), castTarget);
      builder->CreateStore(castVal->getLlvmValue(), ptr);
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
    type = cacheType(
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  }
  const bool isPtrToClass =
      type->is(BaseType::TY_PTR) && type->getElementType()->is(BaseType::TY_CLASS);
  auto symbol = std::make_unique<Value>(name, type,
                                        node->getType() != nullptr
                                            ? SymbolState::INITIALIZED
                                            : SymbolState::DECLARED);
  symbol->setLlvmValue(ptr);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));

  if (node->getValue() != nullptr) {
    lesma::Type* castTarget = isPtrToClass ? type->getElementType() : type;
    auto castVal = cast(node->getSpan(), val.get(), castTarget);
    builder->CreateStore(castVal->getLlvmValue(), ptr);
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
    auto* bIfTrue =
        llvm::BasicBlock::Create(theModule->getContext(), "if.true");
    bIfTrue->insertInto(parentFct);
    auto* bIfFalse = bEnd;
    if (i + 1 < node->getConds().size()) {
      bIfFalse = llvm::BasicBlock::Create(theModule->getContext(), "if.false");
      bIfFalse->insertInto(parentFct);
    }

    node->getConds().at(i)->accept(*this);
    builder->CreateCondBr(result->getLlvmValue(), bIfTrue, bIfFalse);
    builder->SetInsertPoint(bIfTrue);

    scope = scope->createChildBlock("if");
    blockHadReturn = false;
    node->getBlocks().at(i)->accept(*this);

    if (!isBreak && !blockHadReturn) {
      builder->CreateBr(bEnd);
    }

    scope = scope->getParent();
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
  scope = scope->createChildBlock("while");

  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();

  // Create blocks
  llvm::BasicBlock* bCond =
      llvm::BasicBlock::Create(theModule->getContext(), "while.cond");
  llvm::BasicBlock* bLoop =
      llvm::BasicBlock::Create(theModule->getContext(), "while");
  llvm::BasicBlock* bEnd =
      llvm::BasicBlock::Create(theModule->getContext(), "while.end");

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

  scope = scope->getParent();
  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::visit(const FuncDecl* node) -> void {
  if (!node->getGenericParams().empty()) {
    auto savedGenerics = currentGenericTypes;
    for (const auto& name : node->getGenericParams()) {
      currentGenericTypes[name] = cacheType(std::make_unique<Type>(name));
    }
    genericFunctions[node->getName()] = node;
    std::vector<std::unique_ptr<Field>> fields;
    if (selfSymbol != nullptr) fields.push_back(std::make_unique<Field>("self", selfSymbol->getType()));
    for (auto* param : node->getParameters()) {
      fields.push_back(std::make_unique<Field>(param->name, cacheType(std::make_unique<Type>(param->type->getName()))));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
    funcType->setReturnType(cacheType(std::make_unique<Type>(node->getReturnType()->getName())));
    auto* typePtr = cacheType(std::move(funcType));
    auto funcSymbol = std::make_unique<Value>(node->getName(), typePtr);
    funcSymbol->setExported(node->isExported());
    scope->insertSymbol(std::move(funcSymbol));
    currentGenericTypes = std::move(savedGenerics);
    return;
  }
  if (selfSymbol != nullptr && node->getName() == "new" &&
      node->getReturnType()->getType() != TokenType::VOID_TYPE) {
    throw CodegenError(node->getSpan(),
                       "Cannot create class method new with return type {}",
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
      auto* ptrType = cacheType(std::make_unique<Type>(
          BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult &&
        !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(
          node->getSpan(),
          "Declared parameter type and default value do not match for {}",
          param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType,
                                             std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* returnType = result->getType();
  getOrCreateLlvmType(returnType);

  lesma::Value* existingFunc =
      scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc != nullptr && existingFunc->getLlvmValue() == nullptr) {
    // Reuse symbol from typecheck; create LLVM function and set it
    auto mangledName = getMangledName(node->getSpan(), node->getName(),
                                      paramTypes, selfSymbol != nullptr);
    auto linkage =
        shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;
    llvm::FunctionType* funcType = FunctionType::get(
        returnType->getLlvmType(), paramLLVMTypes, node->getVarArgs());
    Function* f =
        Function::Create(funcType, linkage, mangledName, *theModule);
    existingFunc->getType()->setLlvmType(funcType);
    existingFunc->setLlvmValue(f);
    existingFunc->setMangledName(mangledName);
    prototypes.emplace_back(existingFunc, node, selfSymbol);
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto mangledName = getMangledName(node->getSpan(), node->getName(),
                                    paramTypes, selfSymbol != nullptr);
  auto linkage =
      shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  llvm::FunctionType* funcType = FunctionType::get(
      returnType->getLlvmType(), paramLLVMTypes, node->getVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);

  auto funcSymbol = std::make_unique<Value>(
      node->getName(),
      std::make_unique<Type>(BaseType::TY_FUNCTION, funcType,
                             std::move(fields)),
      f);
  funcSymbol->getType()->setReturnType(returnType);
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(mangledName);
  auto* funcSymbolPtr = funcSymbol.get();
  scope->insertSymbol(std::move(funcSymbol));

  prototypes.emplace_back(funcSymbolPtr, node, selfSymbol);
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
      auto* ptrType = cacheType(std::make_unique<Type>(
          BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult &&
        !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(
          node->getSpan(),
          "Declared parameter type and default value do not match for {}",
          param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType,
                                             std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* retType = nullptr;
  if (!result) {
    retType = cacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  } else {
    retType = result->getType();
    getOrCreateLlvmType(retType);
  }

  lesma::Value* existingFunc =
      scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc != nullptr && existingFunc->getLlvmValue() != nullptr) {
    return; // Already declared (e.g. from another module)
  }
  if (existingFunc != nullptr && existingFunc->getLlvmValue() == nullptr) {
    // Reuse symbol from typecheck
    Function* f = nullptr;
    if (theModule->getFunction(node->getName()) != nullptr) {
      f = theModule->getFunction(node->getName());
    } else {
      FunctionType* ft = FunctionType::get(retType->getLlvmType(),
                                           paramLLVMTypes, node->getVarArgs());
      f = llvm::cast<Function>(
          theModule->getOrInsertFunction(node->getName(), ft).getCallee());
      if (node->isExported()) {
        f->setLinkage(llvm::GlobalValue::ExternalLinkage);
      }
    }
    existingFunc->getType()->setLlvmType(f->getFunctionType());
    existingFunc->setLlvmValue(f);
    existingFunc->setMangledName(node->getName());
    return;
  }

  Function* f = nullptr;
  if (theModule->getFunction(node->getName()) != nullptr) {
    f = theModule->getFunction(node->getName());
  } else {
    FunctionType* ft = FunctionType::get(retType->getLlvmType(), paramLLVMTypes,
                                         node->getVarArgs());
    f = llvm::cast<Function>(
        theModule->getOrInsertFunction(node->getName(), ft).getCallee());
    if (node->isExported()) {
      f->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }

  auto funcSymbol = std::make_unique<Value>(
      node->getName(),
      std::make_unique<Type>(BaseType::TY_FUNCTION, f->getFunctionType(),
                             std::move(fields)),
      f);
  funcSymbol->getType()->setReturnType(retType);
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(node->getName());
  scope->insertSymbol(std::move(funcSymbol));
}

auto Codegen::visit(const Assignment* node) -> void {
  lesma::Value* lhs = nullptr;
  std::unique_ptr<lesma::Value> lhsOwner; // Holds owned lhs if from result
  isAssignment = true;
  bool isPtr = false;
  if (dynamic_cast<Literal*>(node->getLeftHandSide()) != nullptr) {
    auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide());
    auto* symbol = scope->lookup(lit->getValue());
    if (symbol == nullptr) {
      throw CodegenError(node->getSpan(), "Variable not found: {}",
                         lit->getValue());
    }
    if (!symbol->getMutability()) {
      throw CodegenError(node->getSpan(),
                         "Assigning immutable variable a new value");
    }

    lhs = symbol;
  } else if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    lhsOwner = std::move(result);
    lhs = lhsOwner.get();
    // DotOp on class field yields pointer (StructGEP); cast RHS to element type.
    isPtr = true;
  } else {
    throw CodegenError(
        node->getSpan(), "Unable to assign {} to {}",
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
    throw CodegenError(node->getSpan(), "Invalid operator: {}",
                       NAMEOF_ENUM(node->getOperator()));
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
    throw CodegenError(node->getSpan(),
                       "Cannot continue without being in a loop");
  }

  auto* block = continueBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::visit(const Return* node) -> void {
  // Check if it's top-level
  if (builder->GetInsertBlock()->getParent() == topLevelFunc) {
    throw CodegenError(node->getSpan(),
                       "Return statements are not allowed at top-level");
  }

  // Execute all deferred statements
  for (auto* inst : deferStack.top()) {
    inst->accept(*this);
  }

  isReturn = true;
  blockHadReturn = true;

  if (node->getValue() == nullptr) {
    if (currentFunction->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      throw CodegenError(
          node->getSpan(),
          "Return type does not match the function return type, expected {}, "
          "actual void",
          currentFunction->getType()->getReturnType()->toString());
    }
  } else {
    node->getValue()->accept(*this);
    getOrCreateLlvmType(result->getType());
    if (builder->getCurrentFunctionReturnType() ==
        result->getType()->getLlvmType()) {
      builder->CreateRet(result->getLlvmValue());
    } else {
      throw CodegenError(
          node->getSpan(),
          "Return type does not match the function return type, expected {}, "
          "actual {}",
          currentFunction->getType()->getReturnType()->toString(),
          result->getType()->toString());
    }
  }
}

auto Codegen::visit(const Defer* node) -> void {
  deferStack.top().push_back(node->getStatement());
}

auto Codegen::visit(const UnimplementedStatement* node) -> void {
  throw CodegenError(node->getSpan(), "{}", node->getMessage());
}

auto Codegen::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Codegen::visit(const Import* node) -> void {
  compileModule(node->getSpan(), node->getFilePath(), node->isStd(),
                node->getAlias(), node->getImportAll(), node->getImportScope(),
                node->getImportedNames());
}

auto Codegen::visit(const Class* node) -> void {
  if (!node->getGenericParams().empty()) {
    genericClasses[node->getIdentifier()] = node;
    return;
  }

  lesma::Value* existingStruct = scope->lookupStruct(node->getIdentifier());
  if (existingStruct != nullptr &&
      existingStruct->getType()->getLlvmType() == nullptr) {
    // Reuse symbol from typecheck; fill LLVM struct type and visit methods
    lesma::Type* type = existingStruct->getType();
    std::vector<llvm::Type*> elementLLVMTypes;
    for (auto* f : type->getFields()) {
      elementLLVMTypes.push_back(getOrCreateLlvmType(f->type));
    }
    auto* structType = llvm::StructType::create(
        theModule->getContext(), elementLLVMTypes, node->getIdentifier());
    type->setLlvmType(structType);

    // Use the typechecker's self (ptr-to-class) type from typeCache so method
    // lookup matches (same Type* as in function param types).
    lesma::Type* selfType = nullptr;
    for (auto& t : typeCache) {
      if (t->is(BaseType::TY_PTR) && t->getElementType() == type) {
        selfType = t.get();
        break;
      }
    }
    if (selfType == nullptr) {
      selfType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
    } else {
      getOrCreateLlvmType(selfType);
    }
    methodSelfSymbols.push_back(
        std::make_unique<Value>(node->getIdentifier(), selfType));
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
    selfSymbol = nullptr;
    if (!hasConstructor) {
      throw CodegenError(node->getSpan(), "Class {} has no constructors",
                         node->getIdentifier());
    }
    return;
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
    fields.push_back(std::make_unique<Field>(field->getIdentifier()->getValue(),
                                             result->getType(),
                                             std::move(defaultVal)));
  }

  llvm::StructType* structType = llvm::StructType::create(
      theModule->getContext(), elementLLVMTypes, node->getIdentifier());

  auto type =
      std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
  auto* typePtr = type.get();
  auto structSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
  structSymbol->setExported(node->isExported());
  auto* structSymbolPtr = structSymbol.get();

  scope->insertType(node->getIdentifier(), std::move(type));
  scope->insertSymbol(std::move(structSymbol));

  auto* selfType = cacheType(
      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  methodSelfSymbols.push_back(
      std::make_unique<Value>(node->getIdentifier(), selfType));
  methodSelfSymbols.back()->setExported(node->isExported());
  selfSymbol = methodSelfSymbols.back().get();
  auto hasConstructor = false;
  for (auto* func : node->getMethods()) {
    func->accept(*this);
    if (func->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = {selfSymbol->getType()};
      auto* constructor = scope->lookupFunction("new", constructorParams);
      structSymbolPtr->setConstructor(constructor);
    }
  }

  if (!hasConstructor) {
    throw CodegenError(node->getSpan(), "Class {} has no constructors",
                       node->getIdentifier());
  }

  selfSymbol = nullptr;
}

auto Codegen::visit(const Enum* node) -> void {
  lesma::Value* existingEnum = scope->lookupStruct(node->getIdentifier());
  if (existingEnum != nullptr &&
      existingEnum->getType()->getLlvmType() == nullptr) {
    lesma::Type* type = existingEnum->getType();
    std::vector<llvm::Type*> elementTypes;
    for (auto* f : type->getFields()) {
      elementTypes.push_back(getOrCreateLlvmType(f->type));
    }
    if (elementTypes.empty()) {
      elementTypes.push_back(builder->getInt8Ty());
    }
    auto* structType = llvm::StructType::create(
        theModule->getContext(), elementTypes, node->getIdentifier());
    type->setLlvmType(structType);
    return;
  }

  std::vector<llvm::Type*> elementTypes = {builder->getInt8Ty()};
  llvm::StructType* structType = llvm::StructType::create(
      theModule->getContext(), elementTypes, node->getIdentifier());
  std::vector<std::unique_ptr<Field>> fields;

  for (const auto& field : node->getValues()) {
    auto* fieldType = cacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    fields.push_back(std::make_unique<Field>(field, fieldType));
  }

  auto type =
      std::make_unique<Type>(BaseType::TY_ENUM, structType, std::move(fields));
  auto* typePtr = type.get();
  auto structSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
  structSymbol->setExported(node->isExported());

  scope->insertType(node->getIdentifier(), std::move(type));
  scope->insertSymbol(std::move(structSymbol));
}

auto Codegen::visit(const FuncCall* node) -> void {
  result = genFuncCall(node, {});
}

auto Codegen::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  auto left = std::move(result);
  node->getRight()->accept(*this);
  auto right = std::move(result);
  lesma::Type* finalType =
      CodegenTypeUtils::getExtendedType(left->getType(), right->getType());

  switch (node->getOperator()) {
  case TokenType::MINUS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFSub(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSub(left->getLlvmValue(), right->getLlvmValue()));
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
          "", finalType,
          builder->CreateFAdd(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateAdd(left->getLlvmValue(), right->getLlvmValue()));
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
          "", finalType,
          builder->CreateFMul(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateMul(left->getLlvmValue(), right->getLlvmValue()));
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
          "", finalType,
          builder->CreateFDiv(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSDiv(left->getLlvmValue(), right->getLlvmValue()));
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
          "", finalType,
          builder->CreateFRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::POWER:
    if (finalType == nullptr) {
      break;
    }

    if (!right->getType()->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw CodegenError(
          node->getSpan(), "Cannot use non-numbers for power coefficient: {}",
          node->getRight()->toString(sourceManager.get(), "", true));
    }

    throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
  case TokenType::EQUAL_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(
            node->getSpan(),
            "Illegal comparison of two different enums: {} and {}", leftName,
            rightName);
      }

      llvm::Value* leftVal =
          builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal =
          builder->CreateExtractValue(right->getLlvmValue(), {0});
      result =
          std::make_unique<Value>("",
                                  cacheType(std::make_unique<Type>(
                                      BaseType::TY_BOOL, builder->getInt1Ty())),
                                  builder->CreateICmpEQ(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::BANG_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(
            node->getSpan(),
            "Illegal comparison of two different enums: {} and {}", leftName,
            rightName);
      }

      llvm::Value* leftVal =
          builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal =
          builder->CreateExtractValue(right->getLlvmValue(), {0});
      result =
          std::make_unique<Value>("",
                                  cacheType(std::make_unique<Type>(
                                      BaseType::TY_BOOL, builder->getInt1Ty())),
                                  builder->CreateICmpNE(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpONE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
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
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
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
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
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
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
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
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          cacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::AND:
    if (!left->getType()->is(BaseType::TY_BOOL) &&
        !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(
          node->getSpan(), "Cannot use non-booleans for and: {} - {}",
          node->getLeft()->toString(sourceManager.get(), "", true),
          node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "",
        cacheType(
            std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalAnd(left->getLlvmValue(), right->getLlvmValue()));
    return;
  case TokenType::OR:
    if (!left->getType()->is(BaseType::TY_BOOL) &&
        !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(
          node->getSpan(), "Cannot use non-booleans for or: {} - {}",
          node->getLeft()->toString(sourceManager.get(), "", true),
          node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "",
        cacheType(
            std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalOr(left->getLlvmValue(), right->getLlvmValue()));
    return;
  default:
    throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}",
                       NAMEOF_ENUM(node->getOperator()));
  }

  throw CodegenError(node->getSpan(),
                     "Operator {} is not supported for types {} and {}",
                     NAMEOF_ENUM(node->getOperator()),
                     left->getType()->toString(),
                     right->getType()->toString());
}

auto Codegen::visit(const DotOp* node) -> void {
  if (auto* left = dynamic_cast<Literal*>(node->getLeft())) {
    if (left->getType() != TokenType::IDENTIFIER) {
      throw CodegenError(
          node->getLeft()->getSpan(),
          "Expected identifier left-hand of dot operator, found {}",
          node->getRight()->toString(sourceManager.get(), "", true));
    }

    auto* typeSym = scope->lookupType(left->getValue());
    if (typeSym != nullptr) {
      // Assuming it's an enum or statically accessed class
      if (!typeSym->isOneOf(
              {BaseType::TY_ENUM, BaseType::TY_CLASS, BaseType::TY_IMPORT})) {
        throw CodegenError(node->getLeft()->getSpan(),
                           "Cannot apply dot accessor on {}", left->getValue());
      }

      auto* right = dynamic_cast<Literal*>(node->getRight());
      if (typeSym->is(BaseType::TY_ENUM)) {
        // Check if right-hand expression is an identifier expression
        if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
          throw CodegenError(
              node->getRight()->getSpan(),
              "Expected identifier right-hand of dot operator, found {}",
              node->getRight()->toString(sourceManager.get(), "", true));
        }

        if (right->getType() != TokenType::IDENTIFIER) {
          throw CodegenError(
              node->getRight()->getSpan(),
              "Expected identifier right-hand of dot operator, found {}",
              node->getRight()->toString(sourceManager.get(), "", true));
        }

        // Setting value to the enum
        auto val = TypeUtils::findIndexInFields(typeSym, right->getValue());
        // Field not found in enum
        if (val == -1) {
          throw CodegenError(node->getLeft()->getSpan(),
                             "Identifier {} not in {}", right->getValue(),
                             left->getValue());
        }

        auto* structVal = scope->lookupStruct(left->getValue());
        auto* enumPtr =
            builder->CreateAlloca(structVal->getType()->getLlvmType());
        auto* field = builder->CreateStructGEP(
            structVal->getType()->getLlvmType(), enumPtr, 0);
        builder->CreateStore(builder->getInt8(val), field);
        // Enum variant literals are returned by value (not pointer).
        auto* enumVal =
            builder->CreateLoad(structVal->getType()->getLlvmType(), enumPtr);

        result = std::make_unique<Value>("", structVal->getType(), enumVal);
        return;
      }

      if (typeSym->is(BaseType::TY_IMPORT)) {
        std::string field;
        FuncCall const* method = nullptr;

        if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
            (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
          throw CodegenError(
              node->getRight()->getSpan(),
              "Expected identifier or method call right-hand of dot operator, "
              "found {}",
              node->getRight()->toString(sourceManager.get(), "", true));
        }

        if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
            dynamic_cast<Literal*>(node->getRight())->getType() ==
                TokenType::IDENTIFIER) {
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
        throw CodegenError(node->getLeft()->getSpan(),
                           "Cannot apply dot accessor on {}", left->getValue());
      }

      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
        throw CodegenError(
            node->getRight()->getSpan(),
            "Expected identifier or method call right-hand of dot operator, "
            "found {}",
            node->getRight()->toString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
          TokenType::IDENTIFIER ==
              dynamic_cast<Literal*>(node->getRight())->getType()) {
        field = dynamic_cast<Literal*>(node->getRight())->getValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->getRight());
      }

      // lookupStruct returns by LLVM struct name; ensure display name matches.
      auto* cls =
          scope->lookupStruct(lesmaType->getLlvmType()->getStructName().str());
      cls->setName(lesmaType->getLlvmType()->getStructName().str());

      if (cls->getType()->is(BaseType::TY_CLASS)) {
        if (!field.empty()) {
          auto index = TypeUtils::findIndexInFields(cls->getType(), field);
          auto* type = TypeUtils::findTypeInFields(cls->getType(), field);
          if (index == -1) {
            throw CodegenError(node->getRight()->getSpan(),
                               "Could not find field {} in {}", field,
                               result->getType()
                                   ->getElementType()
                                   ->getLlvmType()
                                   ->getStructName()
                                   .str());
          }

          auto* ptr = builder->CreateStructGEP(cls->getType()->getLlvmType(),
                                               result->getLlvmValue(), index);
          if (isAssignment) {
            result = std::make_unique<Value>(
                "",
                cacheType(std::make_unique<Type>(BaseType::TY_PTR,
                                                 builder->getPtrTy(), type)),
                ptr);
            return;
          }
          //                    auto &x = cls->GetType()->GetFields()[index];
          result = std::make_unique<Value>(
              "", type, builder->CreateLoad(type->getLlvmType(), ptr));
          return;
        }
        if (method != nullptr) {
          selfSymbol = cls;
          result = genFuncCall(method, {result.get()});
          selfSymbol = nullptr;
          return;
        }
      } else {
        throw CodegenError(node->getLeft()->getSpan(),
                           "Cannot find related class {}",
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

  llvm::Value* val = nullptr;

  if (node->getOperator() == TokenType::IS) {
    val =
        leftType->isEqual(rightType) ? builder->getTrue() : builder->getFalse();
  } else {
    val =
        leftType->isEqual(rightType) ? builder->getFalse() : builder->getTrue();
  }

  result =
      std::make_unique<Value>("",
                              cacheType(std::make_unique<Type>(
                                  BaseType::TY_BOOL, builder->getInt1Ty())),
                              val);
}

auto Codegen::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);

  llvm::Value* val = nullptr;
  lesma::Type* type = result->getType();
  std::unique_ptr<lesma::Type>
      ptrTypeHolder; // Keep owned type alive if created

  if (node->getOperator() == TokenType::MINUS) {
    if (result->getType()->is(BaseType::TY_INT)) {
      val = builder->CreateNeg(result->getLlvmValue());
    } else if (result->getType()->is(BaseType::TY_FLOAT)) {
      val = builder->CreateFNeg(result->getLlvmValue());
    } else {
      throw CodegenError(
          node->getSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->getOperator()),
          node->getExpression()->toString(sourceManager.get(), "", true));
    }
  } else if (node->getOperator() == TokenType::NOT) {
    if (result->getType()->is(BaseType::TY_BOOL)) {
      val = builder->CreateNot(result->getLlvmValue());
    } else {
      throw CodegenError(
          node->getSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->getOperator()),
          node->getExpression()->toString(sourceManager.get(), "", true));
    }
  } else if (node->getOperator() == TokenType::STAR) {
    if (result->getType()->is(BaseType::TY_PTR)) {
      val = builder->CreateLoad(
          result->getType()->getElementType()->getLlvmType(),
          result->getLlvmValue());
      type = result->getType()->getElementType();
    } else {
      throw CodegenError(
          node->getSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->getOperator()),
          node->getExpression()->toString(sourceManager.get(), "", true));
    }
  } else if (node->getOperator() == TokenType::AMPERSAND) {
    val = builder->CreateAlloca(result->getType()->getLlvmType());
    ptrTypeHolder = std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), result->getType());
    type = ptrTypeHolder.get();
    builder->CreateStore(result->getLlvmValue(), val);
  } else {
    throw CodegenError(
        node->getSpan(), "Unknown unary operator, cannot apply {} to {}",
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

auto Codegen::visit(const Literal* node) -> void {
  // Cache Types to prevent dangling pointers when result is reassigned
  if (node->getType() == TokenType::DOUBLE) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(
        "", type,
        ConstantFP::get(theModule->getContext(),
                        APFloat(std::stod(node->getValue()))));
  } else if (node->getType() == TokenType::INTEGER) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(
        "", type,
        ConstantInt::getSigned(builder->getInt64Ty(),
                               std::stoi(node->getValue())));
  } else if (node->getType() == TokenType::BOOL) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(
        "", type,
        node->getValue() == "true" ? builder->getTrue() : builder->getFalse());
  } else if (node->getType() == TokenType::STRING) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(
        "", type, builder->CreateGlobalString(node->getValue()));
  } else if (node->getType() == TokenType::NIL) {
    auto* type = cacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(
        "", type, ConstantPointerNull::getNullValue(builder->getPtrTy()));
  } else if (node->getType() == TokenType::IDENTIFIER) {
    // Look this variable up in the function.
    auto* val = scope->lookup(node->getValue());
    if (val == nullptr) {
      throw CodegenError(node->getSpan(), "Unknown variable name {}",
                         node->getValue());
    }
    getOrCreateLlvmType(val->getType());

    if (val->getType()->isOneOf({BaseType::TY_CLASS})) {
      // If it's a class, don't load the value - make a copy from symbol table
      result = std::make_unique<Value>(*val);
    } else {
      // Load the value.
      llvm::Value* llvmVal = builder->CreateLoad(val->getType()->getLlvmType(),
                                                 val->getLlvmValue());
      result = std::make_unique<Value>("", val->getType(), llvmVal);
    }
  } else {
    throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
  }
}

auto Codegen::visit(const Else* /*node*/) -> void {
  auto* type = cacheType(
      std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
  result = std::make_unique<Value>(
      "", type, llvm::ConstantInt::getTrue(theModule->getContext()));
}

auto Codegen::cast(llvm::SMRange span, lesma::Value* val,
                   lesma::Type* type) -> std::unique_ptr<lesma::Value> {
  return CodegenTypeUtils::cast(span, val, type, builder.get());
}

auto Codegen::getMangledName(llvm::SMRange span, std::string funcName,
                             const std::vector<lesma::Type*>& paramTypes,
                             bool isMethodFlag, std::string alias) -> std::string {
  alias = alias.empty() ? this->alias : alias;
  std::string name =
      (alias.empty() ? "" : "&" + alias + "=>") +
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

auto Codegen::emitCompoundAssign(llvm::SMRange span, TokenType op,
                                  lesma::Value* lhs,
                                  lesma::Value* value) -> void {
  if (!lhs->getType()->is(BaseType::TY_FLOAT) &&
      !lhs->getType()->is(BaseType::TY_INT)) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(op));
  }
  const bool isFloat = lhs->getType()->is(BaseType::TY_FLOAT);
  auto* varVal =
      builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
  llvm::Value* newVal = nullptr;

  switch (op) {
  case TokenType::PLUS_EQUAL:
    newVal = isFloat ? builder->CreateFAdd(value->getLlvmValue(), varVal)
                    : builder->CreateAdd(value->getLlvmValue(), varVal);
    break;
  case TokenType::MINUS_EQUAL:
    newVal = isFloat ? builder->CreateFSub(value->getLlvmValue(), varVal)
                    : builder->CreateSub(value->getLlvmValue(), varVal);
    break;
  case TokenType::SLASH_EQUAL:
    newVal = isFloat ? builder->CreateFDiv(value->getLlvmValue(), varVal)
                    : builder->CreateSDiv(value->getLlvmValue(), varVal);
    break;
  case TokenType::STAR_EQUAL:
    newVal = isFloat ? builder->CreateFMul(value->getLlvmValue(), varVal)
                    : builder->CreateMul(value->getLlvmValue(), varVal);
    break;
  case TokenType::MOD_EQUAL:
    newVal = isFloat ? builder->CreateFRem(value->getLlvmValue(), varVal)
                    : builder->CreateSRem(value->getLlvmValue(), varVal);
    break;
  default:
    throw CodegenError(span, "Invalid compound operator: {}",
                       NAMEOF_ENUM(op));
  }
  builder->CreateStore(newVal, lhs->getLlvmValue());
}

auto Codegen::genFuncCall(const FuncCall* node,
                          const std::vector<lesma::Value*>& extraParams = {})
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;

  for (auto* arg : extraParams) {
    lesma::Type* argType = arg->getType();
    // Method params expect pointer-to-class; use same Type* as in scope for lookup
    if (argType->is(BaseType::TY_CLASS)) {
      for (auto& t : typeCache) {
        if (t->is(BaseType::TY_PTR) && t->getElementType() == argType) {
          argType = t.get();
          break;
        }
      }
      if (argType->is(BaseType::TY_CLASS)) {
        argType = cacheType(
            std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), argType));
      }
    }
    paramTypes.push_back(argType);
    paramsLLVM.push_back(arg->getLlvmValue());
  }

  for (auto* arg : node->getArguments()) {
    arg->accept(*this);
    lesma::Type* argType = result->getType();
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
        argType = cacheType(
            std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), argType));
      }
    }
    paramTypes.push_back(argType);
    paramsLLVM.push_back(result->getLlvmValue());
  }

  Value* symbol = nullptr;
  // Check if it's a constructor like `Classname()`
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->lookupStruct(node->getName());
  llvm::Value* classPtr = nullptr;
  // Keep the Type alive for the duration of the lookup
  std::unique_ptr<Type> selfParamType;

  // Generic class: specialize before using
  if (classSym == nullptr || (classSym->getType()->is(BaseType::TY_CLASS) &&
                              classSym->getType()->getLlvmType() == nullptr)) {
    if (auto git = genericClasses.find(node->getName()); git != genericClasses.end()) {
      classSym = specializeClass(git->second, paramTypes);
    }
  }

  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    // It's a class constructor, allocate and add self param
    classPtr = builder->CreateAlloca(classSym->getType()->getLlvmType());
    paramsLLVM.insert(paramsLLVM.begin(), classPtr);
    selfParamType = std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), classSym->getType());
    paramTypes.insert(paramTypes.begin(), selfParamType.get());

    selfSymbol = classSym;
    symbol = scope->lookupFunction("new", paramTypes);
  } else {
    symbol = scope->lookupFunction(node->getName(), paramTypes);
  }

  if (symbol == nullptr) {
    throw CodegenError(node->getSpan(), "{} {} not in current scope.",
                       classSym != nullptr ? "Constructor for" : "Function",
                       node->getName());
  }

  if (symbol->getType()->getFields().size() == paramTypes.size() &&
      symbol->getLlvmValue() == nullptr) {
    if (auto git = genericFunctions.find(node->getName()); git != genericFunctions.end()) {
      std::vector<std::string> genericNames = git->second->getGenericParams();
      symbol = specializeFunction(git->second, paramTypes, genericNames);
    }
  }

  if (!symbol->getType()->isOneOf(
          {BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
    throw CodegenError(node->getSpan(),
                       "Symbol {} is not a function or constructor.",
                       node->getName());
  }

  if (symbol->getType()->getFields().size() > paramsLLVM.size()) {
    auto fields = symbol->getType()->getFields();

    for (auto* field : fields) {
      if (field->defaultValue == nullptr) {
        throw CodegenError(node->getSpan(),
                           "Something bad happened, lookup found a function "
                           "with incorrect defaults",
                           node->getName());
      }
      paramsLLVM.push_back(field->defaultValue->getLlvmValue());
    }
  }
  auto* func =
      llvm::cast<Function>(symbol->getType()->is(BaseType::TY_CLASS)
                               ? symbol->getConstructor()->getLlvmValue()
                               : symbol->getLlvmValue());
  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    builder->CreateCall(func, paramsLLVM);
    selfSymbol = selfSymbolTmp;

    return std::make_unique<Value>("", classSym->getType(), classPtr);
  }

  return std::make_unique<Value>("", symbol->getType()->getReturnType(),
                                 builder->CreateCall(func, paramsLLVM));
}
