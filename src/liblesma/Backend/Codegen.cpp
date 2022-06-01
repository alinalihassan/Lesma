#include "Codegen.h"

using namespace lesma;

Codegen::Codegen(std::unique_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string &filename, std::vector<std::string> imports, bool jit, bool main) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    ImportedModules = std::move(imports);
    TheJIT = ExitOnErr(LesmaJIT::Create());
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("Lesma JIT", *TheContext);
    TheModule->setDataLayout(TheJIT->getDataLayout());
    TheModule->setSourceFileName(filename);
    Builder = std::make_unique<IRBuilder<>>(*TheContext);
    Parser_ = std::move(parser);
    SourceManager = std::move(srcMgr);
    Scope = new SymbolTable(nullptr);
    TargetMachine = InitializeTargetMachine();

    this->filename = filename;
    isJIT = jit;
    isMain = main;

    TopLevelFunc = InitializeTopLevel();

    // If it's not base.les stdlib, then import it
    if (std::filesystem::absolute(filename) != getStdDir() + "base.les") {
        CompileModule(llvm::SMRange(), getStdDir() + "base.les", true);
    }
}

llvm::Function *Codegen::InitializeTopLevel() {
    std::vector<llvm::Type *> paramTypes = {};

    FunctionType *FT = FunctionType::get(Builder->getInt64Ty(), paramTypes, false);
    Function *F = Function::Create(FT, isMain ? Function::ExternalLinkage : Function::InternalLinkage, "main", *TheModule);

    auto entry = BasicBlock::Create(*TheContext, "entry", F);
    Builder->SetInsertPoint(entry);

    return F;
}

void Codegen::defineFunction(Function *F, FuncDecl *node, SymbolTableEntry *clsSymbol) {
    Scope = Scope->createChildBlock(node->getName());
    deferStack.push({});

    BasicBlock *entry = BasicBlock::Create(*TheContext, "entry", F);
    Builder->SetInsertPoint(entry);

    for (auto &param: F->args()) {
        if (clsSymbol != nullptr && param.getArgNo() == 0)
            param.setName("self");
        else
            param.setName(node->getParameters()[param.getArgNo() - (clsSymbol != nullptr ? 1 : 0)].first);

        llvm::Value *ptr;
        ptr = Builder->CreateAlloca(param.getType(), nullptr, param.getName() + "_ptr");
        Builder->CreateStore(&param, ptr);

        auto symbol = new SymbolTableEntry(param.getName().str(), getType(param.getType()));
        symbol->setLLVMType(param.getType());
        symbol->setLLVMValue(ptr);
        Scope->insertSymbol(symbol);
    }

    visit(node->getBody());

    auto instrs = deferStack.top();
    deferStack.pop();

    if (!isReturn)
        for (auto inst: instrs)
            visit(inst);

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
    llvm::Optional rm = llvm::Optional<llvm::Reloc::Model>();
    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(tripletString, "generic", "", opt, rm));
    return target_machine;
}

