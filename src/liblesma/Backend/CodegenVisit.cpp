#include "Codegen.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/SmallVector.h>
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
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
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

// Declare LLD driver functions using the macro from Driver.h
#ifdef __APPLE__
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif
#include <nameof.hpp>

#include "liblesma/AST/AST.h"
#include "liblesma/Backend/CodegenError.h"
#include "liblesma/Backend/CodegenRuntimeNames.h"
#include "liblesma/Backend/CodegenTypeUtils.h"
#include "liblesma/Backend/MangleUtils.h"
#include "liblesma/Common/ExportDiscovery.h"
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"
#include "liblesma/Typecheck/Typechecker.h"

using namespace lesma;

Codegen::Codegen(std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr,
                 const std::string& filename, std::vector<std::string> imports, bool jit, bool main,
                 std::string alias, const std::shared_ptr<ThreadSafeContext>& context,
                 std::shared_ptr<std::vector<std::string>> sharedModules,
                 std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>> sharedScopes,
                 std::unique_ptr<SymbolTable> preScope,
                 std::vector<std::unique_ptr<lesma::Type>> preTypeCache,
                 std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
                     preSpecializedClassTypeEnvs) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  theContext = context == nullptr
                   ? std::make_shared<ThreadSafeContext>(std::make_unique<LLVMContext>())
                   : context;
  targetMachine = initializeTargetMachine();
  theModule = initializeModule();
  if (jit) {
    theJit = initializeJit();
  }

  builder = std::make_unique<IRBuilder<>>(theModule->getContext());
  this->parser = std::move(parser);
  sourceManager = std::move(srcMgr);
  if (preScope == nullptr) {
    throw CodegenError({}, "Codegen requires a precomputed typecheck scope");
  }
  rootScope = std::move(preScope);
  scope = rootScope.get();
  for (auto& t : preTypeCache) {
    typeCache.push_back(std::move(t));
  }
  specializedClassTypeEnvs.merge(std::move(preSpecializedClassTypeEnvs));

  this->alias = std::move(alias);
  this->filename = filename;
  isMain = main;
  isJit = jit;

  if (sharedModules && sharedScopes) {
    importedModules = std::move(sharedModules);
    importedScopes = std::move(sharedScopes);
  } else {
    importedModules = std::make_shared<std::vector<std::string>>(std::move(imports));
    importedScopes = std::make_shared<std::vector<std::unique_ptr<SymbolTable>>>();
  }
  topLevelFunc = initializeTopLevel();
  // base.les is loaded at the start of run() so we don't load it during
  // constructor re-entrancy when creating Codegens for imported modules.
}

auto Codegen::defineFunction(lesma::Value* value, const FuncDecl* node, Value* clsSymbol) -> void {
  bool const isMethod = value->getDeclarationKind() == ValueDeclarationKind::METHOD;
  if (clsSymbol == nullptr && isMethod) {
    auto fields = value->getType()->getFields();
    if (!fields.empty() && fields.front()->type != nullptr &&
        fields.front()->type->is(BaseType::TY_PTR) &&
        fields.front()->type->getElementType() != nullptr) {
      auto* selfClassType = fields.front()->type->getElementType();
      if (auto it = specializedClassSymbolsByType.find(selfClassType);
          it != specializedClassSymbolsByType.end()) {
        clsSymbol = it->second;
      }
    }
  }
  SymbolTable* savedScope = scope;
  scope = value->getBodyScope();
  if (scope == nullptr) {
    scope = savedScope->createChildBlock(node->getName());
  }
  auto lookupInCurrentScope = [this](const std::string& name) -> Value* {
    for (auto* symbol : scope->getSymbols()) {
      if (symbol->getName() == name) {
        return symbol;
      }
    }
    return nullptr;
  };
  currentFunction = value;
  deferStack.emplace();
  clearArcTracking();

  if (value->getLlvmValue() == nullptr) {
    throw CodegenError(node->getSpan(), "Function {} is declared but has no LLVM body",
                       node->getName());
  }
  auto* f = llvm::cast<Function>(value->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);

  int fieldIndex = 0;
  for (const auto& field : value->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);
    std::string paramName;

    if (field->name == "self") {
      paramName = "self";
    } else {
      size_t const paramIndex = param->getArgNo() - (isMethod ? 1U : 0U);
      if (paramIndex < node->getParameters().size()) {
        paramName = node->getParameters()[paramIndex]->name;
      } else {
        paramName = field->name;
      }
    }
    param->setName(paramName);

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
    builder->CreateStore(param, ptr);

    Value* paramSym = nullptr;
    if (auto* existingParam = lookupInCurrentScope(paramName);
        existingParam != nullptr && existingParam->getLlvmValue() == nullptr) {
      existingParam->setLlvmValue(ptr);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      paramSym = existingParam;
    } else {
      auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
      symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(symbol));
      paramSym = lookupInCurrentScope(paramName);
    }
    if (paramSym != nullptr && field->type != nullptr && field->type->is(BaseType::TY_PTR) &&
        field->type->getElementType() != nullptr &&
        field->type->getElementType()->is(BaseType::TY_CLASS)) {
      // `this` for `new` is not ARC-managed at method exit: the call site owns the +1 from
      // lesma_arc_alloc. Third arg is non-null for class methods (self/this context). Prefer
      // value->getName() — must match the callable symbol name ("new"), not only the AST node.
      const bool isCtorThis =
          clsSymbol != nullptr && fieldIndex == 0 && value->getName() == "new";
      if (!isCtorThis) {
        arcTrackedLocals.push_back(paramSym);
      }
    }

    fieldIndex++;
  }

  node->getBody()->accept(*this);

  auto instrs = deferStack.top();
  deferStack.pop();

  if (!isReturn) {
    for (auto* inst : instrs) {
      inst->accept(*this);
    }
  }

  // Check for well-formness of all BBs. In particular, look for
  // any unterminated BB and try to add a Return to it.
  for (BasicBlock& bb : *f) {
    Instruction* terminator = bb.getTerminator();
    if (terminator != nullptr) {
      continue; // Well-formed
    }

    if (value->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      // Make implicit return of void Function explicit.
      builder->SetInsertPoint(&bb);
      emitArcReleaseAllTrackedLocals();
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->getSpan(), "Function {} does not always return a result",
                         node->getName());
    }
  }

  isReturn = false;

  // Verify only this module's function (not cross-module declarations).
  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(node->getSpan(), "Invalid function {}\n{}", node->getName(), verifyOutput);
  }

  // Insert Function to Symbol Table
  scope = savedScope;

  currentFunction = nullptr;

  // Reset Insert Point to Top Level
  builder->SetInsertPoint(&topLevelFunc->back());
}

auto Codegen::visit(const Statement* node) -> void {
  lesma::print("Visited a blank statement\n{}", node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const Expression* node) -> void {
  lesma::print("Visited a blank expression\n{}", node->toString(sourceManager.get(), "", true));
}


auto Codegen::visit(const Compound* node) -> void {
  for (auto* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

namespace {
[[nodiscard]] auto typeContainsUnboundGeneric(lesma::Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (type->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (type->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    return typeContainsUnboundGeneric(type->getElementType());
  }
  if (type->is(BaseType::TY_CLASS)) {
    for (Field* f : type->getFields()) {
      if (typeContainsUnboundGeneric(f->type)) {
        return true;
      }
    }
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    for (Field* f : type->getFields()) {
      if (typeContainsUnboundGeneric(f->type)) {
        return true;
      }
    }
    return typeContainsUnboundGeneric(type->getReturnType());
  }
  return false;
}
} // namespace

namespace {
/** True when the expression already produces a +1 owned class pointer (constructor/new, boxed str). */
bool exprIsFreshClassOwner(Expression* expr) {
  if (expr == nullptr) {
    return false;
  }
  if (dynamic_cast<FuncCall*>(expr) != nullptr) {
    return true;
  }
  if (auto* lit = dynamic_cast<Literal*>(expr)) {
    if (lit->getType() == TokenType::STRING) {
      lesma::Type* strClass = lit->getResolvedStrClassType();
      if (strClass != nullptr && strClass->is(BaseType::TY_CLASS)) {
        return true;
      }
    }
  }
  return false;
}
} // namespace

auto Codegen::visit(const VarDecl* node) -> void {
  const std::string name = node->getIdentifier()->getValue();
  auto* existing = [&]() -> Value* {
    for (auto* symbol : scope->getSymbols()) {
      if (symbol->getName() == name) {
        return symbol;
      }
    }
    return nullptr;
  }();

  if (existing != nullptr && existing->getLlvmValue() == nullptr) {
    // Reuse symbol from typecheck; fill in LLVM value
    lesma::Type* type = existing->getType();
    std::unique_ptr<lesma::Value> valueResult;
    if (node->getValue() != nullptr) {
      node->getValue()->accept(*this);
      valueResult = std::move(result);
      if (type->is(BaseType::TY_CLASS) && valueResult->getType() != nullptr &&
          valueResult->getType()->is(BaseType::TY_CLASS)) {
        type = valueResult->getType();
      } else if (type->is(BaseType::TY_CLASS) && valueResult->getType() != nullptr &&
                 valueResult->getType()->is(BaseType::TY_PTR) &&
                 valueResult->getType()->getElementType() != nullptr &&
                 valueResult->getType()->getElementType()->is(BaseType::TY_CLASS)) {
        type = valueResult->getType()->getElementType();
      }
    }
    if (valueResult != nullptr && valueResult->getType() != nullptr &&
        typeContainsUnboundGeneric(type) && !typeContainsUnboundGeneric(valueResult->getType())) {
      type = valueResult->getType();
    }
    getOrCreateLlvmType(type);
    if (type->is(BaseType::TY_CLASS)) {
      lesma::Type* ptrType =
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
      existing->setType(ptrType);
    } else if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
               type->getElementType()->is(BaseType::TY_CLASS)) {
      // Value was e.g. *Foo<int> while typecheck left *Foo<T> on the symbol.
      existing->setType(type);
    }
    lesma::Type* storedType = existing->getType();
    const bool isPtrToClass = storedType->is(BaseType::TY_PTR) &&
                              storedType->getElementType() != nullptr &&
                              storedType->getElementType()->is(BaseType::TY_CLASS);
    llvm::Type* allocaTy = (storedType->is(BaseType::TY_CLASS) || isPtrToClass)
                               ? builder->getPtrTy()
                               : storedType->getLlvmType();
    auto* ptr = builder->CreateAlloca(allocaTy, nullptr, name);
    existing->setLlvmValue(ptr);
    existing->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    existing->setMutable(node->getMutability());
    if (valueResult != nullptr) {
      if (isPtrToClass && valueResult->getType() != nullptr &&
          valueResult->getType()->is(BaseType::TY_PTR)) {
        builder->CreateStore(valueResult->getLlvmValue(), ptr);
      } else {
        lesma::Type* castTarget = isPtrToClass ? storedType->getElementType() : storedType;
        auto castVal = cast(node->getSpan(), valueResult.get(), castTarget);
        builder->CreateStore(castVal->getLlvmValue(), ptr);
      }
    }
    if (isPtrToClass) {
      arcTrackedLocals.push_back(existing);
    }
    return;
  }

  lesma::Type* type = nullptr;
  std::unique_ptr<lesma::Value> val;

  if (node->getValue() != nullptr) {
    node->getValue()->accept(*this);
    val = std::move(result);
    type = val->getType();
  }

  if (node->getType() != nullptr) {
    node->getType()->accept(*this);
    type = result->getType();
  }

  getOrCreateLlvmType(type);
  auto* ptr = builder->CreateAlloca(type->getLlvmType(), nullptr, name);

  if (type->is(BaseType::TY_CLASS)) {
    type = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  }
  const bool isPtrToClass =
      type->is(BaseType::TY_PTR) && type->getElementType()->is(BaseType::TY_CLASS);
  auto symbol = std::make_unique<Value>(
      name, type, node->getType() != nullptr ? SymbolState::INITIALIZED : SymbolState::DECLARED);
  symbol->setLlvmValue(ptr);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));

  if (isPtrToClass) {
    if (auto* tracked = scope->lookup(name)) {
      arcTrackedLocals.push_back(tracked);
    }
  }

  if (node->getValue() != nullptr) {
    if (isPtrToClass && val->getType() != nullptr && val->getType()->is(BaseType::TY_PTR)) {
      builder->CreateStore(val->getLlvmValue(), ptr);
    } else {
      lesma::Type* castTarget = isPtrToClass ? type->getElementType() : type;
      auto castVal = cast(node->getSpan(), val.get(), castTarget);
      builder->CreateStore(castVal->getLlvmValue(), ptr);
    }
  }
}

auto Codegen::visit(const If* node) -> void {
  auto* parentFct = builder->GetInsertBlock()->getParent();
  auto* bStart = llvm::BasicBlock::Create(theModule->getContext(), "if.start");
  auto* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "if.end");

  builder->CreateBr(bStart);
  bStart->insertInto(parentFct);
  builder->SetInsertPoint(bStart);

  for (unsigned long i = 0; i < node->getConds().size(); i++) {
    auto* bIfTrue = llvm::BasicBlock::Create(theModule->getContext(), "if.true");
    bIfTrue->insertInto(parentFct);
    auto* bIfFalse = bEnd;
    if (i + 1 < node->getConds().size()) {
      bIfFalse = llvm::BasicBlock::Create(theModule->getContext(), "if.false");
      bIfFalse->insertInto(parentFct);
    }

    node->getConds().at(i)->accept(*this);
    builder->CreateCondBr(result->getLlvmValue(), bIfTrue, bIfFalse);
    builder->SetInsertPoint(bIfTrue);

    node->getBlocks().at(i)->accept(*this);

    if (!isBreak && builder->GetInsertBlock()->getTerminator() == nullptr) {
      builder->CreateBr(bEnd);
    }

    builder->SetInsertPoint(bIfFalse);
  }

  bEnd->insertInto(parentFct);

  if (!isBreak) {
    builder->SetInsertPoint(bEnd);
  } else {
    isBreak = false;
  }
}

