#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
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
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/IPO/StripDeadPrototypes.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include "Codegen.h"
#include <lld/Common/Driver.h>

#include "liblesma/AST/AST.h"

#ifdef __APPLE__
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif

#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Common/LesmaVersion.h"
#include "liblesma/Common/Utils.h"

using namespace lesma;
using namespace llvm;
using namespace llvm::orc;

namespace {

[[nodiscard]] auto llvmErrorToString(Error err) -> std::string {
  std::string msg;
  handleAllErrors(std::move(err), [&](const ErrorInfoBase& ei) { msg = ei.message(); });
  return msg.empty() ? "unknown error" : msg;
}

} // namespace

Codegen::~Codegen() = default;

auto Codegen::initializeDebugMetadata() -> void {
  if (!emitDebugInfo) {
    return;
  }
  diBuilder = std::make_unique<DIBuilder>(*theModule);
  SmallString<512> pathNative(filename);
  sys::path::native(pathNative);
  StringRef const full = pathNative.empty() ? StringRef("<stdin>") : StringRef(pathNative);
  StringRef const dir = pathNative.empty() ? StringRef(".") : sys::path::parent_path(full);
  StringRef const fname = pathNative.empty() ? StringRef("<stdin>") : sys::path::filename(full);
  moduleDiFile = diBuilder->createFile(fname, dir.empty() ? StringRef(".") : dir);

  std::string const producer = std::string("Lesma ") + LESMA_VERSION;
  bool const isOptimized = optimizationLevelForDebug != OptimizationLevel::O0;
  diCompileUnit = diBuilder->createCompileUnit(
      dwarf::DW_LANG_C99, moduleDiFile, producer, isOptimized, "",
      /*RuntimeVersion=*/0U, StringRef(), DICompileUnit::DebugEmissionKind::FullDebug);

  emptyDiSubroutineType =
      diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray(ArrayRef<Metadata*>{}));

  theModule->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);
  theModule->addModuleFlag(Module::Warning, "Dwarf Version", dwarf::DWARF_VERSION);
}

auto Codegen::finalizeDebugMetadata() -> void {
  if (diBuilder != nullptr) {
    diBuilder->finalize();
    diBuilder.reset();
  }
}

auto Codegen::getOrCreateDiFileForBuffer(unsigned bufferId) -> DIFile* {
  if (bufferId == 0U || diBuilder == nullptr) {
    return moduleDiFile;
  }
  if (auto it = diFileByBufferId.find(bufferId); it != diFileByBufferId.end()) {
    return it->second;
  }
  const MemoryBuffer* mb = sourceManager->getMemoryBuffer(bufferId);
  std::string pathStr = std::string(mb->getBufferIdentifier());
  SmallString<512> nativePath(pathStr);
  sys::path::native(nativePath);
  StringRef const full = nativePath;
  StringRef const dir = sys::path::parent_path(full);
  StringRef const fname = sys::path::filename(full);
  DIFile* df = diBuilder->createFile(fname, dir.empty() ? StringRef(".") : dir);
  diFileByBufferId.emplace(bufferId, df);
  return df;
}

auto Codegen::getDiTypeForLlvmType(llvm::Type* t) -> llvm::DIType* {
  if (diBuilder == nullptr || t == nullptr) {
    return nullptr;
  }
  DataLayout const& dl = theModule->getDataLayout();
  if (t->isIntegerTy()) {
    unsigned const bits = t->getIntegerBitWidth();
    dwarf::TypeKind ate = bits == 1U ? dwarf::DW_ATE_boolean : dwarf::DW_ATE_signed;
    return diBuilder->createBasicType("int", bits, ate);
  }
  if (t->isFloatingPointTy()) {
    return diBuilder->createBasicType("float", dl.getTypeSizeInBits(t), dwarf::DW_ATE_float);
  }
  if (t->isPointerTy()) {
    return diBuilder->createBasicType("__ptr", dl.getPointerSizeInBits(0), dwarf::DW_ATE_address);
  }
  if (t->isVoidTy()) {
    return diBuilder->createUnspecifiedType("void");
  }
  if (t->isStructTy() || t->isArrayTy()) {
    return diBuilder->createBasicType("aggregate", dl.getTypeSizeInBits(t), dwarf::DW_ATE_unsigned);
  }
  return diBuilder->createBasicType("opaque", dl.getTypeSizeInBits(t), dwarf::DW_ATE_unsigned);
}