void Codegen::CompileModule(llvm::SMRange span, const std::string &filepath, bool isStd) {
    std::filesystem::path mainPath = filename;
    // Read source
    auto absolute_path = isStd ? filepath : fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath);

    // If module is already imported, don't compile again
    if (std::find(ImportedModules.begin(), ImportedModules.end(), absolute_path) != ImportedModules.end())
        return;

    auto buffer = MemoryBuffer::getFile(absolute_path);
    if (buffer.getError() != std::error_code())
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
        auto codegen = new Codegen(std::move(parser), SourceManager, absolute_path, ImportedModules, isJIT, false);
        codegen->Run();

        // Optimize
        codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
        codegen->TheModule->setModuleIdentifier(filepath);

        ImportedModules = std::move(codegen->ImportedModules);

        if (isJIT) {
            // Add classes and enums
            for (auto sym: codegen->Scope->getSymbols()) {
                if (sym.second->getType()->isOneOf({TY_ENUM, TY_CLASS})) {
                    // TODO: We have to reconstruct classes and enums, fix me
                    auto struct_ty = cast<llvm::StructType>(sym.second->getLLVMType());
                    llvm::StructType *structType = llvm::StructType::create(*TheContext, struct_ty->elements(), sym.first);

                    auto *structSymbol = new SymbolTableEntry(sym.first, sym.second->getType());
                    structSymbol->setLLVMType(structType);
                    Scope->insertType(sym.first, sym.second->getType());
                    Scope->insertSymbol(structSymbol);
                }
            }

            // Link modules together
            if (Linker::linkModules(*TheModule, std::move(codegen->TheModule), (1 << 0)))
                throw CodegenError({}, "Error linking modules together");

            //  Add function to main module
            Module::iterator it;
            Module::iterator end = TheModule->end();
            for (it = TheModule->begin(); it != end; ++it) {
                // If it's external function (only functions without body) don't import
                if ((*it).getBasicBlockList().empty())
                    continue;

                auto name = std::string{(*it).getName()};
                auto symbol = new SymbolTableEntry(name, new SymbolType(SymbolSuperType::TY_FUNCTION));
                symbol->setLLVMType((*it).getFunctionType());
                symbol->setLLVMValue((Value *) &(*it).getFunction());
                Scope->insertSymbol(symbol);
            }
        } else {
            std::string obj_file = fmt::format("tmp{}", ObjectFiles.size());
            codegen->WriteToObjectFile(obj_file);
            ObjectFiles.push_back(fmt::format("{}.o", obj_file));

            // Add function definitions
            Module::iterator it;
            Module::iterator end = codegen->TheModule->end();
            for (it = codegen->TheModule->begin(); it != end; ++it) {
                auto name = std::string{(*it).getName()};
                auto new_func = Function::Create((*it).getFunctionType(),
                                                 Function::ExternalLinkage,
                                                 name,
                                                 *TheModule);

                auto symbol = new SymbolTableEntry(name, new SymbolType(SymbolSuperType::TY_FUNCTION));
                symbol->setLLVMType(new_func->getFunctionType());
                symbol->setLLVMValue(new_func);
                Scope->insertSymbol(symbol);
            }
        }
    } catch (const LesmaError &err) {
        if (!err.getSpan().isValid())
            print(ERROR, err.what());
        else
            showInline(SourceManager.get(), err.getSpan(), err.what(), absolute_path, true);

        throw CodegenError(span, "Unable to import {} due to errors", filepath);
    }
}

void Codegen::Optimize(llvm::PassBuilder::OptimizationLevel opt) {
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
    auto jit_error = TheJIT->addModule(ThreadSafeModule(std::move(TheModule), std::move(TheContext)));
    if (jit_error)
        throw CodegenError({}, "JIT Error:\n{}");

    using MainFnTy = int();
    auto jit_main = jitTargetAddressToFunction<MainFnTy *>(TheJIT->lookup(TopLevelFunc->getName())->getAddress());
    return jit_main();
}

void Codegen::Run() {
    deferStack.push({});
    visit(Parser_->getAST());

    auto instrs = deferStack.top();
    deferStack.pop();

    // Visit all statements
    for (auto inst: instrs)
        visit(inst);

    // Define the function bodies
    for (auto prot: Prototypes)
        defineFunction(std::get<0>(prot), std::get<1>(prot), std::get<2>(prot));

    // Return 0 for top-level function
    Builder->CreateRet(ConstantInt::getSigned(Builder->getInt64Ty(), 0));
}

void Codegen::Dump() {
    TheModule->print(outs(), nullptr);
}

llvm::Value *Codegen::visit(Expression *node) {
    if (dynamic_cast<FuncCall *>(node))
        return visit(dynamic_cast<FuncCall *>(node));
    else if (dynamic_cast<BinaryOp *>(node))
        return visit(dynamic_cast<BinaryOp *>(node));
    else if (dynamic_cast<DotOp *>(node))
        return visit(dynamic_cast<DotOp *>(node));
    else if (dynamic_cast<CastOp *>(node))
        return visit(dynamic_cast<CastOp *>(node));
    else if (dynamic_cast<IsOp *>(node))
        return visit(dynamic_cast<IsOp *>(node));
    else if (dynamic_cast<UnaryOp *>(node))
        return visit(dynamic_cast<UnaryOp *>(node));
    else if (dynamic_cast<Literal *>(node))
        return visit(dynamic_cast<Literal *>(node));
    else if (dynamic_cast<Else *>(node))
        return visit(dynamic_cast<Else *>(node));

    throw CodegenError(node->getSpan(), "Unknown Expression: {}", node->toString(SourceManager.get(), "", true));
}

