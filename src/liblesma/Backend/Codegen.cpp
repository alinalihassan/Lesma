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
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

#include <fmt/format.h>
#include <nameof.hpp>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/LesmaError.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"
#ifdef LESMA_HAS_LLD
#include <lld/Common/Driver.h>
#endif
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename,
                 std::vector<std::string> imports, bool jit, bool main, std::string alias,
                 const std::shared_ptr<ThreadSafeContext> &context) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    TheContext_ = context == nullptr ? std::make_shared<ThreadSafeContext>(std::make_unique<LLVMContext>()) : context;
    TargetMachine_ = initializeTargetMachine();
    TheModule_ = initializeModule();
    if (jit) {
        TheJIT_ = initializeJit();
    }

    Builder_ = std::make_unique<IRBuilder<>>(TheModule_->getContext());
    Parser_ = std::move(parser);
    SourceManager_ = std::move(srcMgr);
    rootScope_ = std::make_unique<SymbolTable>(nullptr);
    Scope_ = rootScope_.get();

    this->alias_ = std::move(alias);
    this->filename_ = filename;
    isMain_ = main;
    isJIT_ = jit;

    ImportedModules_ = std::move(imports);
    TopLevelFunc_ = initializeTopLevel();

    // If it's not base.les stdlib, then import it
    if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
        compileModule(llvm::SMRange(), getStdDir() + "base.les", true, "base", true, true, {});
    }
}

auto Codegen::initializeModule() -> std::unique_ptr<Module> {
    std::unique_ptr<Module> mod;
    TheContext_->withContextDo([&](LLVMContext *Ctx) { mod = std::make_unique<Module>("Lesma", *Ctx); });
    mod->setTargetTriple(TargetMachine_->getTargetTriple());
    mod->setDataLayout(TargetMachine_->createDataLayout());
    mod->setSourceFileName(filename_);

    return mod;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto Codegen::initializeTargetMachine() -> std::unique_ptr<llvm::TargetMachine> {
    // Configure output target
    auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());

    // Search after selected target
    std::string error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(targetTriple.getTriple(), error);
    if (target == nullptr) {
        throw CodegenError({}, "Target not available:\n{}", error);
    }

    llvm::TargetOptions opt;
    llvm::Reloc::Model rm = llvm::Reloc::Model();
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, "generic", "", opt, rm));
    return targetMachine;
}

auto Codegen::initializeJit() -> std::unique_ptr<LLJIT> {
    llvm::orc::LLJITBuilder builder;
    builder.setDataLayout(TheModule_->getDataLayout());
    builder.setJITTargetMachineBuilder(llvm::orc::JITTargetMachineBuilder(TargetMachine_->getTargetTriple()));
    auto jit = llvm::cantFail(builder.create());
    if (!jit) {
        throw CodegenError({}, "Couldn't initialize JIT\n");
    }

    // Add support for C native functions
    auto &mainJd = jit->getMainJITDylib();
    auto generator =
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix()));
    mainJd.addGenerator(std::move(generator));

    return jit;
}

auto Codegen::initializeTopLevel() -> llvm::Function * {
    std::vector<llvm::Type *> paramTypes = {};

    FunctionType *ft = FunctionType::get(Builder_->getInt64Ty(), paramTypes, false);
    Function *f =
        Function::Create(ft, isMain_ ? Function::ExternalLinkage : Function::InternalLinkage, "main", *TheModule_);

    auto *entry = BasicBlock::Create(TheModule_->getContext(), "entry", f);
    Builder_->SetInsertPoint(entry);

    return f;
}

auto Codegen::defineFunction(lesma::Value *value, const FuncDecl *node, Value *clsSymbol) -> void {
    Scope_ = Scope_->createChildBlock(node->getName());
    currentFunction_ = value;
    deferStack_.emplace();

    auto *f = llvm::cast<Function>(value->getLLVMValue());

    BasicBlock *entry = BasicBlock::Create(TheModule_->getContext(), "entry", f);
    Builder_->SetInsertPoint(entry);

    int fieldIndex = 0;
    for (const auto &field: value->getType()->getFields()) {
        auto *param = f->getArg(fieldIndex);

        if (clsSymbol != nullptr && param->getArgNo() == 0) {
            param->setName("self");
        } else {
            param->setName(node->getParameters()[param->getArgNo() - (clsSymbol != nullptr ? 1 : 0)]->name);
        }

        llvm::Value *ptr = Builder_->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
        Builder_->CreateStore(param, ptr);

        auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
        Scope_->insertSymbol(std::move(symbol));

        fieldIndex++;
    }

    node->getBody()->accept(*this);

    auto instrs = deferStack_.top();
    deferStack_.pop();

    if (!isReturn_) {
        for (auto *inst: instrs) { inst->accept(*this); }
    }

    // Check for well-formness of all BBs. In particular, look for
    // any unterminated BB and try to add a Return to it.
    for (BasicBlock &bb: *f) {
        Instruction *terminator = bb.getTerminator();
        if (terminator != nullptr) {
            continue;  // Well-formed
        }

        if (value->getType()->getReturnType()->is(BaseType::TY_VOID)) {
            // Make implicit return of void Function explicit.
            Builder_->SetInsertPoint(&bb);
            Builder_->CreateRetVoid();
        } else {
            throw CodegenError(node->getSpan(), "Function {} does not always return a result", node->getName());
        }
    }

    isReturn_ = false;

    // Verify function
    // TODO: Verify function again, unfortunately functions from other modules have attributes attached without context of usage, and verify gives error
    //    std::string output;
    //    llvm::raw_string_ostream oss(output);
    //    if (llvm::verifyFunction(*F, &oss)) {
    //        F->print(outs());
    //        throw CodegenError(node->getSpan(), "Invalid Function {}\n{}", node->getName(), output);
    //    }

    // Insert Function to Symbol Table
    Scope_ = Scope_->getParent();

    currentFunction_ = nullptr;

    // Reset Insert Point to Top Level
    Builder_->SetInsertPoint(&TopLevelFunc_->back());
}

