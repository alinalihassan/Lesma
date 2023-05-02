#include "Codegen.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main, std::string alias, const std::shared_ptr<ThreadSafeContext> &context) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    TheContext = context == nullptr ? std::make_shared<ThreadSafeContext>(std::make_unique<LLVMContext>()) : context;
    TargetMachine = InitializeTargetMachine();
    TheModule = InitializeModule();
    if (jit) {
        TheJIT = InitializeJIT();
    }

    Builder = std::make_unique<IRBuilder<>>(*TheContext->getContext());
    Parser_ = std::move(parser);
    SourceManager = std::move(srcMgr);
    Scope = new SymbolTable(nullptr);

    this->alias = std::move(alias);
    this->filename = filename;
    isMain = main;
    isJIT = jit;

    ImportedModules = std::move(imports);
    TopLevelFunc = InitializeTopLevel();

    // If it's not base.les stdlib, then import it
    if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
        CompileModule(llvm::SMRange(), getStdDir() + "base.les", true, "base", true, true, {});
    }
}

std::unique_ptr<Module> Codegen::InitializeModule() {
    auto mod = std::make_unique<Module>("Lesma", *TheContext->getContext());
    mod->setTargetTriple(TargetMachine->getTargetTriple().str());
    mod->setDataLayout(TargetMachine->createDataLayout());
    mod->setSourceFileName(filename);

    return mod;
}

std::unique_ptr<llvm::TargetMachine> Codegen::InitializeTargetMachine() {
    // Configure output target
    auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    const std::string &tripletString = targetTriple.getTriple();

    // Search after selected target
    std::string error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(tripletString, error);
    if (!target)
        throw CodegenError({}, "Target not available:\n{}", error);

    llvm::TargetOptions opt;
    llvm::Optional<llvm::Reloc::Model> rm(llvm::Reloc::PIC_);
    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(tripletString, "generic", "", opt, rm));
    return target_machine;
}

std::unique_ptr<LLJIT> Codegen::InitializeJIT() {
    llvm::orc::LLJITBuilder builder;
    builder.setDataLayout(TheModule->getDataLayout());
    builder.setJITTargetMachineBuilder(llvm::orc::JITTargetMachineBuilder(TargetMachine->getTargetTriple()));
    auto jit = llvm::cantFail(builder.create());
    if (!jit) {
        throw CodegenError({}, "Couldn't initialize JIT\n");
    }

    // Add support for C native functions
    auto &MainJD = jit->getMainJITDylib();
    auto Err = MainJD.addGenerator(
            cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
                    jit->getDataLayout().getGlobalPrefix())));

    return jit;
}

llvm::Function *Codegen::InitializeTopLevel() {
    std::vector<llvm::Type *> paramTypes = {};

    FunctionType *FT = FunctionType::get(Builder->getInt64Ty(), paramTypes, false);
    Function *F = Function::Create(FT, isMain ? Function::ExternalLinkage : Function::InternalLinkage, "main", *TheModule);

    auto entry = BasicBlock::Create(*TheContext->getContext(), "entry", F);
    Builder->SetInsertPoint(entry);

    return F;
}

void Codegen::defineFunction(lesma::Value *value, const FuncDecl *node, Value *clsSymbol) {
    Scope = Scope->createChildBlock(node->getName());
    currentFunction = value;
    deferStack.emplace();

    auto *F = cast<Function>(value->getLLVMValue());

    BasicBlock *entry = BasicBlock::Create(*TheContext->getContext(), "entry", F);
    Builder->SetInsertPoint(entry);

    int fieldIndex = 0;
    for (auto &field: value->getType()->getFields()) {
        auto param = F->getArg(fieldIndex);

        if (clsSymbol != nullptr && param->getArgNo() == 0)
            param->setName("self");
        else
            param->setName(node->getParameters()[param->getArgNo() - (clsSymbol != nullptr ? 1 : 0)]->name);

        llvm::Value *ptr = Builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
        Builder->CreateStore(param, ptr);

        auto symbol = new Value(field->name, field->type, ptr);
        Scope->insertSymbol(symbol);

        fieldIndex++;
    }

    node->getBody()->accept(*this);

    auto instrs = deferStack.top();
    deferStack.pop();

    if (!isReturn)
        for (auto inst: instrs)
            inst->accept(*this);

    // Check for well-formness of all BBs. In particular, look for
    // any unterminated BB and try to add a Return to it.
    for (BasicBlock &BB: *F) {
        Instruction *Terminator = BB.getTerminator();
        if (Terminator != nullptr) continue;// Well-formed
        if (value->getType()->getReturnType()->is(TY_VOID)) {
            // Make implicit return of void Function explicit.
            Builder->SetInsertPoint(&BB);
            Builder->CreateRetVoid();
        } else
            throw CodegenError(node->getSpan(), "Function {} does not always return a result", node->getName());
    }

    isReturn = false;

    // Verify function
    // TODO: Verify function again, unfortunately functions from other modules have attributes attached without context of usage, and verify gives error
    //    std::string output;
    //    llvm::raw_string_ostream oss(output);
    //    if (llvm::verifyFunction(*F, &oss)) {
    //        F->print(outs());
    //        throw CodegenError(node->getSpan(), "Invalid Function {}\n{}", node->getName(), output);
    //    }

    // Insert Function to Symbol Table
    Scope = Scope->getParent();

    currentFunction = nullptr;

    // Reset Insert Point to Top Level
    Builder->SetInsertPoint(&TopLevelFunc->back());
}