void Codegen::visit(Statement *node) {
    if (dynamic_cast<VarDecl *>(node))
        return visit(dynamic_cast<VarDecl *>(node));
    else if (dynamic_cast<If *>(node))
        return visit(dynamic_cast<If *>(node));
    else if (dynamic_cast<While *>(node))
        return visit(dynamic_cast<While *>(node));
    else if (dynamic_cast<FuncDecl *>(node))
        return visit(dynamic_cast<FuncDecl *>(node));
    else if (dynamic_cast<Import *>(node))
        return visit(dynamic_cast<Import *>(node));
    else if (dynamic_cast<Class *>(node))
        return visit(dynamic_cast<Class *>(node));
    else if (dynamic_cast<Enum *>(node))
        return visit(dynamic_cast<Enum *>(node));
    else if (dynamic_cast<ExternFuncDecl *>(node))
        return visit(dynamic_cast<ExternFuncDecl *>(node));
    else if (dynamic_cast<Assignment *>(node))
        return visit(dynamic_cast<Assignment *>(node));
    else if (dynamic_cast<Break *>(node))
        return visit(dynamic_cast<Break *>(node));
    else if (dynamic_cast<Continue *>(node))
        return visit(dynamic_cast<Continue *>(node));
    else if (dynamic_cast<Return *>(node))
        return visit(dynamic_cast<Return *>(node));
    else if (dynamic_cast<Defer *>(node))
        return visit(dynamic_cast<Defer *>(node));
    else if (dynamic_cast<ExpressionStatement *>(node))
        return visit(dynamic_cast<ExpressionStatement *>(node));
    else if (dynamic_cast<Compound *>(node))
        return visit(dynamic_cast<Compound *>(node));

    throw CodegenError(node->getSpan(), "Unknown Statement:\n{}", node->toString(SourceManager.get(), "", true));
}

llvm::Type *Codegen::visit(lesma::Type *node) {
    if (node->getType() == TokenType::INT_TYPE)
        return Builder->getInt64Ty();
    if (node->getType() == TokenType::INT8_TYPE)
        return Builder->getInt8Ty();
    if (node->getType() == TokenType::INT16_TYPE)
        return Builder->getInt16Ty();
    if (node->getType() == TokenType::INT32_TYPE)
        return Builder->getInt32Ty();
    if (node->getType() == TokenType::FLOAT_TYPE)
        return Builder->getDoubleTy();
    if (node->getType() == TokenType::FLOAT32_TYPE)
        return Builder->getFloatTy();
    else if (node->getType() == TokenType::BOOL_TYPE)
        return Builder->getInt1Ty();
    else if (node->getType() == TokenType::STRING_TYPE)
        return Builder->getInt8PtrTy();
    else if (node->getType() == TokenType::VOID_TYPE)
        return Builder->getVoidTy();
    else if (node->getType() == TokenType::PTR_TYPE)
        return PointerType::get(visit(node->getElementType()), 0);
    else if (node->getType() == TokenType::FUNC_TYPE) {
        auto ret_type = visit(node->getReturnType());
        std::vector<llvm::Type *> paramsTypes;
        for (auto param_type: node->getParams())
            paramsTypes.push_back(visit(param_type));

        return FunctionType::get(ret_type, paramsTypes, false)->getPointerTo();
    } else if (node->getType() == TokenType::CUSTOM_TYPE) {
        auto typ = Scope->lookupType(node->getName());
        auto sym = Scope->lookup(node->getName());
        if (typ == nullptr || sym->getLLVMType() == nullptr)
            throw CodegenError(node->getSpan(), "Type not found: {}", node->getName());

        return sym->getLLVMType()->getPointerTo();
    }

    throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
}

void Codegen::visit(Compound *node) {
    for (auto elem: node->getChildren())
        visit(elem);
}

void Codegen::visit(VarDecl *node) {
    llvm::Type *type;
    llvm::Value *value;

    if (node->getValue().has_value()) {
        value = visit(node->getValue().value());
        type = value->getType();
    }

    if (node->getType().has_value())
        type = visit(node->getType().value());

    auto ptr = Builder->CreateAlloca(type, nullptr, node->getIdentifier()->getValue());

    auto symbol = new SymbolTableEntry(node->getIdentifier()->getValue(), getType(type), node->getType().has_value() ? INITIALIZED : DECLARED);
    symbol->setLLVMType(type);
    symbol->setLLVMValue(ptr);
    symbol->setMutable(node->getMutability());
    Scope->insertSymbol(symbol);

    if (node->getValue().has_value())
        Builder->CreateStore(Cast(node->getSpan(), value, ptr->getAllocatedType(), true), ptr);
}