auto Codegen::visit(const While* node) -> void {
  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();

  // Create blocks
  llvm::BasicBlock* bCond = llvm::BasicBlock::Create(theModule->getContext(), "while.cond");
  llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(theModule->getContext(), "while");
  llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "while.end");

  breakBlocks.push(bEnd);
  continueBlocks.push(bCond);

  // Jump into condition block
  builder->CreateBr(bCond);

  // Fill condition block
  bCond->insertInto(parentFct);
  builder->SetInsertPoint(bCond);
  node->getCond()->accept(*this);
  builder->CreateCondBr(result->getLlvmValue(), bLoop, bEnd);

  // Fill while body block
  bLoop->insertInto(parentFct);
  builder->SetInsertPoint(bLoop);
  node->getBlock()->accept(*this);

  if (!isBreak) {
    builder->CreateBr(bCond);
  } else {
    isBreak = false;
  }

  // Fill loop end block
  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);

  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::classTypeDeclaresIterable(lesma::Type* classTy) const -> bool {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return false;
  }
  for (const auto& n : classTy->getImplTraitNames()) {
    if (n == "Iterable") {
      return true;
    }
  }
  return false;
}

auto Codegen::visit(const ForIn* node) -> void {
  node->getIterable()->accept(*this);
  std::unique_ptr<lesma::Value> iterable = std::move(result);
  lesma::Type* listType = iterable->getType();
  llvm::Value* listHandle = iterable->getLlvmValue();
  if (listType != nullptr && listType->is(BaseType::TY_PTR) &&
      listType->getElementType() != nullptr && listType->getElementType()->is(BaseType::TY_CLASS)) {
    listType = listType->getElementType();
  }
  if (listType != nullptr && listType->is(BaseType::TY_CLASS) &&
      classTypeDeclaresIterable(listType)) {
    auto fields = listType->getFields();
    if (!fields.empty() && fields.front()->type != nullptr &&
        fields.front()->type->is(BaseType::TY_ARRAY)) {
      auto* storagePtr = builder->CreateStructGEP(
          cast<llvm::StructType>(getOrCreateLlvmType(listType)), iterable->getLlvmValue(),
          classUserFieldLlvmIndex(0), "list.storage.ptr");
      listHandle = builder->CreateLoad(getOrCreateLlvmType(fields.front()->type), storagePtr,
                                       "list.storage");
      listType = fields.front()->type;
    }
  }
  SymbolTable* savedScope = scope;
  scope =
      node->getBodyScope() != nullptr ? node->getBodyScope() : savedScope->createChildBlock("for");
  Value* loopVar = scope->lookup(node->getIdentifier()->getValue());
  if (loopVar == nullptr) {
    throw CodegenError(node->getSpan(), "Missing loop variable symbol for for-in");
  }
  lesma::Type* loopVarType = loopVar->getType();
  getOrCreateLlvmType(loopVarType);
  if (loopVar->getLlvmValue() == nullptr) {
    if (loopVarType->is(BaseType::TY_CLASS)) {
      lesma::Type* ptrType =
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), loopVarType));
      loopVar->setType(ptrType);
    }
    lesma::Type* storedType = loopVar->getType();
    const bool isPtrToClass = storedType->is(BaseType::TY_PTR) &&
                              storedType->getElementType() != nullptr &&
                              storedType->getElementType()->is(BaseType::TY_CLASS);
    llvm::Type* allocaTy = (storedType->is(BaseType::TY_CLASS) || isPtrToClass)
                               ? builder->getPtrTy()
                               : storedType->getLlvmType();
    auto* elemPtr = builder->CreateAlloca(allocaTy, nullptr, loopVar->getName());
    loopVar->setLlvmValue(elemPtr);
    loopVar->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  }
  scope = savedScope;

  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock* bCond = llvm::BasicBlock::Create(theModule->getContext(), "for.cond");
  llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(theModule->getContext(), "for");
  llvm::BasicBlock* bInc = llvm::BasicBlock::Create(theModule->getContext(), "for.inc");
  llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "for.end");
  std::unique_ptr<lesma::Value> iteratorValue;
  llvm::AllocaInst* indexPtr = nullptr;
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) ||
      listType->getElementType() == nullptr) {
    iteratorValue = callMethodByName(node->getSpan(), iterable.get(), "iter");
  } else {
    getOrCreateLlvmType(listType);
    indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "for.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
  }

  breakBlocks.push(bEnd);
  continueBlocks.push(bInc);
  builder->CreateBr(bCond);

  if (listType != nullptr && listType->is(BaseType::TY_ARRAY) &&
      listType->getElementType() != nullptr) {
    bCond->insertInto(parentFct);
    builder->SetInsertPoint(bCond);
    auto* idxVal = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    auto* lenVal = emitListLength(listType, listHandle);
    builder->CreateCondBr(builder->CreateICmpSLT(idxVal, lenVal), bLoop, bEnd);

    bLoop->insertInto(parentFct);
    builder->SetInsertPoint(bLoop);
    auto* elemPtr = emitListElementPointer(node->getSpan(), listType, listHandle, idxVal);
    auto* elemVal = builder->CreateLoad(getListStoredElementType(listType), elemPtr);
    builder->CreateStore(elemVal, loopVar->getLlvmValue());
    scope = node->getBodyScope() != nullptr ? node->getBodyScope() : scope;
    node->getBlock()->accept(*this);
    scope = savedScope;

    if (!isBreak) {
      builder->CreateBr(bInc);
    } else {
      isBreak = false;
    }

    bInc->insertInto(parentFct);
    builder->SetInsertPoint(bInc);
    auto* nextIdx = builder->CreateAdd(builder->CreateLoad(builder->getInt64Ty(), indexPtr),
                                       builder->getInt64(1));
    builder->CreateStore(nextIdx, indexPtr);
    builder->CreateBr(bCond);
  } else {
    bCond->insertInto(parentFct);
    builder->SetInsertPoint(bCond);
    auto hasNextValue = callMethodByName(node->getSpan(), iteratorValue.get(), "has_next");
    builder->CreateCondBr(hasNextValue->getLlvmValue(), bLoop, bEnd);

    bLoop->insertInto(parentFct);
    builder->SetInsertPoint(bLoop);
    auto nextValue = callMethodByName(node->getSpan(), iteratorValue.get(), "next");
    builder->CreateStore(nextValue->getLlvmValue(), loopVar->getLlvmValue());
    scope = node->getBodyScope() != nullptr ? node->getBodyScope() : scope;
    node->getBlock()->accept(*this);
    scope = savedScope;

    if (!isBreak) {
      builder->CreateBr(bInc);
    } else {
      isBreak = false;
    }

    bInc->insertInto(parentFct);
    builder->SetInsertPoint(bInc);
    builder->CreateBr(bCond);
  }

  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);
  breakBlocks.pop();
  continueBlocks.pop();
}