auto Codegen::compileModule(llvm::SMRange span, const std::string &filepath, bool isStd,
                            const std::string &module_alias, bool importAll, bool importToScope,
                            const std::vector<std::pair<std::string, std::string>> &imported_names) -> void {
    std::filesystem::path mainPath = filename_;
    // Read source
    auto absolutePath =
        isStd ? filepath : fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath);

    // If module is already imported, don't compile again
    // TODO: Re-enable this again, currently it destroys nested imports
    //    if (std::find(ImportedModules.begin(), ImportedModules.end(), absolute_path) != ImportedModules.end())
    //        return;

    auto buffer = MemoryBuffer::getFile(absolutePath);
    if (std::error_code ec = buffer.getError()) {
        throw LesmaError(llvm::SMRange(), "Could not read file: {}", absolutePath);
    }

    auto fileId = SourceManager_->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
    // auto sourceStr = SourceManager_->getMemoryBuffer(fileId)->getBuffer().str();
    ImportedModules_.push_back(absolutePath);

    try {
        // Lexer
        auto lexer = std::make_unique<Lexer>(SourceManager_);
        lexer->scanAll();

        // Parser
        auto parser = std::make_unique<Parser>(lexer->getTokens());
        parser->parse();

        // TODO: Delete it, memory leak, smart pointer made us lose the references to other modules
        // Codegen
        auto codegen = std::make_unique<Codegen>(std::move(parser), SourceManager_, absolutePath, ImportedModules_,
                                                 isJIT_, false, !importToScope ? module_alias : "", TheContext_);
        codegen->run();

        // Optimize
        codegen->optimize(OptimizationLevel::O3);
        codegen->TheModule_->setModuleIdentifier(filepath);

        ImportedModules_ = std::move(codegen->ImportedModules_);

        if (!importToScope) {
            auto importTyp = std::make_unique<Type>(BaseType::TY_IMPORT);
            auto *importTypPtr = importTyp.get();
            auto importSym = std::make_unique<Value>(module_alias, importTypPtr);
            Scope_->insertSymbol(std::move(importSym));
            Scope_->insertType(module_alias, std::move(importTyp));
        }

        auto findInImports = [imported_names](const std::string &import) -> std::string {
            for (const auto &impPair: imported_names) {
                if (impPair.first == import) {
                    return impPair.second;
                }
            }

            return "";
        };

        if (isJIT_) {
            // Add the module to JIT
            cantFail(TheJIT_->addIRModule(ThreadSafeModule(std::move(codegen->TheModule_), *TheContext_)),
                     fmt::format("Failed adding import {} to JIT", filename_).c_str());
        } else {
            // Create object file to be linked
            std::string objFile = fmt::format("tmp{}", ObjectFiles_.size());
            codegen->writeToObjectFile(objFile);
            ObjectFiles_.push_back(fmt::format("{}.o", objFile));
        }

        // Import Symbols
        // TODO: This section has ownership issues - Types from imported scope are referenced
        // but the imported scope will be destroyed. Need to clone Types properly.
        for (auto *sym: codegen->Scope_->getSymbols()) {
            auto impAlias = findInImports(sym->getName());
            if (sym->getType()->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) && sym->isExported() &&
                (importAll || !impAlias.empty())) {
                llvm::StructType *structType = StructType::getTypeByName(TheModule_->getContext(), sym->getName());

                auto structSymbol =
                    std::make_unique<Value>(impAlias.empty() ? sym->getName() : impAlias, sym->getType());
                structSymbol->getType()->setLLVMType(structType);
                // Insert non-owning reference to imported Type
                Scope_->insertTypeRef(sym->getName(), sym->getType());
                Scope_->insertSymbol(std::move(structSymbol));
            } else if (sym->getType()->is(BaseType::TY_FUNCTION) && sym->isExported()) {
                auto *f = llvm::dyn_cast<Function>(sym->getLLVMValue());
                auto *fTy = llvm::cast<FunctionType>(sym->getType()->getLLVMType());

                if (isJIT_) {
                    // Insert the function declaration, since we linked the modules earlier
                    f = llvm::cast<Function>(TheModule_->getOrInsertFunction(sym->getMangledName(), fTy).getCallee());
                }

                auto name = sym->getName();
                std::vector<lesma::Type *> paramTypes;
                for (auto *field: sym->getType()->getFields()) { paramTypes.push_back(field->type); }

                Value *funcSymbol = codegen->Scope_->lookupFunction(name, paramTypes);

                // Only import if it's exported
                impAlias = findInImports(name);
                // TODO: methods should only be imported if they class is in the imports specified
                if (funcSymbol != nullptr && funcSymbol->isExported() &&
                    (importAll || !impAlias.empty() || isMethod(sym->getMangledName()))) {
                    auto symbol = std::make_unique<Value>(
                        impAlias.empty() ? name : std::regex_replace(name, std::regex(name), impAlias),
                        funcSymbol->getType());

                    if (isJIT_) {
                        symbol->getType()->setLLVMType(fTy);
                        symbol->setLLVMValue(f);
                        symbol->setExported(false);
                        symbol->setMangledName(sym->getMangledName());
                    } else {
                        // If it's compiled, we need to make a new Function declaration in the importing file
                        auto *newFunc =
                            Function::Create(fTy, Function::ExternalLinkage, sym->getMangledName(), *TheModule_);
                        symbol->getType()->setLLVMType(newFunc->getFunctionType());
                        symbol->setLLVMValue(newFunc);
                        symbol->setExported(false);
                        symbol->setMangledName(sym->getMangledName());
                    }

                    Scope_->insertSymbol(std::move(symbol));
                }
            }
        }

        // Transfer ownership of imported Scope and type cache to keep Types/Values alive
        // (Types/Values in imported scope are referenced by newly created symbols)
        ImportedScopes_.push_back(std::move(codegen->rootScope_));
        codegen->Scope_ = nullptr;  // Clear navigation pointer (rootScope_ now moved)

        // Transfer type cache to keep cached types alive
        for (auto &type: codegen->typeCache_) { typeCache_.push_back(std::move(type)); }
    } catch (const LesmaError &err) {
        if (!err.getSpan().isValid()) {
            print(LogType::ERROR, err.what());
        } else {
            showInline(SourceManager_.get(), fileId, err.getSpan(), err.what(), absolutePath, true);
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

    llvm::PassBuilder pb(&*TargetMachine_);

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
    llvm::ModulePassManager mpm = pb.buildModuleOptimizationPipeline(opt, ThinOrFullLTOPhase::FullLTOPreLink);
    mpm.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(std::move(cgpm)));
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    mpm.addPass(llvm::GlobalDCEPass());

    mpm.run(*TheModule_, mam);
}

