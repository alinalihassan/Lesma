#include "Codegen.h"

using namespace lesma;

Codegen::Codegen(std::unique_ptr<Parser> parser, const std::string& filename) {
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
    InitializePasses();
}

llvm::TargetMachine *Codegen::InitializeTargetMachine() {
    // Configure output target
    auto targetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    const std::string& tripletString = targetTriple.getTriple();
    TheModule->setTargetTriple(tripletString);

    // Search after selected target
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tripletString, error);
    if (!target)
        throw CodegenError("Target not available:\n{}\n", error);

    llvm::TargetOptions opt;
    llvm::Optional rm = llvm::Optional<llvm::Reloc::Model>();
    return target->createTargetMachine(tripletString, "generic", "", opt, rm);
}

void Codegen::InitializePasses() {
//    FPM = std::make_unique<FunctionPassManager>();
//    MPM = std::make_unique<ModulePassManager>();
//
//    FPM->addPass(createDeadCodeEliminationPass());
//    FPM->addPass(createDeadStoreEliminationPass());
}

void Codegen::Compile(const std::string& output) {
    std::error_code err;
    auto out = llvm::raw_fd_ostream(output + ".o", err);

    llvm::legacy::PassManager passManager;
    if (TargetMachine->addPassesToEmitFile(passManager, out, nullptr, llvm::CGFT_ObjectFile))
        throw CodegenError("Target Machine can't emit an object file");
    // Emit object file
    passManager.run(*TheModule);
}

int Codegen::JIT() {
    auto jit_error = TheJIT->addModule(ThreadSafeModule(std::move(TheModule), std::move(TheContext)));
    if (jit_error)
        throw CodegenError("JIT Error:\n{}");

    using MainFnTy = int();
    auto jit_main = jitTargetAddressToFunction<MainFnTy *>(TheJIT->lookup("main")->getAddress());
    return jit_main();
}

void Codegen::Run() {
    Visit(Parser_->getAST());
}

void Codegen::Dump() {
    TheModule->print(outs(), nullptr);
}

llvm::Value *Codegen::Visit(Program* node) {
    return Visit(node->getBlock());
}

llvm::Value *Codegen::Visit(Expression* node) {
    if (dynamic_cast<Var*>(node))
        return Visit(dynamic_cast<Var*>(node));
    else if (dynamic_cast<FuncCall*>(node))
        return Visit(dynamic_cast<FuncCall*>(node));
    else if (dynamic_cast<BinaryOp*>(node))
        return Visit(dynamic_cast<BinaryOp*>(node));
    else if (dynamic_cast<UnaryOp*>(node))
        return Visit(dynamic_cast<UnaryOp*>(node));
    else if (dynamic_cast<Literal*>(node))
        return Visit(dynamic_cast<Literal*>(node));
    else if (dynamic_cast<Else*>(node))
        return Visit(dynamic_cast<Else*>(node));

    throw CodegenError("Unknown Expression: {}", node->toString(0));
}

llvm::Value *Codegen::Visit(Statement* node) {
    if (dynamic_cast<VarDecl*>(node))
        return Visit(dynamic_cast<VarDecl*>(node));
    else if (dynamic_cast<If*>(node))
        return Visit(dynamic_cast<If*>(node));
    else if (dynamic_cast<While*>(node))
        return Visit(dynamic_cast<While*>(node));
    else if (dynamic_cast<FuncDecl*>(node))
        return Visit(dynamic_cast<FuncDecl*>(node));
    else if (dynamic_cast<ExternFuncDecl*>(node))
        return Visit(dynamic_cast<ExternFuncDecl*>(node));
    else if (dynamic_cast<Assignment*>(node))
        return Visit(dynamic_cast<Assignment*>(node));
    else if (dynamic_cast<Break*>(node))
        return Visit(dynamic_cast<Break*>(node));
    else if (dynamic_cast<Continue*>(node))
        return Visit(dynamic_cast<Continue*>(node));
    else if (dynamic_cast<Return*>(node))
        return Visit(dynamic_cast<Return*>(node));
    else if (dynamic_cast<ExpressionStatement*>(node))
        return Visit(dynamic_cast<ExpressionStatement*>(node));
    else if (dynamic_cast<Compound*>(node))
        return Visit(dynamic_cast<Compound*>(node));

    throw CodegenError("Unknown Statement:\n{}", node->toString(0));
}

llvm::Type *Codegen::Visit(lesma::Type *node) {
    if (node->getType() == TokenType::INT_TYPE)
        return Builder->getInt64Ty();
    else if (node->getType() == TokenType::BOOL_TYPE)
        return Builder->getInt1Ty();
    else if (node->getType() == TokenType::VOID_TYPE)
        return Builder->getVoidTy();

    throw CodegenError("Unimplemented type {}", NAMEOF_ENUM(node->getType()));
}

llvm::Value *Codegen::Visit(Compound *node) {
    for (auto elem: node->getChildren()) {
        Visit(elem);
    }

    return nullptr;
}

llvm::Value *Codegen::Visit(VarDecl *node) {
    auto type = Visit(node->getType());
    auto ptr = Builder->CreateAlloca(type, nullptr, node->getVar()->getName());

    Scope->insertSymbol(node->getVar()->getName(), ptr, type);

    if (node->getValue().has_value())
        Builder->CreateStore(Visit(node->getValue().value()), ptr);

    return ptr;
}

llvm::Value *Codegen::Visit(If *node) {
    print("IF\n");
    for (unsigned long i = 0; i < node->getConds().size(); i++) {
        print("IF: \n");
        Visit(node->getBlocks()[i]);
    }
}

