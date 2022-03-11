#include "Codegen.h"

using namespace lesma;

Codegen::Codegen(std::unique_ptr<Parser> parser, const std::string &filename, bool jit, bool main) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    TheJIT = ExitOnErr(LesmaJIT::Create());
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("Lesma JIT", *TheContext);
    TheModule->setDataLayout(TheJIT->getDataLayout());
    TheModule->setSourceFileName(filename);
    Builder = std::make_unique<IRBuilder<>>(*TheContext);
    Parser_ = std::move(parser);
    Scope = new SymbolTable(nullptr);
    TargetMachine = InitializeTargetMachine();

    this->filename = filename;
    isJIT = jit;
    isMain = main;

    TopLevelFunc = InitializeTopLevel();
}

llvm::Function *Codegen::InitializeTopLevel() {
    std::vector<llvm::Type *> paramTypes = {};

    FunctionType *FT = FunctionType::get(Builder->getInt64Ty(), paramTypes, false);
    Function *F = Function::Create(FT, isMain ? Function::ExternalLinkage : Function::InternalLinkage, "main", *TheModule);

    auto entry = BasicBlock::Create(*TheContext, "entry", F);
    Builder->SetInsertPoint(entry);

    return F;
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

void Codegen::CompileModule(Span span, const std::string &filepath) {
    std::filesystem::path mainPath = filename;
    // Read source
    auto source = readFile(fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath));

    try {
        // Lexer
        auto lexer = std::make_unique<Lexer>(source, filepath.substr(filepath.find_last_of("/\\") + 1));
        lexer->ScanAll();

        // Parser
        auto parser = std::make_unique<Parser>(lexer->getTokens());
        parser->Parse();

        // TODO: Delete it, memory leak, smart pointer made us lose the references to other modules
        // Codegen
        auto codegen = new Codegen(std::move(parser), filepath, isJIT, false);
        codegen->Run();

        // Optimize
        codegen->Optimize(llvm::PassBuilder::OptimizationLevel::O3);
        codegen->TheModule->setModuleIdentifier(filepath);

        if (isJIT) {
            // Link modules together
            if (Linker::linkModules(*TheModule, std::move(codegen->TheModule)))
                throw CodegenError({}, "Error linking modules together");

            //  Add function to main module
            Module::iterator it;
            Module::iterator end = TheModule->end();
            for (it = TheModule->begin(); it != end; ++it) {
                auto name = std::string{(*it).getName()};
                if (!Scope->lookup(name))
                    Scope->insertSymbol(name, (Value *) &(*it).getFunction(), (*it).getFunctionType());
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

                Scope->insertSymbol(name, new_func, new_func->getFunctionType());
            }
        }
    } catch (const LesmaError &err) {
        if (err.getSpan() == Span{})
            print(ERROR, err.what());
        else
            showInline(err.getSpan(), err.what(), fmt::format("{}/{}", std::filesystem::absolute(mainPath).parent_path().c_str(), filepath), true);

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
    visit(Parser_->getAST());

    // Return void for top-level function
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
    else if (dynamic_cast<CastOp *>(node))
        return visit(dynamic_cast<CastOp *>(node));
    else if (dynamic_cast<UnaryOp *>(node))
        return visit(dynamic_cast<UnaryOp *>(node));
    else if (dynamic_cast<Literal *>(node))
        return visit(dynamic_cast<Literal *>(node));
    else if (dynamic_cast<Else *>(node))
        return visit(dynamic_cast<Else *>(node));

    throw CodegenError(node->getSpan(), "Unknown Expression: {}", node->toString(0));
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

    throw CodegenError(node->getSpan(), "Unknown Statement:\n{}", node->toString(0));
}

llvm::Type *Codegen::visit(lesma::Type *node) {
    if (node->getType() == TokenType::INT_TYPE)
        return Builder->getInt64Ty();
    if (node->getType() == TokenType::FLOAT_TYPE)
        return Builder->getDoubleTy();
    else if (node->getType() == TokenType::BOOL_TYPE)
        return Builder->getInt1Ty();
    else if (node->getType() == TokenType::VOID_TYPE)
        return Builder->getVoidTy();

    throw CodegenError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
}

void Codegen::visit(Compound *node) {
    for (auto elem: node->getChildren())
        visit(elem);
}

void Codegen::visit(VarDecl *node) {
    llvm::Type *type;
    llvm::Value *value;
    if (node->getType().has_value())
        type = visit(node->getType().value());
    else {
        value = visit(node->getValue().value());
        type = value->getType();
    }

    auto ptr = Builder->CreateAlloca(type, nullptr, node->getIdentifier()->getValue());

    Scope->insertSymbol(node->getIdentifier()->getValue(), ptr, type, node->getMutability());

    if (node->getValue().has_value())
        Builder->CreateStore(Cast(node->getSpan(), visit(node->getValue().value()), ptr->getAllocatedType()), ptr);
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

        // TODO: Handle returns and breaks
        if (!isBreak)
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
    Scope = Scope->createChildBlock(node->getName());

    std::vector<llvm::Type *> paramTypes;

    for (const auto &param: node->getParameters())
        paramTypes.push_back(visit(param.second));

    auto name = getMangledName(node->getSpan(), node->getName(), paramTypes);
    auto linkage = Function::ExternalLinkage;

    FunctionType *FT = FunctionType::get(visit(node->getReturnType()), paramTypes, false);
    Function *F = Function::Create(FT, linkage, name, *TheModule);

    //    deferStack.push({});

    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", F);
    Builder->SetInsertPoint(BB);

    for (auto &param: F->args()) {
        param.setName(node->getParameters()[param.getArgNo()].first);

        auto ptr = Builder->CreateAlloca(param.getType(), nullptr, param.getName() + "_ptr");
        Builder->CreateStore(&param, ptr);

        Scope->insertSymbol(param.getName().str(), ptr, param.getType(), false);
    }
    visit(node->getBody());

    //    auto instrs = deferStack.top();
    //    deferStack.pop();

    if (visit(node->getReturnType()) == Builder->getVoidTy() && !isReturn)
        Builder->CreateRetVoid();

    isReturn = false;

    // Verify function
    std::string output;
    llvm::raw_string_ostream oss(output);
    if (llvm::verifyFunction(*F, &oss)) {
        F->print(outs());
        throw CodegenError(node->getSpan(), "Invalid Function {}\n{}", node->getName(), output);
    }

    // Insert Function to Symbol Table
    Scope = Scope->getParent();
    Scope->insertSymbol(name, F, F->getFunctionType());

    // Reset Insert Point to Top Level
    Builder->SetInsertPoint(&TopLevelFunc->back());
}

void Codegen::visit(ExternFuncDecl *node) {
    std::vector<llvm::Type *> paramTypes;

    for (const auto &param: node->getParameters())
        paramTypes.push_back(visit(param.second));

    FunctionType *FT = FunctionType::get(visit(node->getReturnType()), paramTypes, false);
    Function *F = Function::Create(FT, Function::ExternalLinkage, node->getName(), *TheModule);

    for (auto &param: F->args())
        param.setName(node->getParameters()[param.getArgNo()].first);

    Scope->insertSymbol(node->getName(), F, F->getFunctionType());
}

void Codegen::visit(Assignment *node) {
    auto symbol = Scope->lookup(node->getIdentifier()->getValue());
    if (symbol == nullptr)
        throw CodegenError(node->getSpan(), "Variable not found: {}", node->getIdentifier()->getValue());
    if (!symbol->getMutability())
        throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");

    auto value = visit(node->getExpression());
    value = Cast(node->getSpan(), value, symbol->getType());
    llvm::Value *var_val;

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            Builder->CreateStore(value, symbol->getValue());
            break;
        case TokenType::PLUS_EQUAL:
            var_val = Builder->CreateLoad(symbol->getType(), symbol->getValue(), ".tmp");
            if (symbol->getType()->isFloatingPointTy()) {
                auto new_val = Builder->CreateFAdd(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else if (symbol->getType()->isIntegerTy()) {
                auto new_val = Builder->CreateAdd(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MINUS_EQUAL:
            var_val = Builder->CreateLoad(symbol->getType(), symbol->getValue(), ".tmp");
            if (symbol->getType()->isFloatingPointTy()) {
                auto new_val = Builder->CreateFSub(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else if (symbol->getType()->isIntegerTy()) {
                auto new_val = Builder->CreateSub(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::SLASH_EQUAL:
            var_val = Builder->CreateLoad(symbol->getType(), symbol->getValue(), ".tmp");
            if (symbol->getType()->isFloatingPointTy()) {
                auto new_val = Builder->CreateFDiv(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else if (symbol->getType()->isIntegerTy()) {
                auto new_val = Builder->CreateSDiv(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::STAR_EQUAL:
            var_val = Builder->CreateLoad(symbol->getType(), symbol->getValue(), ".tmp");
            if (symbol->getType()->isFloatingPointTy()) {
                auto new_val = Builder->CreateFMul(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else if (symbol->getType()->isIntegerTy()) {
                auto new_val = Builder->CreateMul(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else
                throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
            break;
        case TokenType::MOD_EQUAL:
            var_val = Builder->CreateLoad(symbol->getType(), symbol->getValue(), ".tmp");
            if (symbol->getType()->isFloatingPointTy()) {
                auto new_val = Builder->CreateFRem(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
            } else if (symbol->getType()->isIntegerTy()) {
                auto new_val = Builder->CreateSRem(value, var_val, ".tmp");
                Builder->CreateStore(new_val, symbol->getValue());
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

    isReturn = true;
    if (node->getValue() == nullptr)
        Builder->CreateRetVoid();
    else
        Builder->CreateRet(visit(node->getValue()));
}

void Codegen::visit(Defer *node) {
    throw CodegenError(node->getSpan(), "Defer functionality unimplemented!");
}

void Codegen::visit(ExpressionStatement *node) {
    visit(node->getExpression());
}

// TODO: Implement me
void Codegen::visit(Import *node) {
    CompileModule(node->getSpan(), node->getFilePath());
}

llvm::Value *Codegen::visit(FuncCall *node) {
    std::vector<llvm::Value *> params;
    std::vector<llvm::Type *> paramTypes;
    for (auto arg: node->getArguments()) {
        params.push_back(visit(arg));
        paramTypes.push_back(params.back()->getType());
    }

    auto name = getMangledName(node->getSpan(), node->getName(), paramTypes);
    auto symbol = Scope->lookup(name);
    // Get function without name mangling in case of extern C functions
    symbol = symbol == nullptr ? Scope->lookup(node->getName()) : symbol;

    if (symbol == nullptr)
        throw CodegenError(node->getSpan(), "Function {} not in current scope.", node->getName());

    if (!static_cast<llvm::FunctionType *>(symbol->getType()))
        throw CodegenError(node->getSpan(), "Symbol {} is not a function.", node->getName());

    auto *func = static_cast<Function *>(symbol->getValue());
    return Builder->CreateCall(func, params, func->getReturnType()->isVoidTy() ? "" : "tmp");
}

llvm::Value *Codegen::visit(BinaryOp *node) {
    llvm::Value *left = visit(node->getLeft());
    llvm::Value *right = visit(node->getRight());
    llvm::Type *finalType = GetExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFSub(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSub(left, right, ".tmp");
            break;
        case TokenType::PLUS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFAdd(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateAdd(left, right, ".tmp");
            break;
        case TokenType::STAR:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFMul(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateMul(left, right, ".tmp");
            break;
        case TokenType::SLASH:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFDiv(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSDiv(left, right, ".tmp");
            break;
        case TokenType::MOD:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFRem(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSRem(left, right, ".tmp");
            break;
        case TokenType::POWER:
            if (!right->getType()->isIntegerTy())
                throw CodegenError(node->getSpan(), "Cannot use non-integers for power coefficient: {}",
                                   node->getRight()->toString(0));
            throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
            break;
        case TokenType::EQUAL_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOEQ(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpEQ(left, right, ".tmp");
            break;
        case TokenType::BANG_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpONE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpNE(left, right, ".tmp");
            break;
        case TokenType::GREATER:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGT(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGT(left, right, ".tmp");
            break;
        case TokenType::GREATER_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGE(left, right, ".tmp");
            break;
        case TokenType::LESS:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLT(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLT(left, right, ".tmp");
            break;
        case TokenType::LESS_EQUAL:
            left = Cast(node->getSpan(), left, finalType);
            right = Cast(node->getSpan(), right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLE(left, right, ".tmp");
            break;
        case TokenType::AND:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                                   node->getLeft()->toString(0), node->getRight()->toString(0));

            return Builder->CreateLogicalAnd(left, right, ".tmp");
        case TokenType::OR:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                                   node->getLeft()->toString(0), node->getRight()->toString(0));

            return Builder->CreateLogicalOr(left, right, ".tmp");
        default:
            throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}", NAMEOF_ENUM(node->getOperator()));
    }

    throw CodegenError(node->getSpan(),
                       "Unimplemented binary operator {} for {} and {}",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getLeft()->toString(0),
                       node->getRight()->toString(0));
}

llvm::Value *Codegen::visit(CastOp *node) {
    return Cast(node->getSpan(), visit(node->getExpression()), visit(node->getType()));
}

llvm::Value *Codegen::visit(UnaryOp *node) {
    Value *operand = visit(node->getExpression());

    if (node->getOperator() == TokenType::MINUS) {
        if (operand->getType()->isIntegerTy())
            return Builder->CreateNeg(operand, ".tmp");
        else if (operand->getType()->isFloatingPointTy())
            return Builder->CreateFNeg(operand, ".tmp");
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
    } else if (node->getOperator() == TokenType::NOT) {
        if (operand->getType()->isIntegerTy(1))
            return Builder->CreateNot(operand, ".tmp");
        else
            throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
    } else
        throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
}

llvm::Value *Codegen::visit(Literal *node) {
    if (node->getType() == TokenType::DOUBLE)
        return ConstantFP::get(*TheContext, APFloat(std::stod(node->getValue())));
    else if (node->getType() == TokenType::INTEGER)
        return ConstantInt::getSigned(Builder->getInt64Ty(), std::stoi(node->getValue()));
    else if (node->getType() == TokenType::BOOL)
        return ConstantInt::getBool(*TheContext, node->getValue() == "true");
    else if (node->getType() == TokenType::IDENTIFIER) {
        // Look this variable up in the function.
        auto val = Scope->lookup(node->getValue());
        if (val == nullptr)
            throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());

        // Load the value.
        return Builder->CreateLoad(val->getType(), val->getValue(), ".tmp");
    } else
        throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
}

llvm::Value *Codegen::visit(Else * /*node*/) {
    return llvm::ConstantInt::getTrue(*TheContext);
}

std::string Codegen::getTypeMangledName(Span span, llvm::Type *type) {
    if (type->isIntegerTy(1))
        return "b";
    else if (type->isIntegerTy())
        return "i";
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
        return "(struct_" + param_str + ")";
    }

    throw CodegenError(span, "Unknown type found during mangling");
}

// TODO: Change to support private/public and module system
std::string Codegen::getMangledName(Span span, std::string func_name, const std::vector<llvm::Type *> &paramTypes) {
    std::string name = "_" + std::move(func_name);

    for (auto param_type: paramTypes)
        name += ":" + getTypeMangledName(span, param_type);

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

llvm::Value *Codegen::Cast(Span span, llvm::Value *val, llvm::Type *type) {
    if (val->getType() == type)
        return val;

    if (type->isIntegerTy()) {
        if (val->getType()->isFloatingPointTy())
            return Builder->CreateFPToSI(val, type, ".tmp");
        else if (val->getType()->isIntegerTy())
            return Builder->CreateIntCast(val, type, true, ".tmp");
    } else if (type->isFloatingPointTy()) {
        if (val->getType()->isIntegerTy())
            return Builder->CreateSIToFP(val, type, ".tmp");
        else if (val->getType()->isFloatingPointTy())
            return Builder->CreateFPCast(val, type, ".tmp");
    }

    throw CodegenError(span, "Unsupported Cast");
}