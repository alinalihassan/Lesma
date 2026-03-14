#include "Codegen.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <system_error>
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
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser,
                 std::shared_ptr<SourceMgr> srcMgr, const std::string& filename,
                 std::vector<std::string> imports, bool jit, bool main,
                 std::string alias,
                 const std::shared_ptr<ThreadSafeContext>& context) {
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
  rootScope = std::make_unique<SymbolTable>(nullptr);
  scope = rootScope.get();

  this->alias = std::move(alias);
  this->filename = filename;
  isMain = main;
  isJit = jit;

  importedModules = std::move(imports);
  topLevelFunc = initializeTopLevel();

  // If it's not base.les stdlib, then import it
  if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
    compileModule(llvm::SMRange(), getStdDir() + "base.les", true, "base", true,
                  true, {});
  }
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

  // Verify function
  // TODO: Verify function again, unfortunately functions from other modules
  // have attributes attached without context of usage, and verify gives error
  //    std::string output;
  //    llvm::raw_string_ostream oss(output);
  //    if (llvm::verifyFunction(*F, &oss)) {
  //        F->print(outs());
  //        throw CodegenError(node->GetSpan(), "Invalid Function {}\n{}",
  //        node->GetName(), output);
  //    }

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

  // If module is already imported, don't compile again. Re-enabling this check
  // would avoid recompiling the same file when reached via different import
  // paths, but it currently breaks nested imports (same file imported by
  // multiple modules).
  //    if (std::find(importedModules.begin(), importedModules.end(),
  //    absolutePath) != importedModules.end())
  //        return;

  auto buffer = MemoryBuffer::getFile(absolutePath);
  if (std::error_code ec = buffer.getError()) {
    throw LesmaError(llvm::SMRange(), "Could not read file: {}", absolutePath);
  }

  auto fileId =
      sourceManager->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  // auto sourceStr =
  // sourceManager->getMemoryBuffer(fileId)->getBuffer().str();
  importedModules.push_back(absolutePath);

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
        std::move(parser), sourceManager, absolutePath, importedModules, isJit,
        false, !importToScope ? moduleAlias : "", theContext);
    codegen->run();

    // Optimize
    codegen->optimize(OptimizationLevel::O3);
    codegen->theModule->setModuleIdentifier(filepath);

    importedModules = std::move(codegen->importedModules);

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

    if (isJit) {
      // Add the module to JIT
      cantFail(theJit->addIRModule(ThreadSafeModule(
                   std::move(codegen->theModule), *theContext)),
               fmt::format("Failed adding import {} to JIT", filename).c_str());
    } else {
      // Create object file to be linked
      std::string objFile = fmt::format("tmp{}", objectFiles.size());
      codegen->writeToObjectFile(objFile);
      objectFiles.push_back(fmt::format("{}.o", objFile));
    }

    // Import Symbols. Ownership: codegen->rootScope is moved into
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
        auto* f = llvm::dyn_cast<Function>(sym->getLlvmValue());
        auto* fTy = llvm::cast<FunctionType>(sym->getType()->getLlvmType());

        if (isJit) {
          // Insert the function declaration, since we linked the modules
          // earlier
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
             isMethod(sym->getMangledName()))) {
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
    importedScopes.push_back(std::move(codegen->rootScope));
    codegen->scope = nullptr; // Clear navigation pointer (rootscope now moved)

    // Transfer type cache to keep cached types alive
    for (auto& type : codegen->typeCache) {
      typeCache.push_back(std::move(type));
    }
  } catch (const LesmaError& err) {
    if (!err.getSpan().isValid()) {
      lesma::print(LogType::ERROR, err.what());
    } else {
      showInline(sourceManager.get(), fileId, err.getSpan(), absolutePath, true,
                 err.what());
    }

    throw CodegenError(span, "Unable to import {} due to errors", filepath);
  }
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
  deferStack.emplace();
  parser->getAst()->accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  // Visit all statements
  for (auto* inst : instrs) {
    inst->accept(*this);
  }

  // Define the function bodies
  for (auto& prot : prototypes) {
    defineFunction(std::get<0>(prot), std::get<1>(prot), std::get<2>(prot));
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
    auto* type = cacheType(std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), result->getType()));
    result = std::make_unique<Value>(type);
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
    auto* typ = scope->lookupType(node->getName());
    auto* sym = scope->lookupStruct(node->getName());
    if (typ == nullptr || sym->getType()->getLlvmType() == nullptr) {
      throw CodegenError(node->getSpan(), "Type not found: {}",
                         node->getName());
    }

    // Borrow from symbol table - make a shallow copy
    result = sym == nullptr ? std::make_unique<Value>(typ)
                            : std::make_unique<Value>(*sym);
  } else {
    throw CodegenError(node->getSpan(), "Unimplemented type {}",
                       NAMEOF_ENUM(node->getType()));
  }
}

