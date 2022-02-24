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
    if (dynamic_cast<FuncCall*>(node))
        return Visit(dynamic_cast<FuncCall*>(node));
    else if (dynamic_cast<BinaryOp*>(node))
        return Visit(dynamic_cast<BinaryOp*>(node));
    else if (dynamic_cast<CastOp*>(node))
        return Visit(dynamic_cast<CastOp*>(node));
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
    if (node->getType() == TokenType::FLOAT_TYPE)
        return Builder->getDoubleTy();
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
    auto ptr = Builder->CreateAlloca(type, nullptr, node->getIdentifier()->getValue());

    Scope->insertSymbol(node->getIdentifier()->getValue(), ptr, type);

    if (node->getValue().has_value())
        Builder->CreateStore(Visit(node->getValue().value()), ptr);

    return ptr;
}

llvm::Value *Codegen::Visit(If *node) {
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

        auto cond = Visit(node->getConds().at(i));
        Builder->CreateCondBr(cond, bIfTrue, bIfFalse);
        Builder->SetInsertPoint(bIfTrue);

        Scope = Scope->createChildBlock("if");
        Visit(node->getBlocks().at(i));

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

    return nullptr;
}

llvm::Value *Codegen::Visit(While *node) {
    Scope = Scope->createChildBlock("while");

    llvm::Function* parentFct = Builder->GetInsertBlock()->getParent();

    // Create blocks
    llvm::BasicBlock* bCond = llvm::BasicBlock::Create(*TheContext, "while.cond");
    llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(*TheContext, "while");
    llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(*TheContext, "while.end");

    breakBlocks.push(bEnd);
    continueBlocks.push(bCond);

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
        throw CodegenError("Invalid Function {}\n{}", node->getName(), output);
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
    auto symbol = Scope->lookup(node->getIdentifier()->getValue());
    auto value = Visit(node->getExpression());
    if (symbol == nullptr)
        throw CodegenError("Variable not found: {}", node->getIdentifier()->getValue());

    switch (node->getOperator()) {
        case TokenType::EQUAL:
            return Builder->CreateStore(value, symbol->getValue());
        case TokenType::PLUS_EQUAL:
        case TokenType::MINUS_EQUAL:
        case TokenType::SLASH_EQUAL:
        case TokenType::STAR_EQUAL:
        case TokenType::MOD_EQUAL:
        case TokenType::POWER_EQUAL:
        default:
            throw CodegenError("Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
    }
}

llvm::Value *Codegen::Visit(Break *node) {
    if (breakBlocks.empty())
        throw CodegenError("Cannot break without being in a loop\n");

    auto block = breakBlocks.top();
    breakBlocks.pop();
    isBreak = true;

    return Builder->CreateBr(block);
}

llvm::Value *Codegen::Visit(Continue *node) {
    if (continueBlocks.empty())
        throw CodegenError("Cannot continue without being in a loop\n");

    auto block = continueBlocks.top();
    continueBlocks.pop();
    isBreak = true;

    return Builder->CreateBr(block);
}

llvm::Value *Codegen::Visit(Return *node) {
    return Builder->CreateRet(Visit(node->getValue()));
}

llvm::Value *Codegen::Visit(ExpressionStatement *node) {
    return Visit(node->getExpression());
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
    return Builder->CreateCall(func, params, func->getReturnType()->isVoidTy() ? "" : "tmp");
}

llvm::Value *Codegen::Visit(BinaryOp *node) {
    llvm::Value *left = Visit(node->getLeft());
    llvm::Value *right = Visit(node->getRight());
    llvm::Type *finalType = GetExtendedType(left->getType(), right->getType());

    switch (node->getOperator()) {
        case TokenType::MINUS:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFSub(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSub(left, right, ".tmp");
            break;
        case TokenType::PLUS:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFAdd(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateAdd(left, right, ".tmp");
            break;
        case TokenType::STAR:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFMul(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateMul(left, right, ".tmp");
            break;
        case TokenType::SLASH:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFDiv(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSDiv(left, right, ".tmp");
            break;
        case TokenType::MOD:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFRem(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateSRem(left, right, ".tmp");
            break;
        case TokenType::POWER:
            if (!right->getType()->isIntegerTy())
                throw CodegenError("Cannot use non-integers for power coefficient: {}\n",
                                   node->getRight()->toString(0));
            break;
        case TokenType::EQUAL_EQUAL:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOEQ(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpEQ(left, right, ".tmp");
            break;
        case TokenType::BANG_EQUAL:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpONE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpNE(left, right, ".tmp");
            break;
        case TokenType::GREATER:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGT(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGT(left, right, ".tmp");
            break;
        case TokenType::GREATER_EQUAL:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOGE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSGE(left, right, ".tmp");
            break;
        case TokenType::LESS:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLT(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLT(left, right, ".tmp");
            break;
        case TokenType::LESS_EQUAL:
            left = Cast(left, finalType);
            right = Cast(right, finalType);

            if (finalType->isFloatingPointTy())
                return Builder->CreateFCmpOLE(left, right, ".tmp");
            else if (finalType->isIntegerTy())
                return Builder->CreateICmpSLE(left, right, ".tmp");
            break;
        case TokenType::AND:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError("Cannot use non-booleans for and: {} - {}\n",
                                   node->getLeft()->toString(0), node->getRight()->toString(0));

            return Builder->CreateLogicalAnd(left, right, ".tmp");
        case TokenType::OR:
            if (!left->getType()->isIntegerTy(1) && !right->getType()->isIntegerTy(1))
                throw CodegenError("Cannot use non-booleans for or: {} - {}\n",
                                   node->getLeft()->toString(0), node->getRight()->toString(0));

            return Builder->CreateLogicalOr(left, right, ".tmp");
        default:
            throw CodegenError("Unimplemented binary operator: {}\n", NAMEOF_ENUM(node->getOperator()));
    }

    throw CodegenError("Unimplemented binary operator {} for {} and {}\n",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getLeft()->toString(0),
                       node->getRight()->toString(0));
}

llvm::Value *Codegen::Visit(CastOp *node) {
    return Cast(Visit(node->getExpression()), Visit(node->getType()));
}

llvm::Value *Codegen::Visit(UnaryOp *node) {
    Value *operand = Visit(node->getExpression());

    if (node->getOperator() == TokenType::MINUS) {
        if (operand->getType()->isIntegerTy())
            return Builder->CreateNeg(operand, ".tmp");
        else if (operand->getType()->isFloatingPointTy())
            return Builder->CreateFNeg(operand, ".tmp");
        else
            throw CodegenError("Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()), node->getExpression()->toString(0));
    } else if (node->getOperator() == TokenType::NOT) {
        if (operand->getType()->isIntegerTy(1))
            return Builder->CreateNot(operand, ".tmp");
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
        return Builder->CreateLoad(val->getType(), val->getValue(), ".tmp");
    } else
        throw CodegenError("Unknown literal {}", node->getValue());
}

llvm::Value *Codegen::Visit(Else *node) {
    return llvm::ConstantInt::getTrue(*TheContext);
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

llvm::Value *Codegen::Cast(llvm::Value* val, llvm::Type* type) {
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

    throw CodegenError("Unsupported Cast");
}