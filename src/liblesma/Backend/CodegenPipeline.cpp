#include "Codegen.h"

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
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

#ifdef __APPLE__
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Common/Utils.h"

using namespace lesma;
using namespace llvm;
using namespace llvm::orc;

auto Codegen::initializeModule() -> std::unique_ptr<Module> {
  std::unique_ptr<Module> mod;
#if LLVM_VERSION_MAJOR >= 21
  theContext->withContextDo([&mod](LLVMContext* ctx) {
    mod = std::make_unique<Module>(std::string{codegen::runtime::kLlvmModuleName}, *ctx);
  });
#else
  {
    auto lock = theContext->getLock();
    LLVMContext* ctx = theContext->getContext();
    mod = std::make_unique<Module>(std::string{codegen::runtime::kLlvmModuleName}, *ctx);
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
  auto jitOrErr = jitBuilder.create();
  if (!jitOrErr) {
    throw CodegenError({}, "Couldn't initialize JIT:\n{}", llvm::toString(jitOrErr.takeError()));
  }
  auto jit = std::move(*jitOrErr);

  // Add support for C native functions
  auto& mainJd = jit->getMainJITDylib();
  auto generatorOrErr = DynamicLibrarySearchGenerator::GetForCurrentProcess(
      jit->getDataLayout().getGlobalPrefix());
  if (!generatorOrErr) {
    throw CodegenError(
        {}, "Couldn't create dynamic library search generator:\n{}",
        llvm::toString(generatorOrErr.takeError()));
  }
  auto generator = std::move(*generatorOrErr);
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
    throw CodegenError({}, "JIT Error:\n{}", llvm::toString(std::move(jitError)));
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
  collectTraitMetadataFromAst();
  // Load implicit stdlib modules once so every module has base (including list) helpers.
  // Done here (not in constructor) to avoid re-entrancy when creating Codegens for imported
  // modules.
  std::vector<std::string> const implicitStdlibModules = {std::string{codegen::runtime::kImplicitStdlibModule}};
  auto const currentPath = normalizeResolvedFilesystemPath(filename);
  auto const basePath =
      normalizeResolvedFilesystemPath((std::filesystem::path(getStdDir()) / "base.les").string());
  const bool mainIsStdlibEntry = !filename.empty() && currentPath == basePath;
  if (!mainIsStdlibEntry || filename.empty()) {
    for (const auto& moduleName : implicitStdlibModules) {
      auto const modulePath =
          normalizeResolvedFilesystemPath((std::filesystem::path(getStdDir()) / moduleName).string());
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
    } else if (auto* cls = std::get<2>(prototypes[pi]);
               cls != nullptr && cls->getType() != nullptr &&
               cls->getType()->is(BaseType::TY_PTR) &&
               cls->getType()->getElementType() != nullptr) {
      if (auto clsEnv = specializedClassTypeEnvs.find(cls->getType()->getElementType());
          clsEnv != specializedClassTypeEnvs.end()) {
        currentGenericTypes = clsEnv->second;
      }
    } else {
      auto fields = fn->getType()->getFields();
      if (!fields.empty() && fields.front()->type != nullptr &&
          fields.front()->type->is(BaseType::TY_PTR) &&
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