auto Codegen::visit(const FuncDecl* node) -> void {
  if (!node->getGenericParams().empty()) {
    auto savedGenerics = currentGenericTypes;
    for (const auto& name : node->getGenericParams()) {
      currentGenericTypes[name] = cacheType(std::make_unique<Type>(name));
    }
    if (selfSymbol != nullptr) {
      genericMethods[selfSymbol->getName()][node->getName()] = node;
    } else {
      genericFunctions[node->getName()] = node;
    }
    currentGenericTypes = std::move(savedGenerics);
    return;
  }
  if (selfSymbol != nullptr && node->getName() == "new" &&
      node->getReturnType()->getType() != TokenType::VOID_TYPE) {
    throw CodegenError(node->getSpan(), "Cannot create class method new with return type {}",
                       node->getReturnType()->getName());
  }
  if (node->getBody() == nullptr) {
    return;
  }

  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;
  bool shouldExport = node->isExported();

  if (selfSymbol != nullptr) {
    paramTypes.push_back(selfSymbol->getType());
    paramLLVMTypes.push_back(builder->getPtrTy());
    fields.push_back(std::make_unique<Field>("self", selfSymbol->getType()));
    shouldExport = selfSymbol->isExported();
  }

  for (auto* param : node->getParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->getType()->is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(node->getSpan(),
                         "Declared parameter type and default value do not match for {}",
                         param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType, std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* returnType = wrapNominalReturnAsPointer(result->getType());
  getOrCreateLlvmType(returnType);

  lesma::Value* existingFunc = scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    auto normalizeFunctionParamType = [](lesma::Type* type) -> lesma::Type* {
      if (type != nullptr && type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
          type->getElementType()->is(BaseType::TY_CLASS)) {
        return type->getElementType();
      }
      return type;
    };
    for (auto* candidate : scope->getSymbols()) {
      if (candidate == nullptr || candidate->getName() != node->getName() ||
          !candidate->getType()->is(BaseType::TY_FUNCTION) ||
          candidate->getLlvmValue() != nullptr) {
        continue;
      }
      auto candidateFields = candidate->getType()->getFields();
      if (candidateFields.size() != paramTypes.size()) {
        continue;
      }
      bool compatible = true;
      for (size_t i = 0; i < candidateFields.size(); ++i) {
        lesma::Type* formalType = normalizeFunctionParamType(candidateFields[i]->type);
        lesma::Type* actualType = normalizeFunctionParamType(paramTypes[i]);
        if ((formalType == nullptr) != (actualType == nullptr) ||
            (formalType != nullptr && !formalType->isEqual(actualType))) {
          compatible = false;
          break;
        }
      }
      if (compatible) {
        existingFunc = candidate;
        break;
      }
    }
  }
  if (!currentGenericTypes.empty() &&
      (existingFunc == nullptr || existingFunc->getLlvmValue() == nullptr)) {
    existingFunc = nullptr;
  }
  if (existingFunc != nullptr && existingFunc->getLlvmValue() != nullptr) {
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto mangledName =
      getMangledName(node->getSpan(), node->getName(), paramTypes, selfSymbol != nullptr);
  auto makeCallableSignatureKey = [](const std::string& name,
                                     const std::vector<lesma::Type*>& types) -> std::string {
    std::string key = name;
    for (auto* type : types) {
      key += "|" + (type != nullptr ? type->toString() : "?");
    }
    return key;
  };
  std::string const signatureKey = makeCallableSignatureKey(node->getName(), paramTypes);
  auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  llvm::Type* llvmReturnType = nullptr;
  if (returnType->is(BaseType::TY_PTR) || returnType->is(BaseType::TY_CLASS)) {
    llvmReturnType = builder->getPtrTy();
  } else {
    llvmReturnType = returnType->getLlvmType();
  }
  llvm::FunctionType* funcType =
      FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);
  auto loweredType = std::make_unique<Type>(BaseType::TY_FUNCTION, funcType, std::move(fields));
  loweredType->setReturnType(returnType);
  loweredType->setGenericParams(node->getGenericParams());
  loweredType->setVarArgs(node->getVarArgs());
  if (existingFunc != nullptr) {
    existingFunc->setType(cacheType(std::move(loweredType)));
    existingFunc->setLlvmValue(f);
    existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
    existingFunc->setMangledName(mangledName);
    specializedFunctions[mangledName] = existingFunc;
    specializedFunctions[signatureKey] = existingFunc;
    prototypes.emplace_back(existingFunc, node, selfSymbol);
    if (!currentGenericTypes.empty()) {
      specializationEnvs[existingFunc] = currentGenericTypes;
    }
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto funcSymbol = std::make_unique<Value>(node->getName(), cacheType(std::move(loweredType)), f);
  funcSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(mangledName);
  auto* funcSymbolPtr = funcSymbol.get();
  scope->insertSymbol(std::move(funcSymbol));
  specializedFunctions[mangledName] = funcSymbolPtr;
  specializedFunctions[signatureKey] = funcSymbolPtr;
  prototypes.emplace_back(funcSymbolPtr, node, selfSymbol);
  if (!currentGenericTypes.empty()) {
    specializationEnvs[funcSymbolPtr] = currentGenericTypes;
  }
  result = std::make_unique<Value>(*funcSymbolPtr);
}

auto Codegen::visit(const ExternFuncDecl* node) -> void {
  std::vector<std::unique_ptr<Field>> fields;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Type*> paramLLVMTypes;

  for (auto* param : node->getParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    // Check if it has either a type or a value or both
    if (param->type) {
      param->type->accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        // Default value determines type - make a copy
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    // If it's a class type, we mean to pass a pointer to a class
    if (typeResult->getType()->is(BaseType::TY_CLASS)) {
      // Cache the pointer type to prevent dangling pointers
      auto* ptrType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), typeResult->getType()));
      typeResult = std::make_unique<Value>("", ptrType);
    }

    if (defaultValResult && !typeResult->getType()->isEqual(defaultValResult->getType())) {
      throw CodegenError(node->getSpan(),
                         "Declared parameter type and default value do not match for {}",
                         param->name);
    }

    lesma::Type* paramType = typeResult->getType();
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    paramLLVMTypes.push_back(paramType->getLlvmType());
    fields.push_back(std::make_unique<Field>(param->name, paramType, std::move(defaultValResult)));
  }

  node->getReturnType()->accept(*this);
  lesma::Type* retType = nullptr;
  if (!result) {
    retType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  } else {
    retType = result->getType();
    getOrCreateLlvmType(retType);
  }

  lesma::Value* existingFunc = scope->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked extern symbol for {}",
                       node->getName());
  }
  if (existingFunc->getLlvmValue() != nullptr) {
    return;
  }

  Function* f = nullptr;
  if (theModule->getFunction(node->getName()) != nullptr) {
    f = theModule->getFunction(node->getName());
  } else {
    llvm::Type* llvmReturnType =
        retType->is(BaseType::TY_CLASS) ? builder->getPtrTy() : retType->getLlvmType();
    FunctionType* ft = FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
    f = llvm::cast<Function>(theModule->getOrInsertFunction(node->getName(), ft).getCallee());
    if (node->isExported()) {
      f->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }

  auto loweredType =
      std::make_unique<Type>(BaseType::TY_FUNCTION, f->getFunctionType(), std::move(fields));
  loweredType->setReturnType(retType);
  loweredType->setGenericParams(node->getGenericParams());
  loweredType->setVarArgs(node->getVarArgs());
  existingFunc->setType(cacheType(std::move(loweredType)));
  existingFunc->setLlvmValue(f);
  existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
  existingFunc->setMangledName(node->getName());
}

auto Codegen::visit(const Assignment* node) -> void {
  if (auto* subscript = dynamic_cast<SubscriptOp*>(node->getLeftHandSide())) {
    subscript->getLeft()->accept(*this);
    auto baseValue = std::move(result);
    if (baseValue != nullptr && baseValue->getType() != nullptr) {
      const TokenType assignOp = node->getOperator();
      if (assignOp == TokenType::EQUAL) {
        subscript->getIndex()->accept(*this);
        auto indexValue = std::move(result);
        node->getRightHandSide()->accept(*this);
        auto rhsValue = std::move(result);
        result = callMethodByName(node->getSpan(), baseValue.get(),
                                  std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                  {indexValue.get(), rhsValue.get()});
        return;
      }
      if (assignOp == TokenType::PLUS_EQUAL || assignOp == TokenType::MINUS_EQUAL ||
          assignOp == TokenType::SLASH_EQUAL || assignOp == TokenType::STAR_EQUAL ||
          assignOp == TokenType::MOD_EQUAL) {
        subscript->getIndex()->accept(*this);
        auto indexValue = std::move(result);
        result =
            callMethodByName(node->getSpan(), baseValue.get(),
                             std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexValue.get()});
        auto currentElem = std::move(result);
        node->getRightHandSide()->accept(*this);
        auto rhsValue = std::move(result);
        auto newValue = emitCompoundSubscriptNewValue(node->getSpan(), assignOp, currentElem.get(),
                                                      rhsValue.get());
        result = callMethodByName(node->getSpan(), baseValue.get(),
                                  std::string{OperatorUtils::SUBSCRIPT_SET_NAME},
                                  {indexValue.get(), newValue.get()});
        return;
      }
      if (assignOp == TokenType::POWER_EQUAL) {
        throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
      }
    }
  }

  lesma::Value* lhs = nullptr;
  std::unique_ptr<lesma::Value> lhsOwner; // Holds owned lhs if from result
  isAssignment = true;
  bool isPtr = false;
  if (dynamic_cast<Literal*>(node->getLeftHandSide()) != nullptr) {
    auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide());
    auto* symbol = scope->lookup(lit->getValue());
    if (symbol == nullptr) {
      throw CodegenError(node->getSpan(), "Variable not found: {}", lit->getValue());
    }
    if (!symbol->getMutability()) {
      throw CodegenError(node->getSpan(), "Assigning immutable variable a new value");
    }

    lhs = symbol;
  } else if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr ||
             dynamic_cast<SubscriptOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    lhsOwner = std::move(result);
    lhs = lhsOwner.get();
    isPtr = true;
  } else {
    throw CodegenError(node->getSpan(), "Unable to assign {} to {}",
                       node->getRightHandSide()->toString(sourceManager.get(), "", true),
                       node->getLeftHandSide()->toString(sourceManager.get(), "", true));
  }
  isAssignment = false;

  node->getRightHandSide()->accept(*this);
  auto value = cast(node->getSpan(), result.get(),
                    isPtr ? lhs->getType()->getElementType() : lhs->getType());

  switch (node->getOperator()) {
  case TokenType::EQUAL: {
    lesma::Type* slotTy = lhs->getType();
    const bool arcSlot = slotTy != nullptr && slotTy->is(BaseType::TY_PTR) &&
                         slotTy->getElementType() != nullptr &&
                         slotTy->getElementType()->is(BaseType::TY_CLASS);
    if (arcSlot) {
      llvm::Value* slot = lhs->getLlvmValue();
      llvm::Value* oldPtr = builder->CreateLoad(builder->getPtrTy(), slot, "arc.assign.old");
      llvm::Value* newPtr = value->getLlvmValue();
      emitArcRetain(newPtr);
      emitArcRelease(oldPtr, slotTy->getElementType());
      builder->CreateStore(newPtr, slot);
    } else {
      builder->CreateStore(value->getLlvmValue(), lhs->getLlvmValue());
    }
    break;
  }
  case TokenType::PLUS_EQUAL:
  case TokenType::MINUS_EQUAL:
  case TokenType::SLASH_EQUAL:
  case TokenType::STAR_EQUAL:
  case TokenType::MOD_EQUAL:
    emitCompoundAssign(node->getSpan(), node->getOperator(), lhs, value.get());
    break;
  case TokenType::POWER_EQUAL:
    throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
  default:
    throw CodegenError(node->getSpan(), "Invalid operator: {}", NAMEOF_ENUM(node->getOperator()));
  }
}

auto Codegen::visit(const Break* node) -> void {
  if (breakBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot break without being in a loop");
  }

  auto* block = breakBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::visit(const Continue* node) -> void {
  if (continueBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");
  }

  auto* block = continueBlocks.top();
  isBreak = true;

  builder->CreateBr(block);
}

auto Codegen::visit(const Return* node) -> void {
  // Check if it's top-level
  if (builder->GetInsertBlock()->getParent() == topLevelFunc) {
    throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");
  }

  // Execute all deferred statements
  for (auto* inst : deferStack.top()) {
    inst->accept(*this);
  }

  isReturn = true;

  if (node->getValue() == nullptr) {
    if (currentFunction->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      emitArcReleaseAllTrackedLocals();
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->getSpan(),
                         "Return type does not match the function return type, expected {}, "
                         "actual void",
                         currentFunction->getType()->getReturnType()->toString());
    }
  } else {
    node->getValue()->accept(*this);
    getOrCreateLlvmType(result->getType());
    lesma::Type* actualType = result->getType();
    lesma::Type* declaredReturnType = currentFunction->getType()->getReturnType();
    if (actualType != nullptr && actualType->is(BaseType::TY_PTR) &&
        actualType->getElementType() != nullptr &&
        actualType->getElementType()->is(BaseType::TY_CLASS)) {
      actualType = actualType->getElementType();
    }
    llvm::Type* actualReturnType = actualType != nullptr && actualType->is(BaseType::TY_CLASS)
                                       ? builder->getPtrTy()
                                       : result->getType()->getLlvmType();
    lesma::Type* declaredClass = nullptr;
    if (declaredReturnType != nullptr && declaredReturnType->is(BaseType::TY_PTR) &&
        declaredReturnType->getElementType() != nullptr &&
        declaredReturnType->getElementType()->is(BaseType::TY_CLASS)) {
      declaredClass = declaredReturnType->getElementType();
    } else if (declaredReturnType != nullptr && declaredReturnType->is(BaseType::TY_CLASS)) {
      declaredClass = declaredReturnType;
    }
    llvm::Type* expectedReturnType =
        declaredClass != nullptr ? builder->getPtrTy() : builder->getCurrentFunctionReturnType();
    if (actualReturnType == expectedReturnType) {
      const bool returnClassPtr = declaredClass != nullptr;
      llvm::Value* retVal = result->getLlvmValue();
      if (returnClassPtr && !exprIsFreshClassOwner(node->getValue())) {
        emitArcRetain(retVal);
      }
      emitArcReleaseAllTrackedLocals();
      builder->CreateRet(retVal);
    } else {
      throw CodegenError(node->getSpan(),
                         "Return type does not match the function return type, expected {}, "
                         "actual {}",
                         currentFunction->getType()->getReturnType()->toString(),
                         result->getType()->toString());
    }
  }
}

auto Codegen::visit(const Defer* node) -> void { deferStack.top().push_back(node->getStatement()); }