auto Codegen::writeToObjectFile(const std::string &output) -> void {
    std::error_code err;
    auto out = llvm::raw_fd_ostream(output + ".o", err);

    if (err) {
        throw CodegenError({}, "Error opening file {} for writing: {}", output, err.message());
    }

    llvm::legacy::PassManager passManager;
    if (TargetMachine_->addPassesToEmitFile(passManager, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw CodegenError({}, "Target Machine can't emit an object file");
    }
    // Emit object file
    passManager.run(*TheModule_);

    // Flush and close the file
    out.flush();
    out.close();
}

#ifdef LESMA_HAS_LLD
[[maybe_unused]] void Codegen::LinkObjectFileWithLLD(const std::string &obj_filename) {
    std::string output = getBasename(obj_filename);

    llvm::SmallVector<const char *, 32> args;
    args.push_back("lld");
    args.push_back("-o");
    args.push_back(output.c_str());
    args.push_back(obj_filename.c_str());
    for (const auto &obj: ObjectFiles_) { args.push_back(obj.c_str()); }
    // Add the standard library path for Apple
#ifdef __APPLE__
    args.push_back("-arch");
    args.push_back("arm64");
    args.push_back("-platform_version");
    args.push_back("macos");  // platform
    args.push_back("11.0");   // min version
    args.push_back("11.0");   // sdk version
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
    for (const auto &obj: ObjectFiles_) llvm::sys::fs::remove(obj);
}
#endif  // LESMA_HAS_LLD

[[maybe_unused]] auto Codegen::linkObjectFileWithClang(const std::string &objFilename) -> void {
    auto clangPath = llvm::sys::findProgramByName("clang");
    if (clangPath.getError()) {
        throw CodegenError({}, "Unable to find clang path");
    }

    std::string output = getBasename(objFilename);

    llvm::SmallVector<const char *, 32> args;
    args.push_back(clangPath.get().c_str());
    args.push_back("-o");
    args.push_back(output.c_str());
    args.push_back(objFilename.c_str());
    for (const auto &obj: ObjectFiles_) { args.push_back(obj.c_str()); }

    // Add the standard library path for Apple
#ifdef __APPLE__
    args.push_back("-L");
    args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
    args.push_back("-lSystem");
#endif

    // Set up the diagnostic engine
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(new clang::DiagnosticIDs());
    clang::DiagnosticOptions diagOpts;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - DiagnosticsEngine takes ownership
    auto *diagClient = new clang::TextDiagnosticPrinter(llvm::errs(), diagOpts);
    clang::DiagnosticsEngine diags(diagIDs, diagOpts, diagClient);

    // Create a compilation using Clang's driver
    clang::driver::Driver theDriver(args[0], TheModule_->getTargetTriple().str(), diags, "Lesma Compiler",
                                    llvm::vfs::getRealFileSystem());
    std::unique_ptr<clang::driver::Compilation> c(theDriver.BuildCompilation(args));

    if (!c) {
        throw CodegenError({}, "Failed to create clang driver compilation");
    }

    // Run the driver
    llvm::SmallVector<std::pair<int, const clang::driver::Command *>, 8> failingCommands;
    int res = theDriver.ExecuteCompilation(*c, failingCommands);

    if (res != 0) {
        throw CodegenError({}, "Linking failed");
    }

    // Remove object files (ignore errors - cleanup is best-effort)
    std::ignore = llvm::sys::fs::remove(objFilename);
    for (const auto &obj: ObjectFiles_) { std::ignore = llvm::sys::fs::remove(obj); }
}

auto Codegen::linkObjectFile(const std::string &obj_filename) -> void { linkObjectFileWithClang(obj_filename); }

auto Codegen::prepareJit() -> void {
    auto jitError = TheJIT_->addIRModule(ThreadSafeModule(std::move(TheModule_), *TheContext_));
    if (jitError) {
        throw CodegenError({}, "JIT Error:\n{}");
    }
    auto mainFunc = TheJIT_->lookup(TopLevelFunc_->getName());
    if (!mainFunc) {
        throw CodegenError({}, "Couldn't find top level function\n");
    }
    mainFuncAddress_ = mainFunc->toPtr<MainFnTy>();
}

auto Codegen::executeJit() -> int {
    if (mainFuncAddress_ == nullptr) {
        throw CodegenError({}, "Main function address not found, did you prepare JIT?\n");
    }

    return mainFuncAddress_();
}

auto Codegen::run() -> void {
    deferStack_.emplace();
    Parser_->getAST()->accept(*this);

    auto instrs = deferStack_.top();
    deferStack_.pop();

    // Visit all statements
    for (auto *inst: instrs) { inst->accept(*this); }

    // Define the function bodies
    for (auto prot: Prototypes_) { defineFunction(std::get<0>(prot), std::get<1>(prot), std::get<2>(prot)); }

    // Return 0 for top-level function
    Builder_->CreateRet(ConstantInt::getSigned(Builder_->getInt64Ty(), 0));
}

auto Codegen::dump() -> void { TheModule_->print(outs(), nullptr); }

auto Codegen::visit(const Statement *node) -> void {
    print("Visited a blank statement\n{}", node->toString(SourceManager_.get(), "", true));
}

auto Codegen::visit(const Expression *node) -> void {
    print("Visited a blank expression\n{}", node->toString(SourceManager_.get(), "", true));
}

auto Codegen::visit(const TypeExpr *node) -> void {
    // For primitive types, cache them so they survive beyond result_'s lifetime
    // This is needed because setReturnType and similar store raw Type* pointers
    if (node->getType() == TokenType::INT_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_INT, Builder_->getInt64Ty()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::INT8_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_INT, Builder_->getInt8Ty()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::INT16_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_INT, Builder_->getInt16Ty()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::INT32_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_INT, Builder_->getInt32Ty()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::FLOAT_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, Builder_->getDoubleTy()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::FLOAT32_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, Builder_->getFloatTy()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::BOOL_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::STRING_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, Builder_->getPtrTy()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::VOID_TYPE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, Builder_->getVoidTy()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::PTR_TYPE) {
        node->getElementType()->accept(*this);
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), result_->getType()));
        result_ = std::make_unique<Value>(type);
    } else if (node->getType() == TokenType::FUNC_TYPE) {
        node->getReturnType()->accept(*this);
        auto retType = std::move(result_);
        std::vector<std::unique_ptr<Field>> fields;
        std::vector<lesma::Type *> paramTypes;
        std::vector<llvm::Type *> paramLLVMTypes;
        for (auto *paramType: node->getParams()) {
            paramType->accept(*this);
            paramLLVMTypes.push_back(result_->getType()->getLLVMType());
            paramTypes.push_back(result_->getType());
            fields.push_back(std::make_unique<Field>(result_->getName(), result_->getType()));
        }

        // With opaque pointers, function pointer types are just `ptr`
        // The actual function signature is tracked in Lesma's Type system via fields
        auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, Builder_->getPtrTy(), std::move(fields));
        funcType->setReturnType(retType->getType());
        result_ = std::make_unique<Value>(cacheType(std::move(funcType)));
    } else if (node->getType() == TokenType::CUSTOM_TYPE) {
        auto *typ = Scope_->lookupType(node->getName());
        auto *sym = Scope_->lookupStruct(node->getName());
        if (typ == nullptr || sym->getType()->getLLVMType() == nullptr) {
            throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());
        }

        // Borrow from symbol table - make a shallow copy
        result_ = sym == nullptr ? std::make_unique<Value>(typ) : std::make_unique<Value>(*sym);
    } else {
        throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
    }
}

auto Codegen::visit(const Compound *node) -> void {
    for (auto *elem: node->getChildren()) { elem->accept(*this); }
}

auto Codegen::visit(const VarDecl *node) -> void {
    lesma::Type *type = nullptr;
    std::unique_ptr<lesma::Value> val;

    // TODO: We shouldn't need to use this
    bool isClass = false;

    if (node->getValue() != nullptr) {
        node->getValue()->accept(*this);
        val = std::move(result_);
        type = val->getType();
    }

    if (node->getType() != nullptr) {
        node->getType()->accept(*this);
        type = result_->getType();
    }

    auto *ptr = Builder_->CreateAlloca(type->getLLVMType(), nullptr, node->getIdentifier()->getValue());

    if (type->is(BaseType::TY_CLASS)) {
        // Cache the pointer type to prevent dangling pointers
        type = cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), type));
        isClass = true;
    }
    auto symbol =
        std::make_unique<Value>(node->getIdentifier()->getValue(), type,
                                node->getType() != nullptr ? SymbolState::INITIALIZED : SymbolState::DECLARED);
    symbol->setLLVMValue(ptr);
    symbol->setMutable(node->getMutability());
    Scope_->insertSymbol(std::move(symbol));

    // Convert declared value to declared type implicitly
    if (node->getValue() != nullptr) {
        auto castVal = cast(node->getSpan(), val.get(), isClass ? type->getElementType() : type);
        Builder_->CreateStore(castVal->getLLVMValue(), ptr);
    }
}

auto Codegen::visit(const If *node) -> void {
    auto *parentFct = Builder_->GetInsertBlock()->getParent();
    auto *bStart = llvm::BasicBlock::Create(TheModule_->getContext(), "if.start");
    auto *bEnd = llvm::BasicBlock::Create(TheModule_->getContext(), "if.end");

    Builder_->CreateBr(bStart);
    bStart->insertInto(parentFct);
    Builder_->SetInsertPoint(bStart);

    for (unsigned long i = 0; i < node->getConds().size(); i++) {
        auto *bIfTrue = llvm::BasicBlock::Create(TheModule_->getContext(), "if.true");
        bIfTrue->insertInto(parentFct);
        auto *bIfFalse = bEnd;
        if (i + 1 < node->getConds().size()) {
            bIfFalse = llvm::BasicBlock::Create(TheModule_->getContext(), "if.false");
            bIfFalse->insertInto(parentFct);
        }

        node->getConds().at(i)->accept(*this);
        Builder_->CreateCondBr(result_->getLLVMValue(), bIfTrue, bIfFalse);
        Builder_->SetInsertPoint(bIfTrue);

        Scope_ = Scope_->createChildBlock("if");
        node->getBlocks().at(i)->accept(*this);

        // TODO: Really slow and hacky way to check if there was a return in block
        bool returned = false;
        for (auto *stat: node->getBlocks().at(i)->getChildren()) {
            if (dynamic_cast<Return *>(stat) != nullptr) {
                returned = true;
            }
        }

        if (!isBreak_ && !returned) {
            Builder_->CreateBr(bEnd);
        }

        Scope_ = Scope_->getParent();
        Builder_->SetInsertPoint(bIfFalse);
    }

    bEnd->insertInto(parentFct);

    if (!isBreak_) {
        Builder_->SetInsertPoint(bEnd);
    } else {
        isBreak_ = false;
    }
}