void Codegen::visit(If *node) {
    auto parentFct = Builder->GetInsertBlock()->getParent();
    auto bStart = llvm::BasicBlock::Create(*TheContext, "if.start");
    auto bEnd = llvm::BasicBlock::Create(*TheContext, "if.end");

    Builder->CreateBr(bStart);
    parentFct->getBasicBlockList().push_back(bStart);
    Builder->SetInsertPoint(bStart);

    for (unsigned long i = 0; i < node->getConds().size(); i++) {
        auto bIfTrue = llvm::BasicBlock::Create(*TheContext, "if.true");
        parentFct->getBasicBlockList().push_back(bIfTrue);
        auto bIfFalse = bEnd;
        if (i + 1 < node->getConds().size()) {
            bIfFalse = llvm::BasicBlock::Create(*TheContext, "if.false");
            parentFct->getBasicBlockList().push_back(bIfFalse);
        }

        auto cond = visit(node->getConds().at(i));
        Builder->CreateCondBr(cond, bIfTrue, bIfFalse);
        Builder->SetInsertPoint(bIfTrue);

        Scope = Scope->createChildBlock("if");
        visit(node->getBlocks().at(i));

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

    parentFct->getBasicBlockList().push_back(bEnd);

    if (!isBreak)
        Builder->SetInsertPoint(bEnd);
    else
        isBreak = false;
}

void Codegen::visit(While *node) {
    Scope = Scope->createChildBlock("while");

    llvm::Function *parentFct = Builder->GetInsertBlock()->getParent();

    // Create blocks
    llvm::BasicBlock *bCond = llvm::BasicBlock::Create(*TheContext, "while.cond");
    llvm::BasicBlock *bLoop = llvm::BasicBlock::Create(*TheContext, "while");
    llvm::BasicBlock *bEnd = llvm::BasicBlock::Create(*TheContext, "while.end");

    breakBlocks.push(bEnd);
    continueBlocks.push(bCond);

    // Jump into condition block
    Builder->CreateBr(bCond);

    // Fill condition block
    parentFct->getBasicBlockList().push_back(bCond);
    Builder->SetInsertPoint(bCond);
    auto condition = visit(node->getCond());
    Builder->CreateCondBr(condition, bLoop, bEnd);

    // Fill while body block
    parentFct->getBasicBlockList().push_back(bLoop);
    Builder->SetInsertPoint(bLoop);
    visit(node->getBlock());

    if (!isBreak)
        Builder->CreateBr(bCond);
    else
        isBreak = false;

    // Fill loop end block
    parentFct->getBasicBlockList().push_back(bEnd);
    Builder->SetInsertPoint(bEnd);

    Scope = Scope->getParent();
    breakBlocks.pop();
    continueBlocks.pop();
}

void Codegen::visit(FuncDecl *node) {
    if (selfSymbol != nullptr && node->getName() == "new" && node->getReturnType()->getType() != TokenType::VOID_TYPE)
        throw CodegenError(node->getSpan(), "Cannot create class method new with return type {}", node->getReturnType()->getName());

    std::vector<llvm::Type *> paramTypes;

    if (selfSymbol != nullptr) {
        paramTypes.push_back(selfSymbol->getLLVMType()->getPointerTo());
    }

    for (const auto &param: node->getParameters())
        paramTypes.push_back(visit(param.second));

    auto name = getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
    auto linkage = Function::ExternalLinkage;

    FunctionType *FT = FunctionType::get(visit(node->getReturnType()), paramTypes, node->getVarArgs());
    Function *F = Function::Create(FT, linkage, name, *TheModule);

    auto func_symbol = new SymbolTableEntry(name, new SymbolType(SymbolSuperType::TY_FUNCTION));
    func_symbol->setLLVMType(F->getFunctionType());
    func_symbol->setLLVMValue(F);
    Scope->insertSymbol(func_symbol);

    Prototypes.emplace_back(F, node, selfSymbol);
}

void Codegen::visit(ExternFuncDecl *node) {
    std::vector<llvm::Type *> paramTypes;

    for (const auto &param: node->getParameters())
        paramTypes.push_back(visit(param.second));

    FunctionCallee F;
    if (TheModule->getFunction(node->getName()) != nullptr && Scope->lookup(node->getName()) != nullptr)
        return;
    else if (TheModule->getFunction(node->getName()) != nullptr) {
        F = TheModule->getFunction(node->getName());
    } else {
        FunctionType *FT = FunctionType::get(visit(node->getReturnType()), paramTypes, node->getVarArgs());
        F = TheModule->getOrInsertFunction(node->getName(), FT);
    }

    auto symbol = new SymbolTableEntry(node->getName(), new SymbolType(SymbolSuperType::TY_FUNCTION));
    symbol->setLLVMType(F.getFunctionType());
    symbol->setLLVMValue(F.getCallee());
    Scope->insertSymbol(symbol);
}

void Codegen::visit(Assignment *node) {
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
        lhs_val = visit(node->getLeftHandSide());
        lhs_type = lhs_val->getType();
    } else {
        throw CodegenError(node->getSpan(), "Unable to assign {} to {}", node->getRightHandSide()->toString(SourceManager.get(), "", true), node->getLeftHandSide()->toString(SourceManager.get(), "", true));
    }
    isAssignment = false;

    auto value = visit(node->getRightHandSide());
    value = Cast(node->getSpan(), value, lhs_type, true);
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

void Codegen::visit(Break *node) {
    if (breakBlocks.empty())
        throw CodegenError(node->getSpan(), "Cannot break without being in a loop");

    auto block = breakBlocks.top();
    breakBlocks.pop();
    isBreak = true;

    Builder->CreateBr(block);
}

void Codegen::visit(Continue *node) {
    if (continueBlocks.empty())
        throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");

    auto block = continueBlocks.top();
    continueBlocks.pop();
    isBreak = true;

    Builder->CreateBr(block);
}

void Codegen::visit(Return *node) {
    // Check if it's top-level
    if (Builder->GetInsertBlock()->getParent() == TopLevelFunc)
        throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");

    // Execute all deferred statements
    for (auto inst: deferStack.top())
        visit(inst);

    isReturn = true;

    if (node->getValue() == nullptr) {
        if (Builder->getCurrentFunctionReturnType() == Builder->getVoidTy())
            Builder->CreateRetVoid();
        else
            throw CodegenError(node->getSpan(), "Return type does not match the function return type");
    } else {
        auto val = visit(node->getValue());
        if (Builder->getCurrentFunctionReturnType() == val->getType())
            Builder->CreateRet(val);
        else
            throw CodegenError(node->getSpan(), "Return type does not match the function return type");
    }
}

void Codegen::visit(Defer *node) {
    deferStack.top().push_back(node->getStatement());
}

void Codegen::visit(ExpressionStatement *node) {
    visit(node->getExpression());
}

void Codegen::visit(Import *node) {
    CompileModule(node->getSpan(), node->getFilePath(), node->isStd());
}

void Codegen::visit(Class *node) {
    std::vector<llvm::Type *> elementTypes = {};
    for (auto field: node->getFields()) {
        auto typ = field->getType().has_value() ? visit(field->getType().value()) : visit(field->getValue().value())->getType();
        elementTypes.push_back(typ);
    }

    llvm::StructType *structType = llvm::StructType::create(*TheContext, elementTypes, node->getIdentifier());

    std::vector<std::tuple<std::string, SymbolType *>> fields;

    for (auto &&[field, elem_type]: zip(node->getFields(), elementTypes))
        fields.emplace_back(field->getIdentifier()->getValue(), getType(elem_type));

    auto *type = new SymbolType(TY_CLASS, fields, nullptr);
    auto *structSymbol = new SymbolTableEntry(node->getIdentifier(), type);
    structSymbol->setLLVMType(structType);
    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);

    selfSymbol = structSymbol;
    auto has_constructor = false;
    for (auto func: node->getMethods()) {
        if (func->getName() == "new")
            has_constructor = true;
        visit(func);
    }

    if (!has_constructor)
        throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());

    selfSymbol = nullptr;
}