auto Codegen::visit(const UnimplementedStatement* node) -> void {
  throw CodegenError(node->getSpan(), "{}", node->getMessage());
}

auto Codegen::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Codegen::visit(const Import* node) -> void {
  compileModule(node->getSpan(), node->getFilePath(), node->isStd(), node->getAlias(),
                node->getImportAll(), node->getImportScope(), node->getImportedNames());
}

auto Codegen::visit(const TraitDecl* /*node*/) -> void {
  // Trait declarations are typechecking-only; witnesses are emitted per impl site when used.
}

auto Codegen::visit(const Class* node) -> void {
  if (!node->getGenericParams().empty()) {
    genericClasses[node->getIdentifier()] = node;
    auto* genericSymbol = scope->lookupStruct(node->getIdentifier());
    if (genericSymbol != nullptr) {
      genericSymbol->setGenericClassTemplate(node);
    }
    return;
  }

  lesma::Value* existingStruct = scope->lookupStruct(node->getIdentifier());
  if (existingStruct == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked class symbol for {}",
                       node->getIdentifier());
  }

  lesma::Type* type = existingStruct->getType();
  if (type->getLlvmType() == nullptr) {
    std::vector<llvm::Type*> elementLLVMTypes;
    elementLLVMTypes.push_back(builder->getInt64Ty());
    for (auto* f : type->getFields()) {
      elementLLVMTypes.push_back(getOrCreateLlvmType(f->type));
    }
    if (elementLLVMTypes.size() == 1U) {
      elementLLVMTypes.push_back(builder->getInt8Ty());
    }
    auto* structType =
        llvm::StructType::create(theModule->getContext(), elementLLVMTypes, node->getIdentifier());
    type->setLlvmType(structType);
  }

  auto* selfType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  methodSelfSymbols.push_back(std::make_unique<Value>(node->getIdentifier(), selfType));
  methodSelfSymbols.back()->setExported(node->isExported());
  selfSymbol = methodSelfSymbols.back().get();
  auto hasConstructor = false;
  for (auto* func : node->getMethods()) {
    func->accept(*this);
    if (func->getName() == "new") {
      hasConstructor = true;
      std::vector<lesma::Type*> constructorParams = {selfSymbol->getType()};
      auto* constructor = scope->lookupFunction("new", constructorParams);
      existingStruct->setConstructor(constructor);
    }
  }

  {
    std::unordered_set<std::string> explicitMethodNames;
    for (FuncDecl* func : node->getMethods()) {
      explicitMethodNames.insert(func->getName());
    }
    for (const std::string& traitName : node->getImplTraitNames()) {
      auto trIt = traitDeclByName.find(traitName);
      if (trIt == traitDeclByName.end()) {
        continue;
      }
      for (FuncDecl* req : trIt->second->getRequirements()) {
        if (req->getBody() == nullptr) {
          continue;
        }
        if (explicitMethodNames.contains(req->getName())) {
          continue;
        }
        req->accept(*this);
      }
    }
  }

  if (!hasConstructor) {
    throw CodegenError(node->getSpan(), "Class {} has no constructors", node->getIdentifier());
  }

  selfSymbol = nullptr;
}

auto Codegen::visit(const Enum* node) -> void {
  lesma::Value* existingEnum = scope->lookupStruct(node->getIdentifier());
  if (existingEnum == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked enum symbol for {}",
                       node->getIdentifier());
  }

  if (existingEnum->getType()->getLlvmType() == nullptr) {
    std::vector<llvm::Type*> elementTypes = {builder->getInt8Ty()};
    auto* structType =
        llvm::StructType::create(theModule->getContext(), elementTypes, node->getIdentifier());
    existingEnum->getType()->setLlvmType(structType);
  }
}

auto Codegen::visit(const FuncCall* node) -> void { result = genFuncCall(node, {}); }