void Codegen::CompileModule(llvm::SMRange span, const std::string &filepath, bool isStd, const std::string &module_alias, bool importAll, bool importToScope, const std::vector<std::pair<std::string, std::string>> &imported_names) {
    std::filesystem::path mainPath = filename;
    // Read source
    auto absolute_path = isStd ? filepath : fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath);

    // If module is already imported, don't compile again
    // TODO: Re-enable this again, currently it destroys nested imports
    //    if (std::find(ImportedModules.begin(), ImportedModules.end(), absolute_path) != ImportedModules.end())
    //        return;

    auto buffer = MemoryBuffer::getFile(absolute_path);
    if (std::error_code ec = buffer.getError())
        throw LesmaError(llvm::SMRange(), "Could not read file: {}", absolute_path);

    auto file_id = SourceManager->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
    auto source_str = SourceManager->getMemoryBuffer(file_id)->getBuffer().str();
    ImportedModules.push_back(absolute_path);

    try {
        // Lexer
        auto lexer = std::make_unique<Lexer>(SourceManager);
        lexer->ScanAll();

        // Parser
        auto parser = std::make_unique<Parser>(lexer->getTokens());
        parser->Parse();

        // TODO: Delete it, memory leak, smart pointer made us lose the references to other modules
        // Codegen
        auto codegen = std::make_unique<Codegen>(std::move(parser), SourceManager, absolute_path, ImportedModules, isJIT, false, !importToScope ? module_alias : "", TheContext);
        codegen->Run();

        // Optimize
        codegen->Optimize(OptimizationLevel::O3);
        codegen->TheModule->setModuleIdentifier(filepath);

        ImportedModules = std::move(codegen->ImportedModules);

        if (!importToScope) {
            auto import_typ = new Type(TY_IMPORT);
            auto import_sym = new Value(module_alias, import_typ);
            Scope->insertSymbol(import_sym);
            Scope->insertType(module_alias, import_typ);
        }

        auto findInImports = [imported_names](const std::string &import) -> std::string {
            for (const auto &imp_pair: imported_names) {
                if (imp_pair.first == import)
                    return imp_pair.second;
            }

            return "";
        };

        if (isJIT) {
            // Add the module to JIT
            cantFail(TheJIT->addIRModule(ThreadSafeModule(std::move(codegen->TheModule), *TheContext)), fmt::format("Failed adding import {} to JIT", filename).c_str());
        } else {
            // Create object file to be linked
            std::string obj_file = fmt::format("tmp{}", ObjectFiles.size());
            codegen->WriteToObjectFile(obj_file);
            ObjectFiles.push_back(fmt::format("{}.o", obj_file));
        }

        // Import Symbols
        for (auto sym: codegen->Scope->getSymbols()) {
            auto imp_alias = findInImports(sym.first);
            if (sym.second->getType()->isOneOf({TY_ENUM, TY_CLASS}) && sym.second->isExported() && (importAll || !imp_alias.empty())) {
                llvm::StructType *structType = StructType::getTypeByName(*TheContext->getContext(), sym.first);

                auto *structSymbol = new Value(imp_alias.empty() ? sym.first : imp_alias, sym.second->getType());
                structSymbol->getType()->setLLVMType(structType);
                Scope->insertType(sym.first, sym.second->getType());
                Scope->insertSymbol(structSymbol);
            } else if (sym.second->getType()->is(TY_FUNCTION) && sym.second->isExported()) {
                auto *F = llvm::dyn_cast<Function>(sym.second->getLLVMValue());
                auto *FTy = llvm::cast<FunctionType>(sym.second->getType()->getLLVMType());

                if (isJIT) {
                    // Insert the function declaration, since we linked the modules earlier
                    F = llvm::cast<Function>(TheModule->getOrInsertFunction(sym.second->getMangledName(), FTy).getCallee());
                }

                auto name = std::string{sym.first};
                std::vector<lesma::Type *> paramTypes;
                for (auto field: sym.second->getType()->getFields()) {
                    paramTypes.push_back(field->type);
                }

                Value *func_symbol = codegen->Scope->lookupFunction(name, paramTypes);

                // Only import if it's exported
                imp_alias = findInImports(name);
                // TODO: methods should only be imported if they class is in the imports specified
                if (func_symbol != nullptr && func_symbol->isExported() && (importAll || !imp_alias.empty() || isMethod(sym.second->getMangledName()))) {
                    auto symbol = new Value(imp_alias.empty() ? name : std::regex_replace(name, std::regex(name), imp_alias), func_symbol->getType());

                    if (isJIT) {
                        symbol->getType()->setLLVMType(FTy);
                        symbol->setLLVMValue(F);
                        symbol->setExported(false);
                        symbol->setMangledName(sym.second->getMangledName());
                    } else {
                        // If it's compiled, we need to make a new Function declaration in the importing file
                        auto new_func = Function::Create(FTy, Function::ExternalLinkage, sym.second->getMangledName(), *TheModule);
                        symbol->getType()->setLLVMType(new_func->getFunctionType());
                        symbol->setLLVMValue(new_func);
                        symbol->setExported(false);
                        symbol->setMangledName(sym.second->getMangledName());
                    }

                    Scope->insertSymbol(symbol);
                }
            }
        }
    } catch (const LesmaError &err) {
        if (!err.getSpan().isValid())
            print(ERROR, err.what());
        else
            showInline(SourceManager.get(), file_id, err.getSpan(), err.what(), absolute_path, true);

        throw CodegenError(span, "Unable to import {} due to errors", filepath);
    }
}

void Codegen::Optimize(OptimizationLevel opt) {
    if (opt == OptimizationLevel::O0)
        return;

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(&*TargetMachine);

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // Add custom passes to LoopPassManager
    llvm::LoopPassManager LPM;
    LPM.addPass(llvm::LoopFullUnrollPass());

    // Add custom passes to FunctionPassManager
    llvm::FunctionPassManager FPM;
    FPM.addPass(llvm::ADCEPass());
    FPM.addPass(llvm::GVNPass());
    FPM.addPass(llvm::DSEPass());
    FPM.addPass(llvm::LoopVectorizePass());
    FPM.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(LPM)));

    // Add custom passes to CGSCCPassManager
    llvm::CGSCCPassManager CGPM;
    CGPM.addPass(llvm::InlinerPass());

    // Add custom pass managers to ModulePassManager
    llvm::ModulePassManager MPM = PB.buildModuleOptimizationPipeline(opt, ThinOrFullLTOPhase::FullLTOPreLink);
    MPM.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(std::move(CGPM)));
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.addPass(llvm::GlobalDCEPass());

    MPM.run(*TheModule, MAM);
}

void Codegen::WriteToObjectFile(const std::string &output) {
    std::error_code err;
    auto out = llvm::raw_fd_ostream(output + ".o", err);

    if (err) {
        throw CodegenError({}, "Error opening file {} for writing: {}", output, err.message());
    }

    llvm::legacy::PassManager passManager;
    if (TargetMachine->addPassesToEmitFile(passManager, out, nullptr, llvm::CGFT_ObjectFile))
        throw CodegenError({}, "Target Machine can't emit an object file");
    // Emit object file
    passManager.run(*TheModule);

    // Flush and close the file
    out.flush();
    out.close();
}

[[maybe_unused]] void Codegen::LinkObjectFileWithLLD(const std::string &obj_filename) {
    std::string output = getBasename(obj_filename);

    llvm::SmallVector<const char *, 32> args;
    args.push_back("lld");
    args.push_back("-o");
    args.push_back(output.c_str());
    args.push_back(obj_filename.c_str());
    for (const auto &obj: ObjectFiles) {
        args.push_back(obj.c_str());
    }
    // Add the standard library path for Apple
#ifdef __APPLE__
    args.push_back("-arch");
    args.push_back("arm64");
    args.push_back("-platform_version");
    args.push_back("macos");// platform
    args.push_back("11.0"); // min version
    args.push_back("11.0"); // sdk version
    args.push_back("-L");
    args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
    args.push_back("-lSystem");
#endif

    // Run the LLD linker
    bool success = false;
#ifdef __APPLE__
    success = lld::macho::link(args, llvm::outs(), llvm::errs(), false, true);
#elif defined(_WIN32)
    success = lld::coff::link(args, llvm::outs(), llvm::errs(), false, true);
#else
    success = lld::elf::link(args, llvm::outs(), llvm::errs(), false, true);
#endif
    if (!success)
        throw CodegenError({}, "Linking Failed");

    // Remove object files
    llvm::sys::fs::remove(obj_filename);
    for (const auto &obj: ObjectFiles)
        llvm::sys::fs::remove(obj);
}