auto Codegen::visit(const While *node) -> void {
    Scope_ = Scope_->createChildBlock("while");

    llvm::Function *parentFct = Builder_->GetInsertBlock()->getParent();

    // Create blocks
    llvm::BasicBlock *bCond = llvm::BasicBlock::Create(TheModule_->getContext(), "while.cond");
    llvm::BasicBlock *bLoop = llvm::BasicBlock::Create(TheModule_->getContext(), "while");
    llvm::BasicBlock *bEnd = llvm::BasicBlock::Create(TheModule_->getContext(), "while.end");

    breakBlocks_.push(bEnd);
    continueBlocks_.push(bCond);

    // Jump into condition block
    Builder_->CreateBr(bCond);

    // Fill condition block
    bCond->insertInto(parentFct);
    Builder_->SetInsertPoint(bCond);
    node->getCond()->accept(*this);
    Builder_->CreateCondBr(result_->getLLVMValue(), bLoop, bEnd);

    // Fill while body block
    bLoop->insertInto(parentFct);
    Builder_->SetInsertPoint(bLoop);
    node->getBlock()->accept(*this);

    if (!isBreak_) {
        Builder_->CreateBr(bCond);
    } else {
        isBreak_ = false;
    }

    // Fill loop end block
    bEnd->insertInto(parentFct);
    Builder_->SetInsertPoint(bEnd);

    Scope_ = Scope_->getParent();
    breakBlocks_.pop();
    continueBlocks_.pop();
}

auto Codegen::visit(const FuncDecl *node) -> void {
    if (selfSymbol_ != nullptr && node->getName() == "new" &&
        node->getReturnType()->getType() != TokenType::VOID_TYPE) {
        throw CodegenError(node->getSpan(), "Cannot create class method new with return type {}",
                           node->getReturnType()->getName());
    }

    std::vector<std::unique_ptr<Field>> fields;
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Type *> paramLLVMTypes;
    bool shouldExport = node->isExported();

    if (selfSymbol_ != nullptr) {
        paramTypes.push_back(selfSymbol_->getType());
        paramLLVMTypes.push_back(Builder_->getPtrTy());
        fields.push_back(std::make_unique<Field>("self", selfSymbol_->getType()));
        shouldExport = selfSymbol_->isExported();
    }

    for (auto *param: node->getParameters()) {
        std::unique_ptr<lesma::Value> typeResult;
        std::unique_ptr<lesma::Value> defaultValResult;

        // Check if it has either a type or a value or both
        if (param->type) {
            param->type->accept(*this);
            typeResult = std::move(result_);
        }
        if (param->default_val) {
            param->default_val->accept(*this);
            defaultValResult = std::move(result_);
            if (!typeResult) {
                // Default value determines type - make a copy
                typeResult = std::make_unique<Value>(*defaultValResult);
            }
        }

        // If it's a class type, we mean to pass a pointer to a class
        if (typeResult->getType()->is(BaseType::TY_CLASS)) {
            // Cache the pointer type to prevent dangling pointers
            auto *ptrType =
                cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), typeResult->getType()));
            typeResult = std::make_unique<Value>("", ptrType);
        }

        if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
            throw CodegenError(node->getSpan(), "Declared parameter type and default value do not match for {}",
                               param->name);
        }

        paramTypes.push_back(typeResult->getType());
        paramLLVMTypes.push_back(typeResult->getType()->getLLVMType());
        fields.push_back(std::make_unique<Field>(param->name, typeResult->getType(), std::move(defaultValResult)));
    }

    auto mangledName = getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol_ != nullptr);
    auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

    node->getReturnType()->accept(*this);

    llvm::FunctionType *funcType =
        FunctionType::get(result_->getType()->getLLVMType(), paramLLVMTypes, node->getVarArgs());
    Function *f = Function::Create(funcType, linkage, mangledName, *TheModule_);

    auto funcSymbol = std::make_unique<Value>(
        node->getName(), std::make_unique<Type>(BaseType::TY_FUNCTION, funcType, std::move(fields)), f);
    funcSymbol->getType()->setReturnType(result_->getType());
    funcSymbol->setExported(node->isExported());
    funcSymbol->setMangledName(mangledName);
    auto *funcSymbolPtr = funcSymbol.get();
    Scope_->insertSymbol(std::move(funcSymbol));

    Prototypes_.emplace_back(funcSymbolPtr, node, selfSymbol_);
    // Even though it's a statement, we pass the func symbol to result so parent classes can modify them
    result_ = std::make_unique<Value>(*funcSymbolPtr);
}

auto Codegen::visit(const ExternFuncDecl *node) -> void {
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Type *> paramLLVMTypes;

    for (auto *param: node->getParameters()) {
        std::unique_ptr<lesma::Value> typeResult;
        std::unique_ptr<lesma::Value> defaultValResult;

        // Check if it has either a type or a value or both
        if (param->type) {
            param->type->accept(*this);
            typeResult = std::move(result_);
        }
        if (param->default_val) {
            param->default_val->accept(*this);
            defaultValResult = std::move(result_);
            if (!typeResult) {
                // Default value determines type - make a copy
                typeResult = std::make_unique<Value>(*defaultValResult);
            }
        }

        // If it's a class type, we mean to pass a pointer to a class
        if (typeResult->getType()->is(BaseType::TY_CLASS)) {
            // Cache the pointer type to prevent dangling pointers
            auto *ptrType =
                cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), typeResult->getType()));
            typeResult = std::make_unique<Value>("", ptrType);
        }

        if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
            throw CodegenError(node->getSpan(), "Declared parameter type and default value do not match for {}",
                               param->name);
        }

        paramTypes.push_back(typeResult->getType());
        paramLLVMTypes.push_back(typeResult->getType()->getLLVMType());
        fields.push_back(std::make_unique<Field>(param->name, typeResult->getType(), std::move(defaultValResult)));
    }

    node->getReturnType()->accept(*this);
    lesma::Type *retType = nullptr;
    if (!result_) {
        retType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, Builder_->getVoidTy()));
    } else {
        retType = result_->getType();
    }

    Function *f = nullptr;
    if (TheModule_->getFunction(node->getName()) != nullptr &&
        Scope_->lookupFunction(node->getName(), paramTypes) != nullptr) {
        return;
    }

    if (TheModule_->getFunction(node->getName()) != nullptr) {
        f = TheModule_->getFunction(node->getName());
    } else {
        FunctionType *ft = FunctionType::get(retType->getLLVMType(), paramLLVMTypes, node->getVarArgs());
        f = llvm::cast<Function>(TheModule_->getOrInsertFunction(node->getName(), ft).getCallee());
        if (node->isExported()) {
            f->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    }

    auto funcSymbol = std::make_unique<Value>(
        node->getName(), std::make_unique<Type>(BaseType::TY_FUNCTION, f->getFunctionType(), std::move(fields)), f);
    funcSymbol->getType()->setReturnType(retType);
    funcSymbol->setExported(node->isExported());
    funcSymbol->setMangledName(node->getName());
    Scope_->insertSymbol(std::move(funcSymbol));
}