auto Codegen::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  auto left = std::move(result);
  node->getRight()->accept(*this);
  auto right = std::move(result);
  lesma::Type* finalType = CodegenTypeUtils::getExtendedType(left->getType(), right->getType());
  if (finalType == nullptr && left->getType()->is(BaseType::TY_ENUM) &&
      right->getType()->is(BaseType::TY_ENUM) && left->getType()->isEqual(right->getType())) {
    finalType = left->getType();
  }

  switch (node->getOperator()) {
  case TokenType::MINUS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFSub(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSub(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFAdd(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateAdd(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFMul(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateMul(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFDiv(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSDiv(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", finalType, builder->CreateFRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", finalType, builder->CreateSRem(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::POWER:
    if (finalType == nullptr) {
      break;
    }

    if (!right->getType()->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw CodegenError(node->getSpan(), "Cannot use non-numbers for power coefficient: {}",
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    throw CodegenError(node->getSpan(), "Power operator not implemented yet.");
  case TokenType::EQUAL_EQUAL: {
    Type* ltyEq = left->getType();
    Type* rtyEq = right->getType();
    if (ltyEq != nullptr && rtyEq != nullptr) {
      if (ltyEq->is(BaseType::TY_PTR) && rtyEq->is(BaseType::TY_INT)) {
        llvm::Value* lv = builder->CreatePtrToInt(left->getLlvmValue(), builder->getInt64Ty());
        llvm::Value* rv = right->getLlvmValue();
        result = std::make_unique<Value>(
            "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
            builder->CreateICmpEQ(lv, rv));
        return;
      }
      if (rtyEq->is(BaseType::TY_PTR) && ltyEq->is(BaseType::TY_INT)) {
        llvm::Value* rv = builder->CreatePtrToInt(right->getLlvmValue(), builder->getInt64Ty());
        llvm::Value* lv = left->getLlvmValue();
        result = std::make_unique<Value>(
            "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
            builder->CreateICmpEQ(lv, rv));
        return;
      }
    }
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                           leftName, rightName);
      }

      llvm::Value* leftVal = builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal = builder->CreateExtractValue(right->getLlvmValue(), {0});
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_BOOL)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpEQ(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  }
  case TokenType::BANG_EQUAL: {
    Type* ltyNe = left->getType();
    Type* rtyNe = right->getType();
    if (ltyNe != nullptr && rtyNe != nullptr) {
      if (ltyNe->is(BaseType::TY_PTR) && rtyNe->is(BaseType::TY_INT)) {
        llvm::Value* lv = builder->CreatePtrToInt(left->getLlvmValue(), builder->getInt64Ty());
        llvm::Value* rv = right->getLlvmValue();
        result = std::make_unique<Value>(
            "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
            builder->CreateICmpNE(lv, rv));
        return;
      }
      if (rtyNe->is(BaseType::TY_PTR) && ltyNe->is(BaseType::TY_INT)) {
        llvm::Value* rv = builder->CreatePtrToInt(right->getLlvmValue(), builder->getInt64Ty());
        llvm::Value* lv = left->getLlvmValue();
        result = std::make_unique<Value>(
            "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
            builder->CreateICmpNE(lv, rv));
        return;
      }
    }
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    // Enum comparison
    if (finalType->is(BaseType::TY_ENUM)) {
      // Both are pointers to structs
      auto leftName = left->getType()->getLlvmType()->getStructName().str();
      auto rightName = right->getType()->getLlvmType()->getStructName().str();

      if (leftName != rightName) {
        throw CodegenError(node->getSpan(), "Illegal comparison of two different enums: {} and {}",
                           leftName, rightName);
      }

      llvm::Value* leftVal = builder->CreateExtractValue(left->getLlvmValue(), {0});
      llvm::Value* rightVal = builder->CreateExtractValue(right->getLlvmValue(), {0});
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(leftVal, rightVal));
      return;
    }

    if (finalType->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpONE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_BOOL)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpNE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }
    break;
  }
  case TokenType::GREATER:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->is(BaseType::TY_FLOAT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGT(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSGE(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLT(left->getLlvmValue(), right->getLlvmValue()));
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
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateICmpSLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    break;
  case TokenType::AND:
    if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(node->getSpan(), "Cannot use non-booleans for and: {} - {}",
                         node->getLeft()->toString(sourceManager.get(), "", true),
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalAnd(left->getLlvmValue(), right->getLlvmValue()));
    return;
  case TokenType::OR:
    if (!left->getType()->is(BaseType::TY_BOOL) && !right->getType()->is(BaseType::TY_BOOL)) {
      throw CodegenError(node->getSpan(), "Cannot use non-booleans for or: {} - {}",
                         node->getLeft()->toString(sourceManager.get(), "", true),
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    result = std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
        builder->CreateLogicalOr(left->getLlvmValue(), right->getLlvmValue()));
    return;
  default:
    throw CodegenError(node->getSpan(), "Unimplemented binary operator: {}",
                       NAMEOF_ENUM(node->getOperator()));
  }

  if (auto operatorName = OperatorUtils::getBinaryOperatorName(node->getOperator());
      operatorName.has_value()) {
    result =
        callMethodByName(node->getSpan(), left.get(), std::string{*operatorName}, {right.get()});
    return;
  }

  throw CodegenError(node->getSpan(), "Operator {} is not supported for types {} and {}",
                     NAMEOF_ENUM(node->getOperator()), left->getType()->toString(),
                     right->getType()->toString());
}

auto Codegen::visit(const SubscriptOp* node) -> void {
  node->getLeft()->accept(*this);
  auto listValue = std::move(result);
  node->getIndex()->accept(*this);
  auto indexValue = std::move(result);
  if (listValue != nullptr && listValue->getType() != nullptr &&
      listValue->getType()->is(BaseType::TY_ARRAY)) {
    if (isAssignment) {
      throw CodegenError(node->getSpan(), "Operator [] assignment requires operator []=");
    }
    result = callMethodByName(node->getSpan(), listValue.get(),
                              std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexValue.get()});
    return;
  }
  if (isAssignment) {
    throw CodegenError(node->getSpan(), "Operator [] assignment requires operator []=");
  }
  result = callMethodByName(node->getSpan(), listValue.get(),
                            std::string{OperatorUtils::SUBSCRIPT_GET_NAME}, {indexValue.get()});
}

auto Codegen::visit(const DotOp* node) -> void {
  node->getLeft()->accept(*this);
  auto leftValue = std::move(result);
  if (leftValue != nullptr && leftValue->getType() != nullptr &&
      leftValue->getType()->is(BaseType::TY_ARRAY)) {
    auto* call = dynamic_cast<FuncCall*>(node->getRight());
    if (call == nullptr) {
      throw CodegenError(node->getSpan(), "Expected list method call after dot");
    }
    std::vector<std::unique_ptr<lesma::Value>> argStorage;
    std::vector<lesma::Value*> args;
    for (auto* arg : call->getArguments()) {
      arg->accept(*this);
      argStorage.push_back(std::move(result));
      args.push_back(argStorage.back().get());
    }
    std::vector<lesma::Type*> explicitTypeArgs;
    for (auto* explicitTypeArg : call->getExplicitTypeArgs()) {
      explicitTypeArg->accept(*this);
      explicitTypeArgs.push_back(result->getType());
    }
    result =
        callMethodByName(node->getSpan(), leftValue.get(), call->getName(), args, explicitTypeArgs);
    return;
  }

  if (leftValue != nullptr && leftValue->getType() != nullptr) {
    lesma::Type* forTrait = leftValue->getType();
    if (forTrait->is(BaseType::TY_PTR) && forTrait->getElementType() != nullptr) {
      forTrait = forTrait->getElementType();
    }
    if (forTrait->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
      auto* call = dynamic_cast<FuncCall*>(node->getRight());
      if (call == nullptr) {
        throw CodegenError(node->getSpan(), "Expected method call after dot on trait value");
      }
      std::vector<std::unique_ptr<lesma::Value>> argStorage;
      std::vector<lesma::Value*> args;
      for (auto* arg : call->getArguments()) {
        arg->accept(*this);
        argStorage.push_back(std::move(result));
        args.push_back(argStorage.back().get());
      }
      std::vector<lesma::Type*> explicitTypeArgs;
      for (auto* explicitTypeArg : call->getExplicitTypeArgs()) {
        explicitTypeArg->accept(*this);
        explicitTypeArgs.push_back(result->getType());
      }
      result = callMethodByName(node->getSpan(), leftValue.get(), call->getName(), args,
                                explicitTypeArgs);
      return;
    }
  }

  if (leftValue != nullptr && leftValue->getType() != nullptr) {
    lesma::Type* receiverType = leftValue->getType();
    if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr &&
        receiverType->getElementType()->is(BaseType::TY_CLASS)) {
      receiverType = receiverType->getElementType();
    }
    if (receiverType->is(BaseType::TY_CLASS)) {
      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
        throw CodegenError(node->getRight()->getSpan(),
                           "Expected identifier or method call right-hand of dot operator, "
                           "found {}",
                           node->getRight()->toString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
          TokenType::IDENTIFIER == dynamic_cast<Literal*>(node->getRight())->getType()) {
        field = dynamic_cast<Literal*>(node->getRight())->getValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->getRight());
      }

      auto* cls = lookupClassStructSymbol(receiverType);
      if (cls == nullptr) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}",
                           receiverType->getDisplayName().empty() ? "(unknown)"
                                                                  : receiverType->getDisplayName());
      }
      cls->setName(receiverType->getDisplayName().empty()
                       ? receiverType->getLlvmType()->getStructName().str()
                       : receiverType->getDisplayName());

      if (!field.empty()) {
        auto index = TypeUtils::findIndexInFields(cls->getType(), field);
        auto* type = TypeUtils::findTypeInFields(cls->getType(), field);
        if (index == -1) {
          throw CodegenError(node->getRight()->getSpan(), "Could not find field {} in {}", field,
                             receiverType->getLlvmType()->getStructName().str());
        }

        const unsigned llvmIdx = cls->getType()->is(BaseType::TY_CLASS)
                                     ? classUserFieldLlvmIndex(static_cast<unsigned>(index))
                                     : static_cast<unsigned>(index);
        auto* ptr = builder->CreateStructGEP(cls->getType()->getLlvmType(),
                                             leftValue->getLlvmValue(), llvmIdx);
        if (isAssignment) {
          result = std::make_unique<Value>(
              "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type)),
              ptr);
          return;
        }
        result = std::make_unique<Value>("", type, builder->CreateLoad(type->getLlvmType(), ptr));
        return;
      }
      if (method != nullptr) {
        auto receiverValue = std::move(leftValue);
        std::vector<std::unique_ptr<lesma::Value>> argStorage;
        std::vector<lesma::Value*> args;
        for (auto* arg : method->getArguments()) {
          arg->accept(*this);
          argStorage.push_back(std::move(result));
          args.push_back(argStorage.back().get());
        }
        std::vector<lesma::Type*> explicitTypeArgs;
        for (auto* explicitTypeArg : method->getExplicitTypeArgs()) {
          explicitTypeArg->accept(*this);
          explicitTypeArgs.push_back(result->getType());
        }
        result = callMethodByName(node->getSpan(), receiverValue.get(), method->getName(), args,
                                  explicitTypeArgs);
        return;
      }
    }
  }

  if (auto* left = dynamic_cast<Literal*>(node->getLeft())) {
    if (left->getType() != TokenType::IDENTIFIER) {
      throw CodegenError(node->getLeft()->getSpan(),
                         "Expected identifier left-hand of dot operator, found {}",
                         node->getRight()->toString(sourceManager.get(), "", true));
    }

    auto* typeSym = scope->lookupType(left->getValue());
    if (typeSym != nullptr) {
      // Assuming it's an enum or statically accessed class
      if (!typeSym->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS, BaseType::TY_IMPORT})) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}",
                           left->getValue());
      }

      auto* right = dynamic_cast<Literal*>(node->getRight());
      if (typeSym->is(BaseType::TY_ENUM)) {
        // Check if right-hand expression is an identifier expression
        if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier right-hand of dot operator, found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        if (right->getType() != TokenType::IDENTIFIER) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier right-hand of dot operator, found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        // Setting value to the enum
        auto val = TypeUtils::findIndexInFields(typeSym, right->getValue());
        // Field not found in enum
        if (val == -1) {
          throw CodegenError(node->getLeft()->getSpan(), "Identifier {} not in {}",
                             right->getValue(), left->getValue());
        }

        auto* structVal = scope->lookupStruct(left->getValue());
        auto* enumPtr = builder->CreateAlloca(structVal->getType()->getLlvmType());
        auto* field = builder->CreateStructGEP(structVal->getType()->getLlvmType(), enumPtr, 0);
        builder->CreateStore(builder->getInt8(val), field);
        // Enum variant literals are returned by value (not pointer).
        auto* enumVal = builder->CreateLoad(structVal->getType()->getLlvmType(), enumPtr);

        result = std::make_unique<Value>("", structVal->getType(), enumVal);
        return;
      }

      if (typeSym->is(BaseType::TY_IMPORT)) {
        std::string field;
        FuncCall const* method = nullptr;

        if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
            (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
          throw CodegenError(node->getRight()->getSpan(),
                             "Expected identifier or method call right-hand of dot operator, "
                             "found {}",
                             node->getRight()->toString(sourceManager.get(), "", true));
        }

        if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
            dynamic_cast<Literal*>(node->getRight())->getType() == TokenType::IDENTIFIER) {
          field = dynamic_cast<Literal*>(node->getRight())->getValue();
        } else {
          method = dynamic_cast<FuncCall*>(node->getRight());
        }

        if (method != nullptr) {
          auto tmpAlias = alias;
          alias = left->getValue();
          result = genFuncCall(method, {});
          alias = tmpAlias;
          return;
        }
      }
    } else {
      // Assuming it's a class instance
      left->accept(*this);
      // We refer to the class type, if it's a pointer, we get the result
      lesma::Type* lesmaType = result->getType();
      if (result->getType()->is(BaseType::TY_PTR) &&
          result->getType()->getElementType()->is(BaseType::TY_CLASS)) {
        lesmaType = result->getType()->getElementType();
      }

      if (!lesmaType->is(BaseType::TY_CLASS)) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot apply dot accessor on {}",
                           left->getValue());
      }

      std::string field;
      FuncCall const* method = nullptr;

      if ((dynamic_cast<Literal*>(node->getRight()) == nullptr) &&
          (dynamic_cast<FuncCall*>(node->getRight()) == nullptr)) {
        throw CodegenError(node->getRight()->getSpan(),
                           "Expected identifier or method call right-hand of dot operator, "
                           "found {}",
                           node->getRight()->toString(sourceManager.get(), "", true));
      }

      if ((dynamic_cast<Literal*>(node->getRight()) != nullptr) &&
          TokenType::IDENTIFIER == dynamic_cast<Literal*>(node->getRight())->getType()) {
        field = dynamic_cast<Literal*>(node->getRight())->getValue();
      } else {
        method = dynamic_cast<FuncCall*>(node->getRight());
      }

      auto* cls = lookupClassStructSymbol(lesmaType);
      if (cls == nullptr) {
        throw CodegenError(node->getLeft()->getSpan(), "Cannot find related class {}",
                           lesmaType->getDisplayName().empty() ? "(unknown)"
                                                               : lesmaType->getDisplayName());
      }
      cls->setName(lesmaType->getDisplayName().empty()
                       ? lesmaType->getLlvmType()->getStructName().str()
                       : lesmaType->getDisplayName());

      if (cls->getType()->is(BaseType::TY_CLASS)) {
        if (!field.empty()) {
          auto index = TypeUtils::findIndexInFields(cls->getType(), field);
          auto* type = TypeUtils::findTypeInFields(cls->getType(), field);
          if (index == -1) {
            throw CodegenError(
                node->getRight()->getSpan(), "Could not find field {} in {}", field,
                result->getType()->getElementType()->getLlvmType()->getStructName().str());
          }

          const unsigned llvmIdx = cls->getType()->is(BaseType::TY_CLASS)
                                       ? classUserFieldLlvmIndex(static_cast<unsigned>(index))
                                       : static_cast<unsigned>(index);
          auto* ptr = builder->CreateStructGEP(cls->getType()->getLlvmType(),
                                             result->getLlvmValue(), llvmIdx);
          if (isAssignment) {
            result = std::make_unique<Value>(
                "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type)),
                ptr);
            return;
          }
          //                    auto &x = cls->GetType()->GetFields()[index];
          result = std::make_unique<Value>("", type, builder->CreateLoad(type->getLlvmType(), ptr));
          return;
        }
        if (method != nullptr) {
          auto receiverValue = std::move(result);
          std::vector<std::unique_ptr<lesma::Value>> argStorage;
          std::vector<lesma::Value*> args;
          for (auto* arg : method->getArguments()) {
            arg->accept(*this);
            argStorage.push_back(std::move(result));
            args.push_back(argStorage.back().get());
          }
          std::vector<lesma::Type*> explicitTypeArgs;
          for (auto* explicitTypeArg : method->getExplicitTypeArgs()) {
            explicitTypeArg->accept(*this);
            explicitTypeArgs.push_back(result->getType());
          }
          result = callMethodByName(node->getSpan(), receiverValue.get(), method->getName(), args,
                                    explicitTypeArgs);
          return;
        }
      }
    }
  }
  throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}",
                     node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  auto expr = std::move(result);
  node->getType()->accept(*this);
  auto* castType = result->getType();
  result = cast(node->getSpan(), expr.get(), castType);
}

auto Codegen::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  auto* leftType = result->getType();
  node->getRight()->accept(*this);
  auto* rightType = result->getType();
  if (leftType != nullptr && leftType->is(BaseType::TY_PTR) &&
      leftType->getElementType() != nullptr && leftType->getElementType()->is(BaseType::TY_CLASS)) {
    leftType = leftType->getElementType();
  }
  if (rightType != nullptr && rightType->is(BaseType::TY_PTR) &&
      rightType->getElementType() != nullptr &&
      rightType->getElementType()->is(BaseType::TY_CLASS)) {
    rightType = rightType->getElementType();
  }

  llvm::Value* val = nullptr;
  bool typesEqual = leftType->isEqual(rightType);
  if (!typesEqual && leftType != nullptr && rightType != nullptr &&
      leftType->getBaseType() == rightType->getBaseType() &&
      TypeUtils::isNominalTypeForIdentity(leftType)) {
    typesEqual = leftType->toString() == rightType->toString();
  }

  if (node->getOperator() == TokenType::IS) {
    val = typesEqual ? builder->getTrue() : builder->getFalse();
  } else {
    val = typesEqual ? builder->getFalse() : builder->getTrue();
  }

  result = std::make_unique<Value>(
      "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), val);
}