[[maybe_unused]] void Codegen::LinkObjectFileWithClang(const std::string &obj_filename) {
    auto clangPath = llvm::sys::findProgramByName("clang");
    if (clangPath.getError())
        throw CodegenError({}, "Unable to find clang path");

    std::string output = getBasename(obj_filename);

    llvm::SmallVector<const char *, 32> args;
    args.push_back(clangPath.get().c_str());
    args.push_back("-o");
    args.push_back(output.c_str());
    args.push_back(obj_filename.c_str());
    for (const auto &obj: ObjectFiles) {
        args.push_back(obj.c_str());
    }

    // Add the standard library path for Apple
#ifdef __APPLE__
    args.push_back("-L");
    args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
    args.push_back("-lSystem");
#endif

    // Set up the diagnostic engine
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(new clang::DiagnosticIDs());
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diagOpts(new clang::DiagnosticOptions());
    auto *diagClient = new clang::TextDiagnosticPrinter(llvm::errs(), &*diagOpts);
    clang::DiagnosticsEngine Diags(diagIDs, &*diagOpts, diagClient);

    // Create a compilation using Clang's driver
    clang::driver::Driver TheDriver(args[0], TheModule->getTargetTriple(), Diags, "Lesma Compiler", llvm::vfs::getRealFileSystem());
    std::unique_ptr<clang::driver::Compilation> C(TheDriver.BuildCompilation(args));

    if (!C) {
        throw CodegenError({}, "Failed to create clang driver compilation");
    }

    // Run the driver
    llvm::SmallVector<std::pair<int, const clang::driver::Command *>, 8> FailingCommands;
    int Res = TheDriver.ExecuteCompilation(*C, FailingCommands);

    if (Res != 0) {
        throw CodegenError({}, "Linking failed");
    }

    // Remove object files
    llvm::sys::fs::remove(obj_filename);
    for (const auto &obj: ObjectFiles)
        llvm::sys::fs::remove(obj);
}

void Codegen::LinkObjectFile(const std::string &obj_filename) {
    LinkObjectFileWithClang(obj_filename);
}

void Codegen::PrepareJIT() {
    auto jit_error = TheJIT->addIRModule(ThreadSafeModule(std::move(TheModule), *TheContext));
    if (jit_error)
        throw CodegenError({}, "JIT Error:\n{}");
    auto main_func = TheJIT->lookup(TopLevelFunc->getName());
    if (!main_func)
        throw CodegenError({}, "Couldn't find top level function\n");
    mainFuncAddress = jitTargetAddressToFunction<MainFnTy *>(main_func->getValue());
}

int Codegen::ExecuteJIT() {
    if (mainFuncAddress == nullptr) {
        throw CodegenError({}, "Main function address not found, did you prepare JIT?\n");
    }

    return mainFuncAddress();
}

void Codegen::Run() {
    deferStack.emplace();
    Parser_->getAST()->accept(*this);

    auto instrs = deferStack.top();
    deferStack.pop();

    // Visit all statements
    for (auto inst: instrs)
        inst->accept(*this);

    // Define the function bodies
    for (auto prot: Prototypes)
        defineFunction(std::get<0>(prot), std::get<1>(prot), std::get<2>(prot));

    // Return 0 for top-level function
    Builder->CreateRet(ConstantInt::getSigned(Builder->getInt64Ty(), 0));
}

void Codegen::Dump() {
    TheModule->print(outs(), nullptr);
}

