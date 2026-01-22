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
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>
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
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

#include <nameof.hpp>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

#ifdef LESMA_HAS_LLD
#include <lld/Common/Driver.h>
#endif
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include "liblesma/Frontend/Lexer.h"

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
  targetMachine = InitializeTargetMachine();
  theModule = InitializeModule();
  if (jit) {
    theJit = InitializeJit();
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
  topLevelFunc = InitializeTopLevel();

  // If it's not base.les stdlib, then import it
  if (std::filesystem::absolute(filename) != GetStdDir() + "base.les") {
    CompileModule(llvm::SMRange(), GetStdDir() + "base.les", true, "base", true,
                  true, {});
  }
}

auto Codegen::InitializeModule() -> std::unique_ptr<Module> {
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
auto Codegen::InitializeTargetMachine()
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

auto Codegen::InitializeJit() -> std::unique_ptr<LLJIT> {
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

auto Codegen::InitializeTopLevel() -> llvm::Function* {
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

auto Codegen::DefineFunction(lesma::Value* value, const FuncDecl* node,
                             Value* clsSymbol) -> void {
  scope = scope->CreateChildBlock(node->GetName());
  currentFunction = value;
  deferStack.emplace();

  auto* f = llvm::cast<Function>(value->GetLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  int fieldIndex = 0;
  for (const auto& field : value->GetType()->GetFields()) {
    auto* param = f->getArg(fieldIndex);

    if (clsSymbol != nullptr && param->getArgNo() == 0) {
      param->setName("self");
    } else {
      param->setName(node->GetParameters()[param->getArgNo() -
                                           (clsSymbol != nullptr ? 1 : 0)]
                         ->name);
    }

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr,
                                             param->getName() + "_ptr");
    builder->CreateStore(param, ptr);

    auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
    scope->InsertSymbol(std::move(symbol));

    fieldIndex++;
  }

  node->GetBody()->Accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  if (!isReturn) {
    for (auto* inst : instrs) {
      inst->Accept(*this);
    }
  }

  // Check for well-formness of all BBs. In particular, look for
  // any unterminated BB and try to add a Return to it.
  for (BasicBlock& bb : *f) {
    Instruction* terminator = bb.getTerminator();
    if (terminator != nullptr) {
      continue; // Well-formed
    }

    if (value->GetType()->GetReturnType()->Is(BaseType::TY_VOID)) {
      // Make implicit return of void Function explicit.
      builder->SetInsertPoint(&bb);
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->GetSpan(),
                         "Function {} does not always return a result",
                         node->GetName());
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
  scope = scope->GetParent();

  currentFunction = nullptr;

  // Reset Insert Point to Top Level
  builder->SetInsertPoint(&topLevelFunc->back());
}

auto Codegen::CompileModule(
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

  // If module is already imported, don't compile again
  // TODO: Re-enable this again, currently it destroys nested imports
  //    if (std::find(ImportedModules.begin(), ImportedModules.end(),
  //    absolute_path) != ImportedModules.end())
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
    lexer->ScanAll();

    // Parser
    auto parser = std::make_unique<Parser>(lexer->GetTokens());
    parser->Parse();

    // TODO: Delete it, memory leak, smart pointer made us lose the references
    // to other modules Codegen
    auto codegen = std::make_unique<Codegen>(
        std::move(parser), sourceManager, absolutePath, importedModules, isJit,
        false, !importToScope ? moduleAlias : "", theContext);
    codegen->Run();

    // Optimize
    codegen->Optimize(OptimizationLevel::O3);
    codegen->theModule->setModuleIdentifier(filepath);

    importedModules = std::move(codegen->importedModules);

    if (!importToScope) {
      auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
      auto* importTypPtr = importTyp.get();
      auto importSym = std::make_unique<Value>(moduleAlias, importTypPtr);
      scope->InsertSymbol(std::move(importSym));
      scope->InsertType(moduleAlias, std::move(importTyp));
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
      codegen->WriteToObjectFile(objFile);
      objectFiles.push_back(fmt::format("{}.o", objFile));
    }

    // Import Symbols
    // TODO: This section has ownership issues - Types from imported scope are
    // referenced but the imported scope will be destroyed. Need to clone Types
    // properly.
    for (auto* sym : codegen->scope->GetSymbols()) {
      auto impAlias = findInImports(sym->GetName());
      if (sym->GetType()->IsOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) &&
          sym->IsExported() && (importAll || !impAlias.empty())) {
        llvm::StructType* structType =
            StructType::getTypeByName(theModule->getContext(), sym->GetName());

        auto structSymbol = std::make_unique<Value>(
            impAlias.empty() ? sym->GetName() : impAlias, sym->GetType());
        structSymbol->GetType()->SetLlvmType(structType);
        // Insert non-owning reference to imported Type
        scope->InsertTypeRef(sym->GetName(), sym->GetType());
        scope->InsertSymbol(std::move(structSymbol));
      } else if (sym->GetType()->Is(BaseType::TY_FUNCTION) &&
                 sym->IsExported()) {
        auto* f = llvm::dyn_cast<Function>(sym->GetLlvmValue());
        auto* fTy = llvm::cast<FunctionType>(sym->GetType()->GetLlvmType());

        if (isJit) {
          // Insert the function declaration, since we linked the modules
          // earlier
          f = llvm::cast<Function>(
              theModule->getOrInsertFunction(sym->GetMangledName(), fTy)
                  .getCallee());
        }

        auto name = sym->GetName();
        std::vector<lesma::Type*> paramTypes;
        for (auto* field : sym->GetType()->GetFields()) {
          paramTypes.push_back(field->type);
        }

        Value* funcSymbol = codegen->scope->LookupFunction(name, paramTypes);

        // Only import if it's exported
        impAlias = findInImports(name);
        // TODO: methods should only be imported if they class is in the imports
        // specified
        if (funcSymbol != nullptr && funcSymbol->IsExported() &&
            (importAll || !impAlias.empty() ||
             IsMethod(sym->GetMangledName()))) {
          auto symbol = std::make_unique<Value>(
              impAlias.empty()
                  ? name
                  : std::regex_replace(name, std::regex(name), impAlias),
              funcSymbol->GetType());

          if (isJit) {
            symbol->GetType()->SetLlvmType(fTy);
            symbol->SetLlvmValue(f);
            symbol->SetExported(false);
            symbol->SetMangledName(sym->GetMangledName());
          } else {
            // If it's compiled, we need to make a new Function declaration in
            // the importing file
            auto* newFunc = Function::Create(fTy, Function::ExternalLinkage,
                                             sym->GetMangledName(), *theModule);
            symbol->GetType()->SetLlvmType(newFunc->getFunctionType());
            symbol->SetLlvmValue(newFunc);
            symbol->SetExported(false);
            symbol->SetMangledName(sym->GetMangledName());
          }

          scope->InsertSymbol(std::move(symbol));
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
    if (!err.GetSpan().isValid()) {
      Print(LogType::ERROR, err.what());
    } else {
      ShowInline(sourceManager.get(), fileId, err.GetSpan(), absolutePath, true,
                 err.what());
    }

    throw CodegenError(span, "Unable to import {} due to errors", filepath);
  }
}

auto Codegen::Optimize(OptimizationLevel opt) -> void {
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

auto Codegen::WriteToObjectFile(const std::string& output) -> void {
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

#ifdef LESMA_HAS_LLD
[[maybe_unused]] void
Codegen::LinkObjectFileWithLLD(const std::string& obj_filename) {
  std::string output = GetBasename(obj_filename);

  llvm::SmallVector<const char*, 32> args;
  args.push_back("lld");
  args.push_back("-o");
  args.push_back(output.c_str());
  args.push_back(obj_filename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }
  // Add the standard library path for Apple
#ifdef __APPLE__
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

  // Run the LLD linker
  lld::Result result;
#ifdef __APPLE__
  result = lld::macho::link(args, llvm::outs(), llvm::errs(), false, false);
#elif defined(_WIN32)
  result = lld::coff::link(args, llvm::outs(), llvm::errs(), false, false);
#else
  result = lld::elf::link(args, llvm::outs(), llvm::errs(), false, false);
#endif
  bool success = result.retCode == 0;
  if (!success)
    throw CodegenError({}, "Linking Failed");

  // Remove object files
  llvm::sys::fs::remove(obj_filename);
  for (const auto& obj : objectFiles)
    llvm::sys::fs::remove(obj);
}
#endif // LESMA_HAS_LLD

[[maybe_unused]] auto
Codegen::LinkObjectFileWithClang(const std::string& objFilename) -> void {
  auto clangPath = llvm::sys::findProgramByName("clang");
  if (clangPath.getError()) {
    throw CodegenError({}, "Unable to find clang path");
  }

  std::string output = GetBasename(objFilename);

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
  args.push_back("-lSystem");
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

auto Codegen::LinkObjectFile(const std::string& objFilename) -> void {
  LinkObjectFileWithClang(objFilename);
}

auto Codegen::PrepareJit() -> void {
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

auto Codegen::ExecuteJit() -> int {
  if (mainFuncAddress == nullptr) {
    throw CodegenError(
        {}, "Main function address not found, did you prepare JIT?\n");
  }

  return mainFuncAddress();
}

auto Codegen::Run() -> void {
  deferStack.emplace();
  parser->GetAst()->Accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  // Visit all statements
  for (auto* inst : instrs) {
    inst->Accept(*this);
  }

  // Define the function bodies
  for (auto& prot : prototypes) {
    DefineFunction(std::get<0>(prot), std::get<1>(prot), std::get<2>(prot));
  }

  // Return 0 for top-level function
  builder->CreateRet(ConstantInt::getSigned(builder->getInt64Ty(), 0));
}

auto Codegen::Dump() -> void { theModule->print(outs(), nullptr); }

auto Codegen::Visit(const Statement* node) -> void {
  Print("Visited a blank statement\n{}",
        node->ToString(sourceManager.get(), "", true));
}

auto Codegen::Visit(const Expression* node) -> void {
  Print("Visited a blank expression\n{}",
        node->ToString(sourceManager.get(), "", true));
}

auto Codegen::Visit(const TypeExpr* node) -> void {
  // For primitive types, cache them so they survive beyond result's lifetime
  // This is needed because setReturnType and similar store raw Type* pointers
  if (node->GetType() == TokenType::INT_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::INT8_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt8Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::INT16_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt16Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::INT32_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt32Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::FLOAT_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::FLOAT32_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getFloatTy()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::BOOL_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::STRING_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::VOID_TYPE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::PTR_TYPE) {
    node->GetElementType()->Accept(*this);
    auto* type = CacheType(std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), result->GetType()));
    result = std::make_unique<Value>(type);
  } else if (node->GetType() == TokenType::FUNC_TYPE) {
    node->GetReturnType()->Accept(*this);
    auto retType = std::move(result);
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<lesma::Type*> paramTypes;
    std::vector<llvm::Type*> paramLLVMTypes;
    for (auto* paramType : node->GetParams()) {
      paramType->Accept(*this);
      paramLLVMTypes.push_back(result->GetType()->GetLlvmType());
      paramTypes.push_back(result->GetType());
      fields.push_back(
          std::make_unique<Field>(result->GetName(), result->GetType()));
    }

    // With opaque pointers, function pointer types are just `ptr`
    // The actual function signature is tracked in Lesma's Type system via
    // fields
    auto funcType = std::make_unique<Type>(
        BaseType::TY_FUNCTION, builder->getPtrTy(), std::move(fields));
    funcType->SetReturnType(retType->GetType());
    result = std::make_unique<Value>(CacheType(std::move(funcType)));
  } else if (node->GetType() == TokenType::CUSTOM_TYPE) {
    auto* typ = scope->LookupType(node->GetName());
    auto* sym = scope->LookupStruct(node->GetName());
    if (typ == nullptr || sym->GetType()->GetLlvmType() == nullptr) {
      throw CodegenError(node->GetSpan(), "Type not found: {}",
                         node->GetName());
    }

    // Borrow from symbol table - make a shallow copy
    result = sym == nullptr ? std::make_unique<Value>(typ)
                            : std::make_unique<Value>(*sym);
  } else {
    throw CodegenError(node->GetSpan(), "Unimplemented type {}",
                       NAMEOF_ENUM(node->GetType()));
  }
}

auto Codegen::Visit(const Compound* node) -> void {
  for (auto* elem : node->GetChildren()) {
    elem->Accept(*this);
  }
}

auto Codegen::Visit(const VarDecl* node) -> void {
  lesma::Type* type = nullptr;
  std::unique_ptr<lesma::Value> val;

  // TODO: We shouldn't need to use this
  bool isClass = false;

  if (node->GetValue() != nullptr) {
    node->GetValue()->Accept(*this);
    val = std::move(result);
    type = val->GetType();
  }

  if (node->GetType() != nullptr) {
    node->GetType()->Accept(*this);
    type = result->GetType();
  }

  auto* ptr = builder->CreateAlloca(type->GetLlvmType(), nullptr,
                                    node->GetIdentifier()->GetValue());

  if (type->Is(BaseType::TY_CLASS)) {
    // Cache the pointer type to prevent dangling pointers
    type = CacheType(
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
    isClass = true;
  }
  auto symbol = std::make_unique<Value>(node->GetIdentifier()->GetValue(), type,
                                        node->GetType() != nullptr
                                            ? SymbolState::INITIALIZED
                                            : SymbolState::DECLARED);
  symbol->SetLlvmValue(ptr);
  symbol->SetMutable(node->GetMutability());
  scope->InsertSymbol(std::move(symbol));

  // Convert declared value to declared type implicitly
  if (node->GetValue() != nullptr) {
    auto castVal = Cast(node->GetSpan(), val.get(),
                        isClass ? type->GetElementType() : type);
    builder->CreateStore(castVal->GetLlvmValue(), ptr);
  }
}

auto Codegen::Visit(const If* node) -> void {
  auto* parentFct = builder->GetInsertBlock()->getParent();
  auto* bStart = llvm::BasicBlock::Create(theModule->getContext(), "if.start");
  auto* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "if.end");

  builder->CreateBr(bStart);
  bStart->insertInto(parentFct);
  builder->SetInsertPoint(bStart);

  for (unsigned long i = 0; i < node->GetConds().size(); i++) {
    auto* bIfTrue =
        llvm::BasicBlock::Create(theModule->getContext(), "if.true");
    bIfTrue->insertInto(parentFct);
    auto* bIfFalse = bEnd;
    if (i + 1 < node->GetConds().size()) {
      bIfFalse = llvm::BasicBlock::Create(theModule->getContext(), "if.false");
      bIfFalse->insertInto(parentFct);
    }

    node->GetConds().at(i)->Accept(*this);
    builder->CreateCondBr(result->GetLlvmValue(), bIfTrue, bIfFalse);
    builder->SetInsertPoint(bIfTrue);

    scope = scope->CreateChildBlock("if");
    node->GetBlocks().at(i)->Accept(*this);

    // TODO: Really slow and hacky way to check if there was a return in block
    bool returned = false;
    for (auto* stat : node->GetBlocks().at(i)->GetChildren()) {
      if (dynamic_cast<Return*>(stat) != nullptr) {
        returned = true;
      }
    }

    if (!isBreak && !returned) {
      builder->CreateBr(bEnd);
    }

    scope = scope->GetParent();
    builder->SetInsertPoint(bIfFalse);
  }

  bEnd->insertInto(parentFct);

  if (!isBreak) {
    builder->SetInsertPoint(bEnd);
  } else {
    isBreak = false;
  }
}

auto Codegen::Visit(const While* node) -> void {
  scope = scope->CreateChildBlock("while");

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
  node->GetCond()->Accept(*this);
  builder->CreateCondBr(result->GetLlvmValue(), bLoop, bEnd);

  // Fill while body block
  bLoop->insertInto(parentFct);
  builder->SetInsertPoint(bLoop);
  node->GetBlock()->Accept(*this);

  if (!isBreak) {
    builder->CreateBr(bCond);
  } else {
    isBreak = false;
  }

  // Fill loop end block
  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);

  scope = scope->GetParent();
  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::Visit(const FuncDecl* node) -> void {
  if (selfSymbol != nullptr && node->GetName() == "new" &&
      node->GetReturnType()->GetType() != TokenType::VOID_TYPE) {
    throw CodegenError(node->GetSpan(),
                       "Cannot create class method new with return type {}",
                       node->GetReturnType()->GetName());
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;
  bool shouldExport = node->IsExported();

  if (selfSymbol != nullptr) {
    paramTypes.push_back(selfSymbol->GetType());
    paramLLVMTypes.push_back(builder->getPtrTy());
    fields.push_back(std::make_unique<Field>("self", selfSymbol->GetType()));
    shouldExport = selfSymbol->IsExported();
  }

  for (auto* param : node->GetParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->Accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->Accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->GetType()->Is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = CacheType(std::make_unique<Type>(
          BaseType::TY_PTR, builder->getPtrTy(), typeResult->GetType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult &&
        !typeResult->GetType()->IsEqual(defaultValResult->GetType())) {
      throw CodegenError(
          node->GetSpan(),
          "Declared parameter type and default value do not match for {}",
          param->name);
    }

    paramTypes.push_back(typeResult->GetType());
    paramLLVMTypes.push_back(typeResult->GetType()->GetLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, typeResult->GetType(),
                                             std::move(defaultValResult)));
  }

  auto mangledName = GetMangledName(node->GetSpan(), node->GetName(),
                                    paramTypes, selfSymbol != nullptr);
  auto linkage =
      shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  node->GetReturnType()->Accept(*this);

  llvm::FunctionType* funcType = FunctionType::get(
      result->GetType()->GetLlvmType(), paramLLVMTypes, node->GetVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);

  auto funcSymbol = std::make_unique<Value>(
      node->GetName(),
      std::make_unique<Type>(BaseType::TY_FUNCTION, funcType,
                             std::move(fields)),
      f);
  funcSymbol->GetType()->SetReturnType(result->GetType());
  funcSymbol->SetExported(node->IsExported());
  funcSymbol->SetMangledName(mangledName);
  auto* funcSymbolPtr = funcSymbol.get();
  scope->InsertSymbol(std::move(funcSymbol));

  prototypes.emplace_back(funcSymbolPtr, node, selfSymbol);
  // Even though it's a statement, we pass the func symbol to result so parent
  // classes can modify them
  result = std::make_unique<Value>(*funcSymbolPtr);
}

auto Codegen::Visit(const ExternFuncDecl* node) -> void {
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;

  for (auto* param : node->GetParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->Accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->Accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->GetType()->Is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = CacheType(std::make_unique<Type>(
          BaseType::TY_PTR, builder->getPtrTy(), typeResult->GetType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult &&
        !typeResult->GetType()->IsEqual(defaultValResult->GetType())) {
      throw CodegenError(
          node->GetSpan(),
          "Declared parameter type and default value do not match for {}",
          param->name);
    }

    paramTypes.push_back(typeResult->GetType());
    paramLLVMTypes.push_back(typeResult->GetType()->GetLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, typeResult->GetType(),
                                             std::move(defaultValResult)));
  }

  node->GetReturnType()->Accept(*this);
  lesma::Type* retType = nullptr;
  if (!result) {
    retType = CacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  } else {
    retType = result->GetType();
  }

  Function* f = nullptr;
  if (theModule->getFunction(node->GetName()) != nullptr &&
      scope->LookupFunction(node->GetName(), paramTypes) != nullptr) {
    return;
  }

  if (theModule->getFunction(node->GetName()) != nullptr) {
    f = theModule->getFunction(node->GetName());
  } else {
    FunctionType* ft = FunctionType::get(retType->GetLlvmType(), paramLLVMTypes,
                                         node->GetVarArgs());
    f = llvm::cast<Function>(
        theModule->getOrInsertFunction(node->GetName(), ft).getCallee());
    if (node->IsExported()) {
      f->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }

  auto funcSymbol = std::make_unique<Value>(
      node->GetName(),
      std::make_unique<Type>(BaseType::TY_FUNCTION, f->getFunctionType(),
                             std::move(fields)),
      f);
  funcSymbol->GetType()->SetReturnType(retType);
  funcSymbol->SetExported(node->IsExported());
  funcSymbol->SetMangledName(node->GetName());
  scope->InsertSymbol(std::move(funcSymbol));
}

auto Codegen::Visit(const Assignment* node) -> void {
  lesma::Value* lhs = nullptr;
  std::unique_ptr<lesma::Value> lhsOwner; // Holds owned lhs if from result
  isAssignment = true;
  bool isPtr = false;
  if (dynamic_cast<Literal*>(node->GetLeftHandSide()) != nullptr) {
    auto* lit = dynamic_cast<Literal*>(node->GetLeftHandSide());
    auto* symbol = scope->Lookup(lit->GetValue());
    if (symbol == nullptr) {
      throw CodegenError(node->GetSpan(), "Variable not found: {}",
                         lit->GetValue());
    }
    if (!symbol->GetMutability()) {
      throw CodegenError(node->GetSpan(),
                         "Assigning immutable variable a new value");
    }

    lhs = symbol;
  } else if (dynamic_cast<DotOp*>(node->GetLeftHandSide()) != nullptr) {
    node->GetLeftHandSide()->Accept(*this);
    lhsOwner = std::move(result);
    lhs = lhsOwner.get();
    // TODO: Fix me, for some reason self.x is a ptr but x is not
    isPtr = true;
  } else {
    throw CodegenError(
        node->GetSpan(), "Unable to assign {} to {}",
        node->GetRightHandSide()->ToString(sourceManager.get(), "", true),
        node->GetLeftHandSide()->ToString(sourceManager.get(), "", true));
  }
  isAssignment = false;

  node->GetRightHandSide()->Accept(*this);
  auto value = Cast(node->GetSpan(), result.get(),
                    isPtr ? lhs->GetType()->GetElementType() : lhs->GetType());
  llvm::Value* varVal = nullptr;

  switch (node->GetOperator()) {
  case TokenType::EQUAL:
    builder->CreateStore(value->GetLlvmValue(), lhs->GetLlvmValue());
    break;
  case TokenType::PLUS_EQUAL:
    varVal =
        builder->CreateLoad(lhs->GetType()->GetLlvmType(), lhs->GetLlvmValue());
    if (lhs->GetType()->Is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFAdd(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else if (lhs->GetType()->Is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateAdd(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else {
      throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->GetOperator()));
    }
    break;
  case TokenType::MINUS_EQUAL:
    varVal =
        builder->CreateLoad(lhs->GetType()->GetLlvmType(), lhs->GetLlvmValue());
    if (lhs->GetType()->Is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFSub(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else if (lhs->GetType()->Is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSub(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else {
      throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->GetOperator()));
    }
    break;
  case TokenType::SLASH_EQUAL:
    varVal =
        builder->CreateLoad(lhs->GetType()->GetLlvmType(), lhs->GetLlvmValue());
    if (lhs->GetType()->Is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFDiv(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else if (lhs->GetType()->Is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSDiv(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else {
      throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->GetOperator()));
    }
    break;
  case TokenType::STAR_EQUAL:
    varVal =
        builder->CreateLoad(lhs->GetType()->GetLlvmType(), lhs->GetLlvmValue());
    if (lhs->GetType()->Is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFMul(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else if (lhs->GetType()->Is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateMul(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else {
      throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->GetOperator()));
    }
    break;
  case TokenType::MOD_EQUAL:
    varVal =
        builder->CreateLoad(lhs->GetType()->GetLlvmType(), lhs->GetLlvmValue());
    if (lhs->GetType()->Is(BaseType::TY_FLOAT)) {
      auto* newVal = builder->CreateFRem(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else if (lhs->GetType()->Is(BaseType::TY_INT)) {
      auto* newVal = builder->CreateSRem(value->GetLlvmValue(), varVal);
      builder->CreateStore(newVal, lhs->GetLlvmValue());
    } else {
      throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                         NAMEOF_ENUM(node->GetOperator()));
    }
    break;
  case TokenType::POWER_EQUAL:
    throw CodegenError(node->GetSpan(), "Power operator not implemented yet.");
  default:
    throw CodegenError(node->GetSpan(), "Invalid operator: {}",
                       NAMEOF_ENUM(node->GetOperator()));
  }
}

auto Codegen::Visit(const Break* node) -> void {
  if (breakBlocks.empty()) {
    throw CodegenError(node->GetSpan(), "Cannot break without being in a loop");
  }

  auto* block = breakBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::Visit(const Continue* node) -> void {
  if (continueBlocks.empty()) {
    throw CodegenError(node->GetSpan(),
                       "Cannot continue without being in a loop");
  }

  auto* block = continueBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::Visit(const Return* node) -> void {
  // Check if it's top-level
  if (builder->GetInsertBlock()->getParent() == topLevelFunc) {
    throw CodegenError(node->GetSpan(),
                       "Return statements are not allowed at top-level");
  }

  // Execute all deferred statements
  for (auto* inst : deferStack.top()) {
    inst->Accept(*this);
  }

  isReturn = true;

  if (node->GetValue() == nullptr) {
    if (currentFunction->GetType()->GetReturnType()->Is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      throw CodegenError(
          node->GetSpan(),
          "Return type does not match the function return type, expected {}, "
          "actual void",
          currentFunction->GetType()->GetReturnType()->ToString());
    }
  } else {
    node->GetValue()->Accept(*this);
    if (builder->getCurrentFunctionReturnType() ==
        result->GetType()->GetLlvmType()) {
      builder->CreateRet(result->GetLlvmValue());
    } else {
      throw CodegenError(
          node->GetSpan(),
          "Return type does not match the function return type, expected {}, "
          "actual {}",
          currentFunction->GetType()->GetReturnType()->ToString(),
          result->GetType()->ToString());
    }
  }
}

auto Codegen::Visit(const Defer* node) -> void {
  deferStack.top().push_back(node->GetStatement());
}

auto Codegen::Visit(const ExpressionStatement* node) -> void {
  node->GetExpression()->Accept(*this);
}

auto Codegen::Visit(const Import* node) -> void {
  CompileModule(node->GetSpan(), node->GetFilePath(), node->IsStd(),
                node->GetAlias(), node->GetImportAll(), node->GetImportScope(),
                node->GetImportedNames());
}

auto Codegen::Visit(const Class* node) -> void {
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<llvm::Type*> elementLLVMTypes;

  for (auto* field : node->GetFields()) {
    if (field->GetType() != nullptr) {
      field->GetType()->Accept(*this);
    } else {
      field->GetValue()->Accept(*this);
    }

    elementLLVMTypes.push_back(result->GetType()->GetLlvmType());
    std::unique_ptr<Value> defaultVal;
    if (field->GetValue() != nullptr) {
      defaultVal = std::move(result);
      // Re-evaluate to get the type for the Field (result was moved)
      if (field->GetType() != nullptr) {
        field->GetType()->Accept(*this);
      } else {
        // Make a copy of the default value to get its type
        result = std::make_unique<Value>(*defaultVal);
      }
    }
    fields.push_back(std::make_unique<Field>(field->GetIdentifier()->GetValue(),
                                             result->GetType(),
                                             std::move(defaultVal)));
  }

  llvm::StructType* structType = llvm::StructType::create(
      theModule->getContext(), elementLLVMTypes, node->GetIdentifier());

  auto type =
      std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
  auto* typePtr = type.get();
  auto structSymbol = std::make_unique<Value>(node->GetIdentifier(), typePtr);
  structSymbol->SetExported(node->IsExported());
  auto* structSymbolPtr = structSymbol.get();

  scope->InsertType(node->GetIdentifier(), std::move(type));
  scope->InsertSymbol(std::move(structSymbol));

  // Cache the self type to prevent dangling pointers in function Fields
  auto* selfType = CacheType(
      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typePtr));
  auto classSelfSymbol =
      std::make_unique<Value>(node->GetIdentifier(), selfType);
  classSelfSymbol->SetExported(node->IsExported());
  selfSymbol = classSelfSymbol.get();
  auto hasConstructor = false;
  for (auto* func : node->GetMethods()) {
    func->Accept(*this);
    if (func->GetName() == "new") {
      hasConstructor = true;
      // result contains a copy of the func symbol; actual symbol is in
      // SymbolTable Look up the actual constructor from scope - needs self type
      // as first param
      std::vector<lesma::Type*> constructorParams = {selfSymbol->GetType()};
      auto* constructor = scope->LookupFunction("new", constructorParams);
      structSymbolPtr->SetConstructor(constructor);
    }
  }

  if (!hasConstructor) {
    throw CodegenError(node->GetSpan(), "Class {} has no constructors",
                       node->GetIdentifier());
  }

  selfSymbol = nullptr;
  // classSelfSymbol unique_ptr goes out of scope and properly deletes the Value
}

auto Codegen::Visit(const Enum* node) -> void {
  std::vector<llvm::Type*> elementTypes = {builder->getInt8Ty()};
  llvm::StructType* structType = llvm::StructType::create(
      theModule->getContext(), elementTypes, node->GetIdentifier());
  std::vector<std::unique_ptr<Field>> fields;

  for (const auto& field : node->GetValues()) {
    // Cache enum field types to prevent dangling pointers
    auto* fieldType = CacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    fields.push_back(std::make_unique<Field>(field, fieldType));
  }

  auto type =
      std::make_unique<Type>(BaseType::TY_ENUM, structType, std::move(fields));
  auto* typePtr = type.get();
  auto structSymbol = std::make_unique<Value>(node->GetIdentifier(), typePtr);
  structSymbol->SetExported(node->IsExported());

  scope->InsertType(node->GetIdentifier(), std::move(type));
  scope->InsertSymbol(std::move(structSymbol));
}

auto Codegen::Visit(const FuncCall* node) -> void {
  result = GenFuncCall(node, {});
}

auto Codegen::Visit(const BinaryOp* node) -> void {
  node->GetLeft()->Accept(*this);
  auto left = std::move(result);
  node->GetRight()->Accept(*this);
  auto right = std::move(result);
  lesma::Type* finalType = GetExtendedType(left->GetType(), right->GetType());

  switch (node->GetOperator()) {
  case TokenType::MINUS:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFSub(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSub(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::PLUS:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFAdd(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateAdd(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::STAR:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFMul(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateMul(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::SLASH:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFDiv(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSDiv(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::MOD:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateFRem(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType,
          builder->CreateSRem(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::POWER:
    if (finalType == nullptr) {
      break;
    }

    if (!right->GetType()->IsOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw CodegenError(
          node->GetSpan(), "Cannot use non-numbers for power coefficient: {}",
          node->GetRight()->ToString(sourceManager.get(), "", true));
    }

    throw CodegenError(node->GetSpan(), "Power operator not implemented yet.");
  case TokenType::EQUAL_EQUAL:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    // Enum comparison
    if (finalType->Is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->GetType()->GetLlvmType()->getStructName().str();
      auto rightName = right->GetType()->GetLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(
            node->GetSpan(),
            "Illegal comparison of two different enums: {} and {}", leftName,
            rightName);
      }

      llvm::Value* leftVal =
          builder->CreateExtractValue(left->GetLlvmValue(), {0});
      llvm::Value* rightVal =
          builder->CreateExtractValue(right->GetLlvmValue(), {0});
      result =
          std::make_unique<Value>("",
                                  CacheType(std::make_unique<Type>(
                                      BaseType::TY_BOOL, builder->getInt1Ty())),
                                  builder->CreateICmpEQ(leftVal, rightVal));
      return;
    }

    if (finalType->Is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOEQ(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::BANG_EQUAL:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    // Enum comparison
    if (finalType->Is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->GetType()->GetLlvmType()->getStructName().str();
      auto rightName = right->GetType()->GetLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(
            node->GetSpan(),
            "Illegal comparison of two different enums: {} and {}", leftName,
            rightName);
      }

      llvm::Value* leftVal =
          builder->CreateExtractValue(left->GetLlvmValue(), {0});
      llvm::Value* rightVal =
          builder->CreateExtractValue(right->GetLlvmValue(), {0});
      result =
          std::make_unique<Value>("",
                                  CacheType(std::make_unique<Type>(
                                      BaseType::TY_BOOL, builder->getInt1Ty())),
                                  builder->CreateICmpNE(leftVal, rightVal));
      return;
    }

    if (finalType->Is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpONE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }
    break;
  case TokenType::GREATER:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGT(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGT(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::GREATER_EQUAL:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::LESS:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLT(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLT(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::LESS_EQUAL:
    left = Cast(node->GetSpan(), left.get(), finalType);
    right = Cast(node->GetSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->Is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    if (finalType->Is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "",
          CacheType(
              std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLE(left->GetLlvmValue(), right->GetLlvmValue()));
      return;
    }

    break;
  case TokenType::AND:
    if (!left->GetType()->Is(BaseType::TY_BOOL) &&
        !right->GetType()->Is(BaseType::TY_BOOL)) {
      throw CodegenError(
          node->GetSpan(), "Cannot use non-booleans for and: {} - {}",
          node->GetLeft()->ToString(sourceManager.get(), "", true),
          node->GetRight()->ToString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "",
        CacheType(
            std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalAnd(left->GetLlvmValue(), right->GetLlvmValue()));
    return;
  case TokenType::OR:
    if (!left->GetType()->Is(BaseType::TY_BOOL) &&
        !right->GetType()->Is(BaseType::TY_BOOL)) {
      throw CodegenError(
          node->GetSpan(), "Cannot use non-booleans for or: {} - {}",
          node->GetLeft()->ToString(sourceManager.get(), "", true),
          node->GetRight()->ToString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "",
        CacheType(
            std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalOr(left->GetLlvmValue(), right->GetLlvmValue()));
    return;
  default:
    throw CodegenError(node->GetSpan(), "Unimplemented binary operator: {}",
                       NAMEOF_ENUM(node->GetOperator()));
  }

  throw CodegenError(node->GetSpan(),
                     "Unimplemented binary operator {} for {} and {}",
                     NAMEOF_ENUM(node->GetOperator()),
                     node->GetLeft()->ToString(sourceManager.get(), "", true),
                     node->GetRight()->ToString(sourceManager.get(), "", true));
}

auto Codegen::Visit(const DotOp* node) -> void {
  if (auto* left = dynamic_cast<Literal*>(node->GetLeft())) {
    if (left->GetType() != TokenType::IDENTIFIER) {
      throw CodegenError(
          node->GetLeft()->GetSpan(),
          "Expected identifier left-hand of dot operator, found {}",
          node->GetRight()->ToString(sourceManager.get(), "", true));
    }

    auto* typeSym = scope->LookupType(left->GetValue());
    if (typeSym != nullptr) {
      // Assuming it's an enum or statically accessed class
      if (!typeSym->IsOneOf(
              {BaseType::TY_ENUM, BaseType::TY_CLASS, BaseType::TY_IMPORT})) {
        throw CodegenError(node->GetLeft()->GetSpan(),
                           "Cannot apply dot accessor on {}", left->GetValue());
      }

      auto* right = dynamic_cast<Literal*>(node->GetRight());
      if (typeSym->Is(BaseType::TY_ENUM)) {
        // Check if right-hand expression is an identifier expression
        if (dynamic_cast<Literal*>(node->GetRight()) == nullptr) {
          throw CodegenError(
              node->GetRight()->GetSpan(),
              "Expected identifier right-hand of dot operator, found {}",
              node->GetRight()->ToString(sourceManager.get(), "", true));
        }

        if (right->GetType() != TokenType::IDENTIFIER) {
          throw CodegenError(
              node->GetRight()->GetSpan(),
              "Expected identifier right-hand of dot operator, found {}",
              node->GetRight()->ToString(sourceManager.get(), "", true));
        }

        // Setting value to the enum
        auto val = FindIndexInFields(typeSym, right->GetValue());
        // Field not found in enum
        if (val == -1) {
          throw CodegenError(node->GetLeft()->GetSpan(),
                             "Identifier {} not in {}", right->GetValue(),
                             left->GetValue());
        }

        auto* structVal = scope->LookupStruct(left->GetValue());
        auto* enumPtr =
            builder->CreateAlloca(structVal->GetType()->GetLlvmType());
        auto* field = builder->CreateStructGEP(
            structVal->GetType()->GetLlvmType(), enumPtr, 0);
        builder->CreateStore(builder->getInt8(val), field);
        // TODO: Returning the enum directly or a ptr to it? We used to return a
        // pointer
        auto* enumVal =
            builder->CreateLoad(structVal->GetType()->GetLlvmType(), enumPtr);

        result = std::make_unique<Value>("", structVal->GetType(), enumVal);
        return;
      }

      if (typeSym->Is(BaseType::TY_IMPORT)) {
        std::string field;
        FuncCall const* method = nullptr;

        if ((dynamic_cast<Literal*>(node->GetRight()) == nullptr) &&
            (dynamic_cast<FuncCall*>(node->GetRight()) == nullptr)) {
          throw CodegenError(
              node->GetRight()->GetSpan(),
              "Expected identifier or method call right-hand of dot operator, "
              "found {}",
              node->GetRight()->ToString(sourceManager.get(), "", true));
        }

        if ((dynamic_cast<Literal*>(node->GetRight()) != nullptr) &&
            dynamic_cast<Literal*>(node->GetRight())->GetType() ==
                TokenType::IDENTIFIER) {
          field = dynamic_cast<Literal*>(node->GetRight())->GetValue();
        } else {
          method = dynamic_cast<FuncCall*>(node->GetRight());
        }

        if (method != nullptr) {
          auto tmpAlias = alias;
          alias = left->GetValue();
          result = GenFuncCall(method, {});
          alias = tmpAlias;
          return;
        }
      }
    } else {
      // Assuming it's a class instance
      left->Accept(*this);
      // We refer to the class type, if it's a pointer, we get the result
      lesma::Type* lesmaType = result->GetType();
      if (result->GetType()->Is(BaseType::TY_PTR) &&
          result->GetType()->GetElementType()->Is(BaseType::TY_CLASS)) {
        lesmaType = result->GetType()->GetElementType();
      }

      if (!lesmaType->Is(BaseType::TY_CLASS)) {
        throw CodegenError(node->GetLeft()->GetSpan(),
                           "Cannot apply dot accessor on {}", left->GetValue());
      }

      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->GetRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->GetRight()) == nullptr)) {
        throw CodegenError(
            node->GetRight()->GetSpan(),
            "Expected identifier or method call right-hand of dot operator, "
            "found {}",
            node->GetRight()->ToString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->GetRight()) != nullptr) &&
          TokenType::IDENTIFIER ==
              dynamic_cast<Literal*>(node->GetRight())->GetType()) {
        field = dynamic_cast<Literal*>(node->GetRight())->GetValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->GetRight());
      }

      // TODO: Somehow, when we call a class method with a variable x,
      //  we lose the class name from cls, so we set it again
      auto* cls =
          scope->LookupStruct(lesmaType->GetLlvmType()->getStructName().str());
      cls->SetName(lesmaType->GetLlvmType()->getStructName().str());

      if (cls->GetType()->Is(BaseType::TY_CLASS)) {
        if (!field.empty()) {
          auto index = FindIndexInFields(cls->GetType(), field);
          auto* type = FindTypeInFields(cls->GetType(), field);
          if (index == -1) {
            throw CodegenError(node->GetRight()->GetSpan(),
                               "Could not find field {} in {}", field,
                               result->GetType()
                                   ->GetElementType()
                                   ->GetLlvmType()
                                   ->getStructName()
                                   .str());
          }

          auto* ptr = builder->CreateStructGEP(cls->GetType()->GetLlvmType(),
                                               result->GetLlvmValue(), index);
          if (isAssignment) {
            result = std::make_unique<Value>(
                "",
                CacheType(std::make_unique<Type>(BaseType::TY_PTR,
                                                 builder->getPtrTy(), type)),
                ptr);
            return;
          }
          //                    auto &x = cls->GetType()->GetFields()[index];
          result = std::make_unique<Value>(
              "", type, builder->CreateLoad(type->GetLlvmType(), ptr));
          return;
        }
        if (method != nullptr) {
          selfSymbol = cls;
          result = GenFuncCall(method, {result.get()});
          selfSymbol = nullptr;
          return;
        }
      } else {
        throw CodegenError(node->GetLeft()->GetSpan(),
                           "Cannot find related class {}",
                           lesmaType->GetLlvmType()->getStructName().str());
      }
    }
  }
  throw CodegenError(node->GetSpan(), "Unimplemented dot accessor: {}",
                     node->ToString(sourceManager.get(), "", true));
}

auto Codegen::Visit(const CastOp* node) -> void {
  node->GetExpression()->Accept(*this);
  auto expr = std::move(result);
  node->GetType()->Accept(*this);
  auto* castType = result->GetType();
  result = Cast(node->GetSpan(), expr.get(), castType);
}

auto Codegen::Visit(const IsOp* node) -> void {
  node->GetLeft()->Accept(*this);
  auto* leftType = result->GetType();
  node->GetRight()->Accept(*this);
  auto* rightType = result->GetType();

  llvm::Value* val = nullptr;

  if (node->GetOperator() == TokenType::IS) {
    val =
        leftType->IsEqual(rightType) ? builder->getTrue() : builder->getFalse();
  } else {
    val =
        leftType->IsEqual(rightType) ? builder->getFalse() : builder->getTrue();
  }

  result =
      std::make_unique<Value>("",
                              CacheType(std::make_unique<Type>(
                                  BaseType::TY_BOOL, builder->getInt1Ty())),
                              val);
}

auto Codegen::Visit(const UnaryOp* node) -> void {
  node->GetExpression()->Accept(*this);

  llvm::Value* val = nullptr;
  lesma::Type* type = result->GetType();
  std::unique_ptr<lesma::Type>
      ptrTypeHolder; // Keep owned type alive if created

  if (node->GetOperator() == TokenType::MINUS) {
    if (result->GetType()->Is(BaseType::TY_INT)) {
      val = builder->CreateNeg(result->GetLlvmValue());
    } else if (result->GetType()->Is(BaseType::TY_FLOAT)) {
      val = builder->CreateFNeg(result->GetLlvmValue());
    } else {
      throw CodegenError(
          node->GetSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->GetOperator()),
          node->GetExpression()->ToString(sourceManager.get(), "", true));
    }
  } else if (node->GetOperator() == TokenType::NOT) {
    if (result->GetType()->Is(BaseType::TY_BOOL)) {
      val = builder->CreateNot(result->GetLlvmValue());
    } else {
      throw CodegenError(
          node->GetSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->GetOperator()),
          node->GetExpression()->ToString(sourceManager.get(), "", true));
    }
  } else if (node->GetOperator() == TokenType::STAR) {
    if (result->GetType()->Is(BaseType::TY_PTR)) {
      val = builder->CreateLoad(
          result->GetType()->GetElementType()->GetLlvmType(),
          result->GetLlvmValue());
      type = result->GetType()->GetElementType();
    } else {
      throw CodegenError(
          node->GetSpan(), "Cannot apply {} to {}",
          NAMEOF_ENUM(node->GetOperator()),
          node->GetExpression()->ToString(sourceManager.get(), "", true));
    }
  } else if (node->GetOperator() == TokenType::AMPERSAND) {
    val = builder->CreateAlloca(result->GetType()->GetLlvmType());
    ptrTypeHolder = std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), result->GetType());
    type = ptrTypeHolder.get();
    builder->CreateStore(result->GetLlvmValue(), val);
  } else {
    throw CodegenError(
        node->GetSpan(), "Unknown unary operator, cannot apply {} to {}",
        NAMEOF_ENUM(node->GetOperator()),
        node->GetExpression()->ToString(sourceManager.get(), "", true));
  }

  // For AMPERSAND case, Value needs to own the Type since ptrTypeHolder will go
  // out of scope
  if (ptrTypeHolder) {
    result = std::make_unique<Value>(std::move(ptrTypeHolder));
    result->SetLlvmValue(val);
  } else {
    result = std::make_unique<Value>("", type, val);
  }
}

auto Codegen::Visit(const Literal* node) -> void {
  // Cache Types to prevent dangling pointers when result is reassigned
  if (node->GetType() == TokenType::DOUBLE) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(
        "", type,
        ConstantFP::get(theModule->getContext(),
                        APFloat(std::stod(node->GetValue()))));
  } else if (node->GetType() == TokenType::INTEGER) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    result = std::make_unique<Value>(
        "", type,
        ConstantInt::getSigned(builder->getInt64Ty(),
                               std::stoi(node->GetValue())));
  } else if (node->GetType() == TokenType::BOOL) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(
        "", type,
        node->GetValue() == "true" ? builder->getTrue() : builder->getFalse());
  } else if (node->GetType() == TokenType::STRING) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
    result = std::make_unique<Value>(
        "", type, builder->CreateGlobalString(node->GetValue()));
  } else if (node->GetType() == TokenType::NIL) {
    auto* type = CacheType(
        std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result = std::make_unique<Value>(
        "", type, ConstantPointerNull::getNullValue(builder->getPtrTy()));
  } else if (node->GetType() == TokenType::IDENTIFIER) {
    // Look this variable up in the function.
    auto* val = scope->Lookup(node->GetValue());
    if (val == nullptr) {
      throw CodegenError(node->GetSpan(), "Unknown variable name {}",
                         node->GetValue());
    }

    if (val->GetType()->IsOneOf({BaseType::TY_CLASS})) {
      // If it's a class, don't load the value - make a copy from symbol table
      result = std::make_unique<Value>(*val);
    } else {
      // Load the value.
      llvm::Value* llvmVal = builder->CreateLoad(val->GetType()->GetLlvmType(),
                                                 val->GetLlvmValue());
      result = std::make_unique<Value>("", val->GetType(), llvmVal);
    }
  } else {
    throw CodegenError(node->GetSpan(), "Unknown literal {}", node->GetValue());
  }
}

auto Codegen::Visit(const Else* /*node*/) -> void {
  auto* type = CacheType(
      std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
  result = std::make_unique<Value>(
      "", type, llvm::ConstantInt::getTrue(theModule->getContext()));
}

auto Codegen::GetTypeMangledName(llvm::SMRange span, lesma::Type* type)
    -> std::string {
  auto* llvmTy = type->GetLlvmType();
  if (type->Is(BaseType::TY_BOOL)) {
    return "b";
  }
  if (type->Is(BaseType::TY_INT) && llvmTy->isIntegerTy(8)) {
    return "c";
  }
  if (type->Is(BaseType::TY_INT) && llvmTy->isIntegerTy(16)) {
    return "i16";
  }
  if (type->Is(BaseType::TY_INT) && llvmTy->isIntegerTy(32)) {
    return "i32";
  }
  if (type->Is(BaseType::TY_INT)) {
    return "i";
  }
  if (type->Is(BaseType::TY_FLOAT) && llvmTy->isFloatTy()) {
    return "f32";
  }
  if (type->Is(BaseType::TY_FLOAT) && llvmTy->isFloatingPointTy()) {
    return "f";
  }
  if (type->Is(BaseType::TY_STRING)) {
    return "str";
  }
  if (type->Is(BaseType::TY_VOID)) {
    return "void";
  }
  if (type->Is(BaseType::TY_ARRAY) && llvmTy->isArrayTy()) {
    return "(arr_" + GetTypeMangledName(span, type->GetElementType()) + ")";
  }
  if (type->Is(BaseType::TY_PTR)) {
    return "(ptr_" + GetTypeMangledName(span, type->GetElementType()) + ")";
  }
  if (type->Is(BaseType::TY_FUNCTION)) {
    std::string paramStr;
    for (const auto& field : type->GetFields()) {
      paramStr += GetTypeMangledName(span, field->type) + "_";
    }
    return "(func_" + paramStr + ")";
  }
  if (type->IsOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
    std::string paramStr;
    for (const auto& field : type->GetFields()) {
      paramStr += GetTypeMangledName(span, field->type) + "_";
    }
    return "(struct_" + type->GetLlvmType()->getStructName().str() + ")";
  }

  throw CodegenError(span, "Unknown type found during mangling");
}

auto Codegen::IsMethod(const std::string& mangledName) -> bool {
  return mangledName.find("::") != std::string::npos;
}

auto Codegen::GetMangledName(llvm::SMRange span, std::string funcName,
                             const std::vector<lesma::Type*>& paramTypes,
                             bool isMethod, std::string alias) -> std::string {
  alias = alias.empty() ? this->alias : alias;
  std::string name =
      (alias.empty() ? "" : "&" + alias + "=>") +
      (selfSymbol != nullptr && isMethod
           ? selfSymbol->GetName() + "::" + std::move(funcName) + ":"
           : "." + std::move(funcName) + ":");
  bool first = true;

  for (auto* paramType : paramTypes) {
    if (!first) {
      name += ",";
    } else {
      first = false;
    }

    name += GetTypeMangledName(span, paramType);
  }

  return name;
}

auto Codegen::IsMangled(std::string name) -> bool {
  return name.find(':') != std::string::npos || name.at(0) == '.';
}

auto Codegen::GetDemangledName(const std::string& name) -> std::string {
  if (!IsMangled(name)) {
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

auto Codegen::GetExtendedType(lesma::Type* left, lesma::Type* right)
    -> lesma::Type* {
  if (left->GetBaseType() == right->GetBaseType()) {
    return left;
  }

  if (left->Is(BaseType::TY_INT) && right->Is(BaseType::TY_INT)) {
    // TODO: We should ideally only have one int type, but our FFI
    // implementation needs access to all types
    if (left->GetLlvmType()->getIntegerBitWidth() >
        right->GetLlvmType()->getIntegerBitWidth()) {
      return left;
    }
    return right;
  }
  if (left->Is(BaseType::TY_INT) && right->Is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->Is(BaseType::TY_FLOAT) && right->Is(BaseType::TY_INT)) {
    return left;
  }
  if (left->Is(BaseType::TY_FLOAT) && right->Is(BaseType::TY_FLOAT)) {
    if (left->GetLlvmType()->isFP128Ty() || right->GetLlvmType()->isFP128Ty()) {
      return left->GetLlvmType()->isFP128Ty() ? left : right;
    }
    if (left->GetLlvmType()->isDoubleTy() ||
        right->GetLlvmType()->isDoubleTy()) {
      return left->GetLlvmType()->isDoubleTy() ? left : right;
    }
    if (left->GetLlvmType()->isFloatTy() || right->GetLlvmType()->isFloatTy()) {
      return left->GetLlvmType()->isFloatTy() ? left : right;
    }
    if (left->GetLlvmType()->isHalfTy() || right->GetLlvmType()->isHalfTy()) {
      return left->GetLlvmType()->isHalfTy() ? left : right;
    }
  }
  return nullptr;
}

auto Codegen::Cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
    -> std::unique_ptr<lesma::Value> {
  if (type == nullptr) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  // If they're the same type
  if (val->GetType()->IsEqual(type)) {
    return std::make_unique<Value>(*val); // Copy for borrowed value
  }

  if (type->Is(BaseType::TY_INT)) {
    if (val->GetType()->Is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPToSI(val->GetLlvmValue(), type->GetLlvmType()));
    }
    if (val->GetType()->Is(BaseType::TY_INT)) {
      return std::make_unique<Value>("", type,
                                     builder->CreateIntCast(val->GetLlvmValue(),
                                                            type->GetLlvmType(),
                                                            type->IsSigned()));
    }
  } else if (type->Is(BaseType::TY_FLOAT)) {
    if (val->GetType()->Is(BaseType::TY_INT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateSIToFP(val->GetLlvmValue(), type->GetLlvmType()));
    }
    if (val->GetType()->Is(BaseType::TY_FLOAT)) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateFPCast(val->GetLlvmValue(), type->GetLlvmType()));
    }
  } else if (type->Is(BaseType::TY_STRING)) {
    if (val->GetType()->Is(BaseType::TY_PTR) &&
        (val->GetType()->GetElementType()->Is(BaseType::TY_INT) ||
         val->GetType()->GetElementType()->Is(BaseType::TY_VOID))) {
      return std::make_unique<Value>(
          "", type,
          builder->CreateBitCast(val->GetLlvmValue(), type->GetLlvmType()));
    }
  }

  throw CodegenError(span, "Unsupported Cast between {} and {}",
                     GetTypeMangledName(span, val->GetType()),
                     GetTypeMangledName(span, type));
}

auto Codegen::GenFuncCall(const FuncCall* node,
                          const std::vector<lesma::Value*>& extraParams = {})
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;

  for (auto* arg : extraParams) {
    paramTypes.push_back(arg->GetType());
    paramsLLVM.push_back(arg->GetLlvmValue());
  }

  for (auto* arg : node->GetArguments()) {
    arg->Accept(*this);
    paramTypes.push_back(result->GetType());
    paramsLLVM.push_back(result->GetLlvmValue());
  }

  Value* symbol = nullptr;
  // Check if it's a constructor like `Classname()`
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->LookupStruct(node->GetName());
  llvm::Value* classPtr = nullptr;
  // Keep the Type alive for the duration of the lookup
  std::unique_ptr<Type> selfParamType;
  if (classSym != nullptr && classSym->GetType()->Is(BaseType::TY_CLASS)) {
    // It's a class constructor, allocate and add self param
    classPtr = builder->CreateAlloca(classSym->GetType()->GetLlvmType());
    paramsLLVM.insert(paramsLLVM.begin(), classPtr);
    selfParamType = std::make_unique<Type>(
        BaseType::TY_PTR, builder->getPtrTy(), classSym->GetType());
    paramTypes.insert(paramTypes.begin(), selfParamType.get());

    selfSymbol = classSym;
    symbol = scope->LookupFunction("new", paramTypes);
  } else {
    symbol = scope->LookupFunction(node->GetName(), paramTypes);
  }

  if (symbol == nullptr) {
    throw CodegenError(node->GetSpan(), "{} {} not in current scope.",
                       classSym != nullptr ? "Constructor for" : "Function",
                       node->GetName());
  }

  if (!symbol->GetType()->IsOneOf(
          {BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
    throw CodegenError(node->GetSpan(),
                       "Symbol {} is not a function or constructor.",
                       node->GetName());
  }

  if (symbol->GetType()->GetFields().size() > paramsLLVM.size()) {
    auto fields = symbol->GetType()->GetFields();

    for (auto* field : fields) {
      if (field->defaultValue == nullptr) {
        throw CodegenError(node->GetSpan(),
                           "Something bad happened, lookup found a function "
                           "with incorrect defaults",
                           node->GetName());
      }
      paramsLLVM.push_back(field->defaultValue->GetLlvmValue());
    }
  }
  auto* func =
      llvm::cast<Function>(symbol->GetType()->Is(BaseType::TY_CLASS)
                               ? symbol->GetConstructor()->GetLlvmValue()
                               : symbol->GetLlvmValue());
  if (classSym != nullptr && classSym->GetType()->Is(BaseType::TY_CLASS)) {
    builder->CreateCall(func, paramsLLVM);
    selfSymbol = selfSymbolTmp;

    return std::make_unique<Value>("", classSym->GetType(), classPtr);
  }

  return std::make_unique<Value>("", symbol->GetType()->GetReturnType(),
                                 builder->CreateCall(func, paramsLLVM));
}

auto Codegen::FindIndexInFields(Type* structType, const std::string& field)
    -> int {
  for (unsigned int i = 0; i < structType->GetFields().size(); i++) {
    if (structType->GetFields()[i]->name == field) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

auto Codegen::FindTypeInFields(Type* structType, const std::string& field)
    -> lesma::Type* {
  for (const auto& i : structType->GetFields()) {
    if (i->name == field) {
      return i->type;
    }
  }

  return nullptr;
}