void Codegen::visit(Enum *node) {
    std::vector<llvm::Type *> elementTypes = {Builder->getInt8Ty()};
    llvm::StructType *structType = llvm::StructType::create(*TheContext, elementTypes, node->getIdentifier());
    std::vector<std::tuple<std::string, SymbolType *>> fields;

    for (const auto &field: node->getValues())
        fields.push_back({field, new SymbolType(TY_VOID)});

    auto *type = new SymbolType(TY_ENUM, fields, nullptr);
    auto *structSymbol = new SymbolTableEntry(node->getIdentifier(), type);
    structSymbol->setLLVMType(structType);
    Scope->insertType(node->getIdentifier(), type);
    Scope->insertSymbol(structSymbol);
}

llvm::Value *Codegen::visit(FuncCall *node) {
    return genFuncCall(node, {});
}

llvm::Value *Codegen::visit(BinaryOp *node) {
    llvm::Value *left = visit(node->getLeft());
    llvm::Value *right = visit(node->getRight());
    llvm::Type *finalType = GetExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFSub(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateSub(left, right);
            break;
        case TokenType::PLUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFAdd(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateAdd(left, right);
            break;
        case TokenType::STAR:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFMul(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateMul(left, right);
            break;
        case TokenType::SLASH:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFDiv(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateSDiv(left, right);
            break;
        case TokenType::MOD:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFRem(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateSRem(left, right);
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
                    if (left->getType()->getPointerElementType() != right->getType()->getPointerElementType())
                        return ConstantInt::getBool(*TheContext, false);
                    else {
                        auto left_gep = Builder->CreateStructGEP(sym->getLLVMType(), left, 0);
                        auto left_load = Builder->CreateLoad(Builder->getInt8Ty(), left_gep);
                        auto right_gep = Builder->CreateStructGEP(sym->getLLVMType(), right, 0);
                        auto right_load = Builder->CreateLoad(Builder->getInt8Ty(), right_gep);
                        return Builder->CreateICmpEQ(left_load, right_load);
                    }
                }

                // It's not an enum, it's a class
            }
            else if (left->getType()->isPointerTy() && right->getType()->isPointerTy())
                return Builder->CreateICmpEQ(left, right);
            else if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOEQ(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpEQ(left, right);
            else if (finalType->isPointerTy() && finalType->getPointerElementType()->isStructTy()) {
                auto struct_ty = Scope->lookupType(finalType->getPointerElementType()->getStructName().str());
                if (struct_ty->is(TY_ENUM)) {
                    auto left_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), left);
                    auto right_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), right);
                    auto left_val = Builder->CreateExtractValue(left_ptr, {0});
                    auto right_val = Builder->CreateExtractValue(right_ptr, {0});
                    return Builder->CreateICmpEQ(left_val, right_val);
                }
            }
            break;
        case TokenType::BANG_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr && (left->getType()->isPointerTy() && left->getType()->getPointerElementType()->isStructTy()) && (right->getType()->isPointerTy() && right->getType()->getPointerElementType()->isStructTy()))
                return ConstantInt::getBool(*TheContext, true);
            else if (left->getType()->isPointerTy() && right->getType()->isPointerTy())
                return Builder->CreateICmpNE(left, right);
            else if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpONE(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpNE(left, right);
            else if (finalType->isPointerTy() && finalType->getPointerElementType()->isStructTy()) {
                auto struct_ty = Scope->lookupType(finalType->getPointerElementType()->getStructName().str());
                if (struct_ty->is(TY_ENUM)) {
                    auto left_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), left);
                    auto right_ptr = Builder->CreateLoad(left->getType()->getPointerElementType(), right);
                    auto left_val = Builder->CreateExtractValue(left_ptr, {0});
                    auto right_val = Builder->CreateExtractValue(right_ptr, {0});
                    return Builder->CreateICmpNE(left_val, right_val);
                }
            }
            break;
        case TokenType::GREATER:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGT(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGT(left, right);
            break;
        case TokenType::GREATER_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGE(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGE(left, right);
            break;
        case TokenType::LESS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLT(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLT(left, right);
            break;
        case TokenType::LESS_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType == nullptr)
                break;
            else if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLE(left, right);
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLE(left, right);
            break;
        case TokenType::AND:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            return Builder->CreateLogicalAnd(left, right);
        case TokenType::OR:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                                   node->getLeft()->toString(SourceManager.get(), "", true), node->getRight()->toString(SourceManager.get(), "", true));

            return Builder->CreateLogicalOr(left, right);
        default:
            throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}", NAMEOF_ENUM(node->getOperator()));
    }

    throw CodegenError(node->getSpan(),
                       "Unimplemented binary operator {} for {} and {}",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getLeft()->toString(SourceManager.get(), "", true),
                       node->getRight()->toString(SourceManager.get(), "", true));
}

