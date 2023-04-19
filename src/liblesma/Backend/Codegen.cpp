#include "Codegen.h"

using namespace lesma;

Codegen::Codegen(std::unique_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main, std::string alias, const std::shared_ptr<ThreadSafeContext> &context) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    ImportedModules = std::move(imports);
    TheJIT = ExitOnErr(LesmaJIT::Create());
    TheContext = context == nullptr ? std::make_shared<ThreadSafeContext>(std::make_unique<LLVMContext>()) : context;
    TheModule = std::make_unique<Module>("Lesma JIT", *TheContext->getContext());
    TheModule->setDataLayout(TheJIT->getDataLayout());
    TheModule->setSourceFileName(filename);
    Builder = std::make_unique<IRBuilder<>>(*TheContext->getContext());
    Parser_ = std::move(parser);
    SourceManager = std::move(srcMgr);
    Scope = new SymbolTable(nullptr);
    TargetMachine = InitializeTargetMachine();

    this->alias = std::move(alias);
    this->filename = filename;
    isJIT = jit;
    isMain = main;

    TopLevelFunc = InitializeTopLevel();

    // If it's not base.les stdlib, then import it
    if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
        CompileModule(llvm::SMRange(), getStdDir() + "base.les", true, "base", true, true, {});
    }
}

llvm::Function *Codegen::InitializeTopLevel() {
    std::vector<llvm::Type *> paramTypes = {};

    FunctionType *FT = FunctionType::get(Builder->getInt64Ty(), paramTypes, false);
    Function *F = Function::Create(FT, isMain ? Function::ExternalLinkage : Function::InternalLinkage, "main", *TheModule);

    auto entry = BasicBlock::Create(*TheContext->getContext(), "entry", F);
    Builder->SetInsertPoint(entry);

    return F;
}