auto Codegen::visit(const Assignment *node) -> void {
    lesma::Value *lhs = nullptr;
    std::unique_ptr<lesma::Value> lhsOwner;  // Holds owned lhs if from result_
    isAssignment_ = true;
    bool isPtr = false;
    if (dynamic_cast<Literal *>(node->getLeftHandSide()) != nullptr) {
        auto *lit = dynamic_cast<Literal *>(node->getLeftHandSide());
        auto *symbol = Scope_->lookup(lit->getValue());
        if (symbol == nullptr) {
            throw CodegenError(node->getSpan(), "Variable not found: {}", lit->getValue());
        }
        if (!symbol->getMutability()) {
            throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");
        }

        lhs = symbol;
    } else if (dynamic_cast<DotOp *>(node->getLeftHandSide()) != nullptr) {
        node->getLeftHandSide()->accept(*this);
        lhsOwner = std::move(result_);
        lhs = lhsOwner.get();
        //TODO: Fix me, for some reason self.x is a ptr but x is not
        isPtr = true;
    } else {
        throw CodegenError(node->getSpan(), "Unable to assign {} to {}",
                           node->getRightHandSide()->toString(SourceManager_.get(), "", true),
                           node->getLeftHandSide()->toString(SourceManager_.get(), "", true));
    }
    isAssignment_ = false;

    node->getRightHandSide()->accept(*this);
    auto value = cast(node->getSpan(), result_.get(), isPtr ? lhs->getType()->getElementType() : lhs->getType());
    llvm::Value *varVal = nullptr;

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            Builder_->CreateStore(value->getLLVMValue(), lhs->getLLVMValue());
            break;
        case TokenType::PLUS_EQUAL:
            varVal = Builder_->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(BaseType::TY_FLOAT)) {
                auto *newVal = Builder_->CreateFAdd(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else if (lhs->getType()->is(BaseType::TY_INT)) {
                auto *newVal = Builder_->CreateAdd(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else {
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            }
            break;
        case TokenType::MINUS_EQUAL:
            varVal = Builder_->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(BaseType::TY_FLOAT)) {
                auto *newVal = Builder_->CreateFSub(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else if (lhs->getType()->is(BaseType::TY_INT)) {
                auto *newVal = Builder_->CreateSub(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else {
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            }
            break;
        case TokenType::SLASH_EQUAL:
            varVal = Builder_->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(BaseType::TY_FLOAT)) {
                auto *newVal = Builder_->CreateFDiv(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else if (lhs->getType()->is(BaseType::TY_INT)) {
                auto *newVal = Builder_->CreateSDiv(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else {
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            }
            break;
        case TokenType::STAR_EQUAL:
            varVal = Builder_->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(BaseType::TY_FLOAT)) {
                auto *newVal = Builder_->CreateFMul(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else if (lhs->getType()->is(BaseType::TY_INT)) {
                auto *newVal = Builder_->CreateMul(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else {
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            }
            break;
        case TokenType::MOD_EQUAL:
            varVal = Builder_->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(BaseType::TY_FLOAT)) {
                auto *newVal = Builder_->CreateFRem(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else if (lhs->getType()->is(BaseType::TY_INT)) {
                auto *newVal = Builder_->CreateSRem(value->getLLVMValue(), varVal);
                Builder_->CreateStore(newVal, lhs->getLLVMValue());
            } else {
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            }
            break;
        case TokenType::POWER_EQUAL:
            throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
        default:
            throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
    }
}

auto Codegen::visit(const Break *node) -> void {
    if (breakBlocks_.empty()) {
        throw CodegenError(node->getSpan(), "Cannot break without being in a loop");
    }

    auto *block = breakBlocks_.top();
    isBreak_ = true;

    Builder_->CreateBr(block);
}

auto Codegen::visit(const Continue *node) -> void {
    if (continueBlocks_.empty()) {
        throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");
    }

    auto *block = continueBlocks_.top();
    isBreak_ = true;

    Builder_->CreateBr(block);
}

auto Codegen::visit(const Return *node) -> void {
    // Check if it's top-level
    if (Builder_->GetInsertBlock()->getParent() == TopLevelFunc_) {
        throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");
    }

    // Execute all deferred statements
    for (auto *inst: deferStack_.top()) { inst->accept(*this); }

    isReturn_ = true;

    if (node->getValue() == nullptr) {
        if (currentFunction_->getType()->getReturnType()->is(BaseType::TY_VOID)) {
            Builder_->CreateRetVoid();
        } else {
            throw CodegenError(node->getSpan(),
                               "Return type does not match the function return type, expected {}, actual void",
                               currentFunction_->getType()->getReturnType()->toString());
        }
    } else {
        node->getValue()->accept(*this);
        if (Builder_->getCurrentFunctionReturnType() == result_->getType()->getLLVMType()) {
            Builder_->CreateRet(result_->getLLVMValue());
        } else {
            throw CodegenError(
                node->getSpan(), "Return type does not match the function return type, expected {}, actual {}",
                currentFunction_->getType()->getReturnType()->toString(), result_->getType()->toString());
        }
    }
}

auto Codegen::visit(const Defer *node) -> void { deferStack_.top().push_back(node->getStatement()); }

auto Codegen::visit(const ExpressionStatement *node) -> void { node->getExpression()->accept(*this); }

auto Codegen::visit(const Import *node) -> void {
    compileModule(node->getSpan(), node->getFilePath(), node->isStd(), node->getAlias(), node->getImportAll(),
                  node->getImportScope(), node->getImportedNames());
}

auto Codegen::visit(const Class *node) -> void {
    std::vector<std::unique_ptr<Field>> fields;
    std::vector<llvm::Type *> elementLLVMTypes;

    for (auto *field: node->getFields()) {
        if (field->getType() != nullptr) {
            field->getType()->accept(*this);
        } else {
            field->getValue()->accept(*this);
        }

        elementLLVMTypes.push_back(result_->getType()->getLLVMType());
        std::unique_ptr<Value> defaultVal;
        if (field->getValue() != nullptr) {
            defaultVal = std::move(result_);
            // Re-evaluate to get the type for the Field (result_ was moved)
            if (field->getType() != nullptr) {
                field->getType()->accept(*this);
            } else {
                // Make a copy of the default value to get its type
                result_ = std::make_unique<Value>(*defaultVal);
            }
        }
        fields.push_back(
            std::make_unique<Field>(field->getIdentifier()->getValue(), result_->getType(), std::move(defaultVal)));
    }

    llvm::StructType *structType =
        llvm::StructType::create(TheModule_->getContext(), elementLLVMTypes, node->getIdentifier());

    auto type = std::make_unique<Type>(BaseType::TY_CLASS, structType, std::move(fields));
    auto *typePtr = type.get();
    auto structSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
    structSymbol->setExported(node->isExported());
    auto *structSymbolPtr = structSymbol.get();

    Scope_->insertType(node->getIdentifier(), std::move(type));
    Scope_->insertSymbol(std::move(structSymbol));

    // Cache the self type to prevent dangling pointers in function Fields
    auto *selfType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), typePtr));
    auto classSelfSymbol = std::make_unique<Value>(node->getIdentifier(), selfType);
    classSelfSymbol->setExported(node->isExported());
    selfSymbol_ = classSelfSymbol.get();
    auto hasConstructor = false;
    for (auto *func: node->getMethods()) {
        func->accept(*this);
        if (func->getName() == "new") {
            hasConstructor = true;
            // result_ contains a copy of the func symbol; actual symbol is in SymbolTable
            // Look up the actual constructor from scope - needs self type as first param
            std::vector<lesma::Type *> constructorParams = {selfSymbol_->getType()};
            auto *constructor = Scope_->lookupFunction("new", constructorParams);
            structSymbolPtr->setConstructor(constructor);
        }
    }

    if (!hasConstructor) {
        throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());
    }

    selfSymbol_ = nullptr;
    // classSelfSymbol unique_ptr goes out of scope and properly deletes the Value
}

auto Codegen::visit(const Enum *node) -> void {
    std::vector<llvm::Type *> elementTypes = {Builder_->getInt8Ty()};
    llvm::StructType *structType =
        llvm::StructType::create(TheModule_->getContext(), elementTypes, node->getIdentifier());
    std::vector<std::unique_ptr<Field>> fields;

    for (const auto &field: node->getValues()) {
        // Cache enum field types to prevent dangling pointers
        auto *fieldType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, Builder_->getVoidTy()));
        fields.push_back(std::make_unique<Field>(field, fieldType));
    }

    auto type = std::make_unique<Type>(BaseType::TY_ENUM, structType, std::move(fields));
    auto *typePtr = type.get();
    auto structSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
    structSymbol->setExported(node->isExported());

    Scope_->insertType(node->getIdentifier(), std::move(type));
    Scope_->insertSymbol(std::move(structSymbol));
}

auto Codegen::visit(const FuncCall *node) -> void { result_ = genFuncCall(node, {}); }

auto Codegen::visit(const BinaryOp *node) -> void {
    node->getLeft()->accept(*this);
    auto left = std::move(result_);
    node->getRight()->accept(*this);
    auto right = std::move(result_);
    lesma::Type *finalType = getExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = cast(node->getSpan(), left.get(), finalType);
            right = cast(node->getSpan(), right.get(), finalType);

            if (finalType == nullptr) {
                break;
            }

            if (finalType->is(BaseType::TY_FLOAT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateFSub(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateSub(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateFAdd(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateAdd(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateFMul(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateMul(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateFDiv(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateSDiv(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateFRem(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>("", finalType,
                                                  Builder_->CreateSRem(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            break;
        case TokenType::POWER:
            if (finalType == nullptr) {
                break;
            }

            if (!right->getType()->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
                throw CodegenError(node->getSpan(), "Cannot use non-numbers for power coefficient: {}",
                                   node->getRight()->toString(SourceManager_.get(), "", true));
            }

            throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
        case TokenType::EQUAL_EQUAL:
            left = cast(node->getSpan(), left.get(), finalType);
            right = cast(node->getSpan(), right.get(), finalType);

            // Enum comparison
            if (finalType->is(BaseType::TY_ENUM)) {
                // Both are pointers to structs
                auto leftName = left->getType()->getLLVMType()->getStructName().str();
                auto rightName = right->getType()->getLLVMType()->getStructName().str();

                if (leftName != rightName) {
                    throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                                       leftName, rightName);
                }

                llvm::Value *leftVal = Builder_->CreateExtractValue(left->getLLVMValue(), {0});
                llvm::Value *rightVal = Builder_->CreateExtractValue(right->getLLVMValue(), {0});
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpEQ(leftVal, rightVal));
                return;
            }

            if (finalType->is(BaseType::TY_PTR)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType == nullptr) {
                break;
            }

            if (finalType->is(BaseType::TY_FLOAT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpOEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            break;
        case TokenType::BANG_EQUAL:
            left = cast(node->getSpan(), left.get(), finalType);
            right = cast(node->getSpan(), right.get(), finalType);

            // Enum comparison
            if (finalType->is(BaseType::TY_ENUM)) {
                // Both are pointers to structs
                auto leftName = left->getType()->getLLVMType()->getStructName().str();
                auto rightName = right->getType()->getLLVMType()->getStructName().str();

                if (leftName != rightName) {
                    throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                                       leftName, rightName);
                }

                llvm::Value *leftVal = Builder_->CreateExtractValue(left->getLLVMValue(), {0});
                llvm::Value *rightVal = Builder_->CreateExtractValue(right->getLLVMValue(), {0});
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpNE(leftVal, rightVal));
                return;
            }

            if (finalType->is(BaseType::TY_PTR)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpNE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_FLOAT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpONE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpNE(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpOGT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpSGT(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpOGE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpSGE(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpOLT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpSLT(left->getLLVMValue(), right->getLLVMValue()));
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
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateFCmpOLE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            if (finalType->is(BaseType::TY_INT)) {
                result_ = std::make_unique<Value>(
                    "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                    Builder_->CreateICmpSLE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }

            break;
        case TokenType::AND:
            if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                                   node->getLeft()->toString(SourceManager_.get(), "", true),
                                   node->getRight()->toString(SourceManager_.get(), "", true));
            }

            result_ =
                std::make_unique<Value>("", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                                        Builder_->CreateLogicalAnd(left->getLLVMValue(), right->getLLVMValue()));
            return;
        case TokenType::OR:
            if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                                   node->getLeft()->toString(SourceManager_.get(), "", true),
                                   node->getRight()->toString(SourceManager_.get(), "", true));
            }

            result_ =
                std::make_unique<Value>("", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())),
                                        Builder_->CreateLogicalOr(left->getLLVMValue(), right->getLLVMValue()));
            return;
        default:
            throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}", NAMEOF_ENUM(node->getOperator()));
    }

    throw CodegenError(node->getSpan(), "Unimplemented binary operator {} for {} and {}",
                       NAMEOF_ENUM(node->getOperator()), node->getLeft()->toString(SourceManager_.get(), "", true),
                       node->getRight()->toString(SourceManager_.get(), "", true));
}

auto Codegen::visit(const DotOp *node) -> void {
    if (auto *left = dynamic_cast<Literal *>(node->getLeft())) {
        if (left->getType() != TokenType::IDENTIFIER) {
            throw CodegenError(node->getLeft()->getSpan(), "Expected identifier left-hand of dot operator, found {}",
                               node->getRight()->toString(SourceManager_.get(), "", true));
        }

        auto *typeSym = Scope_->lookupType(left->getValue());
        if (typeSym != nullptr) {
            // Assuming it's an enum or statically accessed class
            if (!typeSym->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS, BaseType::TY_IMPORT})) {
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());
            }

            auto *right = dynamic_cast<Literal *>(node->getRight());
            if (typeSym->is(BaseType::TY_ENUM)) {
                // Check if right-hand expression is an identifier expression
                if (dynamic_cast<Literal *>(node->getRight()) == nullptr) {
                    throw CodegenError(node->getRight()->getSpan(),
                                       "Expected identifier right-hand of dot operator, found {}",
                                       node->getRight()->toString(SourceManager_.get(), "", true));
                }

                if (right->getType() != TokenType::IDENTIFIER) {
                    throw CodegenError(node->getRight()->getSpan(),
                                       "Expected identifier right-hand of dot operator, found {}",
                                       node->getRight()->toString(SourceManager_.get(), "", true));
                }

                // Setting value to the enum
                auto val = findIndexInFields(typeSym, right->getValue());
                // Field not found in enum
                if (val == -1) {
                    throw CodegenError(node->getLeft()->getSpan(), "Identifier {} not in {}", right->getValue(),
                                       left->getValue());
                }

                auto *structVal = Scope_->lookupStruct(left->getValue());
                auto *enumPtr = Builder_->CreateAlloca(structVal->getType()->getLLVMType());
                auto *field = Builder_->CreateStructGEP(structVal->getType()->getLLVMType(), enumPtr, 0);
                Builder_->CreateStore(Builder_->getInt8(val), field);
                // TODO: Returning the enum directly or a ptr to it? We used to return a pointer
                auto *enumVal = Builder_->CreateLoad(structVal->getType()->getLLVMType(), enumPtr);

                result_ = std::make_unique<Value>("", structVal->getType(), enumVal);
                return;
            }

            if (typeSym->is(BaseType::TY_IMPORT)) {
                std::string field;
                FuncCall *method = nullptr;

                if ((dynamic_cast<Literal *>(node->getRight()) == nullptr) &&
                    (dynamic_cast<FuncCall *>(node->getRight()) == nullptr)) {
                    throw CodegenError(node->getRight()->getSpan(),
                                       "Expected identifier or method call right-hand of dot operator, found {}",
                                       node->getRight()->toString(SourceManager_.get(), "", true));
                }

                if ((dynamic_cast<Literal *>(node->getRight()) != nullptr) &&
                    dynamic_cast<Literal *>(node->getRight())->getType() == TokenType::IDENTIFIER) {
                    field = dynamic_cast<Literal *>(node->getRight())->getValue();
                } else {
                    method = dynamic_cast<FuncCall *>(node->getRight());
                }

                if (method != nullptr) {
                    auto tmpAlias = alias_;
                    alias_ = left->getValue();
                    result_ = genFuncCall(method, {});
                    alias_ = tmpAlias;
                    return;
                }
            }
        } else {
            // Assuming it's a class instance
            left->accept(*this);
            // We refer to the class type, if it's a pointer, we get the result
            lesma::Type *lesmaType = result_->getType();
            if (result_->getType()->is(BaseType::TY_PTR) &&
                result_->getType()->getElementType()->is(BaseType::TY_CLASS)) {
                lesmaType = result_->getType()->getElementType();
            }

            if (!lesmaType->is(BaseType::TY_CLASS)) {
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());
            }

            std::string field;
            FuncCall *method = nullptr;

            if ((dynamic_cast<Literal *>(node->getRight()) == nullptr) &&
                (dynamic_cast<FuncCall *>(node->getRight()) == nullptr)) {
                throw CodegenError(node->getRight()->getSpan(),
                                   "Expected identifier or method call right-hand of dot operator, found {}",
                                   node->getRight()->toString(SourceManager_.get(), "", true));
            }

            if ((dynamic_cast<Literal *>(node->getRight()) != nullptr) &&
                TokenType::IDENTIFIER == dynamic_cast<Literal *>(node->getRight())->getType()) {
                field = dynamic_cast<Literal *>(node->getRight())->getValue();
            } else {
                method = dynamic_cast<FuncCall *>(node->getRight());
            }

            // TODO: Somehow, when we call a class method with a variable x,
            //  we lose the class name from cls, so we set it again
            auto *cls = Scope_->lookupStruct(lesmaType->getLLVMType()->getStructName().str());
            cls->setName(lesmaType->getLLVMType()->getStructName().str());

            if (cls->getType()->is(BaseType::TY_CLASS)) {
                if (!field.empty()) {
                    auto index = findIndexInFields(cls->getType(), field);
                    auto *type = findTypeInFields(cls->getType(), field);
                    if (index == -1) {
                        throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field,
                                           result_->getType()->getElementType()->getLLVMType()->getStructName().str());
                    }

                    auto *ptr =
                        Builder_->CreateStructGEP(cls->getType()->getLLVMType(), result_->getLLVMValue(), index);
                    if (isAssignment_) {
                        result_ = std::make_unique<Value>(
                            "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), type)), ptr);
                        return;
                    }
                    //                    auto &x = cls->getType()->getFields()[index];
                    result_ = std::make_unique<Value>("", type, Builder_->CreateLoad(type->getLLVMType(), ptr));
                    return;
                }
                if (method != nullptr) {
                    selfSymbol_ = cls;
                    result_ = genFuncCall(method, {result_.get()});
                    selfSymbol_ = nullptr;
                    return;
                }
            } else {
                throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}",
                                   lesmaType->getLLVMType()->getStructName().str());
            }
        }
    }
    throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}",
                       node->toString(SourceManager_.get(), "", true));
}

auto Codegen::visit(const CastOp *node) -> void {
    node->getExpression()->accept(*this);
    auto expr = std::move(result_);
    node->getType()->accept(*this);
    auto *castType = result_->getType();
    result_ = cast(node->getSpan(), expr.get(), castType);
}

auto Codegen::visit(const IsOp *node) -> void {
    node->getLeft()->accept(*this);
    auto *leftType = result_->getType();
    node->getRight()->accept(*this);
    auto *rightType = result_->getType();

    llvm::Value *val = nullptr;

    if (node->getOperator() == TokenType::IS) {
        val = leftType->isEqual(rightType) ? Builder_->getTrue() : Builder_->getFalse();
    } else {
        val = leftType->isEqual(rightType) ? Builder_->getFalse() : Builder_->getTrue();
    }

    result_ =
        std::make_unique<Value>("", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty())), val);
}

auto Codegen::visit(const UnaryOp *node) -> void {
    node->getExpression()->accept(*this);

    llvm::Value *val = nullptr;
    lesma::Type *type = result_->getType();
    std::unique_ptr<lesma::Type> ptrTypeHolder;  // Keep owned type alive if created

    if (node->getOperator() == TokenType::MINUS) {
        if (result_->getType()->is(BaseType::TY_INT)) {
            val = Builder_->CreateNeg(result_->getLLVMValue());
        } else if (result_->getType()->is(BaseType::TY_FLOAT)) {
            val = Builder_->CreateFNeg(result_->getLLVMValue());
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()),
                               node->getExpression()->toString(SourceManager_.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::NOT) {
        if (result_->getType()->is(BaseType::TY_BOOL)) {
            val = Builder_->CreateNot(result_->getLLVMValue());
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()),
                               node->getExpression()->toString(SourceManager_.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::STAR) {
        if (result_->getType()->is(BaseType::TY_PTR)) {
            val = Builder_->CreateLoad(result_->getType()->getElementType()->getLLVMType(), result_->getLLVMValue());
            type = result_->getType()->getElementType();
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()),
                               node->getExpression()->toString(SourceManager_.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::AMPERSAND) {
        val = Builder_->CreateAlloca(result_->getType()->getLLVMType());
        ptrTypeHolder = std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), result_->getType());
        type = ptrTypeHolder.get();
        Builder_->CreateStore(result_->getLLVMValue(), val);
    } else {
        throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}",
                           NAMEOF_ENUM(node->getOperator()),
                           node->getExpression()->toString(SourceManager_.get(), "", true));
    }

    // For AMPERSAND case, Value needs to own the Type since ptrTypeHolder will go out of scope
    if (ptrTypeHolder) {
        result_ = std::make_unique<Value>(std::move(ptrTypeHolder));
        result_->setLLVMValue(val);
    } else {
        result_ = std::make_unique<Value>("", type, val);
    }
}

auto Codegen::visit(const Literal *node) -> void {
    // Cache Types to prevent dangling pointers when result_ is reassigned
    if (node->getType() == TokenType::DOUBLE) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, Builder_->getDoubleTy()));
        result_ = std::make_unique<Value>(
            "", type, ConstantFP::get(TheModule_->getContext(), APFloat(std::stod(node->getValue()))));
    } else if (node->getType() == TokenType::INTEGER) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_INT, Builder_->getInt64Ty()));
        result_ = std::make_unique<Value>("", type,
                                          ConstantInt::getSigned(Builder_->getInt64Ty(), std::stoi(node->getValue())));
    } else if (node->getType() == TokenType::BOOL) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty()));
        result_ =
            std::make_unique<Value>("", type, node->getValue() == "true" ? Builder_->getTrue() : Builder_->getFalse());
    } else if (node->getType() == TokenType::STRING) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, Builder_->getPtrTy()));
        result_ = std::make_unique<Value>("", type, Builder_->CreateGlobalString(node->getValue()));
    } else if (node->getType() == TokenType::NIL) {
        auto *type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, Builder_->getVoidTy()));
        result_ = std::make_unique<Value>("", type, ConstantPointerNull::getNullValue(Builder_->getPtrTy()));
    } else if (node->getType() == TokenType::IDENTIFIER) {
        // Look this variable up in the function.
        auto *val = Scope_->lookup(node->getValue());
        if (val == nullptr) {
            throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());
        }

        if (val->getType()->isOneOf({BaseType::TY_CLASS})) {
            // If it's a class, don't load the value - make a copy from symbol table
            result_ = std::make_unique<Value>(*val);
        } else {
            // Load the value.
            llvm::Value *llvmVal = Builder_->CreateLoad(val->getType()->getLLVMType(), val->getLLVMValue());
            result_ = std::make_unique<Value>("", val->getType(), llvmVal);
        }
    } else {
        throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
    }
}