auto Codegen::visit(const Compound* node) -> void {
  for (auto* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

auto Codegen::visit(const VarDecl* node) -> void {
  lesma::Type* type = nullptr;
  std::unique_ptr<lesma::Value> val;

  // TODO: We shouldn't need to use this
  bool isClass = false;

  if (node->getValue() != nullptr) {
    node->getValue()->accept(*this);
    val = std::move(result);
    type = val->getType();
  }

  if (node->getType() != nullptr) {
    node->getType()->accept(*this);
    type = result->getType();
  }

  auto* ptr = builder->CreateAlloca(type->getLlvmType(), nullptr,
                                    node->getIdentifier()->getValue());

  if (type->is(BaseType::TY_CLASS)) {
    // Cache the pointer type to prevent dangling pointers
    type = cacheType(
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
    isClass = true;
  }
  auto symbol = std::make_unique<Value>(node->getIdentifier()->getValue(), type,
                                        node->getType() != nullptr
                                            ? SymbolState::INITIALIZED
                                            : SymbolState::DECLARED);
  symbol->setLlvmValue(ptr);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));

  // Convert declared value to declared type implicitly
  if (node->getValue() != nullptr) {
    auto castVal = cast(node->getSpan(), val.get(),
                        isClass ? type->getElementType() : type);
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

    paramTypes.push_back(typeResult->getType());
    paramLLVMTypes.push_back(typeResult->getType()->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, typeResult->getType(),
                                             std::move(defaultValResult)));
  }

  auto mangledName = getMangledName(node->getSpan(), node->getName(),
                                    paramTypes, selfSymbol != nullptr);
  auto linkage =
      shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  node->getReturnType()->accept(*this);

  llvm::FunctionType* funcType = FunctionType::get(
      result->getType()->getLlvmType(), paramLLVMTypes, node->getVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);

  auto funcSymbol = std::make_unique<Value>(
      node->getName(),
      std::make_unique<Type>(BaseType::TY_FUNCTION, funcType,
                             std::move(fields)),
      f);
  funcSymbol->getType()->setReturnType(result->getType());
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(mangledName);
  auto* funcSymbolPtr = funcSymbol.get();
  scope->insertSymbol(std::move(funcSymbol));

  prototypes.emplace_back(funcSymbolPtr, node, selfSymbol);
  // Even though it's a statement, we pass the func symbol to result so parent
  // classes can modify them
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

    paramTypes.push_back(typeResult->getType());
    paramLLVMTypes.push_back(typeResult->getType()->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, typeResult->getType(),
                                             std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* retType = nullptr;
  if (!result) {
    retType = cacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  } else {
    retType = result->getType();
  }

  Function* f = nullptr;
  if (theModule->getFunction(node->getName()) != nullptr &&
      scope->lookupFunction(node->getName(), paramTypes) != nullptr) {
    return;
  }

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
    // TODO: Fix me, for some reason self.x is a ptr but x is not
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
  llvm::Value* varVal = nullptr;

  switch (node->getOperator()) {
  case TokenType::EQUAL:
    builder->CreateStore(value->getLlvmValue(), lhs->getLlvmValue());
    break;
  case TokenType::PLUS_EQUAL:
    varVal =
        builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
    if (lhs->getType()->is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFAdd(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else if (lhs->getType()->is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateAdd(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
    }
    break;
  case TokenType::MINUS_EQUAL:
    varVal =
        builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
    if (lhs->getType()->is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFSub(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else if (lhs->getType()->is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSub(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
    }
    break;
  case TokenType::SLASH_EQUAL:
    varVal =
        builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
    if (lhs->getType()->is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFDiv(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else if (lhs->getType()->is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSDiv(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
    }
    break;
  case TokenType::STAR_EQUAL:
    varVal =
        builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
    if (lhs->getType()->is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFMul(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else if (lhs->getType()->is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateMul(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
    }
    break;
  case TokenType::MOD_EQUAL:
    varVal =
        builder->CreateLoad(lhs->getType()->getLlvmType(), lhs->getLlvmValue());
    if (lhs->getType()->is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFRem(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else if (lhs->getType()->is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSRem(value->getLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->getLlvmValue());
    } else {
      throw CodegenError(node->getSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
    }
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
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<llvm::Type*> elementLLVMTypes;

  for (auto* field : node->getFields()) {
    if (field->getType() != nullptr) {
      field->getType()->accept(*this);
    } else {
      field->getValue()->accept(*this);
    }

    elementLLVMTypes.push_back(result->getType()->getLlvmType());
    std::unique_ptr<Value> defaultVal;
    if (field->getValue() != nullptr) {
      defaultVal = std::move(result);
      // Re-evaluate to get the type for the Field (result was moved)
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
      } else {
        // Make a copy of the default value to get its type
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

  // Cache the self type to prevent dangling pointers in function Fields
  auto* selfType = cacheType(
      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  auto classSelfSymbol =
      std::make_unique<Value>(node->getIdentifier(), selfType);
  classSelfSymbol->setExported(node->isExported());
  selfSymbol = classSelfSymbol.get();
  auto hasConstructor = false;
  for (auto* func : node->getMethods()) {
    func->accept(*this);
    if (func->getName() == "new") {
      hasConstructor = true;
      // result contains a copy of the func symbol; actual symbol is in
      // SymbolTable Look up the actual constructor from scope - needs self type
      // as first param
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
  // classSelfSymbol unique_ptr goes out of scope and properly deletes the Value
}

auto Codegen::visit(const Enum* node) -> void {
  std::vector<llvm::Type*> elementTypes = {builder->getInt8Ty()};
  llvm::StructType* structType = llvm::StructType::create(
      theModule->getContext(), elementTypes, node->getIdentifier());
  std::vector<std::unique_ptr<Field>> fields;

  for (const auto& field : node->getValues()) {
    // Cache enum field types to prevent dangling pointers
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
  lesma::Type* finalType = getExtendedType(left->getType(), right->getType());

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
                     "Unimplemented binary operator {} for {} and {}",
                     NAMEOF_ENUM(node->getOperator()),
                     node->getLeft()->toString(sourceManager.get(), "", true),
                     node->getRight()->toString(sourceManager.get(), "", true));
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
        auto val = findIndexInFields(typeSym, right->getValue());
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
        // TODO: Returning the enum directly or a ptr to it? We used to return a
        // pointer
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

      // TODO: Somehow, when we call a class method with a variable x,
      //  we lose the class name from cls, so we set it again
      auto* cls =
          scope->lookupStruct(lesmaType->getLlvmType()->getStructName().str());
      cls->setName(lesmaType->getLlvmType()->getStructName().str());

      if (cls->getType()->is(BaseType::TY_CLASS)) {
        if (!field.empty()) {
          auto index = findIndexInFields(cls->getType(), field);
          auto* type = findTypeInFields(cls->getType(), field);
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

auto Codegen::getTypeMangledName(llvm::SMRange span,
                                 lesma::Type* type) -> std::string {
  auto* llvmTy = type->getLlvmType();
  if (type->is(BaseType::TY_BOOL)) {
    return "b";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(8)) {
    return "c";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(16)) {
    return "i16";
  }
  if (type->is(BaseType::TY_INT) && llvmTy->isIntegerTy(32)) {
    return "i32";
  }
  if (type->is(BaseType::TY_INT)) {
    return "i";
  }
  if (type->is(BaseType::TY_FLOAT) && llvmTy->isFloatTy()) {
    return "f32";
  }
  if (type->is(BaseType::TY_FLOAT) && llvmTy->isFloatingPointTy()) {
    return "f";
  }
  if (type->is(BaseType::TY_STRING)) {
    return "str";
  }
  if (type->is(BaseType::TY_VOID)) {
    return "void";
  }
  if (type->is(BaseType::TY_ARRAY) && llvmTy->isArrayTy()) {
    return "(arr_" + getTypeMangledName(span, type->getElementType()) + ")";
  }
  if (type->is(BaseType::TY_PTR)) {
    return "(ptr_" + getTypeMangledName(span, type->getElementType()) + ")";
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    std::string paramStr;
    for (const auto& field : type->getFields()) {
      paramStr += getTypeMangledName(span, field->type) + "_";
    }
    return "(func_" + paramStr + ")";
  }
  if (type->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    std::string paramStr;
    for (const auto& field : type->getFields()) {
      paramStr += getTypeMangledName(span, field->type) + "_";
    }
    return "(struct_" + type->getLlvmType()->getStructName().str() + ")";
  }

  throw CodegenError(span, "Unknown type found during mangling");
}

auto Codegen::isMethod(const std::string& mangledName) -> bool {
  return mangledName.find("::") != std::string::npos;
}

auto Codegen::getMangledName(llvm::SMRange span, std::string funcName,
                             const std::vector<lesma::Type*>& paramTypes,
                             bool isMethod, std::string alias) -> std::string {
  alias = alias.empty() ? this->alias : alias;
  std::string name =
      (alias.empty() ? "" : "&" + alias + "=>") +
      (selfSymbol != nullptr && isMethod
           ? selfSymbol->getName() + "::" + std::move(funcName) + ":"
           : "." + std::move(funcName) + ":");
  bool first = true;

  for (auto* paramType : paramTypes) {
    if (!first) {
      name += ",";
    } else {
      first = false;
    }

    name += getTypeMangledName(span, paramType);
  }

  return name;
}

auto Codegen::isMangled(std::string name) -> bool {
  return name.find(':') != std::string::npos || name.at(0) == '.';
}

auto Codegen::getDemangledName(const std::string& name) -> std::string {
  if (!isMangled(name)) {
    return name;
  }

  auto demangledName = name;

  // Remove class mangling
  auto classMangling = demangledName.find("::");
  if (classMangling != std::string::npos) {
    demangledName = demangledName.substr(classMangling + 2);
  }

  // Remove standard '.' mangling to differentiate from native functions
  if (demangledName.at(0) == '.') {
    demangledName.erase(0, 1);
  }

  // Remove parameters mangling
  auto parameterMangling = demangledName.find(':');
  if (parameterMangling != std::string::npos) {
    demangledName = demangledName.substr(0, parameterMangling);
  }

  return demangledName;
}

auto Codegen::getExtendedType(lesma::Type* left,
                              lesma::Type* right) -> lesma::Type* {
  if (left->getBaseType() == right->getBaseType()) {
    return left;
  }

  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    // TODO: We should ideally only have one int type, but our FFI
    // implementation needs access to all types
    if (left->getLlvmType()->getIntegerBitWidth() >
        right->getLlvmType()->getIntegerBitWidth()) {
      return left;
    }
    return right;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    if (left->getLlvmType()->isFP128Ty() || right->getLlvmType()->isFP128Ty()) {
      return left->getLlvmType()->isFP128Ty() ? left : right;
    }
    if (left->getLlvmType()->isDoubleTy() ||
        right->getLlvmType()->isDoubleTy()) {
      return left->getLlvmType()->isDoubleTy() ? left : right;
    }
    if (left->getLlvmType()->isFloatTy() || right->getLlvmType()->isFloatTy()) {
      return left->getLlvmType()->isFloatTy() ? left : right;
    }
    if (left->getLlvmType()->isHalfTy() || right->getLlvmType()->isHalfTy()) {
      return left->getLlvmType()->isHalfTy() ? left : right;
    }
  }
  return nullptr;
}

auto Codegen::cast(llvm::SMRange span, lesma::Value* val,
                   lesma::Type* type) -> std::unique_ptr<lesma::Value> {
  if (type == nullptr) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  // If they're the same type
  if (val->getType()->isEqual(type)) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  if (type->is(BaseType::TY_INT)) {
    if (val->getType()->is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPToSI(val->getLlvmValue(), type->getLlvmType()));
    }
    if (val->getType()->is(BaseType::TY_INT)) {
      return std::make_unique<Value>("", type,
                                     builder->CreateIntCast(val->getLlvmValue(),
                                                            type->getLlvmType(),
                                                            type->isSigned()));
    }
  } else if (type->is(BaseType::TY_FLOAT)) {
    if (val->getType()->is(BaseType::TY_INT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateSIToFP(val->getLlvmValue(), type->getLlvmType()));
    }
    if (val->getType()->is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPCast(val->getLlvmValue(), type->getLlvmType()));
    }
  } else if (type->is(BaseType::TY_STRING)) {
    if (val->getType()->is(BaseType::TY_PTR) &&
        (val->getType()->getElementType()->is(BaseType::TY_INT) ||
         val->getType()->getElementType()->is(BaseType::TY_VOID))) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateBitCast(val->getLlvmValue(), type->getLlvmType()));
    }
  }

  throw CodegenError(span, "Unsupported Cast between {} and {}",
                     getTypeMangledName(span, val->getType()),
                     getTypeMangledName(span, type));
}

auto Codegen::genFuncCall(const FuncCall* node,
                          const std::vector<lesma::Value*>& extraParams = {})
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;

  for (auto* arg : extraParams) {
    paramTypes.push_back(arg->getType());
    paramsLLVM.push_back(arg->getLlvmValue());
  }

  for (auto* arg : node->getArguments()) {
    arg->accept(*this);
    paramTypes.push_back(result->getType());
    paramsLLVM.push_back(result->getLlvmValue());
  }

  Value* symbol = nullptr;
  // Check if it's a constructor like `Classname()`
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->lookupStruct(node->getName());
  llvm::Value* classPtr = nullptr;
  // Keep the Type alive for the duration of the lookup
  std::unique_ptr<Type> selfParamType;
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

auto Codegen::findIndexInFields(Type* structType,
                                const std::string& field) -> int {
  for (unsigned int i = 0; i < structType->getFields().size(); i++) {
    if (structType->getFields()[i]->name == field) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

auto Codegen::findTypeInFields(Type* structType,
                               const std::string& field) -> lesma::Type* {
  for (const auto& i : structType->getFields()) {
    if (i->name == field) {
      return i->type;
    }
  }

  return nullptr;
}