void Codegen::defineFunction(Function *F, const FuncDecl *node, Value *clsSymbol) {
    Scope = Scope->createChildBlock(node->getName());
    deferStack.emplace();

    BasicBlock *entry = BasicBlock::Create(*TheContext->getContext(), "entry", F);
    Builder->SetInsertPoint(entry);

    for (auto &param: F->args()) {
        if (clsSymbol != nullptr && param.getArgNo() == 0)
            param.setName("self");
        else
            param.setName(node->getParameters()[param.getArgNo() - (clsSymbol != nullptr ? 1 : 0)]->name);

        llvm::Value *ptr;
        ptr = Builder->CreateAlloca(param.getType(), nullptr, param.getName() + "_ptr");
        Builder->CreateStore(&param, ptr);

        auto symbol = new Value(param.getName().str(), getType(param.getType()));
        symbol->setLLVMType(param.getType());
        symbol->setLLVMValue(ptr);
        Scope->insertSymbol(symbol);
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
        if (F->getReturnType()->isVoidTy()) {
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

    // Reset Insert Point to Top Level
    Builder->SetInsertPoint(&TopLevelFunc->back());
}

std::unique_ptr<llvm::TargetMachine> Codegen::InitializeTargetMachine() {
    // Configure output target
    auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    const std::string &tripletString = targetTriple.getTriple();
    TheModule->setTargetTriple(tripletString);

    // Search after selected target
    std::string error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(tripletString, error);
    if (!target)
        throw CodegenError({}, "Target not available:\n{}", error);

    llvm::TargetOptions opt;
    llvm::Optional<llvm::Reloc::Model> rm = llvm::Optional<llvm::Reloc::Model>();
    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(tripletString, "generic", "", opt, rm));
    return target_machine;
}

void Codegen::CompileModule(llvm::SMRange span, const std::string &filepath, bool isStd, const std::string &module_alias, bool importAll, bool importToScope, const std::vector<std::pair<std::string, std::string>> &imported_names) {
    std::filesystem::path mainPath = filename;
    // Read source
    auto absolute_path = isStd ? filepath : fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath);

    // If module is already imported, don't compile again
    if (std::find(ImportedModules.begin(), ImportedModules.end(), absolute_path) != ImportedModules.end())
        return;

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

        // Linking the two modules together
        if (isJIT) {
            // Link modules together
            if (Linker::linkModules(*TheModule, std::move(codegen->TheModule), (1 << 0)))
                throw CodegenError({}, "Error linking modules together");

            // TODO: Currently unused since we link everything to main module, we need to develop the JIT more in the future
            // ExitOnErr(TheJIT->addModule(ThreadSafeModule(std::move(codegen->TheModule), *TheContext)));
        } else {
            // Create object file to be linked
            std::string obj_file = fmt::format("tmp{}", ObjectFiles.size());
            codegen->WriteToObjectFile(obj_file);
            ObjectFiles.push_back(fmt::format("{}.o", obj_file));
        }

        // Import Functions
        // Loop over the main module if it's JIT, since we link them in one, otherwise check the imported module for symbols
        for (auto &it: isJIT ? *TheModule : *codegen->TheModule) {
            auto name = std::string{it.getName()};
            std::vector<llvm::Type *> paramTypes;
            for (unsigned param_i = 0; param_i < it.getFunctionType()->getNumParams(); param_i++)
                paramTypes.push_back(it.getFunctionType()->getParamType(param_i));

            // TODO: Get function without name mangling in case of extern C functions?
            Value *func_symbol = codegen->Scope->lookup(name);

            // Only import if it's exported
            auto demangled_name = getDemangledName(name);
            auto imp_alias = findInImports(demangled_name);
            if (func_symbol != nullptr && func_symbol->isExported() && (importAll || !imp_alias.empty() || isMethod(name))) {
                auto symbol = new Value(imp_alias.empty() ? name : std::regex_replace(name, std::regex(demangled_name), imp_alias), new Type(BaseType::TY_FUNCTION));

                if (isJIT) {
                    symbol->setLLVMType(it.getFunctionType());
                    symbol->setLLVMValue((llvm::Value *) &it.getFunction());
                } else {
                    auto new_func = Function::Create(it.getFunctionType(), Function::ExternalLinkage, name, *TheModule);
                    symbol->setLLVMType(new_func->getFunctionType());
                    symbol->setLLVMValue(new_func);
                }
                Scope->insertSymbol(symbol);
            }
        }

        // Import Classes and Enums
        for (auto sym: codegen->Scope->getSymbols()) {
            auto imp_alias = findInImports(sym.first);
            if (sym.second->getType()->isOneOf({TY_ENUM, TY_CLASS}) && sym.second->isExported() && (importAll || !imp_alias.empty())) {
                llvm::StructType *structType = StructType::getTypeByName(*TheContext->getContext(), sym.first);

                auto *structSymbol = new Value(imp_alias.empty() ? sym.first : imp_alias, sym.second->getType());
                structSymbol->setLLVMType(structType);
                Scope->insertType(sym.first, sym.second->getType());
                Scope->insertSymbol(structSymbol);
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

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(opt);

    MPM.run(*TheModule, MAM);
}

void Codegen::WriteToObjectFile(const std::string &output) {
    std::error_code err;
    auto out = llvm::raw_fd_ostream(output + ".o", err);

    llvm::legacy::PassManager passManager;
    if (TargetMachine->addPassesToEmitFile(passManager, out, nullptr, llvm::CGFT_ObjectFile))
        throw CodegenError({}, "Target Machine can't emit an object file");
    // Emit object file
    passManager.run(*TheModule);
}

void Codegen::LinkObjectFile(const std::string &obj_filename) {
    auto clangPath = llvm::sys::findProgramByName("clang");
    if (clangPath.getError())
        throw CodegenError({}, "Unable to find clang path");

    std::string output = getBasename(obj_filename);

    std::vector<const char *> args;
    args.push_back(clangPath.get().c_str());
    args.push_back(obj_filename.c_str());
    for (const auto &obj: ObjectFiles)
        args.push_back(obj.c_str());
    args.push_back("-o");
    args.push_back(output.c_str());
#ifdef __APPLE__
    args.push_back("-L");
    args.push_back("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
    args.push_back("-lSystem");
#endif

    auto DiagOpts = new clang::DiagnosticOptions();
    auto DiagID = new clang::DiagnosticIDs();
    clang::DiagnosticsEngine Diags(DiagID, &*DiagOpts);
    clang::driver::Driver TheDriver(args[0], llvm::sys::getDefaultTargetTriple(), Diags);

    // Create the set of actions to perform
    auto compilation = TheDriver.BuildCompilation(args);

    // Carry out the actions
    int Res = 0;
    SmallVector<std::pair<int, const clang::driver::Command *>, 4> FailingCommands;
    if (compilation)
        Res = TheDriver.ExecuteCompilation(*compilation, FailingCommands);

    if (Res)
        throw CodegenError({}, "Linking Failed");

    // Remove object files
    remove(obj_filename.c_str());
    for (const auto &obj: ObjectFiles)
        remove(obj.c_str());
}

int Codegen::JIT() {
    auto jit_error = TheJIT->addModule(ThreadSafeModule(std::move(TheModule), *TheContext));
    if (jit_error)
        throw CodegenError({}, "JIT Error:\n{}");
    using MainFnTy = int();
    auto main_func = TheJIT->lookup(TopLevelFunc->getName());
    if (!main_func)
        throw CodegenError({}, "Couldn't find top level function\n");
    auto jit_main = jitTargetAddressToFunction<MainFnTy *>(main_func->getAddress());
    auto ret = jit_main();

    return ret;
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
        result_type = Builder->getInt64Ty();
    else if (node->getType() == TokenType::INT8_TYPE)
        result_type = Builder->getInt8Ty();
    else if (node->getType() == TokenType::INT16_TYPE)
        result_type = Builder->getInt16Ty();
    else if (node->getType() == TokenType::INT32_TYPE)
        result_type = Builder->getInt32Ty();
    else if (node->getType() == TokenType::FLOAT_TYPE)
        result_type = Builder->getDoubleTy();
    else if (node->getType() == TokenType::FLOAT32_TYPE)
        result_type = Builder->getFloatTy();
    else if (node->getType() == TokenType::BOOL_TYPE)
        result_type = Builder->getInt1Ty();
    else if (node->getType() == TokenType::STRING_TYPE)
        result_type = Builder->getInt8PtrTy();
    else if (node->getType() == TokenType::VOID_TYPE)
        result_type = Builder->getVoidTy();
    else if (node->getType() == TokenType::PTR_TYPE) {
        node->getElementType()->accept(*this);
        result_type = PointerType::get(result_type, 0);
    } else if (node->getType() == TokenType::FUNC_TYPE) {
        node->getReturnType()->accept(*this);
        auto ret_type = result_type;
        std::vector<llvm::Type *> paramsTypes;
        for (auto param_type: node->getParams()) {
            param_type->accept(*this);
            paramsTypes.push_back(result_type);
        }

        result_type = FunctionType::get(ret_type, paramsTypes, false)->getPointerTo();
    } else if (node->getType() == TokenType::CUSTOM_TYPE) {
        auto typ = Scope->lookupType(node->getName());
        auto sym = Scope->lookup(node->getName());
        if (typ == nullptr || sym->getLLVMType() == nullptr)
            throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());

        result_type = sym->getLLVMType()->getPointerTo();
    } else {
        throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
    }
}

void Codegen::visit(const Compound *node) {
    for (auto elem: node->getChildren())
        elem->accept(*this);
}

void Codegen::visit(const VarDecl *node) {
    llvm::Type *type;

    if (node->getValue().has_value()) {
        node->getValue().value()->accept(*this);
        type = result->getType();
    }

    if (node->getType().has_value()) {
        node->getType().value()->accept(*this);
        type = result_type;
    }

    auto ptr = Builder->CreateAlloca(type, nullptr, node->getIdentifier()->getValue());

    auto symbol = new Value(node->getIdentifier()->getValue(), getType(type), node->getType().has_value() ? INITIALIZED : DECLARED);
    symbol->setLLVMType(type);
    symbol->setLLVMValue(ptr);
    symbol->setMutable(node->getMutability());
    Scope->insertSymbol(symbol);

    if (node->getValue().has_value())
        Builder->CreateStore(Cast(node->getSpan(), result, ptr->getAllocatedType(), true), ptr);
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
        Builder->CreateCondBr(result, bIfTrue, bIfFalse);
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
    Builder->CreateCondBr(result, bLoop, bEnd);

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

    std::vector<llvm::Type *> paramTypes;

    if (selfSymbol != nullptr) {
        paramTypes.push_back(selfSymbol->getLLVMType()->getPointerTo());
    }

    for (const auto &param: node->getParameters()) {
        param->type->accept(*this);
        paramTypes.push_back(result_type);
    }

    auto name = getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
    auto linkage = node->isExported() ? Function::ExternalLinkage : Function::PrivateLinkage;

    node->getReturnType()->accept(*this);
    FunctionType *FT = FunctionType::get(result_type, paramTypes, node->getVarArgs());
    Function *F = Function::Create(FT, linkage, name, *TheModule);

    auto func_symbol = new Value(name, new Type(BaseType::TY_FUNCTION));
    func_symbol->setLLVMType(F->getFunctionType());
    func_symbol->setLLVMValue(F);
    func_symbol->setExported(node->isExported());
    Scope->insertSymbol(func_symbol);

    Prototypes.emplace_back(F, node, selfSymbol);
}

void Codegen::visit(const ExternFuncDecl *node) {
    std::vector<llvm::Type *> paramTypes;

    for (const auto &param: node->getParameters()) {
        param->type->accept(*this);
        paramTypes.push_back(result_type);
    }

    FunctionCallee F;
    if (TheModule->getFunction(node->getName()) != nullptr && Scope->lookup(node->getName()) != nullptr)
        return;
    else if (TheModule->getFunction(node->getName()) != nullptr) {
        F = TheModule->getFunction(node->getName());
    } else {
        node->getReturnType()->accept(*this);
        FunctionType *FT = FunctionType::get(result_type, paramTypes, node->getVarArgs());
        F = TheModule->getOrInsertFunction(node->getName(), FT);
    }

    auto symbol = new Value(node->getName(), new Type(BaseType::TY_FUNCTION));
    symbol->setLLVMType(F.getFunctionType());
    symbol->setLLVMValue(F.getCallee());
    symbol->setExported(node->isExported());
    Scope->insertSymbol(symbol);
}

void Codegen::visit(const Assignment *node) {
    llvm::Type *lhs_type;
    llvm::Value *lhs_val;
    isAssignment = true;
    if (dynamic_cast<Literal *>(node->getLeftHandSide())) {
        auto lit = dynamic_cast<Literal *>(node->getLeftHandSide());
        auto symbol = Scope->lookup(lit->getValue());
        if (symbol == nullptr)
            throw CodegenError(node->getSpan(), "Variable not found: {}", lit->getValue());
        if (!symbol->getMutability())
            throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");

        lhs_type = symbol->getLLVMType();
        lhs_val = symbol->getLLVMValue();
    } else if (dynamic_cast<DotOp *>(node->getLeftHandSide())) {
        node->getLeftHandSide()->accept(*this);
        lhs_type = result->getType();
    } else {
        throw CodegenError(node->getSpan(), "Unable to assign {} to {}", node->getRightHandSide()->toString(SourceManager.get(), "", true), node->getLeftHandSide()->toString(SourceManager.get(), "", true));
    }
    isAssignment = false;

    node->getRightHandSide()->accept(*this);
    auto value = Cast(node->getSpan(), result, lhs_type, true);
    llvm::Value *var_val;

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            Builder->CreateStore(value, lhs_val);
            break;
        case TokenType::PLUS_EQUAL:
            var_val = Builder->CreateLoad(lhs_type, lhs_val);
            if (lhs_type->isFloatingPointTy()) {
                auto new_val = Builder->CreateFAdd(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else if (lhs_type->isIntegerTy()) {
                auto new_val = Builder->CreateAdd(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MINUS_EQUAL:
            var_val = Builder->CreateLoad(lhs_type, lhs_val);
            if (lhs_type->isFloatingPointTy()) {
                auto new_val = Builder->CreateFSub(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else if (lhs_type->isIntegerTy()) {
                auto new_val = Builder->CreateSub(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::SLASH_EQUAL:
            var_val = Builder->CreateLoad(lhs_type, lhs_val);
            if (lhs_type->isFloatingPointTy()) {
                auto new_val = Builder->CreateFDiv(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else if (lhs_type->isIntegerTy()) {
                auto new_val = Builder->CreateSDiv(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::STAR_EQUAL:
            var_val = Builder->CreateLoad(lhs_type, lhs_val);
            if (lhs_type->isFloatingPointTy()) {
                auto new_val = Builder->CreateFMul(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else if (lhs_type->isIntegerTy()) {
                auto new_val = Builder->CreateMul(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MOD_EQUAL:
            var_val = Builder->CreateLoad(lhs_type, lhs_val);
            if (lhs_type->isFloatingPointTy()) {
                auto new_val = Builder->CreateFRem(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else if (lhs_type->isIntegerTy()) {
                auto new_val = Builder->CreateSRem(value, var_val);
                Builder->CreateStore(new_val, lhs_val);
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
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
        if (Builder->getCurrentFunctionReturnType() == Builder->getVoidTy())
            Builder->CreateRetVoid();
        else
            throw CodegenError(node->getSpan(), "Return type does not match the function return type");
    } else {
        node->getValue()->accept(*this);
        if (Builder->getCurrentFunctionReturnType() == result->getType())
            Builder->CreateRet(result);
        else
            throw CodegenError(node->getSpan(), "Return type does not match the function return type");
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
    std::vector<llvm::Type *> elementTypes = {};
    for (auto field: node->getFields()) {
        if (field->getType().has_value()) {
            field->getType().value()->accept(*this);
        } else {
            field->getValue().value()->accept(*this);
            result_type = result->getType();
        }

        elementTypes.push_back(result_type);
    }

    llvm::StructType *structType = llvm::StructType::create(*TheContext->getContext(), elementTypes, node->getIdentifier());
    std::vector<std::unique_ptr<Field>> fields;

    for (auto &&[field, elem_type]: zip(node->getFields(), elementTypes))
        fields.push_back(std::make_unique<Field>(Field{field->getIdentifier()->getValue(), getType(elem_type)}));

    auto *type = new Type(TY_CLASS, std::move(fields), nullptr);
    auto *structSymbol = new Value(node->getIdentifier(), type);
    structSymbol->setLLVMType(structType);
    structSymbol->setExported(node->isExported());
    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);

    selfSymbol = structSymbol;
    auto has_constructor = false;
    for (auto func: node->getMethods()) {
        if (func->getName() == "new")
            has_constructor = true;
        func->accept(*this);
    }

    if (!has_constructor)
        throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());

    selfSymbol = nullptr;
}

void Codegen::visit(const Enum *node) {
    std::vector<llvm::Type *> elementTypes = {Builder->getInt8Ty()};
    llvm::StructType *structType = llvm::StructType::create(*TheContext->getContext(), elementTypes, node->getIdentifier());
    std::vector<std::unique_ptr<Field>> fields;

    for (const auto &field: node->getValues())
        fields.push_back(std::make_unique<Field>(Field{field, new Type(TY_VOID)}));

    auto *type = new Type(TY_ENUM, std::move(fields), nullptr);
    auto *structSymbol = new Value(node->getIdentifier(), type);
    structSymbol->setLLVMType(structType);
    structSymbol->setExported(node->isExported());
    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);
}

void Codegen::visit(const FuncCall *node) {
    result = genFuncCall(node, {});
}

void Codegen::visit(const BinaryOp *node) {
    node->getLeft()->accept(*this);
    llvm::Value *left = result;
    node->getRight()->accept(*this);
    llvm::Value *right = result;
    llvm::Type *finalType = GetExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFSub(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateSub(left, right);
                return;
            }
            break;
        case TokenType::PLUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFAdd(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateAdd(left, right);
                return;
            }
            break;
        case TokenType::STAR:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFMul(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateMul(left, right);
                return;
            }
            break;
        case TokenType::SLASH:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFDiv(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateSDiv(left, right);
                return;
            }
            break;
        case TokenType::MOD:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFRem(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateSRem(left, right);
                return;
            }
            break;
        case TokenType::POWER:
            if (finalType == nullptr)
                break;
            else if (!right->getType()->isIntegerTy())
                throw CodegenError(node->getSpan(), "Cannot use non-integers for power coefficient: {}",
                                   node->getRight()->toString(SourceManager.get(), "", true));
            throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
        case TokenType::EQUAL_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if ((left->getType()->isPointerTy() && left->getType()->getPointerElementType()->isStructTy()) && (right->getType()->isPointerTy() && right->getType()->getPointerElementType()->isStructTy())) {
                // Both are pointers to structs
                auto sym = Scope->lookup(left->getType()->getPointerElementType()->getStructName().str());
                if (sym != nullptr && sym->getType()->is(TY_ENUM)) {
                    if (left->getType()->getPointerElementType() != right->getType()->getPointerElementType()) {
                        result = ConstantInt::getBool(*TheContext->getContext(), false);
                        return;
                    } else {
                        auto left_gep = Builder->CreateStructGEP(sym->getLLVMType(), left, 0);
                        auto left_load = Builder->CreateLoad(Builder->getInt8Ty(), left_gep);
                        auto right_gep = Builder->CreateStructGEP(sym->getLLVMType(), right, 0);
                        auto right_load = Builder->CreateLoad(Builder->getInt8Ty(), right_gep);
                        result = Builder->CreateICmpEQ(left_load, right_load);
                        return;
                    }
                }

                // It's not an enum, it's a class
            } else if (left->getType()->isPointerTy() && right->getType()->isPointerTy()) {
                result = Builder->CreateICmpEQ(left, right);
                return;
            } else if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpOEQ(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpEQ(left, right);
                return;
            } else if (finalType->isPointerTy() && finalType->getPointerElementType()->isStructTy()) {
                auto struct_ty = Scope->lookupType(finalType->getPointerElementType()->getStructName().str());
                if (struct_ty->is(TY_ENUM)) {
                    auto left_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), left);
                    auto right_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), right);
                    auto left_val = Builder->CreateExtractValue(left_ptr, {0});
                    auto right_val = Builder->CreateExtractValue(right_ptr, {0});
                    result = Builder->CreateICmpEQ(left_val, right_val);
                    return;
                }
            }
            break;
        case TokenType::BANG_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr && (left->getType()->isPointerTy() && left->getType()->getPointerElementType()->isStructTy()) && (right->getType()->isPointerTy() && right->getType()->getPointerElementType()->isStructTy())) {
                result = ConstantInt::getBool(*TheContext->getContext(), true);
                return;
            } else if (left->getType()->isPointerTy() && right->getType()->isPointerTy()) {
                result = Builder->CreateICmpNE(left, right);
                return;
            } else if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpONE(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpNE(left, right);
                return;
            } else if (finalType->isPointerTy() && finalType->getPointerElementType()->isStructTy()) {
                auto struct_ty = Scope->lookupType(finalType->getPointerElementType()->getStructName().str());
                if (struct_ty->is(TY_ENUM)) {
                    auto left_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), left);
                    auto right_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), right);
                    auto left_val = Builder->CreateExtractValue(left_ptr, {0});
                    auto right_val = Builder->CreateExtractValue(right_ptr, {0});
                    result = Builder->CreateICmpNE(left_val, right_val);
                    return;
                }
            }
            break;
        case TokenType::GREATER:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpOGT(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpSGT(left, right);
                return;
            }
            break;
        case TokenType::GREATER_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpOGE(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpSGE(left, right);
                return;
            }
            break;
        case TokenType::LESS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpOLT(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpSLT(left, right);
                return;
            }
            break;
        case TokenType::LESS_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy()) {
                result = Builder->CreateFCmpOLE(left, right);
                return;
            } else if (finalType->isIntegerTy()) {
                result = Builder->CreateICmpSLE(left, right);
                return;
            }
            break;
        case TokenType::AND:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            result = Builder->CreateLogicalAnd(left, right);
            return;
        case TokenType::OR:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            result = Builder->CreateLogicalOr(left, right);
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

                auto struct_ty = Scope->lookup(left->getValue());
                auto enum_ptr = Builder->CreateAlloca(struct_ty->getLLVMType());
                auto field = Builder->CreateStructGEP(struct_ty->getLLVMType(), enum_ptr, 0);
                Builder->CreateStore(Builder->getInt8(val), field);

                result = enum_ptr;
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
            if (!(result->getType()->isPointerTy() && result->getType()->getPointerElementType()->isStructTy()))
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());

            std::string field;
            FuncCall *method;

            if (!dynamic_cast<Literal *>(node->getRight()) && !dynamic_cast<FuncCall *>(node->getRight()))
                throw CodegenError(node->getRight()->getSpan(), "Expected identifier or method call right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

            if (dynamic_cast<Literal *>(node->getRight()) && dynamic_cast<Literal *>(node->getRight())->getType() == TokenType::IDENTIFIER)
                field = dynamic_cast<Literal *>(node->getRight())->getValue();
            else
                method = dynamic_cast<FuncCall *>(node->getRight());

            auto cls = Scope->lookupStructByName(result->getType()->getPointerElementType()->getStructName().str());
            if (cls->getType()->is(TY_CLASS)) {
                if (!field.empty()) {
                    auto index = FindIndexInFields(cls->getType(), field);
                    if (index == -1)
                        throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field, result->getType()->getPointerElementType()->getStructName().str());

                    auto ptr = Builder->CreateStructGEP(cls->getLLVMType(), result, index);
                    if (isAssignment) {
                        result = ptr;
                        return;
                    }
                    //                    auto &x = cls->getType()->getFields()[index];
                    result = Builder->CreateLoad(ptr->getType()->getPointerElementType(), ptr);
                    return;
                } else if (method != nullptr) {
                    selfSymbol = cls;
                    auto ret_val = genFuncCall(method, {result});
                    selfSymbol = nullptr;
                    result = ret_val;
                    return;
                }
            }
        }
    }
    throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}", node->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const CastOp *node) {
    node->getExpression()->accept(*this);
    node->getType()->accept(*this);
    result = Cast(node->getSpan(), result, result_type);
}

void Codegen::visit(const IsOp *node) {
    node->getLeft()->accept(*this);
    node->getRight()->accept(*this);
    auto left_type = result->getType();
    if (node->getOperator() == TokenType::IS)
        result = left_type == result_type ? Builder->getTrue() : Builder->getFalse();
    else
        result = left_type == result_type ? Builder->getFalse() : Builder->getTrue();
}

void Codegen::visit(const UnaryOp *node) {
    node->getExpression()->accept(*this);

    if (node->getOperator() == TokenType::MINUS) {
        if (result->getType()->isIntegerTy())
            result = Builder->CreateNeg(result);
        else if (result->getType()->isFloatingPointTy())
            result = Builder->CreateFNeg(result);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::NOT) {
        if (result->getType()->isIntegerTy(1))
            result = Builder->CreateNot(result);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::STAR) {
        if (result->getType()->isPointerTy())
            result = Builder->CreateLoad(result->getType()->getPointerElementType(), result);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::AMPERSAND) {
        auto ptr = Builder->CreateAlloca(result->getType(), nullptr);
        Builder->CreateStore(result, ptr);
        result = ptr;
    } else
        throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
}

void Codegen::visit(const Literal *node) {
    if (node->getType() == TokenType::DOUBLE)
        result = ConstantFP::get(*TheContext->getContext(), APFloat(std::stod(node->getValue())));
    else if (node->getType() == TokenType::INTEGER)
        result = ConstantInt::getSigned(Builder->getInt64Ty(), std::stoi(node->getValue()));
    else if (node->getType() == TokenType::BOOL)
        result = node->getValue() == "true" ? Builder->getTrue() : Builder->getFalse();
    else if (node->getType() == TokenType::STRING)
        result = Builder->CreateGlobalStringPtr(node->getValue());
    else if (node->getType() == TokenType::NIL)
        result = ConstantPointerNull::getNullValue(Builder->getInt8PtrTy(0));
    else if (node->getType() == TokenType::IDENTIFIER) {
        // Look this variable up in the function.
        auto val = Scope->lookup(node->getValue());
        if (val == nullptr)
            throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());

        // If it's a struct, don't load the value
        if (val->getLLVMType()->isStructTy()) {
            result = val->getLLVMValue();
            return;
        }

        // Load the value.
        result = Builder->CreateLoad(val->getLLVMType(), val->getLLVMValue());
    } else
        throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
}

void Codegen::visit(const Else * /*node*/) {
    result = llvm::ConstantInt::getTrue(*TheContext->getContext());
}

std::string Codegen::getTypeMangledName(llvm::SMRange span, llvm::Type *type) {
    if (type->isIntegerTy(1))
        return "b";
    else if (type->isIntegerTy(8))
        return "c";
    else if (type->isIntegerTy(16))
        return "i16";
    else if (type->isIntegerTy(32))
        return "i32";
    else if (type->isIntegerTy())
        return "i";
    else if (type->isFloatTy())
        return "f32";
    else if (type->isFloatingPointTy())
        return "f";
    else if (type->isVoidTy())
        return "v";
    else if (type->isArrayTy())
        return "(arr_" + getTypeMangledName(span, type->getArrayElementType()) + ")";
    else if (type->isPointerTy())
        return "(ptr_" + getTypeMangledName(span, type->getPointerElementType()) + ")";
    else if (type->isFunctionTy()) {
        std::string param_str;
        for (unsigned int i = 0; i < type->getFunctionNumParams(); i++)
            param_str += getTypeMangledName(span, type->getFunctionParamType(i)) + "_";
        return "(func_" + param_str + ")";
    } else if (type->isStructTy()) {
        std::string param_str;
        for (unsigned int i = 0; i < type->getStructNumElements(); i++)
            param_str += getTypeMangledName(span, type->getStructElementType(i)) + "_";
        return "(struct_" + type->getStructName().str() + ")";
    }

    throw CodegenError(span, "Unknown type found during mangling");
}


bool Codegen::isMethod(const std::string &mangled_name) {
    return mangled_name.find("::") != std::string::npos;
}

std::string Codegen::getMangledName(llvm::SMRange span, std::string func_name, const std::vector<llvm::Type *> &paramTypes, bool isMethod, std::string module_alias) {
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

llvm::Type *Codegen::GetExtendedType(llvm::Type *left, llvm::Type *right) {
    if (left == right)
        return left;

    if (left->isIntegerTy() && right->isIntegerTy()) {
        if (left->getIntegerBitWidth() > right->getIntegerBitWidth())
            return left;
        else
            return right;
    } else if (left->isIntegerTy() && right->isFloatingPointTy())
        return right;
    else if (left->isFloatingPointTy() && right->isIntegerTy())
        return left;
    else if (left->isFloatingPointTy() && right->isFloatingPointTy()) {
        if (left->isFP128Ty() || right->isFP128Ty())
            return left->isFP128Ty() ? left : right;
        else if (left->isDoubleTy() || right->isDoubleTy())
            return left->isDoubleTy() ? left : right;
        else if (left->isFloatTy() || right->isFloatTy())
            return left->isFloatTy() ? left : right;
        else if (left->isHalfTy() || right->isHalfTy())
            return left->isHalfTy() ? left : right;
    }
    return nullptr;
}

lesma::Type *Codegen::getType(llvm::Type *type) {
    if (type->isIntegerTy(1))
        return new Type(BaseType::TY_BOOL);
    else if (type->isIntegerTy(8))
        return new Type(BaseType::TY_STRING);
    else if (type->isFloatingPointTy())
        return new Type(BaseType::TY_FLOAT);
    else if (type->isIntegerTy())
        return new Type(BaseType::TY_INT);
    else if (type->isFunctionTy())
        return new Type(BaseType::TY_FUNCTION);

    return new Type(BaseType::TY_INVALID);
}

llvm::Value *Codegen::Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type) {
    return Cast(span, val, type, false);
}

llvm::Value *Codegen::Cast(llvm::SMRange span, llvm::Value *val, llvm::Type *type, bool isStore) {
    // TODO: Fix me pls
    if (type == nullptr || val->getType() == type || (isStore && val->getType()->getPointerTo() == type) || (isStore && val->getType() == type->getPointerTo()) || (val->getType()->isPointerTy() && val->getType()->getPointerElementType()->isStructTy()))
        return val;

    if (type->isIntegerTy()) {
        if (val->getType()->isFloatingPointTy())
            return Builder->CreateFPToSI(val, type);
        else if (val->getType()->isIntegerTy())
            return Builder->CreateIntCast(val, type, true);
    } else if (type->isFloatingPointTy()) {
        if (val->getType()->isIntegerTy())
            return Builder->CreateSIToFP(val, type);
        else if (val->getType()->isFloatingPointTy())
            return Builder->CreateFPCast(val, type);
    } else if (type->isPointerTy() && type->getPointerElementType()->isIntegerTy()) {
        if (val->getType()->isPointerTy() && (val->getType()->getPointerElementType()->isIntegerTy() || val->getType()->getPointerElementType()->isVoidTy()))
            return Builder->CreateBitCast(val, type);
    }

    throw CodegenError(span, "Unsupported Cast");
}

llvm::Value *Codegen::genFuncCall(const FuncCall *node, const std::vector<llvm::Value *> &extra_params = {}) {
    std::vector<llvm::Value *> params;
    std::vector<llvm::Type *> paramTypes;

    for (auto arg: extra_params) {
        params.push_back(arg);
        paramTypes.push_back(params.back()->getType());
    }

    for (auto arg: node->getArguments()) {
        arg->accept(*this);
        params.push_back(result);
        paramTypes.push_back(params.back()->getType());
    }

    std::string name;
    auto selfSymbolTmp = selfSymbol;
    auto class_sym = Scope->lookup(node->getName());
    llvm::Value *class_ptr = nullptr;
    if (class_sym != nullptr && class_sym->getType()->is(TY_CLASS)) {
        // It's a class constructor, allocate and add self param
        class_ptr = Builder->CreateAlloca(class_sym->getLLVMType(), nullptr);
        params.insert(params.begin(), class_ptr);
        paramTypes.insert(paramTypes.begin(), class_ptr->getType());

        selfSymbol = class_sym;
        name = getMangledName(node->getSpan(), "new", paramTypes, true);
    } else {
        name = getMangledName(node->getSpan(), node->getName(), paramTypes, !extra_params.empty());
    }

    auto symbol = Scope->lookup(name);
    // Get function without name mangling in case of extern C functions
    symbol = symbol == nullptr ? Scope->lookup(node->getName()) : symbol;

    if (symbol == nullptr)
        throw CodegenError(node->getSpan(), "Function {} not in current scope.", node->getName());

    if (!symbol->getLLVMType()->isFunctionTy())
        throw CodegenError(node->getSpan(), "Symbol {} is not a function.", node->getName());

    auto *func = cast<Function>(symbol->getLLVMValue());
    if (class_sym != nullptr && class_sym->getType()->is(TY_CLASS)) {
        Builder->CreateCall(func, params);
        auto val = Builder->CreateLoad(class_sym->getLLVMType(), class_ptr);
        selfSymbol = selfSymbolTmp;

        return val;
    }
    return Builder->CreateCall(func, params);
}

int Codegen::FindIndexInFields(Type *_struct, const std::string &field) {
    int val = -1;
    for (unsigned int i = 0; i < _struct->getFields().size(); i++) {
        if (_struct->getFields()[i]->name == field) {
            val = static_cast<int>(i);
            break;
        }
    }

    return val;
}