auto Codegen::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);
  auto operand = std::move(result);

  llvm::Value* val = nullptr;
  lesma::Type* type = operand->getType();
  std::unique_ptr<lesma::Type> ptrTypeHolder; // Keep owned type alive if created

  if (node->getOperator() == TokenType::MINUS) {
    if (operand->getType()->is(BaseType::TY_INT)) {
      val = builder->CreateNeg(operand->getLlvmValue());
    } else if (operand->getType()->is(BaseType::TY_FLOAT)) {
      val = builder->CreateFNeg(operand->getLlvmValue());
    } else {
      result =
          callMethodByName(node->getSpan(), operand.get(),
                           std::string{*OperatorUtils::getUnaryOperatorName(node->getOperator())});
      return;
    }
  } else if (node->getOperator() == TokenType::NOT || node->getOperator() == TokenType::BANG) {
    if (operand->getType()->is(BaseType::TY_BOOL)) {
      val = builder->CreateNot(operand->getLlvmValue());
    } else {
      result =
          callMethodByName(node->getSpan(), operand.get(),
                           std::string{*OperatorUtils::getUnaryOperatorName(node->getOperator())});
      return;
    }
  } else if (node->getOperator() == TokenType::STAR) {
    if (operand->getType()->is(BaseType::TY_PTR)) {
      val = builder->CreateLoad(operand->getType()->getElementType()->getLlvmType(),
                                operand->getLlvmValue());
      type = operand->getType()->getElementType();
    } else {
      throw CodegenError(node->getSpan(), "Cannot apply {} to {}", NAMEOF_ENUM(node->getOperator()),
                         node->getExpression()->toString(sourceManager.get(), "", true));
    }
  } else if (node->getOperator() == TokenType::AMPERSAND) {
    val = builder->CreateAlloca(operand->getType()->getLlvmType());
    ptrTypeHolder =
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), operand->getType());
    type = ptrTypeHolder.get();
    builder->CreateStore(operand->getLlvmValue(), val);
  } else {
    throw CodegenError(node->getSpan(), "Unknown unary operator, cannot apply {} to {}",
                       NAMEOF_ENUM(node->getOperator()),
                       node->getExpression()->toString(sourceManager.get(), "", true));
  }

  // For AMPERSAND case, Value needs to own the Type since ptrTypeHolder will go
  // out of scope
  if (ptrTypeHolder) {
    result = std::make_unique<Value>(std::move(ptrTypeHolder));
    result->setLlvmValue(val);
  } else {
    result = std::make_unique<Value>("", type, val);
  }
}

auto Codegen::visit(const ListLiteral* node) -> void {
  lesma::Type* listType = node->getResolvedType();
  if (listType != nullptr && listType->is(BaseType::TY_CLASS)) {
    auto fields = listType->getFields();
    if (fields.empty() || fields.front()->type == nullptr ||
        !fields.front()->type->is(BaseType::TY_ARRAY) ||
        fields.front()->type->getElementType() == nullptr) {
      throw CodegenError(node->getSpan(),
                         "List literal resolved to invalid class (expected __buffer<T> field)");
    }
    auto* bufferType = fields.front()->type;
    auto* structType = cast<llvm::StructType>(getOrCreateLlvmType(listType));
    auto* classSize =
        builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
    auto* classHandle = emitArcAllocClass(classSize, "list.obj");
    auto* storagePtr = builder->CreateStructGEP(structType, classHandle, classUserFieldLlvmIndex(0),
                                                "list.storage.ptr");

    auto* listStructTy = getOrCreateListStructType(bufferType);
    auto* headerSize = builder->getInt64(
        theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
    auto* bufferHandle = emitMalloc(headerSize, "list.header");
    llvm::Value* dataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
    auto elements = node->getElements();
    if (!elements.empty()) {
      auto* elementLlvmType = getListStoredElementType(bufferType);
      auto* byteSize = builder->getInt64(
          theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue() *
          elements.size());
      dataPtr = emitMalloc(byteSize, "list.data");
      for (size_t i = 0; i < elements.size(); ++i) {
        elements[i]->accept(*this);
        auto* elementPtr =
            builder->CreateGEP(elementLlvmType, dataPtr, builder->getInt64(i), "list.elem.ptr");
        builder->CreateStore(
            getListStoredElementValue(node->getSpan(), result.get(), bufferType->getElementType()),
            elementPtr);
      }
    }
    auto* count = builder->getInt64(elements.size());
    emitStoreListDataPtr(bufferType, bufferHandle, dataPtr);
    emitStoreListLength(bufferType, bufferHandle, count);
    emitStoreListCapacity(bufferType, bufferHandle, count);
    builder->CreateStore(bufferHandle, storagePtr);
    result = std::make_unique<Value>("", listType, classHandle);
    return;
  }
  if (listType == nullptr || !listType->is(BaseType::TY_ARRAY) ||
      listType->getElementType() == nullptr) {
    throw CodegenError(node->getSpan(), "List literal has no resolved list/buffer type");
  }

  lesma::Type* elementType = listType->getElementType();
  llvm::Type* elementLlvmType = getListStoredElementType(listType);
  getOrCreateLlvmType(listType);
  auto* listStructTy = getOrCreateListStructType(listType);
  std::vector<Expression*> elements = node->getElements();
  auto* headerSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(listStructTy).getFixedValue());
  auto* listHandle = emitMalloc(headerSize, "list.header");

  llvm::Value* dataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
  if (!elements.empty()) {
    uint64_t elementSize =
        theModule->getDataLayout().getTypeAllocSize(elementLlvmType).getFixedValue();
    auto* byteSize = builder->getInt64(elementSize * elements.size());
    dataPtr = emitMalloc(byteSize, "list.data");

    for (size_t i = 0; i < elements.size(); ++i) {
      elements[i]->accept(*this);
      auto* elementPtr =
          builder->CreateGEP(elementLlvmType, dataPtr, builder->getInt64(i), "list.elem.ptr");
      builder->CreateStore(
          getListStoredElementValue(elements[i]->getSpan(), result.get(), elementType), elementPtr);
    }
  }

  auto* count = builder->getInt64(elements.size());
  emitStoreListDataPtr(listType, listHandle, dataPtr);
  emitStoreListLength(listType, listHandle, count);
  emitStoreListCapacity(listType, listHandle, count);
  result = std::make_unique<Value>("", listType, listHandle);
}

auto Codegen::visit(const Literal* node) -> void {
  // Cache Types to prevent dangling pointers when result is reassigned
  if (node->getType() == TokenType::DOUBLE) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_FLOAT, builder->getDoubleTy()));
    result = std::make_unique<Value>(
        "", type, ConstantFP::get(theModule->getContext(), APFloat(std::stod(node->getValue()))));
  } else if (node->getType() == TokenType::INTEGER) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    type->setIntWidth(64);
    result = std::make_unique<Value>(
        "", type, ConstantInt::getSigned(builder->getInt64Ty(), std::stoi(node->getValue())));
  } else if (node->getType() == TokenType::BOOL) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
    result = std::make_unique<Value>(
        "", type, node->getValue() == "true" ? builder->getTrue() : builder->getFalse());
  } else if (node->getType() == TokenType::STRING) {
    lesma::Type* strClass = node->getResolvedStrClassType();
    if (strClass != nullptr && strClass->is(BaseType::TY_CLASS)) {
      auto fields = strClass->getFields();
      if (fields.empty() || fields.front()->type == nullptr ||
          !fields.front()->type->is(BaseType::TY_STRING)) {
        throw CodegenError(node->getSpan(), "String literal resolved to invalid stdlib str class");
      }
      auto* structType = cast<llvm::StructType>(getOrCreateLlvmType(strClass));
      auto* classSize = builder->getInt64(
          theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
      auto* classHandle = emitArcAllocClass(classSize, "str.obj");
      auto* storagePtr =
          builder->CreateStructGEP(structType, classHandle, classUserFieldLlvmIndex(0), "str.storage.ptr");
      llvm::Value* globalStr = builder->CreateGlobalString(node->getValue());
      builder->CreateStore(globalStr, storagePtr);
      result = std::make_unique<Value>("", strClass, classHandle);
    } else {
      auto* type = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
      result = std::make_unique<Value>("", type, builder->CreateGlobalString(node->getValue()));
    }
  } else if (node->getType() == TokenType::NIL) {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    result =
        std::make_unique<Value>("", type, ConstantPointerNull::getNullValue(builder->getPtrTy()));
  } else if (node->getType() == TokenType::IDENTIFIER) {
    // Look this variable up in the function.
    auto* val = scope->lookup(node->getValue());
    if (val == nullptr) {
      throw CodegenError(node->getSpan(), "Unknown variable name {}", node->getValue());
    }
    result = materializeSymbolValue(val);
  } else {
    throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
  }
}

auto Codegen::visit(const Else* /*node*/) -> void {
  auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
  result = std::make_unique<Value>("", type, llvm::ConstantInt::getTrue(theModule->getContext()));
}

auto Codegen::cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
    -> std::unique_ptr<lesma::Value> {
  return CodegenTypeUtils::cast(span, val, type, builder.get());
}

auto Codegen::symbolUsesDirectLlvmValue(const lesma::Value* symbol) const -> bool {
  return symbol != nullptr && symbol->usesDirectLlvmValue();
}

auto Codegen::materializeSymbolValue(lesma::Value* symbol) -> std::unique_ptr<lesma::Value> {
  if (symbol == nullptr) {
    return nullptr;
  }

  getOrCreateLlvmType(symbol->getType());
  if (symbolUsesDirectLlvmValue(symbol)) {
    return std::make_unique<Value>(*symbol);
  }

  llvm::Value* llvmVal =
      builder->CreateLoad(symbol->getType()->getLlvmType(), symbol->getLlvmValue());
  return std::make_unique<Value>("", symbol->getType(), llvmVal);
}

auto Codegen::getMangledName(llvm::SMRange span, std::string funcName,
                             const std::vector<lesma::Type*>& paramTypes, bool isMethodFlag,
                             std::string alias) -> std::string {
  alias = alias.empty() ? this->alias : alias;
  std::string name = (alias.empty() ? "" : "&" + alias + "=>") +
                     (selfSymbol != nullptr && isMethodFlag
                          ? selfSymbol->getName() + "::" + std::move(funcName) + ":"
                          : "." + std::move(funcName) + ":");
  bool first = true;

  for (auto* paramType : paramTypes) {
    if (!first) {
      name += ",";
    } else {
      first = false;
    }
    name += MangleUtils::getTypeMangledName(span, paramType);
  }

  return name;
}

auto Codegen::emitCompoundAssignArithmetic(llvm::SMRange span, TokenType compoundOp,
                                           lesma::Value* loaded, lesma::Value* rhs)
    -> std::unique_ptr<lesma::Value> {
  lesma::Type* targetType = loaded->getType();
  if (targetType == nullptr ||
      (!targetType->is(BaseType::TY_FLOAT) && !targetType->is(BaseType::TY_INT))) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(compoundOp));
  }
  const bool isFloat = targetType->is(BaseType::TY_FLOAT);
  auto* varVal = loaded->getLlvmValue();
  llvm::Value* newVal = nullptr;

  switch (compoundOp) {
  case TokenType::PLUS_EQUAL:
    newVal = isFloat ? builder->CreateFAdd(rhs->getLlvmValue(), varVal)
                     : builder->CreateAdd(rhs->getLlvmValue(), varVal);
    break;
  case TokenType::MINUS_EQUAL:
    newVal = isFloat ? builder->CreateFSub(varVal, rhs->getLlvmValue())
                     : builder->CreateSub(varVal, rhs->getLlvmValue());
    break;
  case TokenType::SLASH_EQUAL:
    newVal = isFloat ? builder->CreateFDiv(varVal, rhs->getLlvmValue())
                     : builder->CreateSDiv(varVal, rhs->getLlvmValue());
    break;
  case TokenType::STAR_EQUAL:
    newVal = isFloat ? builder->CreateFMul(rhs->getLlvmValue(), varVal)
                     : builder->CreateMul(rhs->getLlvmValue(), varVal);
    break;
  case TokenType::MOD_EQUAL:
    newVal = isFloat ? builder->CreateFRem(varVal, rhs->getLlvmValue())
                     : builder->CreateSRem(varVal, rhs->getLlvmValue());
    break;
  default:
    throw CodegenError(span, "Invalid compound operator: {}", NAMEOF_ENUM(compoundOp));
  }
  return std::make_unique<Value>("", targetType, newVal);
}