void Codegen::visit(const Statement *node) {
    print("Visited a blank statement\n{}", node->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const Expression *node) {
    print("Visited a blank expression\n{}", node->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const TypeExpr *node) {
    if (node->getType() == TokenType::INT_TYPE)
        result = new lesma::Value(new lesma::Type(TY_INT, Builder->getInt64Ty()));
    else if (node->getType() == TokenType::INT8_TYPE)
        result = new lesma::Value(new lesma::Type(TY_INT, Builder->getInt8Ty()));
    else if (node->getType() == TokenType::INT16_TYPE)
        result = new lesma::Value(new lesma::Type(TY_INT, Builder->getInt16Ty()));
    else if (node->getType() == TokenType::INT32_TYPE)
        result = new lesma::Value(new lesma::Type(TY_INT, Builder->getInt32Ty()));
    else if (node->getType() == TokenType::FLOAT_TYPE)
        result = new lesma::Value(new lesma::Type(TY_FLOAT, Builder->getDoubleTy()));
    else if (node->getType() == TokenType::FLOAT32_TYPE)
        result = new lesma::Value(new lesma::Type(TY_FLOAT, Builder->getFloatTy()));
    else if (node->getType() == TokenType::BOOL_TYPE)
        result = new lesma::Value(new lesma::Type(TY_BOOL, Builder->getInt1Ty()));
    else if (node->getType() == TokenType::STRING_TYPE) {
        auto class_sym = Scope->lookupStruct("String");

        if (selfSymbol == nullptr || !selfSymbol->getType()->is(TY_CLASS)) {
            throw CodegenError(node->getSpan(), "Cannot find String class");
        }

        result = new lesma::Value(new Type(TY_PTR, Builder->getPtrTy(), class_sym->getType()));
    } else if (node->getType() == TokenType::VOID_TYPE)
        result = new lesma::Value(new lesma::Type(TY_VOID, Builder->getVoidTy()));
    else if (node->getType() == TokenType::PTR_TYPE) {
        node->getElementType()->accept(*this);
        result = new lesma::Value(new lesma::Type(TY_PTR, Builder->getPtrTy(), result->getType()));
    } else if (node->getType() == TokenType::FUNC_TYPE) {
        node->getReturnType()->accept(*this);
        auto ret_type = result;
        std::vector<Field *> fields;
        std::vector<lesma::Type *> paramTypes;
        std::vector<llvm::Type *> paramLLVMTypes;
        for (auto param_type: node->getParams()) {
            param_type->accept(*this);
            paramLLVMTypes.push_back(result->getType()->getLLVMType());
            paramTypes.push_back(result->getType());
            fields.push_back(new Field{result->getName(), result->getType()});
        }

        llvm::Type *funcType = FunctionType::get(ret_type->getType()->getLLVMType(), paramLLVMTypes, false)->getPointerTo();
        result = new lesma::Value(new lesma::Type(TY_FUNCTION, funcType, std::move(fields)));
    } else if (node->getType() == TokenType::CUSTOM_TYPE) {
        auto typ = Scope->lookupType(node->getName());
        auto sym = Scope->lookupStruct(node->getName());
        if (typ == nullptr || sym->getType()->getLLVMType() == nullptr)
            throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());

        result = sym == nullptr ? new lesma::Value(typ) : sym;
    } else {
        throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
    }
}

void Codegen::visit(const Compound *node) {
    for (auto elem: node->getChildren())
        elem->accept(*this);
}

void Codegen::visit(const VarDecl *node) {
    lesma::Type *type;
    lesma::Value *val;

    // TODO: We shouldn't need to use this
    bool isClass = false;

    if (node->getValue().has_value()) {
        node->getValue().value()->accept(*this);
        val = result;
        type = result->getType();
    }

    if (node->getType().has_value()) {
        node->getType().value()->accept(*this);
        type = result->getType();
    }

    auto ptr = Builder->CreateAlloca(type->getLLVMType(), nullptr, node->getIdentifier()->getValue());

    if (type->is(TY_CLASS)) {
        type = new Type(TY_PTR, Builder->getPtrTy(), type);
        isClass = true;
    }
    auto symbol = new Value(node->getIdentifier()->getValue(), type, node->getType().has_value() ? INITIALIZED : DECLARED);
    symbol->setLLVMValue(ptr);
    symbol->setMutable(node->getMutability());
    Scope->insertSymbol(symbol);

    // Convert declared value to declared type implicitly
    if (node->getValue().has_value()) {
        Builder->CreateStore(Cast(node->getSpan(), val, isClass ? type->getElementType() : type)->getLLVMValue(), ptr);
    }
}

void Codegen::visit(const If *node) {
    auto parentFct = Builder->GetInsertBlock()->getParent();
    auto bStart = llvm::BasicBlock::Create(*TheContext->getContext(), "if.start");
    auto bEnd = llvm::BasicBlock::Create(*TheContext->getContext(), "if.end");

    Builder->CreateBr(bStart);
    bStart->insertInto(parentFct);
    Builder->SetInsertPoint(bStart);

    for (unsigned long i = 0; i < node->getConds().size(); i++) {
        auto bIfTrue = llvm::BasicBlock::Create(*TheContext->getContext(), "if.true");
        bIfTrue->insertInto(parentFct);
        auto bIfFalse = bEnd;
        if (i + 1 < node->getConds().size()) {
            bIfFalse = llvm::BasicBlock::Create(*TheContext->getContext(), "if.false");
            bIfFalse->insertInto(parentFct);
        }

        node->getConds().at(i)->accept(*this);
        Builder->CreateCondBr(result->getLLVMValue(), bIfTrue, bIfFalse);
        Builder->SetInsertPoint(bIfTrue);

        Scope = Scope->createChildBlock("if");
        node->getBlocks().at(i)->accept(*this);

        // TODO: Really slow and hacky way to check if there was a return in block
        bool returned = false;
        for (auto stat: node->getBlocks().at(i)->getChildren())
            if (dynamic_cast<Return *>(stat))
                returned = true;

        if (!isBreak && !returned)
            Builder->CreateBr(bEnd);

        Scope = Scope->getParent();
        Builder->SetInsertPoint(bIfFalse);
    }

    bEnd->insertInto(parentFct);

    if (!isBreak)
        Builder->SetInsertPoint(bEnd);
    else
        isBreak = false;
}

void Codegen::visit(const While *node) {
    Scope = Scope->createChildBlock("while");

    llvm::Function *parentFct = Builder->GetInsertBlock()->getParent();

    // Create blocks
    llvm::BasicBlock *bCond = llvm::BasicBlock::Create(*TheContext->getContext(), "while.cond");
    llvm::BasicBlock *bLoop = llvm::BasicBlock::Create(*TheContext->getContext(), "while");
    llvm::BasicBlock *bEnd = llvm::BasicBlock::Create(*TheContext->getContext(), "while.end");

    breakBlocks.push(bEnd);
    continueBlocks.push(bCond);

    // Jump into condition block
    Builder->CreateBr(bCond);

    // Fill condition block
    bCond->insertInto(parentFct);
    Builder->SetInsertPoint(bCond);
    node->getCond()->accept(*this);
    Builder->CreateCondBr(result->getLLVMValue(), bLoop, bEnd);

    // Fill while body block
    bLoop->insertInto(parentFct);
    Builder->SetInsertPoint(bLoop);
    node->getBlock()->accept(*this);

    if (!isBreak)
        Builder->CreateBr(bCond);
    else
        isBreak = false;

    // Fill loop end block
    bEnd->insertInto(parentFct);
    Builder->SetInsertPoint(bEnd);

    Scope = Scope->getParent();
    breakBlocks.pop();
    continueBlocks.pop();
}

void Codegen::visit(const FuncDecl *node) {
    if (selfSymbol != nullptr && node->getName() == "new" && node->getReturnType()->getType() != TokenType::VOID_TYPE)
        throw CodegenError(node->getSpan(), "Cannot create class method new with return type {}", node->getReturnType()->getName());

    std::vector<Field *> fields;
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Type *> paramLLVMTypes;
    bool shouldExport = node->isExported();

    if (selfSymbol != nullptr) {
        paramTypes.push_back(selfSymbol->getType());
        paramLLVMTypes.push_back(selfSymbol->getType()->getLLVMType()->getPointerTo());
        fields.push_back(new Field{"self", selfSymbol->getType()});
        shouldExport = selfSymbol->isExported();
    }

    for (auto param: node->getParameters()) {
        lesma::Value *typeResult = nullptr;
        lesma::Value *defaultValResult = nullptr;

        // Check if it has either a type or a value or both
        if (param->type) {
            param->type->accept(*this);
            typeResult = result;
        }
        if (param->default_val) {
            param->default_val->accept(*this);
            defaultValResult = result;
            if (!typeResult) {
                typeResult = defaultValResult;
            }
        }

        // If it's a class type, we mean to pass a pointer to a class
        if (typeResult->getType()->is(TY_CLASS)) {
            typeResult = new Value("", new Type(TY_PTR, Builder->getPtrTy(), result->getType()));
        }

        if (defaultValResult != nullptr && !typeResult->getType()->isEqual(defaultValResult->getType())) {
            throw CodegenError(node->getSpan(), "Declared parameter type and default value do not match for {}", param->name);
        }

        paramTypes.push_back(typeResult->getType());
        paramLLVMTypes.push_back(typeResult->getType()->getLLVMType());
        fields.push_back(new Field{param->name, typeResult->getType(), defaultValResult});
    }

    auto mangledName = getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
    auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

    node->getReturnType()->accept(*this);

    llvm::FunctionType *funcType = FunctionType::get(result->getType()->getLLVMType(), paramLLVMTypes, node->getVarArgs());
    Function *F = Function::Create(funcType, linkage, mangledName, *TheModule);

    auto func_symbol = new Value(node->getName(), new Type(BaseType::TY_FUNCTION, funcType, std::move(fields)), F);
    func_symbol->getType()->setReturnType(result->getType());
    func_symbol->setExported(node->isExported());
    func_symbol->setMangledName(mangledName);
    Scope->insertSymbol(func_symbol);

    Prototypes.emplace_back(func_symbol, node, selfSymbol);
    // Even though it's a statement, we pass the func symbol to result so parent classes can modify them
    result = func_symbol;
}

void Codegen::visit(const ExternFuncDecl *node) {
    std::vector<Field *> fields;
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Type *> paramLLVMTypes;

    for (auto param: node->getParameters()) {
        lesma::Value *typeResult = nullptr;
        lesma::Value *defaultValResult = nullptr;

        // Check if it has either a type or a value or both
        if (param->type) {
            param->type->accept(*this);
            typeResult = result;
        }
        if (param->default_val) {
            param->default_val->accept(*this);
            defaultValResult = result;
            if (!typeResult) {
                typeResult = defaultValResult;
            }
        }

        // If it's a class type, we mean to pass a pointer to a class
        if (typeResult->getType()->is(TY_CLASS)) {
            typeResult = new Value("", new Type(TY_PTR, Builder->getPtrTy(), result->getType()));
        }

        if (defaultValResult != nullptr && !typeResult->getType()->isEqual(defaultValResult->getType())) {
            throw CodegenError(node->getSpan(), "Declared parameter type and default value do not match for {}", param->name);
        }

        paramTypes.push_back(typeResult->getType());
        paramLLVMTypes.push_back(typeResult->getType()->getLLVMType());
        fields.push_back(new Field{param->name, typeResult->getType(), defaultValResult});
    }

    node->getReturnType()->accept(*this);
    auto ret_type = result->getType();

    Function *F;
    if (TheModule->getFunction(node->getName()) != nullptr && Scope->lookupFunction(node->getName(), paramTypes) != nullptr)
        return;
    else if (TheModule->getFunction(node->getName()) != nullptr) {
        F = TheModule->getFunction(node->getName());
    } else {
        FunctionType *FT = FunctionType::get(ret_type->getLLVMType(), paramLLVMTypes, node->getVarArgs());
        F = llvm::cast<Function>(TheModule->getOrInsertFunction(node->getName(), FT).getCallee());
        if (node->isExported()) {
            F->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    }

    auto func_symbol = new Value(node->getName(), new Type(BaseType::TY_FUNCTION, F->getFunctionType(), fields), F);
    func_symbol->getType()->setReturnType(ret_type);
    func_symbol->setExported(node->isExported());
    func_symbol->setMangledName(node->getName());
    Scope->insertSymbol(func_symbol);
}

void Codegen::visit(const Assignment *node) {
    lesma::Value *lhs;
    isAssignment = true;
    bool isPtr = false;
    if (dynamic_cast<Literal *>(node->getLeftHandSide())) {
        auto lit = dynamic_cast<Literal *>(node->getLeftHandSide());
        auto symbol = Scope->lookup(lit->getValue());
        if (symbol == nullptr)
            throw CodegenError(node->getSpan(), "Variable not found: {}", lit->getValue());
        if (!symbol->getMutability())
            throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");

        lhs = symbol;
    } else if (dynamic_cast<DotOp *>(node->getLeftHandSide())) {
        node->getLeftHandSide()->accept(*this);
        lhs = result;
        //TODO: Fix me, for some reason self.x is a ptr but x is not
        isPtr = true;
    } else {
        throw CodegenError(node->getSpan(), "Unable to assign {} to {}", node->getRightHandSide()->toString(SourceManager.get(), "", true), node->getLeftHandSide()->toString(SourceManager.get(), "", true));
    }
    isAssignment = false;

    node->getRightHandSide()->accept(*this);
    auto value = Cast(node->getSpan(), result, isPtr ? lhs->getType()->getElementType() : lhs->getType());
    llvm::Value *var_val;

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            Builder->CreateStore(value->getLLVMValue(), lhs->getLLVMValue());
            break;
        case TokenType::PLUS_EQUAL:
            var_val = Builder->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(TY_FLOAT)) {
                auto new_val = Builder->CreateFAdd(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else if (lhs->getType()->is(TY_INT)) {
                auto new_val = Builder->CreateAdd(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MINUS_EQUAL:
            var_val = Builder->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(TY_FLOAT)) {
                auto new_val = Builder->CreateFSub(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else if (lhs->getType()->is(TY_INT)) {
                auto new_val = Builder->CreateSub(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::SLASH_EQUAL:
            var_val = Builder->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(TY_FLOAT)) {
                auto new_val = Builder->CreateFDiv(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else if (lhs->getType()->is(TY_INT)) {
                auto new_val = Builder->CreateSDiv(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::STAR_EQUAL:
            var_val = Builder->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(TY_FLOAT)) {
                auto new_val = Builder->CreateFMul(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else if (lhs->getType()->is(TY_INT)) {
                auto new_val = Builder->CreateMul(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MOD_EQUAL:
            var_val = Builder->CreateLoad(lhs->getType()->getLLVMType(), lhs->getLLVMValue());
            if (lhs->getType()->is(TY_FLOAT)) {
                auto new_val = Builder->CreateFRem(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
            } else if (lhs->getType()->is(TY_INT)) {
                auto new_val = Builder->CreateSRem(value->getLLVMValue(), var_val);
                Builder->CreateStore(new_val, lhs->getLLVMValue());
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

void Codegen::visit(const Break *node) {
    if (breakBlocks.empty())
        throw CodegenError(node->getSpan(), "Cannot break without being in a loop");

    auto block = breakBlocks.top();
    breakBlocks.pop();
    isBreak = true;

    Builder->CreateBr(block);
}

void Codegen::visit(const Continue *node) {
    if (continueBlocks.empty())
        throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");

    auto block = continueBlocks.top();
    continueBlocks.pop();
    isBreak = true;

    Builder->CreateBr(block);
}

void Codegen::visit(const Return *node) {
    // Check if it's top-level
    if (Builder->GetInsertBlock()->getParent() == TopLevelFunc)
        throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");

    // Execute all deferred statements
    for (auto inst: deferStack.top())
        inst->accept(*this);

    isReturn = true;

    if (node->getValue() == nullptr) {
        if (currentFunction->getType()->getReturnType()->is(TY_VOID)) {
            Builder->CreateRetVoid();
        } else {
            throw CodegenError(node->getSpan(), "Return type does not match the function return type, expected {}, actual void",
                               currentFunction->getType()->getReturnType()->toString());
        }
    } else {
        node->getValue()->accept(*this);
        if (Builder->getCurrentFunctionReturnType() == result->getType()->getLLVMType()) {
            Builder->CreateRet(result->getLLVMValue());
        } else {
            throw CodegenError(node->getSpan(), "Return type does not match the function return type, expected {}, actual {}",
                               currentFunction->getType()->getReturnType()->toString(), result->getType()->toString());
        }
    }
}

void Codegen::visit(const Defer *node) {
    deferStack.top().push_back(node->getStatement());
}

void Codegen::visit(const ExpressionStatement *node) {
    node->getExpression()->accept(*this);
}

void Codegen::visit(const Import *node) {
    CompileModule(node->getSpan(), node->getFilePath(), node->isStd(), node->getAlias(), node->getImportAll(), node->getImportScope(), node->getImportedNames());
}

void Codegen::visit(const Class *node) {
    std::vector<Field *> fields;
    std::vector<llvm::Type *> elementLLVMTypes;

    for (auto field: node->getFields()) {
        if (field->getType().has_value()) {
            field->getType().value()->accept(*this);
        } else {
            field->getValue().value()->accept(*this);
        }

        elementLLVMTypes.push_back(result->getType()->getLLVMType());
        fields.push_back(new Field{field->getIdentifier()->getValue(), result->getType(), field->getValue().has_value() ? result : nullptr});
    }

    llvm::StructType *structType = llvm::StructType::create(*TheContext->getContext(), elementLLVMTypes, node->getIdentifier());

    auto *type = new Type(TY_CLASS, structType, std::move(fields));
    auto *structSymbol = new Value(node->getIdentifier(), type);
    structSymbol->setExported(node->isExported());

    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);

    selfSymbol = new Value(node->getIdentifier(), new Type(TY_PTR, structType->getPointerTo(), type));
    selfSymbol->setExported(node->isExported());
    auto has_constructor = false;
    for (auto func: node->getMethods()) {
        func->accept(*this);
        if (func->getName() == "new") {
            has_constructor = true;
            structSymbol->setConstructor(result);
        }
    }

    if (!has_constructor)
        throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());

    selfSymbol = nullptr;
}

void Codegen::visit(const Enum *node) {
    std::vector<llvm::Type *> elementTypes = {Builder->getInt8Ty()};
    llvm::StructType *structType = llvm::StructType::create(*TheContext->getContext(), elementTypes, node->getIdentifier());
    std::vector<Field *> fields;

    for (const auto &field: node->getValues())
        fields.push_back(new Field{field, new Type(TY_VOID, Builder->getVoidTy())});

    auto *type = new Type(TY_ENUM, structType, std::move(fields));
    auto *structSymbol = new Value(node->getIdentifier(), type);
    structSymbol->setExported(node->isExported());

    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);
}

void Codegen::visit(const FuncCall *node) {
    result = genFuncCall(node, {});
}

void Codegen::visit(const BinaryOp *node) {
    node->getLeft()->accept(*this);
    lesma::Value *left = result;
    node->getRight()->accept(*this);
    lesma::Value *right = result;
    lesma::Type *finalType = GetExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", finalType, Builder->CreateFSub(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", finalType, Builder->CreateSub(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::PLUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", finalType, Builder->CreateFAdd(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", finalType, Builder->CreateAdd(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::STAR:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", finalType, Builder->CreateFMul(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", finalType, Builder->CreateMul(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::SLASH:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", finalType, Builder->CreateFDiv(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", finalType, Builder->CreateSDiv(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::MOD:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", finalType, Builder->CreateFRem(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", finalType, Builder->CreateSRem(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::POWER:
            if (finalType == nullptr)
                break;
            else if (!right->getType()->isOneOf({TY_INT, TY_FLOAT}))
                throw CodegenError(node->getSpan(), "Cannot use non-numbers for power coefficient: {}",
                                   node->getRight()->toString(SourceManager.get(), "", true));
            throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
        case TokenType::EQUAL_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            // Enum comparison
            if (finalType->is(TY_ENUM)) {
                // Both are pointers to structs
                auto left_name = left->getType()->getLLVMType()->getStructName().str();
                auto right_name = right->getType()->getLLVMType()->getStructName().str();

                if (left_name != right_name) {
                    throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}", left_name, right_name);
                }

                llvm::Value *left_val = Builder->CreateExtractValue(left->getLLVMValue(), {0});
                llvm::Value *right_val = Builder->CreateExtractValue(right->getLLVMValue(), {0});
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpEQ(left_val, right_val));
                return;
            } else if (finalType->is(TY_PTR)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpOEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpEQ(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::BANG_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            // Enum comparison
            if (finalType->is(TY_ENUM)) {
                // Both are pointers to structs
                auto left_name = left->getType()->getLLVMType()->getStructName().str();
                auto right_name = right->getType()->getLLVMType()->getStructName().str();

                if (left_name != right_name) {
                    throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}", left_name, right_name);
                }

                llvm::Value *left_val = Builder->CreateExtractValue(left->getLLVMValue(), {0});
                llvm::Value *right_val = Builder->CreateExtractValue(right->getLLVMValue(), {0});
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpNE(left_val, right_val));
                return;
            } else if (finalType->is(TY_PTR)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpNE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpONE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpNE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::GREATER:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpOGT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpSGT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::GREATER_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpOGE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpSGE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::LESS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpOLT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpSLT(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::LESS_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->is(TY_FLOAT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateFCmpOLE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            } else if (finalType->is(TY_INT)) {
                result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateICmpSLE(left->getLLVMValue(), right->getLLVMValue()));
                return;
            }
            break;
        case TokenType::AND:
            if (!left->getType()->is(TY_BOOL) && !right->getType()->is(TY_BOOL))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateLogicalAnd(left->getLLVMValue(), right->getLLVMValue()));
            return;
        case TokenType::OR:
            if (!left->getType()->is(TY_BOOL) && !right->getType()->is(TY_BOOL))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), Builder->CreateLogicalOr(left->getLLVMValue(), right->getLLVMValue()));
            return;
        default:
            throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}", NAMEOF_ENUM(node->getOperator()));
    }

    throw CodegenError(node->getSpan(),
                       "Unimplemented binary operator {} for {} and {}",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getLeft()->toString(SourceManager.get(), "", true),
                       node->getRight()->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const DotOp *node) {
    if (auto left = dynamic_cast<Literal *>(node->getLeft())) {
        if (left->getType() != TokenType::IDENTIFIER)
            throw CodegenError(node->getLeft()->getSpan(), "Expected identifier left-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

        auto type_sym = Scope->lookupType(left->getValue());
        if (type_sym != nullptr) {
            // Assuming it's an enum or statically accessed class
            if (!type_sym->isOneOf({TY_ENUM, TY_CLASS, TY_IMPORT}))
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());

            auto right = dynamic_cast<Literal *>(node->getRight());
            if (type_sym->is(TY_ENUM)) {
                // Check if right-hand expression is an identifier expression
                if (!dynamic_cast<Literal *>(node->getRight()))
                    throw CodegenError(node->getRight()->getSpan(), "Expected identifier right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

                if (right->getType() != TokenType::IDENTIFIER)
                    throw CodegenError(node->getRight()->getSpan(), "Expected identifier right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

                // Setting value to the enum
                auto val = FindIndexInFields(type_sym, right->getValue());
                // Field not found in enum
                if (val == -1)
                    throw CodegenError(node->getLeft()->getSpan(), "Identifier {} not in {}", right->getValue(), left->getValue());

                auto struct_val = Scope->lookupStruct(left->getValue());
                auto enum_ptr = Builder->CreateAlloca(struct_val->getType()->getLLVMType());
                auto field = Builder->CreateStructGEP(struct_val->getType()->getLLVMType(), enum_ptr, 0);
                Builder->CreateStore(Builder->getInt8(val), field);
                // TODO: Returning the enum directly or a ptr to it? We used to return a pointer
                auto enum_val = Builder->CreateLoad(struct_val->getType()->getLLVMType(), enum_ptr);

                result = new Value("", struct_val->getType(), enum_val);
                return;
            } else if (type_sym->is(TY_IMPORT)) {
                std::string field;
                FuncCall *method = nullptr;

                if (!dynamic_cast<Literal *>(node->getRight()) && !dynamic_cast<FuncCall *>(node->getRight()))
                    throw CodegenError(node->getRight()->getSpan(), "Expected identifier or method call right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

                if (dynamic_cast<Literal *>(node->getRight()) && dynamic_cast<Literal *>(node->getRight())->getType() == TokenType::IDENTIFIER)
                    field = dynamic_cast<Literal *>(node->getRight())->getValue();
                else
                    method = dynamic_cast<FuncCall *>(node->getRight());

                if (method != nullptr) {
                    auto tmp_alias = alias;
                    alias = left->getValue();
                    auto ret_val = genFuncCall(method, {});
                    alias = tmp_alias;
                    result = ret_val;
                    return;
                }
            }
        } else {
            // Assuming it's a class instance
            left->accept(*this);
            // We refer to the class type, if it's a pointer, we get the result
            lesma::Type *lesma_type = result->getType();
            if (result->getType()->is(TY_PTR) && result->getType()->getElementType()->is(TY_CLASS)) {
                lesma_type = result->getType()->getElementType();
            }

            if (!lesma_type->is(TY_CLASS))
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());

            std::string field;
            FuncCall *method;

            if (!dynamic_cast<Literal *>(node->getRight()) && !dynamic_cast<FuncCall *>(node->getRight()))
                throw CodegenError(node->getRight()->getSpan(), "Expected identifier or method call right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

            if (dynamic_cast<Literal *>(node->getRight()) && dynamic_cast<Literal *>(node->getRight())->getType() == TokenType::IDENTIFIER) {
                field = dynamic_cast<Literal *>(node->getRight())->getValue();
            } else {
                method = dynamic_cast<FuncCall *>(node->getRight());
            }

            // TODO: Somehow, when we call a class method with a variable x,
            //  we lose the class name from cls, so we set it again
            auto cls = Scope->lookupStruct(lesma_type->getLLVMType()->getStructName().str());
            cls->setName(lesma_type->getLLVMType()->getStructName().str());

            if (cls->getType()->is(TY_CLASS)) {
                if (!field.empty()) {
                    auto index = FindIndexInFields(cls->getType(), field);
                    auto type = FindTypeInFields(cls->getType(), field);
                    if (index == -1)
                        throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field, result->getType()->getElementType()->getLLVMType()->getStructName().str());

                    auto ptr = Builder->CreateStructGEP(cls->getType()->getLLVMType(), result->getLLVMValue(), index);
                    if (isAssignment) {
                        result = new Value("", new Type(TY_PTR, Builder->getPtrTy(), type), ptr);
                        return;
                    }
                    //                    auto &x = cls->getType()->getFields()[index];
                    result = new Value("", type, Builder->CreateLoad(type->getLLVMType(), ptr));
                    return;
                } else if (method != nullptr) {
                    selfSymbol = cls;
                    auto ret_val = genFuncCall(method, {result});
                    selfSymbol = nullptr;
                    result = ret_val;
                    return;
                }
            } else {
                throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}", lesma_type->getLLVMType()->getStructName().str());
            }
        }
    }
    throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}", node->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const CastOp *node) {
    node->getExpression()->accept(*this);
    auto expr = result;
    node->getType()->accept(*this);
    auto castType = result->getType();
    result = Cast(node->getSpan(), expr, castType);
}

void Codegen::visit(const IsOp *node) {
    node->getLeft()->accept(*this);
    auto left_type = result->getType();
    node->getRight()->accept(*this);
    auto right_type = result->getType();

    llvm::Value *val;

    if (node->getOperator() == TokenType::IS) {
        val = left_type->isEqual(right_type) ? Builder->getTrue() : Builder->getFalse();
    } else {
        val = left_type->isEqual(right_type) ? Builder->getFalse() : Builder->getTrue();
    }

    result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), val);
}

void Codegen::visit(const UnaryOp *node) {
    node->getExpression()->accept(*this);

    llvm::Value *val;
    lesma::Type *type = result->getType();

    if (node->getOperator() == TokenType::MINUS) {
        if (result->getType()->is(TY_INT)) {
            val = Builder->CreateNeg(result->getLLVMValue());
        } else if (result->getType()->is(TY_FLOAT)) {
            val = Builder->CreateFNeg(result->getLLVMValue());
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::NOT) {
        if (result->getType()->is(TY_BOOL)) {
            val = Builder->CreateNot(result->getLLVMValue());
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::STAR) {
        if (result->getType()->is(TY_PTR)) {
            val = Builder->CreateLoad(result->getType()->getElementType()->getLLVMType(), result->getLLVMValue());
            type = result->getType()->getElementType();
        } else {
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
        }
    } else if (node->getOperator() == TokenType::AMPERSAND) {
        val = Builder->CreateAlloca(result->getType()->getLLVMType());
        type = new Type(TY_PTR, Builder->getPtrTy(), result->getType());
        Builder->CreateStore(result->getLLVMValue(), val);
    } else {
        throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    }

    result = new Value("", type, val);
}

void Codegen::visit(const Literal *node) {
    if (node->getType() == TokenType::DOUBLE) {
        result = new Value("", new Type(TY_FLOAT, Builder->getDoubleTy()), ConstantFP::get(*TheContext->getContext(), APFloat(std::stod(node->getValue()))));
    } else if (node->getType() == TokenType::INTEGER) {
        result = new Value("", new Type(TY_INT, Builder->getInt64Ty()), ConstantInt::getSigned(Builder->getInt64Ty(), std::stoi(node->getValue())));
    } else if (node->getType() == TokenType::BOOL) {
        result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), node->getValue() == "true" ? Builder->getTrue() : Builder->getFalse());
    } else if (node->getType() == TokenType::NIL) {
        result = new Value("", new Type(TY_VOID, Builder->getVoidTy()), ConstantPointerNull::getNullValue(Builder->getInt8PtrTy(0)));
    } else if (node->getType() == TokenType::STRING) {
        // Try to instantiate a String class and call the constructor
        auto tmp_selfSymbol = selfSymbol;
        selfSymbol = Scope->lookupStruct("String");

        if (selfSymbol == nullptr || !selfSymbol->getType()->is(TY_CLASS)) {
            throw CodegenError(node->getSpan(), "Cannot find String class");
        }

        // It's a class constructor, allocate and add self param
        auto class_ptr = Builder->CreateAlloca(selfSymbol->getType()->getLLVMType());
        auto tmp_str = Builder->CreateGlobalStringPtr(node->getValue());

        std::vector<llvm::Value *> paramsLLVM;
        std::vector<lesma::Type *> paramTypes;

        paramsLLVM.push_back(class_ptr);
        paramTypes.push_back(new Type(TY_PTR, Builder->getPtrTy(), selfSymbol->getType()));

        paramsLLVM.push_back(tmp_str);
        paramTypes.push_back(new Type(TY_PTR, Builder->getInt8PtrTy(), new Type(TY_INT, Builder->getInt8PtrTy())));

        auto str_constructor = Scope->lookupClassConstructor("String", paramTypes);
        if (str_constructor == nullptr) {
            throw CodegenError(node->getSpan(), "Cannot find constructor for String");
        }

        Builder->CreateCall(llvm::cast<Function>(str_constructor->getLLVMValue()), paramsLLVM);

        result = new Value("", new Type(TY_PTR, Builder->getPtrTy(), selfSymbol->getType()), class_ptr);
        selfSymbol = tmp_selfSymbol;
    } else if (node->getType() == TokenType::IDENTIFIER) {
        // Look this variable up in the function.
        auto val = Scope->lookup(node->getValue());
        if (val == nullptr)
            throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());

        if (val->getType()->isOneOf({TY_CLASS})) {
            // If it's a class, don't load the value
            result = val;
        } else {
            // Load the value.
            llvm::Value *llvmVal = Builder->CreateLoad(val->getType()->getLLVMType(), val->getLLVMValue());
            result = new Value("", val->getType(), llvmVal);
        }
    } else {
        throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
    }
}

void Codegen::visit(const Else * /*node*/) {
    result = new Value("", new Type(TY_BOOL, Builder->getInt1Ty()), llvm::ConstantInt::getTrue(*TheContext->getContext()));
}

std::string Codegen::getTypeMangledName(llvm::SMRange span, lesma::Type *type) {
    auto *llvm_ty = type->getLLVMType();
    if (type->is(TY_BOOL))
        return "b";
    else if (type->is(TY_INT) && llvm_ty->isIntegerTy(8))
        return "c";
    else if (type->is(TY_INT) && llvm_ty->isIntegerTy(16))
        return "i16";
    else if (type->is(TY_INT) && llvm_ty->isIntegerTy(32))
        return "i32";
    else if (type->is(TY_INT))
        return "i";
    else if (type->is(TY_FLOAT) && llvm_ty->isFloatTy())
        return "f32";
    else if (type->is(TY_FLOAT) && llvm_ty->isFloatingPointTy())
        return "f";
    else if (type->is(TY_STRING))
        return "str";
    else if (type->is(TY_VOID))
        return "void";
    else if (type->is(TY_ARRAY) && llvm_ty->isArrayTy())
        return "(arr_" + getTypeMangledName(span, type->getElementType()) + ")";
    else if (type->is(TY_PTR))
        return "(ptr_" + getTypeMangledName(span, type->getElementType()) + ")";
    else if (type->is(TY_FUNCTION)) {
        std::string param_str;
        for (auto &field: type->getFields()) {
            param_str += getTypeMangledName(span, field->type) + "_";
        }
        return "(func_" + param_str + ")";
    } else if (type->isOneOf({TY_CLASS, TY_ENUM})) {
        std::string param_str;
        for (auto &field: type->getFields()) {
            param_str += getTypeMangledName(span, field->type) + "_";
        }
        return "(struct_" + type->getLLVMType()->getStructName().str() + ")";
    }

    throw CodegenError(span, "Unknown type found during mangling");
}


bool Codegen::isMethod(const std::string &mangled_name) {
    return mangled_name.find("::") != std::string::npos;
}

std::string Codegen::getMangledName(llvm::SMRange span, std::string func_name, const std::vector<lesma::Type *> &paramTypes, bool isMethod, std::string module_alias) {
    module_alias = module_alias.empty() ? this->alias : module_alias;
    std::string name = (module_alias.empty() ? "" : "&" + module_alias + "=>") +
                       (selfSymbol != nullptr && isMethod ? selfSymbol->getName() + "::" + std::move(func_name) + ":" : "." + std::move(func_name) + ":");
    bool first = true;

    for (auto param_type: paramTypes) {
        if (!first)
            name += ",";
        else
            first = false;

        name += getTypeMangledName(span, param_type);
    }

    return name;
}

bool Codegen::isMangled(std::string name) {
    return name.find(':') != std::string::npos || name.at(0) == '.';
}

std::string Codegen::getDemangledName(const std::string &name) {
    if (!isMangled(name)) {
        return name;
    }

    auto demangled_name = name;

    // Remove class mangling
    auto class_mangling = demangled_name.find("::");
    if (class_mangling != std::string::npos)
        demangled_name = demangled_name.substr(class_mangling + 2);

    // Remove standard '.' mangling to differentiate from native functions
    if (demangled_name.at(0) == '.')
        demangled_name.erase(0, 1);

    // Remove parameters mangling
    auto parameter_mangling = demangled_name.find(':');
    if (parameter_mangling != std::string::npos)
        demangled_name = demangled_name.substr(0, parameter_mangling);

    return demangled_name;
}

lesma::Type *Codegen::GetExtendedType(lesma::Type *left, lesma::Type *right) {
    if (left->getBaseType() == right->getBaseType())
        return left;

    if (left->is(TY_INT) && right->is(TY_INT)) {
        // TODO: We should ideally only have one int type, but our FFI implementation needs access to all types
        if (left->getLLVMType()->getIntegerBitWidth() > right->getLLVMType()->getIntegerBitWidth())
            return left;
        else
            return right;
    } else if (left->is(TY_INT) && right->is(TY_FLOAT))
        return right;
    else if (left->is(TY_FLOAT) && right->is(TY_INT))
        return left;
    else if (left->is(TY_FLOAT) && right->is(TY_FLOAT)) {
        if (left->getLLVMType()->isFP128Ty() || right->getLLVMType()->isFP128Ty())
            return left->getLLVMType()->isFP128Ty() ? left : right;
        else if (left->getLLVMType()->isDoubleTy() || right->getLLVMType()->isDoubleTy())
            return left->getLLVMType()->isDoubleTy() ? left : right;
        else if (left->getLLVMType()->isFloatTy() || right->getLLVMType()->isFloatTy())
            return left->getLLVMType()->isFloatTy() ? left : right;
        else if (left->getLLVMType()->isHalfTy() || right->getLLVMType()->isHalfTy())
            return left->getLLVMType()->isHalfTy() ? left : right;
    }
    return nullptr;
}

lesma::Value *Codegen::Cast(llvm::SMRange span, lesma::Value *val, lesma::Type *type) {
    if (type == nullptr)
        return val;

    // If they're the same type
    if (val->getType()->isEqual(type))
        return val;

    if (type->is(TY_INT)) {
        if (val->getType()->is(TY_FLOAT)) {
            return new Value("", type, Builder->CreateFPToSI(val->getLLVMValue(), type->getLLVMType()));
        } else if (val->getType()->is(TY_INT)) {
            return new Value("", type, Builder->CreateIntCast(val->getLLVMValue(), type->getLLVMType(), type->isSigned()));
        }
    } else if (type->is(TY_FLOAT)) {
        if (val->getType()->is(TY_INT)) {
            return new Value("", type, Builder->CreateSIToFP(val->getLLVMValue(), type->getLLVMType()));
        } else if (val->getType()->is(TY_FLOAT)) {
            return new Value("", type, Builder->CreateFPCast(val->getLLVMValue(), type->getLLVMType()));
        }
    }

    throw CodegenError(span, "Unsupported Cast between {} and {}", getTypeMangledName(span, val->getType()), getTypeMangledName(span, type));
}

lesma::Value *Codegen::genFuncCall(const FuncCall *node, const std::vector<lesma::Value *> &extra_params = {}) {
    std::vector<lesma::Type *> paramTypes;
    std::vector<llvm::Value *> paramsLLVM;

    for (auto arg: extra_params) {
        paramTypes.push_back(arg->getType());
        paramsLLVM.push_back(arg->getLLVMValue());
    }

    for (auto arg: node->getArguments()) {
        arg->accept(*this);
        paramTypes.push_back(result->getType());
        paramsLLVM.push_back(result->getLLVMValue());
    }

    Value *symbol;
    // Check if it's a constructor like `Classname()`
    auto selfSymbolTmp = selfSymbol;
    auto class_sym = Scope->lookupStruct(node->getName());
    llvm::Value *class_ptr = nullptr;
    if (class_sym != nullptr && class_sym->getType()->is(TY_CLASS)) {
        // It's a class constructor, allocate and add self param
        class_ptr = Builder->CreateAlloca(class_sym->getType()->getLLVMType());
        paramsLLVM.insert(paramsLLVM.begin(), class_ptr);
        paramTypes.insert(paramTypes.begin(), new Type(TY_PTR, Builder->getPtrTy(), class_sym->getType()));

        selfSymbol = class_sym;
        symbol = Scope->lookupFunction("new", paramTypes);
    } else {
        symbol = Scope->lookupFunction(node->getName(), paramTypes);
    }

    if (symbol == nullptr) {
        throw CodegenError(node->getSpan(), "{} {} not in current scope.", class_sym != nullptr ? "Constructor for" : "Function", node->getName());
    }

    if (!symbol->getType()->isOneOf({TY_CLASS, TY_FUNCTION}))
        throw CodegenError(node->getSpan(), "Symbol {} is not a function or constructor.", node->getName());

    if (symbol->getType()->getFields().size() > paramsLLVM.size()) {
        auto fields = symbol->getType()->getFields();
        size_t start = paramsLLVM.size();

        for (auto it = fields.begin() + start; it != fields.end(); ++it) {
            if (!(*it)->defaultValue) {
                throw CodegenError(node->getSpan(), "Something bad happened, lookup found a function with incorrect defaults", node->getName());
            }
            paramsLLVM.push_back((*it)->defaultValue->getLLVMValue());
        }
    }
    auto *func = cast<Function>(symbol->getType()->is(TY_CLASS) ? symbol->getConstructor()->getLLVMValue() : symbol->getLLVMValue());
    if (class_sym != nullptr && class_sym->getType()->is(TY_CLASS)) {
        Builder->CreateCall(func, paramsLLVM);
        selfSymbol = selfSymbolTmp;

        return new Value("", class_sym->getType(), class_ptr);
    }

    return new Value("", symbol->getType()->getReturnType(), Builder->CreateCall(func, paramsLLVM));
}

int Codegen::FindIndexInFields(Type *_struct, const std::string &field) {
    for (unsigned int i = 0; i < _struct->getFields().size(); i++) {
        if (_struct->getFields()[i]->name == field) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

lesma::Type *Codegen::FindTypeInFields(Type *_struct, const std::string &field) {
    for (const auto &i: _struct->getFields()) {
        if (i->name == field) {
            return i->type;
        }
    }

    return nullptr;
}