auto Codegen::visit(const Else * /*node*/) -> void {
    auto *type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, Builder_->getInt1Ty()));
    result_ = std::make_unique<Value>("", type, llvm::ConstantInt::getTrue(TheModule_->getContext()));
}

auto Codegen::getTypeMangledName(llvm::SMRange span, lesma::Type *type) -> std::string {
    auto *llvmTy = type->getLLVMType();
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
        for (const auto &field: type->getFields()) { paramStr += getTypeMangledName(span, field->type) + "_"; }
        return "(func_" + paramStr + ")";
    }
    if (type->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
        std::string paramStr;
        for (const auto &field: type->getFields()) { paramStr += getTypeMangledName(span, field->type) + "_"; }
        return "(struct_" + type->getLLVMType()->getStructName().str() + ")";
    }

    throw CodegenError(span, "Unknown type found during mangling");
}

auto Codegen::isMethod(const std::string &mangled_name) -> bool { return mangled_name.find("::") != std::string::npos; }

auto Codegen::getMangledName(llvm::SMRange span, std::string func_name, const std::vector<lesma::Type *> &paramTypes,
                             bool isMethod, std::string module_alias) -> std::string {
    module_alias = module_alias.empty() ? this->alias_ : module_alias;
    std::string name = (module_alias.empty() ? "" : "&" + module_alias + "=>") +
                       (selfSymbol_ != nullptr && isMethod ? selfSymbol_->getName() + "::" + std::move(func_name) + ":"
                                                           : "." + std::move(func_name) + ":");
    bool first = true;

    for (auto *paramType: paramTypes) {
        if (!first) {
            name += ",";
        } else {
            first = false;
        }

        name += getTypeMangledName(span, paramType);
    }

    return name;
}