llvm::Value *Codegen::visit(DotOp *node) {
    if (auto left = dynamic_cast<Literal *>(node->getLeft())) {
        if (left->getType() != TokenType::IDENTIFIER)
            throw CodegenError(node->getLeft()->getSpan(), "Expected identifier left-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

        auto _enum = Scope->lookupType(left->getValue());
        if (_enum != nullptr) {
            // Assuming it's an enum or statically accessed class
            if (!_enum->isOneOf({TY_ENUM, TY_CLASS}))
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());

            // Check if right-hand expression is an identifier expression
            if (!dynamic_cast<Literal *>(node->getRight()))
                throw CodegenError(node->getRight()->getSpan(), "Expected identifier right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

            auto right = dynamic_cast<Literal *>(node->getRight());
            if (right->getType() != TokenType::IDENTIFIER)
                throw CodegenError(node->getRight()->getSpan(), "Expected identifier right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

            if (_enum->is(TY_ENUM)) {
                // Setting value to the enum
                auto val = FindIndexInFields(_enum, right->getValue());
                // Field not found in enum
                if (val == -1)
                    throw CodegenError(node->getLeft()->getSpan(), "Identifier {} not in {}", right->getValue(), left->getValue());

                auto struct_ty = Scope->lookup(left->getValue());
                auto enum_ptr = Builder->CreateAlloca(struct_ty->getLLVMType());
                auto field = Builder->CreateStructGEP(struct_ty->getLLVMType(), enum_ptr, 0);
                Builder->CreateStore(Builder->getInt8(val), field);

                return enum_ptr;
            }
        } else {
            // Assuming it's a class
            auto val = visit(left);
            if (!(val->getType()->isPointerTy() && val->getType()->getPointerElementType()->isStructTy()))
                throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}", left->getValue());

            std::string field;
            FuncCall *method;

            if (!dynamic_cast<Literal *>(node->getRight()) && !dynamic_cast<FuncCall *>(node->getRight()))
                throw CodegenError(node->getRight()->getSpan(), "Expected identifier or method call right-hand of dot operator, found {}", node->getRight()->toString(SourceManager.get(), "", true));

            if (dynamic_cast<Literal *>(node->getRight()) && dynamic_cast<Literal *>(node->getRight())->getType() == TokenType::IDENTIFIER)
                field = dynamic_cast<Literal *>(node->getRight())->getValue();
            else
                method = dynamic_cast<FuncCall *>(node->getRight());

            auto cls = Scope->lookup(val->getType()->getPointerElementType()->getStructName().str());
            if (cls->getType()->is(TY_CLASS)) {
                if (!field.empty()) {
                    auto index = FindIndexInFields(cls->getType(), field);
                    if (index == -1)
                        throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field, val->getType()->getPointerElementType()->getStructName().str());

                    auto ptr = Builder->CreateStructGEP(cls->getLLVMType(), val, index);
                    if (isAssignment)
                        return ptr;
                    auto x = cls->getType()->getFields()[index];
                    return Builder->CreateLoad(ptr->getType()->getPointerElementType(), ptr);
                } else if (method != nullptr) {
                    selfSymbol = cls;
                    auto ret_val = genFuncCall(method, {val});
                    selfSymbol = nullptr;
                    return ret_val;
                }
            }
        }
    }
    throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}", node->toString(SourceManager.get(), "", true));
}