auto Codegen::emitCompoundSubscriptNewValue(llvm::SMRange span, TokenType compoundOp,
                                            lesma::Value* currentElem, lesma::Value* rhs)
    -> std::unique_ptr<lesma::Value> {
  TokenType binOp = TokenType::PLUS;
  switch (compoundOp) {
  case TokenType::PLUS_EQUAL:
    binOp = TokenType::PLUS;
    break;
  case TokenType::MINUS_EQUAL:
    binOp = TokenType::MINUS;
    break;
  case TokenType::STAR_EQUAL:
    binOp = TokenType::STAR;
    break;
  case TokenType::SLASH_EQUAL:
    binOp = TokenType::SLASH;
    break;
  case TokenType::MOD_EQUAL:
    binOp = TokenType::MOD;
    break;
  default:
    throw CodegenError(span, "Invalid compound operator: {}", NAMEOF_ENUM(compoundOp));
  }

  lesma::Type* lhsTy = currentElem->getType();
  lesma::Type* rhsTy = rhs->getType();
  lesma::Type* finalType = CodegenTypeUtils::getExtendedType(lhsTy, rhsTy);
  if (finalType == nullptr && lhsTy->is(BaseType::TY_ENUM) && rhsTy->is(BaseType::TY_ENUM) &&
      lhsTy->isEqual(rhsTy)) {
    finalType = lhsTy;
  }
  auto left = cast(span, currentElem, finalType);
  auto right = cast(span, rhs, finalType);
  if (finalType != nullptr && finalType->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
    return emitCompoundAssignArithmetic(span, compoundOp, left.get(), right.get());
  }
  if (auto operatorName = OperatorUtils::getBinaryOperatorName(binOp); operatorName.has_value()) {
    return callMethodByName(span, left.get(), std::string{*operatorName}, {right.get()});
  }
  throw CodegenError(span, "Operator {} is not supported for compound subscript assignment",
                     NAMEOF_ENUM(compoundOp));
}

auto Codegen::emitCompoundAssign(llvm::SMRange span, TokenType op, lesma::Value* lhs,
                                 lesma::Value* value) -> void {
  lesma::Type* targetType = lhs->getType();
  if (targetType != nullptr && targetType->is(BaseType::TY_PTR) &&
      targetType->getElementType() != nullptr) {
    targetType = targetType->getElementType();
  }
  if (targetType == nullptr ||
      (!targetType->is(BaseType::TY_FLOAT) && !targetType->is(BaseType::TY_INT))) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(op));
  }
  auto* varVal = builder->CreateLoad(targetType->getLlvmType(), lhs->getLlvmValue());
  auto loaded = std::make_unique<Value>("", targetType, varVal);
  auto newVal = emitCompoundAssignArithmetic(span, op, loaded.get(), value);
  builder->CreateStore(newVal->getLlvmValue(), lhs->getLlvmValue());
}

auto Codegen::appendCallableArgument(lesma::Value* arg, std::vector<lesma::Type*>& paramTypes,
                                     std::vector<llvm::Value*>& paramsLLVM) -> void {
  lesma::Type* argType = arg->getType();
  if (argType->is(BaseType::TY_CLASS)) {
    lesma::Type* ptrType = nullptr;
    for (auto& t : typeCache) {
      if (t->is(BaseType::TY_PTR) && t->getElementType() == argType) {
        ptrType = t.get();
        break;
      }
    }
    if (ptrType != nullptr) {
      argType = ptrType;
    } else {
      argType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), argType));
    }
  }
  paramTypes.push_back(argType);
  llvm::Value* llvmArg = arg->getLlvmValue();
  // Static class methods use a null `self` at the call site; materialize as a typed null pointer so
  // LLVM does not get an untyped null operand (breaks the optimizer on invalid IR).
  if (llvmArg == nullptr && argType->is(BaseType::TY_PTR)) {
    llvm::Type* lt = getOrCreateLlvmType(argType);
    if (lt != nullptr && lt->isPointerTy()) {
      llvmArg = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(lt));
    }
  }
  paramsLLVM.push_back(llvmArg);
}

auto Codegen::callNamedFunction(llvm::SMRange span, const std::string& functionName,
                                const std::vector<lesma::Type*>& paramTypes,
                                const std::vector<llvm::Value*>& paramsLLVM,
                                const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> localParamTypes = paramTypes;
  std::vector<llvm::Value*> localParamsLLVM = paramsLLVM;
  auto makeCallableSignatureKey = [](const std::string& name,
                                     const std::vector<lesma::Type*>& types) -> std::string {
    std::string key = name;
    for (auto* type : types) {
      key += "|" + (type != nullptr ? type->toString() : "?");
    }
    return key;
  };
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }
  Value* symbol = nullptr;
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->lookupStruct(functionName);
  llvm::Value* classPtr = nullptr;
  std::unique_ptr<Type> selfParamType;

  if (classSym == nullptr || (classSym->getType()->is(BaseType::TY_CLASS) &&
                              classSym->getType()->getLlvmType() == nullptr)) {
    const Class* templateClass = nullptr;
    if (auto git = genericClasses.find(functionName); git != genericClasses.end()) {
      templateClass = git->second;
    } else if (classSym != nullptr && classSym->getGenericClassTemplate() != nullptr) {
      templateClass = static_cast<const Class*>(classSym->getGenericClassTemplate());
    }
    if (templateClass != nullptr) {
      classSym = specializeClass(templateClass, localParamTypes, explicitTypeArgs);
    }
  }

  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    auto* classLlvmType = getOrCreateLlvmType(classSym->getType());
    auto* classSize = builder->getInt64(
        theModule->getDataLayout().getTypeAllocSize(classLlvmType).getFixedValue());
    classPtr = emitArcAllocClass(classSize, functionName + ".obj");
    localParamsLLVM.insert(localParamsLLVM.begin(), classPtr);
    selfParamType =
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), classSym->getType());
    localParamTypes.insert(localParamTypes.begin(), selfParamType.get());

    selfSymbol = classSym;
    symbol = scope->lookupFunction("new", localParamTypes);
  } else {
    auto directSignatureKey = makeCallableSignatureKey(functionName, localParamTypes);
    if (auto directIt = specializedFunctions.find(directSignatureKey);
        directIt != specializedFunctions.end()) {
      symbol = directIt->second;
    }
    auto normalizeFunctionParamType = [](lesma::Type* type) -> lesma::Type* {
      if (type != nullptr && type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
          type->getElementType()->is(BaseType::TY_CLASS)) {
        return type->getElementType();
      }
      return type;
    };
    for (auto* candidate : scope->getSymbols()) {
      if (symbol != nullptr) {
        break;
      }
      if (candidate == nullptr || candidate->getName() != functionName ||
          !candidate->getType()->is(BaseType::TY_FUNCTION) ||
          candidate->getLlvmValue() == nullptr) {
        continue;
      }
      auto candidateFields = candidate->getType()->getFields();
      if (candidateFields.size() != localParamTypes.size()) {
        continue;
      }
      bool compatible = true;
      for (size_t i = 0; i < candidateFields.size(); ++i) {
        lesma::Type* formalType = normalizeFunctionParamType(candidateFields[i]->type);
        lesma::Type* actualType = normalizeFunctionParamType(localParamTypes[i]);
        if ((formalType == nullptr) != (actualType == nullptr) ||
            (formalType != nullptr && !formalType->isEqual(actualType))) {
          compatible = false;
          break;
        }
      }
      if (compatible) {
        symbol = candidate;
        break;
      }
    }
    if (symbol == nullptr) {
      auto directMangledName = getMangledName(span, functionName, localParamTypes, false);
      if (auto directIt = specializedFunctions.find(directMangledName);
          directIt != specializedFunctions.end()) {
        symbol = directIt->second;
      } else {
        symbol = scope->lookupFunction(functionName, localParamTypes);
      }
    }
  }

  if (symbol == nullptr) {
    throw CodegenError(span, "{} {} not in current scope.",
                       classSym != nullptr ? "Constructor for" : "Function", functionName);
  }
  if (symbol->getLlvmValue() == nullptr) {
    auto directMangledName =
        getMangledName(span, functionName, localParamTypes, selfSymbol != nullptr);
    if (auto* directFunction = theModule->getFunction(directMangledName);
        directFunction != nullptr) {
      symbol->setLlvmValue(directFunction);
    }
  }

  if (symbol->getType()->getFields().size() == localParamTypes.size() &&
      symbol->getLlvmValue() == nullptr) {
    const FuncDecl* templateDecl = nullptr;
    if (selfSymbol != nullptr) {
      auto cit = genericMethods.find(selfSymbol->getName());
      if (cit != genericMethods.end()) {
        auto mit = cit->second.find(functionName);
        if (mit != cit->second.end()) {
          templateDecl = mit->second;
        }
      }
    }
    if (templateDecl == nullptr) {
      auto git = genericFunctions.find(functionName);
      if (git != genericFunctions.end()) {
        templateDecl = git->second;
      }
    }
    if (templateDecl != nullptr) {
      std::vector<std::string> genericNames = templateDecl->getGenericParams();
      symbol = specializeFunction(templateDecl, localParamTypes, genericNames, explicitTypeArgs);
    }
  }

  if (symbol != nullptr && symbol->getType() != nullptr &&
      symbol->getType()->is(BaseType::TY_FUNCTION)) {
    auto ff = symbol->getType()->getFields();
    for (size_t i = 0; i < localParamsLLVM.size() && i < ff.size(); ++i) {
      lesma::Type* formal = ff[i]->type;
      if (formal != nullptr && formal->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
        lesma::Type* actual = localParamTypes[i];
        lesma::Type* cls = actual;
        if (actual->is(BaseType::TY_PTR) && actual->getElementType() != nullptr &&
            actual->getElementType()->is(BaseType::TY_CLASS)) {
          cls = actual->getElementType();
        }
        if (cls->is(BaseType::TY_CLASS)) {
          getOrCreateLlvmType(formal);
          localParamsLLVM[i] = emitBoxClassToExistential(formal, actual, localParamsLLVM[i]);
          localParamTypes[i] = formal;
        }
      }
    }
  }

  if (!symbol->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
    throw CodegenError(span, "Symbol {} is not a function or constructor.", functionName);
  }

  if (symbol->getType()->getFields().size() > localParamsLLVM.size()) {
    auto fields = symbol->getType()->getFields();
    for (size_t i = localParamsLLVM.size(); i < fields.size(); ++i) {
      auto* field = fields[i];
      if (field->defaultValue == nullptr) {
        throw CodegenError(
            span, "Something bad happened, lookup found a function with incorrect defaults",
            functionName);
      }
      localParamsLLVM.push_back(field->defaultValue->getLlvmValue());
    }
  }

  llvm::Value* callableValue = nullptr;
  if (symbol->getType()->is(BaseType::TY_CLASS)) {
    if (symbol->getConstructor() == nullptr ||
        symbol->getConstructor()->getLlvmValue() == nullptr) {
      throw CodegenError(span, "Constructor for {} is declared but not defined", functionName);
    }
    callableValue = symbol->getConstructor()->getLlvmValue();
  } else {
    if (symbol->getLlvmValue() == nullptr) {
      throw CodegenError(span, "Function {} is declared but not defined", functionName);
    }
    callableValue = symbol->getLlvmValue();
  }
  auto* func = llvm::cast<Function>(callableValue);
  auto fieldsForArc = symbol->getType()->getFields();
  const size_t skipArcRetain =
      (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) ? 1U : 0U;
  emitArcRetainOutgoingCallArgs(fieldsForArc, localParamsLLVM, skipArcRetain);
  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    builder->CreateCall(func, localParamsLLVM);
    selfSymbol = selfSymbolTmp;
    return std::make_unique<Value>("", classSym->getType(), classPtr);
  }

  selfSymbol = selfSymbolTmp;
  return std::make_unique<Value>("", symbol->getType()->getReturnType(),
                                 builder->CreateCall(func, localParamsLLVM));
}

auto Codegen::isBuiltinListBuiltinMethodName(const std::string& methodName) const -> bool {
  return methodName == "len" || methodName == "clear" || methodName == "push" ||
         methodName == "pop" || methodName == "copy" ||
         methodName == std::string{OperatorUtils::SUBSCRIPT_GET_NAME} ||
         methodName == std::string{OperatorUtils::SUBSCRIPT_SET_NAME};
}