auto Codegen::attachFunctionDebugInfo(Function* f, StringRef displayName, StringRef linkageName,
                                      SMRange declSpan, GlobalValue::LinkageTypes linkage,
                                      bool isMainSubprogram) -> void {
  if (!emitDebugInfo || diBuilder == nullptr || emptyDiSubroutineType == nullptr ||
      diCompileUnit == nullptr) {
    return;
  }
  DIFile* file = moduleDiFile;
  unsigned line = 1U;
  if (declSpan.isValid() && declSpan.Start.isValid()) {
    unsigned const bufId = sourceManager->FindBufferContainingLoc(declSpan.Start);
    if (bufId != 0U) {
      file = getOrCreateDiFileForBuffer(bufId);
    }
    auto const lc = sourceManager->getLineAndColumn(declSpan.Start);
    line = lc.first;
  }
  bool const localToUnit =
      linkage == GlobalValue::InternalLinkage || linkage == GlobalValue::PrivateLinkage;
  bool const isOpt = optimizationLevelForDebug != OptimizationLevel::O0;
  DISubprogram::DISPFlags const spFlags = DISubprogram::toSPFlags(
      localToUnit, true, isOpt, DISubprogram::SPFlagNonvirtual, isMainSubprogram);
  DISubprogram* sp = diBuilder->createFunction(static_cast<DIScope*>(diCompileUnit), displayName,
                                               linkageName, file, line, emptyDiSubroutineType, line,
                                               DINode::FlagPrototyped, spFlags);
  f->setSubprogram(sp);
}

auto Codegen::emitParameterDebugDeclare(llvm::Function* fn, llvm::Value* storage,
                                        llvm::StringRef name, unsigned dwArgNo, llvm::DIFile* file,
                                        unsigned line, llvm::Type* paramLlvmTy,
                                        llvm::Instruction* insertBefore) -> void {
  if (!emitDebugInfo || diBuilder == nullptr || fn->getSubprogram() == nullptr) {
    return;
  }
  DISubprogram* sp = fn->getSubprogram();
  DIType* ty = getDiTypeForLlvmType(paramLlvmTy);
  if (ty == nullptr) {
    ty = getDiTypeForLlvmType(builder->getInt64Ty());
  }
  DILocalVariable* v = diBuilder->createParameterVariable(sp, name, dwArgNo, file, line, ty);
  DILocation* dl = DILocation::get(fn->getContext(), line, 0, sp);
  if (insertBefore != nullptr) {
    diBuilder->insertDeclare(storage, v, diBuilder->createExpression(), dl,
                             InsertPosition(insertBefore->getIterator()));
  }
}

auto Codegen::emitAutoVarDebugDeclare(llvm::AllocaInst* allocaInst, llvm::StringRef name,
                                      SMRange span, llvm::Instruction* insertBefore) -> void {
  if (!emitDebugInfo || diBuilder == nullptr) {
    return;
  }
  Function* fn = allocaInst->getFunction();
  DISubprogram* sp = fn->getSubprogram();
  if (sp == nullptr) {
    return;
  }
  DIFile* file = moduleDiFile;
  unsigned line = 1U;
  if (span.isValid() && span.Start.isValid()) {
    unsigned const bufId = sourceManager->FindBufferContainingLoc(span.Start);
    if (bufId != 0U) {
      file = getOrCreateDiFileForBuffer(bufId);
    }
    line = sourceManager->getLineAndColumn(span.Start).first;
  }
  DIType* ty = getDiTypeForLlvmType(allocaInst->getAllocatedType());
  if (ty == nullptr) {
    ty = getDiTypeForLlvmType(builder->getInt64Ty());
  }
  DILocalVariable* v = diBuilder->createAutoVariable(sp, name, file, line, ty);
  DILocation* dl = DILocation::get(allocaInst->getContext(), line, 0, sp);
  DIExpression* expr = diBuilder->createExpression();
  if (insertBefore != nullptr) {
    diBuilder->insertDeclare(allocaInst, v, expr, dl, InsertPosition(insertBefore->getIterator()));
    return;
  }
  if (llvm::Instruction* next = allocaInst->getNextNode()) {
    diBuilder->insertDeclare(allocaInst, v, expr, dl, InsertPosition(next->getIterator()));
    return;
  }
  diBuilder->insertDeclare(allocaInst, v, expr, dl, InsertPosition(allocaInst->getParent()->end()));
}