llvm::Value *Codegen::visit(CastOp *node) {
    return Cast(node->getSpan(), visit(node->getExpression()), visit(node->getType()));
}

llvm::Value *Codegen::visit(IsOp *node) {
    if (node->getOperator() == TokenType::IS)
        return visit(node->getLeft())->getType() == visit(node->getRight()) ? Builder->getTrue() : Builder->getFalse();
    else
        return visit(node->getLeft())->getType() == visit(node->getRight()) ? Builder->getFalse() : Builder->getTrue();
}

llvm::Value *Codegen::visit(UnaryOp *node) {
    Value *operand = visit(node->getExpression());

    if (node->getOperator() == TokenType::MINUS) {
        if (operand->getType()->isIntegerTy())
            return Builder->CreateNeg(operand);
        else if (operand->getType()->isFloatingPointTy())
            return Builder->CreateFNeg(operand);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::NOT) {
        if (operand->getType()->isIntegerTy(1))
            return Builder->CreateNot(operand);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::STAR) {
        if (operand->getType()->isPointerTy())
            return Builder->CreateLoad(operand->getType()->getPointerElementType(), operand);
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
    } else if (node->getOperator() == TokenType::AMPERSAND) {
        auto ptr = Builder->CreateAlloca(operand->getType(), nullptr);
        Builder->CreateStore(operand, ptr);
        return ptr;
    } else
        throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(SourceManager.get(), "", true));
}