llvm::Value *Codegen::Visit(While *node) {
    print("WHILE\n");
    llvm::Function* parentFct = Builder->GetInsertBlock()->getParent();

    // Create blocks
    llvm::BasicBlock* bCond = llvm::BasicBlock::Create(*TheContext, "while.cond");
    llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(*TheContext, "while");
    llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(*TheContext, "while.end");

    // Jump into condition block
    Builder->CreateBr(bCond);

    // Fill condition block
    parentFct->getBasicBlockList().push_back(bCond);
    Builder->SetInsertPoint(bCond);
    auto condition = Visit(node->getCond());
    Builder->CreateCondBr(condition, bLoop, bEnd);

    // Fill while body block
    parentFct->getBasicBlockList().push_back(bLoop);
    Builder->SetInsertPoint(bLoop);
    Visit(node->getBlock());
    Builder->CreateBr(bCond);

    // Fill loop end block
    parentFct->getBasicBlockList().push_back(bEnd);
    Builder->SetInsertPoint(bEnd);

    return Builder->getTrue();
}

llvm::Value *Codegen::Visit(FuncDecl *node) {
    Scope = Scope->createChildBlock(node->getName());

    std::vector<llvm::Type*> paramTypes;

    for (const auto& param: node->getParameters())
        paramTypes.push_back(Visit(param.second));

    FunctionType *FT = FunctionType::get(Visit(node->getReturnType()), paramTypes, false);
    Function *F = Function::Create(FT, Function::ExternalLinkage, node->getName(), TheModule.get());

    for (auto &param: F->args())
        param.setName(node->getParameters()[param.getArgNo()].first);

    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", F);
    Builder->SetInsertPoint(BB);

    Visit(node->getBody());

    if (Visit(node->getReturnType()) == Builder->getVoidTy())
        Builder->CreateRetVoid();

    std::string output;
    llvm::raw_string_ostream oss(output);
    if (llvm::verifyFunction(*F, &oss)) {
        F->print(outs());
        throw CodegenError("Invalid Function {}", node->getName());
    }

    Scope = Scope->getParent();
    Scope->insertSymbol(node->getName(), F, F->getFunctionType());

    return F;
}

llvm::Value *Codegen::Visit(ExternFuncDecl *node) {
    std::vector<llvm::Type*> paramTypes;

    for (const auto& param: node->getParameters())
        paramTypes.push_back(Visit(param.second));

    FunctionType *FT = FunctionType::get(Visit(node->getReturnType()), paramTypes, false);
    Function *F = Function::Create(FT, Function::ExternalLinkage, node->getName(), TheModule.get());

    for (auto &param: F->args())
        param.setName(node->getParameters()[param.getArgNo()].first);

    Scope->insertSymbol(node->getName(), F, F->getFunctionType());

    return F;
}

llvm::Value *Codegen::Visit(Assignment *node) {
    auto symbol = Scope->lookup(node->getVar()->getName());
    auto value = Visit(node->getExpression());
    if (symbol == nullptr)
        throw CodegenError("Variable not found: {}", node->getVar()->getName());

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            return Builder->CreateStore(value, symbol->getValue());
        default:
            throw CodegenError("Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
    }
}

llvm::Value *Codegen::Visit(Break *node) {
    print("BREAK\n");
}

llvm::Value *Codegen::Visit(Continue *node) {
    print("CONTINUE\n");
}

llvm::Value *Codegen::Visit(Return *node) {
    return Builder->CreateRet(Visit(node->getValue()));
}

llvm::Value *Codegen::Visit(ExpressionStatement *node) {
    return Visit(node->getExpression());
}

llvm::Value *Codegen::Visit(Var *node) {
    print("VAR\n");
}

llvm::Value *Codegen::Visit(FuncCall *node) {
    if (Scope->lookup(node->getName()) == nullptr)
        throw CodegenError("Function {} not in current scope.\n", node->getName());

    auto symbol = Scope->lookup(node->getName());
    if (!static_cast<llvm::FunctionType*>(symbol->getType()))
        throw CodegenError("Symbol {} not of FunctionType.\n", node->getName());

    std::vector<llvm::Value*> params;
    for (auto arg: node->getArguments())
        params.push_back(Visit(arg));

    auto* func = static_cast<Function *>(symbol->getValue());
    return Builder->CreateCall(func, params);
}

llvm::Value *Codegen::Visit(BinaryOp *node) {
    print("BINARYOP\n");
}

llvm::Value *Codegen::Visit(UnaryOp *node) {
    Value *operand = Visit(node->getExpression());

    if (node->getOperator() == TokenType::MINUS) {
        if (operand->getType()->isIntegerTy())
            return Builder->CreateNeg(operand);
        else if (operand->getType()->isFloatingPointTy())
            return Builder->CreateFNeg(operand);
        else
            throw CodegenError("Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
    } else if (node->getOperator() == TokenType::NOT) {
        if (operand->getType()->isIntegerTy(1))
            return Builder->CreateNot(operand);
        else
            throw CodegenError("Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
    } else
        throw CodegenError("Unknown unary operator, cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
}

llvm::Value *Codegen::Visit(Literal *node) {
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
            throw CodegenError("Unknown variable name {}", node->getValue());

        // Load the value.
        return Builder->CreateLoad(Builder->getInt64Ty(), val->getValue(), node->getValue());
    } else
        throw CodegenError("Unknown literal {}", node->getValue());
}

llvm::Value *Codegen::Visit(Else *node) {
    print("ELSE\n");
    return nullptr;
}