auto Codegen::setDebugLoc(SMRange span) -> void {
  if (!emitDebugInfo || diBuilder == nullptr) {
    return;
  }
  BasicBlock* bb = builder->GetInsertBlock();
  if (bb == nullptr) {
    return;
  }
  Function* fn = bb->getParent();
  DISubprogram* sp = fn->getSubprogram();
  if (sp == nullptr) {
    builder->SetCurrentDebugLocation(DebugLoc());
    return;
  }
  if (!span.isValid() || !span.Start.isValid()) {
    builder->SetCurrentDebugLocation(DebugLoc());
    return;
  }
  unsigned const bufId = sourceManager->FindBufferContainingLoc(span.Start);
  if (bufId == 0U) {
    builder->SetCurrentDebugLocation(DebugLoc());
    return;
  }
  auto const lc = sourceManager->getLineAndColumn(span.Start);
  DILocation* loc = DILocation::get(theModule->getContext(), lc.first, lc.second, sp);
  builder->SetCurrentDebugLocation(loc);
}

auto Codegen::createAllocaInEntry(llvm::Function* fn, llvm::Type* elemTy, const std::string& name)
    -> llvm::AllocaInst* {
  llvm::BasicBlock& entry = fn->getEntryBlock();
  llvm::IRBuilder<> atEntry(&entry, entry.getFirstInsertionPt());
  return atEntry.CreateAlloca(elemTy, nullptr, name);
}