llvm::Value *Codegen::visit(Literal *node) {
    if (node->getType() == TokenType::DOUBLE)
        return ConstantFP::get(*TheContext, APFloat(std::stod(node->getValue())));
    else if (node->getType() == TokenType::INTEGER)
        return ConstantInt::getSigned(Builder->getInt64Ty(), std::stoi(node->getValue()));
    else if (node->getType() == TokenType::BOOL)
        return node->getValue() == "true" ? Builder->getTrue() : Builder->getFalse();
    else if (node->getType() == TokenType::STRING)
        return Builder->CreateGlobalStringPtr(node->getValue());
    else if (node->getType() == TokenType::NIL)
        return ConstantPointerNull::getNullValue(Builder->getInt8PtrTy(0));
    else if (node->getType() == TokenType::IDENTIFIER) {
        // Look this variable up in the function.
        auto val = Scope->lookup(node->getValue());
        if (val == nullptr)
            throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());

        // If it's a struct, don't load the value
        if (val->getLLVMType()->isStructTy())
            return val->getLLVMValue();

        // Load the value.
        return Builder->CreateLoad(val->getLLVMType(), val->getLLVMValue());
    } else
        throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
}

llvm::Value *Codegen::visit(Else * /*node*/) {
    return llvm::ConstantInt::getTrue(*TheContext);
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

// TODO: Change to support private/public and module system
std::string Codegen::getMangledName(llvm::SMRange span, std::string func_name, const std::vector<llvm::Type *> &paramTypes, bool isMethod) {
    std::string name = selfSymbol != nullptr && isMethod ? selfSymbol->getName() + "::" + std::move(func_name) + ":" : "_" + std::move(func_name) + ":";
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

SymbolType *Codegen::getType(llvm::Type *type) {
    if (type->isIntegerTy(1))
        return new SymbolType(SymbolSuperType::TY_BOOL);
    else if (type->isIntegerTy(8))
        return new SymbolType(SymbolSuperType::TY_STRING);
    else if (type->isFloatingPointTy())
        return new SymbolType(SymbolSuperType::TY_FLOAT);
    else if (type->isIntegerTy())
        return new SymbolType(SymbolSuperType::TY_INT);
    else if (type->isFunctionTy())
        return new SymbolType(SymbolSuperType::TY_FUNCTION);

    return new SymbolType(SymbolSuperType::TY_INVALID);
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

llvm::Value *Codegen::genFuncCall(FuncCall *node, const std::vector<llvm::Value *> &extra_params = {}) {
    std::vector<llvm::Value *> params;
    std::vector<llvm::Type *> paramTypes;

    for (auto arg: extra_params) {
        params.push_back(arg);
        paramTypes.push_back(params.back()->getType());
    }

    for (auto arg: node->getArguments()) {
        params.push_back(visit(arg));
        paramTypes.push_back(params.back()->getType());
    }

    std::string name;
    auto selfSymbolTmp = selfSymbol;
    auto class_sym = Scope->lookup(node->getName());
    Value *class_ptr = nullptr;
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

int Codegen::FindIndexInFields(SymbolType *_struct, const std::string &field) {
    int val = -1;
    for (unsigned int i = 0; i < _struct->getFields().size(); i++) {
        if (std::get<0>(_struct->getFields()[i]) == field) {
            val = static_cast<int>(i);
            break;
        }
    }

    return val;
}