auto Codegen::callListMethodByName(llvm::SMRange span, lesma::Value* receiver,
                                   const std::string& methodName,
                                   const std::vector<lesma::Value*>& args,
                                   const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  (void) explicitTypeArgs;
  lesma::Type* receiverType = receiver->getType();
  lesma::Type* listClassType = nullptr;
  lesma::Type* bufferType = nullptr;
  llvm::Value* bufferHandle = receiver->getLlvmValue();

  if (receiverType->is(BaseType::TY_ARRAY)) {
    bufferType = receiverType;
  } else {
    if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr) {
      receiverType = receiverType->getElementType();
    }
    if (receiverType->is(BaseType::TY_CLASS)) {
      auto fields = receiverType->getFields();
      if (!fields.empty() && fields.front()->type != nullptr &&
          fields.front()->type->is(BaseType::TY_ARRAY)) {
        listClassType = receiverType;
        bufferType = fields.front()->type;
        auto* storagePtr = builder->CreateStructGEP(
            cast<llvm::StructType>(getOrCreateLlvmType(listClassType)), receiver->getLlvmValue(),
            classUserFieldLlvmIndex(0), "list.storage.ptr");
        bufferHandle =
            builder->CreateLoad(getOrCreateLlvmType(bufferType), storagePtr, "list.storage");
      }
    }
  }
  if (bufferType == nullptr || bufferHandle == nullptr) {
    throw CodegenError(span, "Function {} not in current scope.", methodName);
  }

  if (methodName == "len") {
    auto* type = cacheType(std::make_unique<Type>(BaseType::TY_INT, builder->getInt64Ty()));
    type->setIntWidth(64);
    return std::make_unique<Value>("", type, emitListLength(bufferType, bufferHandle));
  }
  if (methodName == "clear") {
    auto* currentData = emitListDataPtr(bufferType, bufferHandle);
    llvm::Function* parentFunction = builder->GetInsertBlock()->getParent();
    auto* freeBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "list.clear.free", parentFunction);
    auto* doneBlock =
        llvm::BasicBlock::Create(theModule->getContext(), "list.clear.done", parentFunction);
    builder->CreateCondBr(
        builder->CreateICmpNE(currentData, llvm::ConstantPointerNull::get(builder->getPtrTy())),
        freeBlock, doneBlock);
    builder->SetInsertPoint(freeBlock);
    emitListReleaseClassElementsIfNeeded(bufferType, bufferHandle);
    emitFree(currentData);
    builder->CreateBr(doneBlock);
    builder->SetInsertPoint(doneBlock);
    emitStoreListDataPtr(bufferType, bufferHandle,
                         llvm::ConstantPointerNull::get(builder->getPtrTy()));
    emitStoreListLength(bufferType, bufferHandle, builder->getInt64(0));
    emitStoreListCapacity(bufferType, bufferHandle, builder->getInt64(0));
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  if (methodName == "push") {
    if (args.size() != 1U) {
      throw CodegenError(span, "push expects one argument");
    }
    auto* length = emitListLength(bufferType, bufferHandle);
    auto* nextLength = builder->CreateAdd(length, builder->getInt64(1));
    emitListEnsureCapacity(bufferType, bufferHandle, nextLength);
    auto* elementPtr =
        builder->CreateGEP(getListStoredElementType(bufferType),
                           emitListDataPtr(bufferType, bufferHandle), length, "list.push.ptr");
    llvm::Value* elemVal =
        getListStoredElementValue(span, args[0], bufferType->getElementType());
    if (bufferType->getElementType() != nullptr &&
        bufferType->getElementType()->is(BaseType::TY_CLASS)) {
      emitArcRetain(elemVal);
    }
    builder->CreateStore(elemVal, elementPtr);
    emitStoreListLength(bufferType, bufferHandle, nextLength);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  if (methodName == "pop") {
    auto* length = emitListLength(bufferType, bufferHandle);
    emitListBoundsCheck(span, bufferType, bufferHandle,
                        builder->CreateSub(length, builder->getInt64(1)));
    auto* newLength = builder->CreateSub(length, builder->getInt64(1), "list.pop.len");
    auto* elementPtr =
        builder->CreateGEP(getListStoredElementType(bufferType),
                           emitListDataPtr(bufferType, bufferHandle), newLength, "list.pop.ptr");
    auto* poppedValue = builder->CreateLoad(getListStoredElementType(bufferType), elementPtr);
    emitStoreListLength(bufferType, bufferHandle, newLength);
    return std::make_unique<Value>("", bufferType->getElementType(), poppedValue);
  }
  if (methodName == "copy") {
    auto* copiedBuffer = emitListDeepCopy(bufferType, bufferHandle);
    if (listClassType == nullptr) {
      return std::make_unique<Value>("", bufferType, copiedBuffer);
    }
    auto* classLlvmType = getOrCreateLlvmType(listClassType);
    auto* classSize = builder->getInt64(
        theModule->getDataLayout().getTypeAllocSize(classLlvmType).getFixedValue());
    auto* classHandle = emitArcAllocClass(classSize, "list.copy.obj");
    auto* storagePtr = builder->CreateStructGEP(cast<llvm::StructType>(classLlvmType), classHandle,
                                                classUserFieldLlvmIndex(0), "list.copy.storage.ptr");
    builder->CreateStore(copiedBuffer, storagePtr);
    return std::make_unique<Value>("", listClassType, classHandle);
  }
  if (methodName == std::string{OperatorUtils::SUBSCRIPT_GET_NAME}) {
    if (args.size() != 1U) {
      throw CodegenError(span, "operator [] expects one argument");
    }
    auto* elementPtr =
        emitListElementPointer(span, bufferType, bufferHandle, args[0]->getLlvmValue());
    return std::make_unique<Value>(
        "", bufferType->getElementType(),
        builder->CreateLoad(getListStoredElementType(bufferType), elementPtr));
  }
  if (methodName == std::string{OperatorUtils::SUBSCRIPT_SET_NAME}) {
    if (args.size() != 2U) {
      throw CodegenError(span, "operator []= expects two arguments");
    }
    auto* elementPtr =
        emitListElementPointer(span, bufferType, bufferHandle, args[0]->getLlvmValue());
    llvm::Value* newVal =
        getListStoredElementValue(span, args[1], bufferType->getElementType());
    if (bufferType->getElementType() != nullptr &&
        bufferType->getElementType()->is(BaseType::TY_CLASS)) {
      llvm::Value* oldVal =
          builder->CreateLoad(getListStoredElementType(bufferType), elementPtr, "list.sub.old");
      emitArcRetain(newVal);
      emitArcRelease(oldVal, bufferType->getElementType());
    }
    builder->CreateStore(newVal, elementPtr);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }

  throw CodegenError(span, "Function {} not in current scope.", methodName);
}

auto Codegen::callMethodByName(llvm::SMRange span, lesma::Value* receiver,
                               const std::string& methodName,
                               const std::vector<lesma::Value*>& args,
                               const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  if (receiver->getType()->is(BaseType::TY_ARRAY)) {
    return callListMethodByName(span, receiver, methodName, args, explicitTypeArgs);
  }

  lesma::Type* receiverType = receiver->getType();
  if (receiverType->is(BaseType::TY_PTR) && receiverType->getElementType() != nullptr) {
    receiverType = receiverType->getElementType();
  }
  if (receiverType->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    return callExistentialMethod(span, receiver, methodName, args, explicitTypeArgs);
  }
  if (receiverType->is(BaseType::TY_CLASS)) {
    auto fields = receiverType->getFields();
    if (!fields.empty() && fields.front()->type != nullptr &&
        fields.front()->type->is(BaseType::TY_ARRAY)) {
      if (isBuiltinListBuiltinMethodName(methodName)) {
        return callListMethodByName(span, receiver, methodName, args, explicitTypeArgs);
      }
    }
  }
  if (!receiverType->is(BaseType::TY_CLASS)) {
    throw CodegenError(span, "Method {} requires class receiver", methodName);
  }

  auto* cls = lookupClassStructSymbol(receiverType);
  if (cls == nullptr) {
    throw CodegenError(span, "Cannot find related class {}",
                       receiverType->getDisplayName().empty() ? "(unknown)"
                                                              : receiverType->getDisplayName());
  }
  cls->setName(receiverType->getDisplayName().empty()
                   ? receiverType->getLlvmType()->getStructName().str()
                   : receiverType->getDisplayName());

  auto* savedSelfSymbol = selfSymbol;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  appendCallableArgument(receiver, paramTypes, paramsLLVM);
  for (auto* arg : args) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }
  lesma::Value* directMethod = scope->lookupFunction(methodName, paramTypes);
  if (directMethod == nullptr) {
    for (const auto& importedScope : *importedScopes) {
      if (importedScope == nullptr) {
        continue;
      }
      directMethod = importedScope->lookupFunction(methodName, paramTypes);
      if (directMethod != nullptr) {
        break;
      }
    }
  }
  if (directMethod != nullptr && directMethod->getLlvmValue() != nullptr) {
    // Method symbols may retain template types with TY_GENERIC; call sites are not inside
    // defineFunction(), so currentGenericTypes is empty. Restore the specialization env so
    // getOrCreateLlvmType (e.g. on return types) can resolve T.
    auto savedGenerics = currentGenericTypes;
    if (auto genIt = specializationEnvs.find(directMethod); genIt != specializationEnvs.end()) {
      currentGenericTypes = genIt->second;
    } else if (auto clsEnvIt = specializedClassTypeEnvs.find(receiverType);
               clsEnvIt != specializedClassTypeEnvs.end()) {
      currentGenericTypes = clsEnvIt->second;
    } else {
      // Keys are Type* from specializeClass; the receiver may reference a distinct but equal Type
      // from the type cache (same class, different pointer).
      for (const auto& entry : specializedClassTypeEnvs) {
        if (entry.first != nullptr && entry.first->isEqual(receiverType)) {
          currentGenericTypes = entry.second;
          break;
        }
      }
    }
    try {
      std::vector<llvm::Value*> finalParams;
      auto fields = directMethod->getType()->getFields();
      finalParams.reserve(paramsLLVM.size());
      for (size_t i = 0; i < paramsLLVM.size(); ++i) {
        auto paramVal = std::make_unique<Value>("", paramTypes[i], paramsLLVM[i]);
        auto castVal = cast(span, paramVal.get(), fields[i]->type);
        finalParams.push_back(castVal->getLlvmValue());
      }
      lesma::Type* returnTy = directMethod->getType()->getReturnType();
      if (returnTy != nullptr) {
        getOrCreateLlvmType(returnTy);
      }
      selfSymbol = savedSelfSymbol;
      emitArcRetainOutgoingCallArgs(fields, finalParams, 0U);
      auto* callResult =
          builder->CreateCall(llvm::cast<Function>(directMethod->getLlvmValue()), finalParams);
      currentGenericTypes = std::move(savedGenerics);
      return std::make_unique<Value>("", returnTy, callResult);
    } catch (...) {
      currentGenericTypes = std::move(savedGenerics);
      throw;
    }
  }
  selfSymbol = cls;
  auto resultValue = callNamedFunction(span, methodName, paramTypes, paramsLLVM, explicitTypeArgs);
  selfSymbol = savedSelfSymbol;
  return resultValue;
}

auto Codegen::genFuncCall(const FuncCall* node, const std::vector<lesma::Value*>& extraParams = {})
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  std::vector<lesma::Type*> explicitTypeArgs;

  for (auto* arg : extraParams) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }

  for (auto* arg : node->getArguments()) {
    arg->accept(*this);
    appendCallableArgument(result.get(), paramTypes, paramsLLVM);
  }

  for (auto* explicitTypeArg : node->getExplicitTypeArgs()) {
    explicitTypeArg->accept(*this);
    explicitTypeArgs.push_back(result->getType());
  }

  if (isListIntrinsicName(node->getName())) {
    return genListIntrinsicCall(node, paramTypes, paramsLLVM);
  }
  return callNamedFunction(node->getSpan(), node->getName(), paramTypes, paramsLLVM,
                           explicitTypeArgs);
}