auto Codegen::initializeModule() -> std::unique_ptr<Module> {
  std::unique_ptr<Module> mod;
#if LLVM_VERSION_MAJOR >= 21
  theContext->withContextDo([&mod](LLVMContext* ctx) {
    mod = std::make_unique<Module>(std::string{codegen::runtime::LLVM_MODULE_NAME}, *ctx);
  });
#else
  {
    auto lock = theContext->getLock();
    LLVMContext* ctx = theContext->getContext();
    mod = std::make_unique<Module>(std::string{codegen::runtime::LLVM_MODULE_NAME}, *ctx);
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

  std::string cpu(llvm::sys::getHostCPUName());
  if (cpu.empty()) {
    cpu = "generic";
  }
  std::string featuresStr;
  if (cpu != "generic") {
    SubtargetFeatures feat;
    for (const auto& entry : llvm::sys::getHostCPUFeatures()) {
      feat.AddFeature(entry.getKey(), entry.getValue());
    }
    featuresStr = feat.getString();
  }

  std::unique_ptr<llvm::TargetMachine> targetMachine(
#if LLVM_VERSION_MAJOR >= 21
      target->createTargetMachine(targetTriple, cpu, featuresStr, opt, rm));
#else
      target->createTargetMachine(targetTriple.str(), cpu, featuresStr, opt, rm));
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
    throw CodegenError({}, std::string("Couldn't initialize JIT:\n") +
                               llvm::toString(jitOrErr.takeError()));
  }
  auto jit = std::move(*jitOrErr);

  // Default LLJIT uses JITLink on supported targets (in-process). Debugger registration via
  // llvm::orc::enableDebuggerSupport is omitted: LLVM 21's helper can assert on darwin-arm64 with
  // this stack; revisit when emitting JIT DWARF or when upstream stabilizes the API.

  // Add support for C native functions
  auto& mainJd = jit->getMainJITDylib();
  auto generatorOrErr =
      DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix());
  if (!generatorOrErr) {
    throw CodegenError({}, std::string("Couldn't create dynamic library search generator:\n") +
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

  if (emitDebugInfo) {
    SMRange span;
    if (parser != nullptr && parser->getAst() != nullptr) {
      span = parser->getAst()->getSpan();
    }
    attachFunctionDebugInfo(f, isMain ? StringRef("main") : StringRef("<module>"), f->getName(),
                            span, f->getLinkage(), isMain);
  }

  return f;
}

auto Codegen::verifyIrModuleOrThrow(const std::string& contextLabel) const -> void {
  if (theModule == nullptr) {
    throw CodegenError({}, "Internal error: no LLVM module to verify ({})", contextLabel);
  }
  std::string err;
  raw_string_ostream os(err);
  if (verifyModule(*theModule, &os)) {
    os.flush();
    // Avoid fmt::format: LLVM diagnostic text can contain braces and break formatting.
    throw CodegenError({}, std::string("Invalid LLVM IR (") + contextLabel + "):\n" + err);
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
  // Split promotable aggregate allocas, then mem2reg: reduces stack traffic and dead lifetime slots
  // left after inlining (e.g. unused this/arg spill allocas in inlined callees).
  fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
  fpm.addPass(llvm::PromotePass());
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
  mpm.addPass(llvm::StripDeadPrototypesPass());
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

auto Codegen::resolveAppleSdkUsrLib() const -> std::string {
  if (const char* explicitDir = std::getenv("LESMA_APPLE_SDK_LIB")) {
    StringRef const ev(explicitDir);
    if (!ev.empty() && sys::fs::exists(explicitDir)) {
      return std::string(explicitDir);
    }
  }
  if (const char* sdkRoot = std::getenv("SDKROOT")) {
    StringRef const sdkRef(sdkRoot);
    if (!sdkRef.empty()) {
      SmallString<512> usrLib(sdkRoot);
      sys::path::append(usrLib, "usr", "lib");
      if (sys::fs::exists(usrLib)) {
        return std::string(usrLib.str());
      }
    }
  }
  static constexpr std::array<StringRef, 2> fallbacks = {
      StringRef("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib"),
      StringRef(
          "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/"
          "MacOSX.sdk/usr/lib"),
  };
  for (StringRef path : fallbacks) {
    if (sys::fs::exists(path)) {
      return std::string(path.str());
    }
  }
  throw CodegenError(
      {}, "Could not locate macOS SDK usr/lib for linking. Set SDKROOT, LESMA_APPLE_SDK_LIB, or "
          "install Xcode / Command Line Tools.");
}

auto Codegen::darwinLinkerArch() const -> std::string {
  llvm::Triple const triple = targetMachine->getTargetTriple();
  switch (triple.getArch()) {
  case llvm::Triple::aarch64:
    return "arm64";
  case llvm::Triple::x86_64:
    return "x86_64";
  default:
    throw CodegenError({}, "Unsupported Darwin architecture for native linking");
  }
}

auto Codegen::appendDarwinLinkArgs(std::vector<const char*>& args, std::vector<std::string>& owned,
                                   const std::string& outputBase,
                                   const std::string& objFilename) const -> void {
  auto pushOwned = [&args, &owned](std::string s) {
    owned.push_back(std::move(s));
    args.push_back(owned.back().c_str());
  };

  args.push_back("-w");
  pushOwned("-o");
  pushOwned(outputBase);
  args.push_back(objFilename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }
  pushOwned("-arch");
  pushOwned(darwinLinkerArch());
  pushOwned("-platform_version");
  pushOwned("macos");
  const char* minVer = std::getenv("LESMA_MACOS_MIN_VERSION");
  const char* sdkVer = std::getenv("LESMA_MACOS_SDK_VERSION");
  StringRef const minRef(minVer != nullptr ? minVer : "");
  StringRef const sdkRef(sdkVer != nullptr ? sdkVer : "");
  pushOwned(!minRef.empty() ? minRef.str() : std::string("11.0"));
  pushOwned(!sdkRef.empty() ? sdkRef.str() : std::string("11.0"));
  pushOwned("-L");
  pushOwned(resolveAppleSdkUsrLib());
  pushOwned("-lSystem");
}

auto Codegen::appendElfLinkArgs(std::vector<const char*>& args, std::vector<std::string>& owned,
                                const std::string& outputBase, const std::string& objFilename) const
    -> void {
  auto pushOwned = [&args, &owned](std::string s) {
    owned.push_back(std::move(s));
    args.push_back(owned.back().c_str());
  };

  const char* muslRt = std::getenv("LESMA_MUSL_RUNTIME");
  StringRef const muslRef(muslRt != nullptr ? muslRt : "");
  const bool muslDirSet = muslRt != nullptr && !muslRef.empty();
  const bool useMuslStatic = muslDirSet && linkMode != LinkMode::Dynamic;
  const bool elfStaticFlag =
      linkMode == LinkMode::Static && !useMuslStatic; // plain -static when no musl bundle

  if (useMuslStatic) {
    auto objectPath = [muslRt](const char* name) {
      SmallString<512> path(muslRt);
      sys::path::append(path, name);
      return std::string(path.str());
    };
    std::string crt1 = objectPath("crt1.o");
    std::string crti = objectPath("crti.o");
    std::string crtn = objectPath("crtn.o");
    std::string libcA = objectPath("libc.a");
    if (!sys::fs::exists(crt1) || !sys::fs::exists(crti) || !sys::fs::exists(crtn) ||
        !sys::fs::exists(libcA)) {
      throw CodegenError(
          {}, "LESMA_MUSL_RUNTIME must be a directory containing crt1.o, crti.o, crtn.o, and "
              "libc.a for static musl linking.");
    }
    args.push_back("-w");
    pushOwned("-static");
    pushOwned(std::move(crt1));
    pushOwned(std::move(crti));
    args.push_back(objFilename.c_str());
    for (const auto& obj : objectFiles) {
      args.push_back(obj.c_str());
    }
    pushOwned(std::move(libcA));
    pushOwned(std::move(crtn));
    pushOwned("-o");
    pushOwned(outputBase);
    return;
  }

  args.push_back("-w");
  if (elfStaticFlag) {
    pushOwned("-static");
  }
  pushOwned("-o");
  pushOwned(outputBase);
  args.push_back(objFilename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }
}

auto Codegen::appendCoffLinkArgs(std::vector<const char*>& args, std::vector<std::string>& owned,
                                 const std::string& outputBase,
                                 const std::string& objFilename) const -> void {
  auto pushOwned = [&args, &owned](std::string s) {
    owned.push_back(std::move(s));
    args.push_back(owned.back().c_str());
  };

  args.push_back("-w");
  pushOwned("-o");
  pushOwned(outputBase);
  args.push_back(objFilename.c_str());
  for (const auto& obj : objectFiles) {
    args.push_back(obj.c_str());
  }
}

void Codegen::linkObjectFileWithLld(const std::string& objFilename) {
  std::string const output = getBasename(objFilename);

  std::vector<std::string> owned;
  owned.reserve(32U);
  std::vector<const char*> args;

#ifdef __APPLE__
  args.push_back("ld64.lld");
  appendDarwinLinkArgs(args, owned, output, objFilename);
#elif defined(_WIN32)
  args.push_back("lld-link");
  appendCoffLinkArgs(args, owned, output, objFilename);
#else
  args.push_back("ld.lld");
  appendElfLinkArgs(args, owned, output, objFilename);
#endif

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

  // Mach-O link leaves a debug map pointing at these .o paths; removing them
  // breaks lldb/dsymutil. Keep them when DWARF was requested.
  if (!emitDebugInfo) {
    std::ignore = llvm::sys::fs::remove(objFilename);
    for (const auto& obj : objectFiles) {
      std::ignore = llvm::sys::fs::remove(obj);
    }
  }
}

auto Codegen::linkObjectFile(const std::string& objFilename) -> void {
  linkObjectFileWithLld(objFilename);
}

auto Codegen::prepareJit() -> void {
  if (Error jitError = theJit->addIRModule(ThreadSafeModule(std::move(theModule), *theContext))) {
    // Concatenate: LLVM error text may contain characters that break fmt::format placeholders.
    throw CodegenError({}, std::string("JIT addIRModule failed: ") +
                               llvmErrorToString(std::move(jitError)));
  }
  Expected<ExecutorAddr> mainFuncOrErr = theJit->lookup(topLevelFunc->getName());
  if (!mainFuncOrErr) {
    throw CodegenError({}, std::string("Couldn't find top-level function '") +
                               topLevelFunc->getName().str() +
                               "': " + llvmErrorToString(mainFuncOrErr.takeError()));
  }
  mainFuncAddress = mainFuncOrErr->toPtr<MainFnTy>();

  if (pendingJitModuleInits != nullptr) {
    for (const std::string& sym : *pendingJitModuleInits) {
      Expected<ExecutorAddr> initAddr = theJit->lookup(sym);
      if (!initAddr) {
        throw CodegenError({}, std::string("JIT could not resolve module initializer ") + sym +
                                   ": " + llvmErrorToString(initAddr.takeError()));
      }
      using ModuleInitTy = int64_t();
      std::ignore = initAddr->toPtr<ModuleInitTy>()();
    }
    pendingJitModuleInits->clear();
  }
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
  std::vector<std::string> const implicitStdlibModules = {
      std::string{codegen::runtime::IMPLICIT_STDLIB_MODULE}};
  auto const currentPath = normalizeResolvedFilesystemPath(filename);
  auto const basePath =
      normalizeResolvedFilesystemPath((std::filesystem::path(getStdDir()) / "base.les").string());
  const bool mainIsStdlibEntry = !filename.empty() && currentPath == basePath;
  if (!mainIsStdlibEntry || filename.empty()) {
    for (const auto& moduleName : implicitStdlibModules) {
      auto const modulePath = normalizeResolvedFilesystemPath(
          (std::filesystem::path(getStdDir()) / moduleName).string());
      if (currentPath == modulePath) {
        continue;
      }
      compileModule(llvm::SMRange(), getStdDir() + moduleName, true, moduleName, true, true, {});
    }
  }

  deferStack.emplace();
  pushDeferBaseline();
  if (parser->getAst() != nullptr) {
    setDebugLoc(parser->getAst()->getSpan());
  }
  parser->getAst()->accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();
  deferBaselineStack.pop();

  runDeferredStatements(instrs);

  // Define the function bodies (index-based: specializeFunction may append
  // new prototypes while we iterate, e.g. when combine<int> triggers add<int>)
  for (size_t pi = 0; pi < prototypes.size(); ++pi) {
    auto* fn = std::get<0>(prototypes[pi]);
    auto savedGenerics = currentGenericTypes;
    auto* savedSelfForFnBody = selfSymbol;
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
    // Prototypes store the owning class symbol in slot 2. Instance methods get `self` as the first
    // LLVM parameter, but static methods do not—yet nested codegen (e.g. `Cell(v)` inside
    // `static func of`) still needs that symbol on `selfSymbol` so constructor lookup and
    // `genericBindingHint`-driven paths in `callNamedFunction` see the monomorphized class.
    if (auto* cls = std::get<2>(prototypes[pi]); cls != nullptr) {
      selfSymbol = cls;
    }
    defineFunction(fn, std::get<1>(prototypes[pi]), std::get<2>(prototypes[pi]));
    selfSymbol = savedSelfForFnBody;
    currentGenericTypes = std::move(savedGenerics);
  }

  for (size_t li = 0; li < lambdaPrototypes.size(); ++li) {
    auto* fn = lambdaPrototypes[li].first;
    const LambdaExpr* lam = lambdaPrototypes[li].second;
    auto savedGenerics = currentGenericTypes;
    if (auto env = specializationEnvs.find(fn); env != specializationEnvs.end()) {
      currentGenericTypes = env->second;
    }
    defineLambdaFunction(fn, lam);
    currentGenericTypes = std::move(savedGenerics);
  }

  for (size_t si = 0; si < syntheticConstructorBodies.size(); ++si) {
    lesma::Value* ctorSym = syntheticConstructorBodies[si].first;
    const Class* cls = syntheticConstructorBodies[si].second;
    auto savedGenerics = currentGenericTypes;
    if (auto env = specializationEnvs.find(ctorSym); env != specializationEnvs.end()) {
      currentGenericTypes = env->second;
    } else {
      auto fields = ctorSym->getType()->getFields();
      if (!fields.empty() && fields.front()->type != nullptr &&
          fields.front()->type->is(BaseType::TY_PTR) &&
          fields.front()->type->getElementType() != nullptr) {
        if (auto clsEnv = specializedClassTypeEnvs.find(fields.front()->type->getElementType());
            clsEnv != specializedClassTypeEnvs.end()) {
          currentGenericTypes = clsEnv->second;
        }
      }
    }
    defineSynthesizedClassConstructor(ctorSym, cls);
    currentGenericTypes = std::move(savedGenerics);
  }

  // Return 0 for top-level function (location must match topLevelFunc's DISubprogram)
  if (parser->getAst() != nullptr) {
    setDebugLoc(parser->getAst()->getSpan());
  }
  builder->CreateRet(ConstantInt::getSigned(builder->getInt64Ty(), 0));

  finalizeDebugMetadata();
}

auto Codegen::dump() -> void { theModule->print(outs(), nullptr); }