auto Codegen::isMangled(std::string name) -> bool { return name.find(':') != std::string::npos || name.at(0) == '.'; }

auto Codegen::getDemangledName(const std::string &name) -> std::string {
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

auto Codegen::getExtendedType(lesma::Type *left, lesma::Type *right) -> lesma::Type * {
    if (left->getBaseType() == right->getBaseType()) {
        return left;
    }

    if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
        // TODO: We should ideally only have one int type, but our FFI implementation needs access to all types
        if (left->getLLVMType()->getIntegerBitWidth() > right->getLLVMType()->getIntegerBitWidth()) {
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
        if (left->getLLVMType()->isFP128Ty() || right->getLLVMType()->isFP128Ty()) {
            return left->getLLVMType()->isFP128Ty() ? left : right;
        }
        if (left->getLLVMType()->isDoubleTy() || right->getLLVMType()->isDoubleTy()) {
            return left->getLLVMType()->isDoubleTy() ? left : right;
        }
        if (left->getLLVMType()->isFloatTy() || right->getLLVMType()->isFloatTy()) {
            return left->getLLVMType()->isFloatTy() ? left : right;
        }
        if (left->getLLVMType()->isHalfTy() || right->getLLVMType()->isHalfTy()) {
            return left->getLLVMType()->isHalfTy() ? left : right;
        }
    }
    return nullptr;
}

auto Codegen::cast(llvm::SMRange span, lesma::Value *val, lesma::Type *type) -> std::unique_ptr<lesma::Value> {
    if (type == nullptr) {
        return std::make_unique<Value>(*val);  // Copy for borrowed value
    }

    // If they're the same type
    if (val->getType()->isEqual(type)) {
        return std::make_unique<Value>(*val);  // Copy for borrowed value
    }

    if (type->is(BaseType::TY_INT)) {
        if (val->getType()->is(BaseType::TY_FLOAT)) {
            return std::make_unique<Value>("", type, Builder_->CreateFPToSI(val->getLLVMValue(), type->getLLVMType()));
        }
        if (val->getType()->is(BaseType::TY_INT)) {
            return std::make_unique<Value>(
                "", type, Builder_->CreateIntCast(val->getLLVMValue(), type->getLLVMType(), type->isSigned()));
        }
    } else if (type->is(BaseType::TY_FLOAT)) {
        if (val->getType()->is(BaseType::TY_INT)) {
            return std::make_unique<Value>("", type, Builder_->CreateSIToFP(val->getLLVMValue(), type->getLLVMType()));
        }
        if (val->getType()->is(BaseType::TY_FLOAT)) {
            return std::make_unique<Value>("", type, Builder_->CreateFPCast(val->getLLVMValue(), type->getLLVMType()));
        }
    } else if (type->is(BaseType::TY_STRING)) {
        if (val->getType()->is(BaseType::TY_PTR) && (val->getType()->getElementType()->is(BaseType::TY_INT) ||
                                                     val->getType()->getElementType()->is(BaseType::TY_VOID))) {
            return std::make_unique<Value>("", type, Builder_->CreateBitCast(val->getLLVMValue(), type->getLLVMType()));
        }
    }

    throw CodegenError(span, "Unsupported Cast between {} and {}", getTypeMangledName(span, val->getType()),
                       getTypeMangledName(span, type));
}

auto Codegen::genFuncCall(const FuncCall *node, const std::vector<lesma::Value *> &extra_params = {})
    -> std::unique_ptr<lesma::Value> {
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Value *> paramsLLVM;

    for (auto *arg: extra_params) {
        paramTypes.push_back(arg->getType());
        paramsLLVM.push_back(arg->getLLVMValue());
    }

    for (auto *arg: node->getArguments()) {
        arg->accept(*this);
        paramTypes.push_back(result_->getType());
        paramsLLVM.push_back(result_->getLLVMValue());
    }

    Value *symbol = nullptr;
    // Check if it's a constructor like `Classname()`
    auto *selfSymbolTmp = selfSymbol_;
    auto *classSym = Scope_->lookupStruct(node->getName());
    llvm::Value *classPtr = nullptr;
    // Keep the Type alive for the duration of the lookup
    std::unique_ptr<Type> selfParamType;
    if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
        // It's a class constructor, allocate and add self param
        classPtr = Builder_->CreateAlloca(classSym->getType()->getLLVMType());
        paramsLLVM.insert(paramsLLVM.begin(), classPtr);
        selfParamType = std::make_unique<Type>(BaseType::TY_PTR, Builder_->getPtrTy(), classSym->getType());
        paramTypes.insert(paramTypes.begin(), selfParamType.get());

        selfSymbol_ = classSym;
        symbol = Scope_->lookupFunction("new", paramTypes);
    } else {
        symbol = Scope_->lookupFunction(node->getName(), paramTypes);
    }

    if (symbol == nullptr) {
        throw CodegenError(node->getSpan(), "{} {} not in current scope.",
                           classSym != nullptr ? "Constructor for" : "Function", node->getName());
    }

    if (!symbol->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
        throw CodegenError(node->getSpan(), "Symbol {} is not a function or constructor.", node->getName());
    }

    if (symbol->getType()->getFields().size() > paramsLLVM.size()) {
        auto fields = symbol->getType()->getFields();

        for (auto *field: fields) {
            if (field->defaultValue == nullptr) {
                throw CodegenError(node->getSpan(),
                                   "Something bad happened, lookup found a function with incorrect defaults",
                                   node->getName());
            }
            paramsLLVM.push_back(field->defaultValue->getLLVMValue());
        }
    }
    auto *func = llvm::cast<Function>(
        symbol->getType()->is(BaseType::TY_CLASS) ? symbol->getConstructor()->getLLVMValue() : symbol->getLLVMValue());
    if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
        Builder_->CreateCall(func, paramsLLVM);
        selfSymbol_ = selfSymbolTmp;

        return std::make_unique<Value>("", classSym->getType(), classPtr);
    }

    return std::make_unique<Value>("", symbol->getType()->getReturnType(), Builder_->CreateCall(func, paramsLLVM));
}

auto Codegen::findIndexInFields(Type *_struct, const std::string &field) -> int {
    for (unsigned int i = 0; i < _struct->getFields().size(); i++) {
        if (_struct->getFields()[i]->name == field) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

auto Codegen::findTypeInFields(Type *_struct, const std::string &field) -> lesma::Type * {
    for (const auto &i: _struct->getFields()) {
        if (i->name == field) {
            return i->type;
        }
    }

    return nullptr;
}