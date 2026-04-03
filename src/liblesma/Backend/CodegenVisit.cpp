#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "Codegen.h"
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
#include "liblesma/Common/OperatorUtils.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"
#include "liblesma/Typecheck/Typechecker.h"

namespace {

[[nodiscard]] auto makeCallableSignatureKey(const std::string& name,
                                            const std::vector<lesma::Type*>& types) -> std::string {
  std::string key = name;
  for (lesma::Type* type : types) {
    key += "|" + (type != nullptr ? type->toString() : "?");
  }
  return key;
}

[[nodiscard]] auto makeResolvedCallableKey(const lesma::Value* symbol) -> std::string {
  if (symbol == nullptr) {
    return {};
  }
  lesma::Type* callableType = symbol->getType();
  if (callableType == nullptr || !callableType->is(lesma::BaseType::TY_FUNCTION)) {
    return symbol->getName();
  }
  auto fields = callableType->getFields();
  std::vector<lesma::Type*> lookupArgs;
  size_t start = fields.empty() ? 0U : 1U;
  lookupArgs.reserve(fields.size() - start);
  for (size_t i = start; i < fields.size(); ++i) {
    lesma::Field* field = fields[i];
    lookupArgs.push_back(field != nullptr ? field->type : nullptr);
  }
  return makeCallableSignatureKey(symbol->getName(), lookupArgs);
}

/** Vtable initializers must reference Function* in the emitting module; imported codegen may still
 *  hold pointers into a module that was already moved to the JIT. */
[[nodiscard]] auto materializeVtableFunctionPointer(llvm::Module* mod,
                                                    llvm::PointerType* ptrTyTyped, lesma::Value* rs)
    -> llvm::Constant* {
  if (rs == nullptr) {
    return nullptr;
  }
  llvm::Function* fn = nullptr;
  std::string const& mangled = rs->getMangledName();
  if (!mangled.empty()) {
    fn = mod->getFunction(mangled);
  }
  if (fn == nullptr) {
    llvm::Value* lv = rs->getLlvmValue();
    if (llvm::isa_and_present<llvm::Function>(lv)) {
      auto* maybe = llvm::cast<llvm::Function>(lv);
      if (maybe->getParent() == mod) {
        fn = maybe;
      } else if (!maybe->getName().empty()) {
        fn = mod->getFunction(maybe->getName());
        if (fn == nullptr) {
          fn = llvm::Function::Create(maybe->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                                      maybe->getName(), *mod);
          fn->setVisibility(llvm::GlobalValue::HiddenVisibility);
        }
      }
    }
  }
  if (fn == nullptr) {
    return nullptr;
  }
  return llvm::ConstantExpr::getBitCast(fn, ptrTyTyped);
}

[[nodiscard]] auto remapVtableSlotConstant(llvm::Module* mod, llvm::PointerType* ptrTyTyped,
                                           llvm::Constant* c) -> llvm::Constant* {
  if (c == nullptr || llvm::isa<llvm::ConstantPointerNull>(c)) {
    return c;
  }
  llvm::SmallPtrSet<llvm::Value*, 8> visited;
  llvm::Value* strip = c;
  while (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(strip)) {
    if (!visited.insert(strip).second) {
      return c;
    }
    unsigned const op = ce->getOpcode();
    if (op == llvm::Instruction::BitCast || op == llvm::Instruction::AddrSpaceCast) {
      llvm::Value* next = ce->getOperand(0);
      if (!llvm::isa_and_present<llvm::Value>(next)) {
        return c;
      }
      strip = next;
      continue;
    }
    break;
  }
  if (!llvm::isa_and_present<llvm::Function>(strip)) {
    return c;
  }
  auto* fn = llvm::cast<llvm::Function>(strip);
  llvm::Function* local = mod->getFunction(fn->getName());
  if (local == nullptr) {
    local = llvm::Function::Create(fn->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                                   fn->getName(), *mod);
    local->setVisibility(llvm::GlobalValue::HiddenVisibility);
  }
  return llvm::ConstantExpr::getBitCast(local, ptrTyTyped);
}

} // namespace

using namespace lesma;

Codegen::Codegen(
    std::shared_ptr<Parser> parser, std::shared_ptr<SourceMgr> srcMgr, const std::string& filename,
    std::vector<std::string> imports, bool jit, bool main, std::string alias,
    const std::shared_ptr<ThreadSafeContext>& context,
    std::shared_ptr<std::vector<std::string>> sharedModules,
    std::shared_ptr<std::vector<std::unique_ptr<SymbolTable>>> sharedScopes,
    std::shared_ptr<std::vector<ImportedSpecializationState>> sharedImportedSpecializationStates,
    std::unique_ptr<SymbolTable> preScope, std::vector<std::unique_ptr<lesma::Type>> preTypeCache,
    std::unordered_map<lesma::Type*, std::unordered_map<std::string, lesma::Type*>>
        preSpecializedClassTypeEnvs,
    std::unordered_map<lesma::Type*, lesma::Type*> preSpecializedClassTemplateOf,
    std::unordered_map<std::string, lesma::Type*> preSpecializedClassTypesByKey, bool emitDebug,
    llvm::OptimizationLevel optimizationLevelForDebugArg,
    std::shared_ptr<std::vector<std::string>> sharedPendingJitModuleInits) {
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
  specializedClassTemplateOf.merge(std::move(preSpecializedClassTemplateOf));
  specializedClassTypesByKey.merge(std::move(preSpecializedClassTypesByKey));

  this->alias = std::move(alias);
  this->filename = filename;
  isMain = main;
  isJit = jit;

  if (sharedPendingJitModuleInits != nullptr) {
    pendingJitModuleInits = std::move(sharedPendingJitModuleInits);
  } else if (jit) {
    pendingJitModuleInits = std::make_shared<std::vector<std::string>>();
  }

  if (sharedModules && sharedScopes) {
    importedModules = std::move(sharedModules);
    importedScopes = std::move(sharedScopes);
    importedSpecializationStates =
        sharedImportedSpecializationStates != nullptr
            ? std::move(sharedImportedSpecializationStates)
            : std::make_shared<std::vector<ImportedSpecializationState>>();
  } else {
    importedModules = std::make_shared<std::vector<std::string>>(std::move(imports));
    importedScopes = std::make_shared<std::vector<std::unique_ptr<SymbolTable>>>();
    importedSpecializationStates = std::make_shared<std::vector<ImportedSpecializationState>>();
  }
  emitDebugInfo = emitDebug;
  optimizationLevelForDebug = optimizationLevelForDebugArg;
  initializeDebugMetadata();
  topLevelFunc = initializeTopLevel();
  // base.les is loaded at the start of run() so we don't load it during
  // constructor re-entrancy when creating Codegens for imported modules.
}

auto Codegen::defineFunction(lesma::Value* value, const FuncDecl* node, Value* clsSymbol) -> void {
  bool const isMethod = value->getDeclarationKind() == ValueDeclarationKind::METHOD;
  auto const fieldsForParams = value->getType()->getFields();
  bool const usesImplicitSelfParam = isMethod && !value->isStaticMethod() &&
                                     !fieldsForParams.empty() &&
                                     fieldsForParams.front()->name == "self";
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
  SymbolTable* templateBodyScope = value->getBodyScope();
  scope = templateBodyScope;
  if (scope != nullptr && specializationEnvs.contains(value)) {
    scope = savedScope->createChildBlock(node->getName() + ".specialized");
    std::vector<std::string> paramNames;
    paramNames.reserve(node->getParameters().size() + 1U);
    if (usesImplicitSelfParam) {
      paramNames.push_back("self");
    }
    for (auto* param : node->getParameters()) {
      paramNames.push_back(param->name);
    }
    for (auto* symbol : templateBodyScope->getSymbols()) {
      if (symbol == nullptr) {
        continue;
      }
      if (std::find(paramNames.begin(), paramNames.end(), symbol->getName()) == paramNames.end()) {
        continue;
      }
      auto seeded = std::make_unique<Value>(*symbol);
      seeded->setLlvmValue(nullptr);
      seeded->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(seeded));
    }
  }
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
  pushDeferBaseline();

  if (value->getLlvmValue() == nullptr) {
    throw CodegenError(node->getSpan(), "Function {} is declared but has no LLVM body",
                       node->getName());
  }
  auto* f = llvm::cast<Function>(value->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);
  setDebugLoc(node->getBody()->getSpan());

  llvm::DIFile* declFile = moduleDiFile;
  unsigned declLine = 1U;
  if (node->getSpan().isValid() && node->getSpan().Start.isValid()) {
    unsigned const bid = sourceManager->FindBufferContainingLoc(node->getSpan().Start);
    if (bid != 0U) {
      declFile = getOrCreateDiFileForBuffer(bid);
    }
    declLine = sourceManager->getLineAndColumn(node->getSpan().Start).first;
  }

  int fieldIndex = 0;
  for (const auto& field : value->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);
    std::string paramName;

    if (field->name == "self") {
      paramName = "self";
    } else {
      size_t const paramIndex = param->getArgNo() - (usesImplicitSelfParam ? 1U : 0U);
      if (paramIndex < node->getParameters().size()) {
        paramName = node->getParameters()[paramIndex]->name;
      } else {
        paramName = field->name;
      }
    }
    param->setName(paramName);

    Value* existingParam = lookupInCurrentScope(paramName);
    if (field->type != nullptr && field->type->is(BaseType::TY_FUNCTION) &&
        existingParam != nullptr && existingParam->getStoresFuncValuePair()) {
      existingParam->setType(field->type);
      existingParam->setLlvmValue(param);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      emitParameterDebugDeclare(f, param, paramName, static_cast<unsigned>(fieldIndex + 1),
                                declFile, declLine, param->getType(), nullptr);
      fieldIndex++;
      continue;
    }

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
    llvm::Instruction* storeParam = builder->CreateStore(param, ptr);
    emitParameterDebugDeclare(f, ptr, paramName, static_cast<unsigned>(fieldIndex + 1), declFile,
                              declLine, param->getType(), storeParam);

    if (existingParam != nullptr && existingParam->getLlvmValue() == nullptr) {
      existingParam->setType(field->type);
      existingParam->setLlvmValue(ptr);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    } else {
      auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
      symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(symbol));
    }

    fieldIndex++;
  }

  // Vtable pointer is set at the allocation site (`ClassName(...)` / malloc) for the concrete
  // class. Do not re-emit here: a base `super.new(self)` would otherwise overwrite a derived
  // object's vtable and break dynamic dispatch (traits / overrides).

  node->getBody()->accept(*this);

  if (llvm::BasicBlock* cur = builder->GetInsertBlock();
      cur != nullptr && cur->getTerminator() == nullptr && cur->empty()) {
    lesma::Type* rt = value->getType()->getReturnType();
    if (rt != nullptr && rt->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      builder->CreateUnreachable();
    }
  }

  auto instrs = deferStack.top();
  deferStack.pop();
  deferBaselineStack.pop();

  if (!isReturn) {
    runDeferredStatements(instrs);
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
  // Clear stale DILocation so top-level IR does not inherit the last function's DISubprogram.
  builder->SetCurrentDebugLocation(llvm::DebugLoc());
}

auto Codegen::defineLambdaFunction(lesma::Value* value, const LambdaExpr* node) -> void {
  SymbolTable* savedScope = scope;
  scope = value->getBodyScope();
  if (scope == nullptr) {
    scope = savedScope->createChildBlock("lambda_spec");
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
  pushDeferBaseline();

  if (value->getLlvmValue() == nullptr) {
    throw CodegenError(node->getSpan(), "Lambda specialization has no LLVM function");
  }
  auto* f = llvm::cast<Function>(value->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);
  llvm::SMRange const bodySpan = node->isExpressionBody() ? node->getExpressionBody()->getSpan()
                                                          : node->getBlockBody()->getSpan();
  setDebugLoc(bodySpan);

  llvm::DIFile* declFile = moduleDiFile;
  unsigned declLine = 1U;
  if (node->getSpan().isValid() && node->getSpan().Start.isValid()) {
    unsigned const bid = sourceManager->FindBufferContainingLoc(node->getSpan().Start);
    if (bid != 0U) {
      declFile = getOrCreateDiFileForBuffer(bid);
    }
    declLine = sourceManager->getLineAndColumn(node->getSpan().Start).first;
  }

  int fieldIndex = 0;
  for (const auto& field : value->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);
    std::string paramName = field->name;
    if (fieldIndex < static_cast<int>(node->getParameters().size())) {
      paramName = node->getParameters()[static_cast<size_t>(fieldIndex)]->name;
    }
    param->setName(paramName);

    Value* existingParam = lookupInCurrentScope(paramName);
    if (field->type != nullptr && field->type->is(BaseType::TY_FUNCTION) &&
        existingParam != nullptr && existingParam->getStoresFuncValuePair()) {
      existingParam->setLlvmValue(param);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      emitParameterDebugDeclare(f, param, paramName, static_cast<unsigned>(fieldIndex + 1),
                                declFile, declLine, param->getType(), nullptr);
      fieldIndex++;
      continue;
    }

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
    llvm::Instruction* storeParam = builder->CreateStore(param, ptr);
    emitParameterDebugDeclare(f, ptr, paramName, static_cast<unsigned>(fieldIndex + 1), declFile,
                              declLine, param->getType(), storeParam);

    if (existingParam != nullptr && existingParam->getLlvmValue() == nullptr) {
      existingParam->setLlvmValue(ptr);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    } else {
      auto sym = std::make_unique<Value>(field->name, field->type, ptr);
      sym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(sym));
    }

    fieldIndex++;
  }

  if (node->isExpressionBody()) {
    node->getExpressionBody()->accept(*this);
    llvm::Value* rv = result != nullptr ? result->getLlvmValue() : nullptr;
    Type* rt = value->getType()->getReturnType();
    flushDeferredFramesForReturn();
    if (rt != nullptr && rt->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      if (rv == nullptr) {
        throw CodegenError(node->getSpan(), "Lambda body did not produce a value");
      }
      builder->CreateRet(rv);
    }
  } else if (node->getBlockBody() != nullptr) {
    node->getBlockBody()->accept(*this);
    if (builder->GetInsertBlock()->getTerminator() == nullptr) {
      Type* rt = value->getType()->getReturnType();
      if (rt != nullptr && rt->is(BaseType::TY_VOID)) {
        flushDeferredFramesForReturn();
        builder->CreateRetVoid();
      } else {
        throw CodegenError(node->getSpan(), "Non-void lambda may reach end without returning");
      }
    }
  }

  deferStack.pop();
  deferBaselineStack.pop();

  for (BasicBlock& bb : *f) {
    if (bb.getTerminator() != nullptr) {
      continue;
    }
    if (value->getType()->getReturnType()->is(BaseType::TY_VOID)) {
      builder->SetInsertPoint(&bb);
      builder->CreateRetVoid();
    } else {
      throw CodegenError(node->getSpan(), "Lambda does not always return a result");
    }
  }

  isReturn = false;
  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(node->getSpan(), "Invalid specialized lambda\n{}", verifyOutput);
  }

  scope = savedScope;
  currentFunction = nullptr;
  builder->SetInsertPoint(&topLevelFunc->back());
  builder->SetCurrentDebugLocation(llvm::DebugLoc());
}

auto Codegen::getFuncValuePairLlvmType() -> llvm::StructType* {
  if (funcValuePairLlvmType == nullptr) {
    funcValuePairLlvmType = llvm::StructType::create(
        theModule->getContext(),
        std::array<llvm::Type*, 2>{builder->getPtrTy(), builder->getPtrTy()}, "lesma.funcval");
  }
  return funcValuePairLlvmType;
}

auto Codegen::visit(const Statement* node) -> void {
  lesma::print("Visited a blank statement\n{}", node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const Expression* node) -> void {
  lesma::print("Visited a blank expression\n{}", node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const Compound* node) -> void {
  setDebugLoc(node->getSpan());
  for (auto* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

namespace {
[[nodiscard]] auto typeContainsUnboundGenericImpl(lesma::Type* type,
                                                  std::unordered_set<lesma::Type*>& active)
    -> bool {
  if (type == nullptr) {
    return false;
  }
  if (type->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (type->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    return typeContainsUnboundGenericImpl(type->getElementType(), active);
  }
  if (type->is(BaseType::TY_CLASS)) {
    if (active.contains(type)) {
      return false;
    }
    active.insert(type);
    bool any = false;
    for (Field* f : type->getFields()) {
      if (typeContainsUnboundGenericImpl(f->type, active)) {
        any = true;
        break;
      }
    }
    if (!any) {
      for (Field* f : type->getStaticFields()) {
        if (typeContainsUnboundGenericImpl(f->type, active)) {
          any = true;
          break;
        }
      }
    }
    active.erase(type);
    return any;
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    for (Field* f : type->getFields()) {
      if (typeContainsUnboundGenericImpl(f->type, active)) {
        return true;
      }
    }
    return typeContainsUnboundGenericImpl(type->getReturnType(), active);
  }
  if (type->is(BaseType::TY_TUPLE)) {
    for (Field* f : type->getFields()) {
      if (typeContainsUnboundGenericImpl(f->type, active)) {
        return true;
      }
    }
    return false;
  }
  if (type->is(BaseType::TY_UNION)) {
    for (lesma::Type* m : type->getUnionMembers()) {
      if (typeContainsUnboundGenericImpl(m, active)) {
        return true;
      }
    }
    return false;
  }
  return false;
}

[[nodiscard]] auto typeContainsUnboundGeneric(lesma::Type* type) -> bool {
  std::unordered_set<lesma::Type*> active;
  return typeContainsUnboundGenericImpl(type, active);
}
} // namespace

auto Codegen::isLesmaPtrToClass(lesma::Type* t) -> bool {
  return t != nullptr && t->is(BaseType::TY_PTR) && t->getElementType() != nullptr &&
         t->getElementType()->is(BaseType::TY_CLASS);
}

auto Codegen::llvmStorageTypeForVarSlot(lesma::Type* storedType, lesma::Value* existing)
    -> llvm::Type* {
  const bool ptrToClass = isLesmaPtrToClass(storedType);
  llvm::Type* storageLlvmTy = (storedType->is(BaseType::TY_CLASS) || ptrToClass)
                                  ? builder->getPtrTy()
                                  : storedType->getLlvmType();
  if (existing != nullptr && storedType->is(BaseType::TY_FUNCTION) &&
      existing->getStoresFuncValuePair()) {
    storageLlvmTy = getFuncValuePairLlvmType();
  }
  return storageLlvmTy;
}

auto Codegen::emitSimpleClassPtrOrCastStore(llvm::SMRange span, llvm::Value* destPtr,
                                            std::unique_ptr<lesma::Value>& valueResult,
                                            lesma::Type* storedType) -> llvm::Instruction* {
  std::unique_ptr<lesma::Value> unwrappedFnPair;
  lesma::Value* valueForStore = valueResult.get();
  if (valueResult->getStoresFuncValuePair()) {
    llvm::StructType* pairTy = getFuncValuePairLlvmType();
    llvm::Value* payload =
        builder->CreateLoad(pairTy, valueResult->getLlvmValue(), "fnpair.simplestore.unwrap");
    unwrappedFnPair = std::make_unique<Value>("", valueResult->getType(), payload);
    unwrappedFnPair->setStoresFuncValuePair(false);
    unwrappedFnPair->setCategory(valueResult->getCategory());
    valueForStore = unwrappedFnPair.get();
  }
  const bool ptrToClass = isLesmaPtrToClass(storedType);
  if (ptrToClass && valueForStore->getType() != nullptr &&
      valueForStore->getType()->is(BaseType::TY_PTR)) {
    return builder->CreateStore(valueForStore->getLlvmValue(), destPtr);
  }
  lesma::Type* castTarget = ptrToClass ? storedType->getElementType() : storedType;
  auto castVal = cast(span, valueForStore, castTarget);
  return builder->CreateStore(castVal->getLlvmValue(), destPtr);
}

auto Codegen::emitExistingVarSlotInitializerStore(const VarDecl* node, llvm::Value* destPtr,
                                                  std::unique_ptr<lesma::Value>& valueResult,
                                                  lesma::Type* storedType,
                                                  const std::string& dbgName)
    -> llvm::Instruction* {
  const bool ptrToClass = isLesmaPtrToClass(storedType);
  if (valueResult->getLlvmValue() == nullptr && storedType->is(BaseType::TY_FUNCTION) &&
      !storedType->getGenericParams().empty()) {
    return builder->CreateStore(llvm::ConstantAggregateZero::get(getFuncValuePairLlvmType()),
                                destPtr);
  }
  if (storedType->is(BaseType::TY_FUNCTION) && valueResult->getLlvmValue() != nullptr &&
      valueResult->getLlvmValue()->getType() == getFuncValuePairLlvmType()) {
    return builder->CreateStore(valueResult->getLlvmValue(), destPtr);
  }
  if (valueResult->getStoresFuncValuePair() && storedType->is(BaseType::TY_FUNCTION)) {
    llvm::StructType* pt = getFuncValuePairLlvmType();
    llvm::Value* loaded =
        builder->CreateLoad(pt, valueResult->getLlvmValue(), dbgName + ".fnpair.load");
    return builder->CreateStore(loaded, destPtr);
  }
  if (ptrToClass && valueResult->getType() != nullptr &&
      valueResult->getType()->is(BaseType::TY_PTR)) {
    return builder->CreateStore(valueResult->getLlvmValue(), destPtr);
  }
  lesma::Type* castTarget = ptrToClass ? storedType->getElementType() : storedType;
  auto castVal = cast(node->getSpan(), valueResult.get(), castTarget);
  return builder->CreateStore(castVal->getLlvmValue(), destPtr);
}

auto Codegen::makeBoolCompareResult(llvm::Value* cmpVal) -> std::unique_ptr<lesma::Value> {
  return std::make_unique<Value>(
      "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), cmpVal);
}

auto Codegen::emitPromotedArithmetic(llvm::SMRange span, TokenType op,
                                     std::unique_ptr<lesma::Value>& left,
                                     std::unique_ptr<lesma::Value>& right, lesma::Type* finalType)
    -> std::unique_ptr<lesma::Value> {
  left = cast(span, left.get(), finalType);
  right = cast(span, right.get(), finalType);
  if (finalType == nullptr) {
    return nullptr;
  }
  llvm::Value* const l = left->getLlvmValue();
  llvm::Value* const r = right->getLlvmValue();
  if (finalType->isFloatingPoint()) {
    switch (op) {
    case TokenType::MINUS:
      return std::make_unique<Value>("", finalType, builder->CreateFSub(l, r));
    case TokenType::PLUS:
      return std::make_unique<Value>("", finalType, builder->CreateFAdd(l, r));
    case TokenType::STAR:
      return std::make_unique<Value>("", finalType, builder->CreateFMul(l, r));
    case TokenType::SLASH:
      return std::make_unique<Value>("", finalType, builder->CreateFDiv(l, r));
    case TokenType::MOD:
      return std::make_unique<Value>("", finalType, builder->CreateFRem(l, r));
    default:
      return nullptr;
    }
  }
  if (finalType->is(BaseType::TY_INT)) {
    switch (op) {
    case TokenType::MINUS:
      return std::make_unique<Value>("", finalType, builder->CreateSub(l, r));
    case TokenType::PLUS:
      return std::make_unique<Value>("", finalType, builder->CreateAdd(l, r));
    case TokenType::STAR:
      return std::make_unique<Value>("", finalType, builder->CreateMul(l, r));
    case TokenType::SLASH: {
      llvm::Value* const div =
          finalType->isSigned() ? builder->CreateSDiv(l, r) : builder->CreateUDiv(l, r);
      return std::make_unique<Value>("", finalType, div);
    }
    case TokenType::MOD: {
      llvm::Value* const rem =
          finalType->isSigned() ? builder->CreateSRem(l, r) : builder->CreateURem(l, r);
      return std::make_unique<Value>("", finalType, rem);
    }
    default:
      return nullptr;
    }
  }
  return nullptr;
}

void Codegen::emitForInLoopIteration(llvm::Function* parentFct, const ForIn* node,
                                     SymbolTable* outerScope, SymbolTable* loopBodyScope,
                                     llvm::BasicBlock* bLoop, llvm::BasicBlock* bInc,
                                     const std::function<void()>& loadElementIntoLoopVar) {
  bLoop->insertInto(parentFct);
  builder->SetInsertPoint(bLoop);
  loadElementIntoLoopVar();
  scope = loopBodyScope;
  deferStack.emplace();
  node->getBlock()->accept(*this);
  scope = outerScope;

  if (builder->GetInsertBlock() != nullptr &&
      builder->GetInsertBlock()->getTerminator() == nullptr) {
    finishLoopDeferFrameIfAny();
    builder->CreateBr(bInc);
  }
  isBreak = false;
}

auto Codegen::visit(const VarDecl* node) -> void {
  setDebugLoc(node->getSpan());
  std::vector<Literal*> const unpackNames = node->getVarLiterals();
  if (unpackNames.size() > 1U) {
    std::unique_ptr<lesma::Value> valueResult;
    if (node->getValue() != nullptr) {
      node->getValue()->accept(*this);
      valueResult = std::move(result);
    }
    if (valueResult == nullptr || valueResult->getType() == nullptr ||
        !valueResult->getType()->is(BaseType::TY_TUPLE)) {
      throw CodegenError(node->getSpan(), "Destructuring requires a tuple value");
    }
    getOrCreateLlvmType(valueResult->getType());
    llvm::Value* agg = valueResult->getLlvmValue();
    std::vector<Field*> const tf = valueResult->getType()->getFields();
    std::vector<Value*> const& resolvedUnpack = node->getResolvedSymbols();
    llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
    for (size_t i = 0; i < unpackNames.size(); ++i) {
      std::string const elemName = unpackNames[i]->getValue();
      lesma::Type* elemTy = tf[i]->type;
      getOrCreateLlvmType(elemTy);
      llvm::Value* ev =
          builder->CreateExtractValue(agg, static_cast<unsigned>(i), elemName + ".tup");
      lesma::Value* existing =
          i < resolvedUnpack.size() ? resolvedUnpack[i] : scope->lookup(elemName);
      llvm::Type* allocaTy = llvmStorageTypeForVarSlot(elemTy, nullptr);
      llvm::AllocaInst* ptr = createAllocaInEntry(parentFct, allocaTy, elemName);
      if (elemTy->is(BaseType::TY_CLASS)) {
        lesma::Type* ptrType =
            cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), elemTy));
        if (existing != nullptr) {
          existing->setType(ptrType);
        }
      }
      llvm::Instruction* st = builder->CreateStore(ev, ptr);
      emitAutoVarDebugDeclare(llvm::cast<llvm::AllocaInst>(ptr), elemName, node->getSpan(), st);
      if (existing != nullptr) {
        existing->setLlvmValue(ptr);
        existing->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
        existing->setMutable(node->getMutability());
      }
    }
    return;
  }

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
    if (type != nullptr && !currentGenericTypes.empty() && typeContainsUnboundGeneric(type)) {
      type = substituteTypeForSpecializationEnv(type, currentGenericTypes);
    }
    if (type != nullptr && existing->getType() != type &&
        !(type->is(BaseType::TY_CLASS) ||
          (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
           type->getElementType()->is(BaseType::TY_CLASS)))) {
      existing->setType(type);
    }
    const bool polyFuncStoredAsPair =
        type->is(BaseType::TY_FUNCTION) && existing->getStoresFuncValuePair() &&
        (!type->getGenericParams().empty() || typeContainsUnboundGeneric(type));
    if (!polyFuncStoredAsPair) {
      getOrCreateLlvmType(type);
    }
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
    llvm::Type* storageLlvmTy = llvmStorageTypeForVarSlot(storedType, existing);

    if (node->isExported()) {
      std::string const mangled = MangleUtils::getGlobalVariableSymbolName(
          filename.empty() ? std::string() : normalizeResolvedFilesystemPath(filename), name);
      llvm::GlobalVariable* gv = theModule->getGlobalVariable(mangled, true);
      if (gv == nullptr) {
        gv = new llvm::GlobalVariable(*theModule, storageLlvmTy, false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      llvm::Constant::getNullValue(storageLlvmTy), mangled);
      }
      existing->setLlvmValue(gv);
      existing->setMangledName(mangled);
      existing->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      existing->setMutable(node->getMutability());
      if (valueResult != nullptr) {
        emitSimpleClassPtrOrCastStore(node->getSpan(), gv, valueResult, storedType);
      }
      return;
    }

    llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
    llvm::AllocaInst* ptr = createAllocaInEntry(parentFct, storageLlvmTy, name);
    existing->setLlvmValue(ptr);
    existing->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    existing->setMutable(node->getMutability());
    if (valueResult != nullptr) {
      llvm::Instruction* const st =
          emitExistingVarSlotInitializerStore(node, ptr, valueResult, storedType, name);
      emitAutoVarDebugDeclare(llvm::cast<llvm::AllocaInst>(ptr), name, node->getSpan(), st);
    } else {
      emitAutoVarDebugDeclare(llvm::cast<llvm::AllocaInst>(ptr), name, node->getSpan(), nullptr);
    }
    if (node->getValue() != nullptr) {
      if (auto* le = dynamic_cast<LambdaExpr*>(node->getValue())) {
        if (Value* rs = le->getResolvedSymbol(); rs != nullptr) {
          existing->setClosureCalleeUsesEnvParameter(rs->getClosureCalleeUsesEnvParameter());
        }
      } else if (dynamic_cast<FuncCall*>(node->getValue()) != nullptr &&
                 storedType->is(BaseType::TY_FUNCTION)) {
        existing->setClosureCalleeUsesEnvParameter(true);
      }
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
  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
  llvm::AllocaInst* ptr = createAllocaInEntry(parentFct, type->getLlvmType(), name);

  if (type->is(BaseType::TY_CLASS)) {
    type = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type));
  }
  auto symbol = std::make_unique<Value>(
      name, type, node->getType() != nullptr ? SymbolState::INITIALIZED : SymbolState::DECLARED);
  symbol->setLlvmValue(ptr);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));

  if (node->getValue() != nullptr) {
    llvm::Instruction* const st = emitSimpleClassPtrOrCastStore(node->getSpan(), ptr, val, type);
    emitAutoVarDebugDeclare(llvm::cast<llvm::AllocaInst>(ptr), name, node->getSpan(), st);
  } else {
    emitAutoVarDebugDeclare(llvm::cast<llvm::AllocaInst>(ptr), name, node->getSpan(), nullptr);
  }
}

auto Codegen::cgUnionTryGetIsOpVarSymbol(const IsOp* is, SymbolTable* scope) -> lesma::Value* {
  if (is == nullptr) {
    return nullptr;
  }
  const auto* lit = dynamic_cast<const Literal*>(is->getLeft());
  if (lit == nullptr || lit->getType() != TokenType::IDENTIFIER) {
    return nullptr;
  }
  if (lesma::Value* rs = lit->getResolvedSymbol()) {
    return rs;
  }
  return scope->lookup(lit->getValue());
}

auto Codegen::cgUnionComplementMemberIndex(lesma::Type* unionTy, lesma::Type* excluded)
    -> std::optional<unsigned> {
  if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION) || excluded == nullptr) {
    return std::nullopt;
  }
  const auto& mem = unionTy->getUnionMembers();
  if (mem.size() != 2U) {
    return std::nullopt;
  }
  for (unsigned i = 0; i < mem.size(); ++i) {
    if (!mem[i]->isEqual(excluded)) {
      return i;
    }
  }
  return std::nullopt;
}

auto Codegen::lookupUnionNarrowVariant(lesma::Value* sym) const -> std::optional<unsigned> {
  if (sym == nullptr) {
    return std::nullopt;
  }
  const UnionNarrowingStableKey key = unionNarrowingStableKeyForSymbol(sym);
  if (!key.declarationSpan.isValid() && key.fallbackAnchor == nullptr) {
    return std::nullopt;
  }
  for (auto it = unionNarrowVariantStack.rbegin(); it != unionNarrowVariantStack.rend(); ++it) {
    auto j = it->find(key);
    if (j != it->end()) {
      return j->second;
    }
  }
  return std::nullopt;
}

auto Codegen::unionVariantIndexOf(lesma::Type* unionTy, lesma::Type* memberTy) const
    -> std::optional<unsigned> {
  if (unionTy == nullptr || memberTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
    return std::nullopt;
  }
  const auto& mem = unionTy->getUnionMembers();
  for (unsigned i = 0; i < mem.size(); ++i) {
    if (mem[i]->isEqual(memberTy)) {
      return i;
    }
  }
  return std::nullopt;
}

auto Codegen::fillCodegenUnionNarrowVariantMap(
    const If* node, unsigned blockIndex,
    std::unordered_map<UnionNarrowingStableKey, unsigned, UnionNarrowingStableKeyHash,
                       UnionNarrowingStableKeyEq>& out) -> void {
  if (blockIndex >= node->getBlocks().size()) {
    return;
  }
  Expression* cond = node->getConds()[blockIndex];
  if (dynamic_cast<const Else*>(cond) != nullptr) {
    if (blockIndex == 0U) {
      return;
    }
    const std::vector<Expression*> conds = node->getConds();
    lesma::Value* sym = nullptr;
    std::set<unsigned> possible;
    for (unsigned j = 0; j < blockIndex; ++j) {
      const auto* isJ = dynamic_cast<const IsOp*>(conds[j]);
      if (isJ == nullptr) {
        continue;
      }
      lesma::Value* symJ = cgUnionTryGetIsOpVarSymbol(isJ, scope);
      if (symJ == nullptr || symJ->getType() == nullptr ||
          !symJ->getType()->is(BaseType::TY_UNION)) {
        continue;
      }
      if (sym == nullptr) {
        sym = symJ;
        const auto& mem = sym->getType()->getUnionMembers();
        for (unsigned vi = 0; vi < mem.size(); ++vi) {
          possible.insert(vi);
        }
      } else if (symJ != sym) {
        continue;
      }
      lesma::Type* unionTy = sym->getType();
      lesma::Type* rhsTy = isJ->getResolvedRhsType();
      if (rhsTy == nullptr) {
        continue;
      }
      if (isJ->getOperator() == TokenType::IS) {
        if (auto idx = unionVariantIndexOf(unionTy, rhsTy)) {
          possible.erase(*idx);
        }
      } else if (isJ->getOperator() == TokenType::IS_NOT) {
        if (auto idx = unionVariantIndexOf(unionTy, rhsTy)) {
          if (possible.contains(*idx)) {
            possible = {*idx};
          } else {
            possible.clear();
          }
        }
      }
    }
    if (sym == nullptr || possible.size() != 1U) {
      return;
    }
    const UnionNarrowingStableKey key = unionNarrowingStableKeyForSymbol(sym);
    if (key.declarationSpan.isValid() || key.fallbackAnchor != nullptr) {
      out[key] = *possible.begin();
    }
    return;
  }
  const auto* is = dynamic_cast<const IsOp*>(cond);
  if (is == nullptr) {
    return;
  }
  lesma::Value* sym = cgUnionTryGetIsOpVarSymbol(is, scope);
  if (sym == nullptr || sym->getType() == nullptr || !sym->getType()->is(BaseType::TY_UNION)) {
    return;
  }
  lesma::Type* rhsTy = is->getResolvedRhsType();
  if (rhsTy == nullptr) {
    return;
  }
  if (is->getOperator() == TokenType::IS) {
    if (auto idx = unionVariantIndexOf(sym->getType(), rhsTy)) {
      const UnionNarrowingStableKey key = unionNarrowingStableKeyForSymbol(sym);
      if (key.declarationSpan.isValid() || key.fallbackAnchor != nullptr) {
        out[key] = *idx;
      }
    }
  } else if (is->getOperator() == TokenType::IS_NOT) {
    if (auto idx = cgUnionComplementMemberIndex(sym->getType(), rhsTy)) {
      const UnionNarrowingStableKey key = unionNarrowingStableKeyForSymbol(sym);
      if (key.declarationSpan.isValid() || key.fallbackAnchor != nullptr) {
        out[key] = *idx;
      }
    }
  }
}

auto Codegen::emitUnionPayloadLoadFromSlot(llvm::Value* unionAllocaPtr, lesma::Type* unionTy,
                                           lesma::Type* memberTy) -> llvm::Value* {
  getOrCreateLlvmType(unionTy);
  getOrCreateLlvmType(memberTy);
  auto* st = llvm::cast<llvm::StructType>(unionTy->getLlvmType());
  llvm::Value* payloadPtr = builder->CreateStructGEP(st, unionAllocaPtr, 1U, "union.payload.ptr");
  llvm::Type* memLt = getStoredAggregateFieldLlvmType(memberTy);
  return builder->CreateLoad(memLt, payloadPtr, "union.payload");
}

auto Codegen::emitUnionWrapValueToSlot(llvm::SMRange /*span*/, lesma::Value* val,
                                       lesma::Type* unionTy, unsigned variantIndex,
                                       llvm::Value* destSlot) -> std::unique_ptr<lesma::Value> {
  getOrCreateLlvmType(unionTy);
  getOrCreateLlvmType(val->getType());
  auto* st = llvm::cast<llvm::StructType>(unionTy->getLlvmType());
  llvm::Value* tagPtr = builder->CreateStructGEP(st, destSlot, 0U, "union.tag.ptr");
  llvm::Type* tagTy = getOrCreateUnionTagLlvmType(unionTy);
  builder->CreateStore(llvm::ConstantInt::get(tagTy, variantIndex), tagPtr);
  llvm::Value* payPtr = builder->CreateStructGEP(st, destSlot, 1U, "union.pay.ptr");
  llvm::Type* payStoredTy = getStoredAggregateFieldLlvmType(val->getType());
  llvm::Value* v = val->getLlvmValue();
  if (v->getType() != payStoredTy) {
    if (v->getType()->isIntegerTy() && payStoredTy->isIntegerTy()) {
      v = builder->CreateIntCast(v, payStoredTy, /*isSigned=*/true, "union.pay.ic");
    } else if (v->getType()->isFloatingPointTy() && payStoredTy->isFloatingPointTy()) {
      v = builder->CreateFPCast(v, payStoredTy, "union.pay.fc");
    } else if (theModule->getDataLayout().getTypeSizeInBits(v->getType()) ==
               theModule->getDataLayout().getTypeSizeInBits(payStoredTy)) {
      v = builder->CreateBitCast(v, payStoredTy, "union.pay.cast");
    } else {
      throw CodegenError({}, "Internal error: union payload store type mismatch");
    }
  }
  llvm::Value* typedPayPtr = builder->CreateBitCast(
      payPtr, llvm::PointerType::get(theModule->getContext(), 0U), "union.pay.tptr");
  builder->CreateStore(v, typedPayPtr);
  llvm::Value* agg = builder->CreateLoad(st, destSlot, "union.val");
  return std::make_unique<lesma::Value>("", unionTy, agg);
}

auto Codegen::emitUnionWrapValue(llvm::SMRange span, lesma::Value* val, lesma::Type* unionTy,
                                 unsigned variantIndex) -> std::unique_ptr<lesma::Value> {
  getOrCreateLlvmType(unionTy);
  getOrCreateLlvmType(val->getType());
  auto* st = llvm::cast<llvm::StructType>(unionTy->getLlvmType());
  llvm::Function* f = builder->GetInsertBlock()->getParent();
  llvm::AllocaInst* slot = createAllocaInEntry(f, st, "union.wrap.slot");
  return emitUnionWrapValueToSlot(span, val, unionTy, variantIndex, slot);
}

auto Codegen::visit(const If* node) -> void {
  setDebugLoc(node->getSpan());
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

    std::unordered_map<UnionNarrowingStableKey, unsigned, UnionNarrowingStableKeyHash,
                       UnionNarrowingStableKeyEq>
        narrowMap;
    fillCodegenUnionNarrowVariantMap(node, static_cast<unsigned>(i), narrowMap);
    UnionNarrowingScope const unionNarrowScope{unionNarrowVariantStack, std::move(narrowMap)};
    node->getBlocks().at(i)->accept(*this);

    if (!isBreak && builder->GetInsertBlock()->getTerminator() == nullptr) {
      builder->CreateBr(bEnd);
    }

    builder->SetInsertPoint(bIfFalse);
  }

  bEnd->insertInto(parentFct);

  if (!isBreak) {
    builder->SetInsertPoint(bEnd);
  }
  // Do not clear `isBreak` here: break/continue set it so the enclosing loop skips its tail
  // (e.g. a second `finishLoopDeferFrameIfAny`). While/ForIn reset the flag.
}

auto Codegen::visit(const While* node) -> void {
  setDebugLoc(node->getSpan());
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
  size_t const deferDepthBeforeLoop = deferStack.size();
  deferStack.emplace();
  node->getBlock()->accept(*this);

  // Fall-through paths (e.g. an if's false branch followed by i++) still need the per-iteration
  // defer flush and the back-edge; break/continue already terminated their block.
  if (builder->GetInsertBlock() != nullptr &&
      builder->GetInsertBlock()->getTerminator() == nullptr) {
    finishLoopDeferFrameIfAny();
    builder->CreateBr(bCond);
  }
  isBreak = false;

  // Fill loop end block
  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);

  breakBlocks.pop();
  continueBlocks.pop();

  while (deferStack.size() > deferDepthBeforeLoop) {
    deferStack.pop();
  }
}

auto Codegen::classTypeDeclaresIterable(lesma::Type* classTy) const -> bool {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return false;
  }
  return std::ranges::any_of(classTy->getImplTraitNames(),
                             [](const std::string& n) -> bool { return n == "Iterable"; });
}

auto Codegen::visit(const ForIn* node) -> void {
  setDebugLoc(node->getSpan());
  node->getIterable()->accept(*this);
  std::unique_ptr<lesma::Value> iterable = std::move(result);
  lesma::Type* listType = iterable->getType();
  llvm::Value* listHandle = iterable->getLlvmValue();
  if (listType != nullptr && listType->is(BaseType::TY_PTR) &&
      listType->getElementType() != nullptr && listType->getElementType()->is(BaseType::TY_CLASS)) {
    listType = listType->getElementType();
  }
  if (listType != nullptr && listType->is(BaseType::TY_CLASS) &&
      classTypeDeclaresIterable(listType) && classHasSingleBufferStorageField(listType)) {
    auto fields = listType->getFields();
    if (!fields.empty() && fields.front()->type != nullptr &&
        fields.front()->type->is(BaseType::TY_ARRAY)) {
      unsigned const bufIdx = TypeUtils::classDataFieldStructIndex(listType, 0U);
      auto* storagePtr =
          builder->CreateStructGEP(cast<llvm::StructType>(getOrCreateLlvmType(listType)),
                                   iterable->getLlvmValue(), bufIdx, "list.storage.ptr");
      listHandle = builder->CreateLoad(getOrCreateLlvmType(fields.front()->type), storagePtr,
                                       "list.storage");
      listType = fields.front()->type;
    }
  }
  SymbolTable* savedScope = scope;
  SymbolTable* forBodyScope =
      node->getBodyScope() != nullptr ? node->getBodyScope() : savedScope->createChildBlock("for");
  scope = forBodyScope;
  Value* loopVar = scope->lookup(node->getIdentifier()->getValue());
  if (loopVar == nullptr) {
    throw CodegenError(node->getSpan(), "Missing loop variable symbol for for-in");
  }
  lesma::Type* loopVarType = loopVar->getType();
  getOrCreateLlvmType(loopVarType);
  llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
  bool const useArrayIndex = listType != nullptr && listType->is(BaseType::TY_ARRAY) &&
                             listType->getElementType() != nullptr;
  if (loopVar->getLlvmValue() == nullptr) {
    if (loopVarType->is(BaseType::TY_CLASS)) {
      lesma::Type* ptrType =
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), loopVarType));
      loopVar->setType(ptrType);
    }
    lesma::Type* storedType = loopVar->getType();
    llvm::Type* allocaTy = llvmStorageTypeForVarSlot(storedType, nullptr);
    llvm::AllocaInst* elemPtr = createAllocaInEntry(parentFct, allocaTy, loopVar->getName());
    loopVar->setLlvmValue(elemPtr);
    loopVar->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  }
  scope = savedScope;

  llvm::BasicBlock* bCond = llvm::BasicBlock::Create(theModule->getContext(), "for.cond");
  llvm::BasicBlock* bLoop = llvm::BasicBlock::Create(theModule->getContext(), "for");
  llvm::BasicBlock* bInc = llvm::BasicBlock::Create(theModule->getContext(), "for.inc");
  llvm::BasicBlock* bEnd = llvm::BasicBlock::Create(theModule->getContext(), "for.end");
  std::unique_ptr<lesma::Value> iteratorValue;
  llvm::AllocaInst* indexPtr = nullptr;
  if (!useArrayIndex) {
    iteratorValue = callMethodByName(node->getSpan(), iterable.get(), "iter");
  } else {
    getOrCreateLlvmType(listType);
    indexPtr = createAllocaInEntry(parentFct, builder->getInt64Ty(), "for.index");
    builder->CreateStore(builder->getInt64(0), indexPtr);
  }

  breakBlocks.push(bEnd);
  continueBlocks.push(bInc);
  builder->CreateBr(bCond);

  size_t const deferDepthBeforeLoop = deferStack.size();

  if (useArrayIndex) {
    bCond->insertInto(parentFct);
    builder->SetInsertPoint(bCond);
    auto* idxVal = builder->CreateLoad(builder->getInt64Ty(), indexPtr);
    auto* lenVal = emitListLength(listType, listHandle);
    builder->CreateCondBr(builder->CreateICmpSLT(idxVal, lenVal), bLoop, bEnd);

    emitForInLoopIteration(parentFct, node, savedScope, forBodyScope, bLoop, bInc, [&] {
      auto* elemPtr = emitListElementPointer(node->getSpan(), listType, listHandle, idxVal);
      auto* elemVal = builder->CreateLoad(getListStoredElementType(listType), elemPtr);
      builder->CreateStore(elemVal, loopVar->getLlvmValue());
    });

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

    emitForInLoopIteration(parentFct, node, savedScope, forBodyScope, bLoop, bInc, [&] {
      auto nextValue = callMethodByName(node->getSpan(), iteratorValue.get(), "next");
      builder->CreateStore(nextValue->getLlvmValue(), loopVar->getLlvmValue());
    });

    bInc->insertInto(parentFct);
    builder->SetInsertPoint(bInc);
    builder->CreateBr(bCond);
  }

  bEnd->insertInto(parentFct);
  builder->SetInsertPoint(bEnd);
  breakBlocks.pop();
  continueBlocks.pop();

  while (deferStack.size() > deferDepthBeforeLoop) {
    deferStack.pop();
  }
}

auto Codegen::buildClassMethodParamTypesForLookup(const FuncDecl* node)
    -> std::vector<lesma::Type*> {
  std::vector<lesma::Type*> paramTypes;
  if (selfSymbol != nullptr && !node->getIsStatic()) {
    paramTypes.push_back(selfSymbol->getType());
  }
  for (auto* param : node->getParameters()) {
    std::unique_ptr<lesma::Value> typeResult;
    std::unique_ptr<lesma::Value> defaultValResult;

    if (param->type) {
      param->type->accept(*this);
      typeResult = std::move(result);
    }
    if (param->defaultVal) {
      param->defaultVal->accept(*this);
      defaultValResult = std::move(result);
      if (!typeResult) {
        typeResult = std::make_unique<Value>(*defaultValResult);
      }
    }

    if (typeResult->getType()->is(BaseType::TY_CLASS)) {
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
  }
  return paramTypes;
}

auto Codegen::declareSynthesizedClassConstructor(const Class* astNode, lesma::Type* classType,
                                                 lesma::Value* classStructSym) -> lesma::Value* {
  Type* selfPtrType =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), classType));
  std::vector<lesma::Type*> paramTypes = {selfPtrType};
  std::vector<std::unique_ptr<Field>> fields;
  fields.push_back(std::make_unique<Field>("self", selfPtrType));
  std::vector<Field*> const typeFields = classType->getFields();
  size_t ti = 0;
  for (VarDecl* v : astNode->getFields()) {
    if (v->getIsStatic()) {
      continue;
    }
    if (ti >= typeFields.size()) {
      break;
    }
    Type* storageTy = typeFields[ti]->type;
    ti++;
    if (v->getValue() != nullptr) {
      continue;
    }
    Type* paramType = storageTy;
    if (paramType->is(BaseType::TY_CLASS)) {
      paramType =
          cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), paramType));
    }
    getOrCreateLlvmType(paramType);
    paramTypes.push_back(paramType);
    fields.push_back(std::make_unique<Field>(v->getIdentifier()->getValue(), paramType));
  }
  Type* returnType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
  getOrCreateLlvmType(returnType);

  Value* savedSelfSym = selfSymbol;
  selfSymbol = classStructSym;

  auto mangledName = getMangledName(astNode->getSpan(), "new", paramTypes, selfSymbol != nullptr);
  std::string signatureKey = makeCallableSignatureKey("new", paramTypes);
  if (!currentGenericTypes.empty()) {
    std::vector<std::string> bindingOrder;
    if (selfSymbol != nullptr && selfSymbol->getType() != nullptr &&
        selfSymbol->getType()->getElementType() != nullptr) {
      bindingOrder = selfSymbol->getType()->getElementType()->getGenericParams();
    }
    if (bindingOrder.empty()) {
      bindingOrder.reserve(currentGenericTypes.size());
      for (const auto& kv : currentGenericTypes) {
        bindingOrder.push_back(kv.first);
      }
      std::sort(bindingOrder.begin(), bindingOrder.end());
    }
    appendGenericBindingSuffix(astNode->getSpan(), mangledName, bindingOrder, currentGenericTypes);
    appendGenericBindingSuffix(astNode->getSpan(), signatureKey, bindingOrder, currentGenericTypes);
  }
  const bool shouldExport = classStructSym->isExported();
  auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;

  std::vector<llvm::Type*> paramLLVMTypes;
  paramLLVMTypes.reserve(paramTypes.size());
  for (auto* pt : paramTypes) {
    paramLLVMTypes.push_back(pt->getLlvmType());
  }
  llvm::FunctionType* llvmFnTy = FunctionType::get(builder->getVoidTy(), paramLLVMTypes, false);
  Function* f = Function::Create(llvmFnTy, linkage, mangledName, *theModule);
  attachFunctionDebugInfo(f, "new", mangledName, astNode->getNameSpan(), linkage, false);
  auto loweredType = std::make_unique<Type>(BaseType::TY_FUNCTION, llvmFnTy, std::move(fields));
  loweredType->setReturnType(returnType);
  loweredType->setVarArgs(false);
  Type* loweredTypePtr = cacheType(std::move(loweredType));

  const bool isSpecializedInstance = specializedClassSymbolsByType.contains(classType);

  if (isSpecializedInstance) {
    SymbolTable* bodyScope = scope->createChildBlock("synthetic_ctor");
    auto selfPs = std::make_unique<Value>("self", paramTypes[0]);
    selfPs->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    selfPs->setDeclarationKind(ValueDeclarationKind::PARAMETER);
    bodyScope->insertSymbol(std::move(selfPs));
    unsigned reqIdx = 0;
    for (VarDecl* v : astNode->getFields()) {
      if (v->getIsStatic()) {
        continue;
      }
      if (v->getValue() != nullptr) {
        continue;
      }
      auto ps = std::make_unique<Value>(v->getIdentifier()->getValue(), paramTypes[1U + reqIdx]);
      ps->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      ps->setDeclarationKind(ValueDeclarationKind::PARAMETER);
      bodyScope->insertSymbol(std::move(ps));
      reqIdx++;
    }
    auto funcSymbol = std::make_unique<Value>("new", loweredTypePtr, f);
    funcSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
    funcSymbol->setDeclarationKind(ValueDeclarationKind::METHOD);
    funcSymbol->setExported(shouldExport);
    funcSymbol->setMangledName(mangledName);
    funcSymbol->setBodyScope(bodyScope);
    auto* funcSymbolPtr = funcSymbol.get();
    scope->insertSymbol(std::move(funcSymbol));
    specializedFunctions[mangledName] = funcSymbolPtr;
    specializedFunctions[signatureKey] = funcSymbolPtr;
    specializationEnvs[funcSymbolPtr] = currentGenericTypes;
    syntheticConstructorBodies.emplace_back(funcSymbolPtr, astNode);
    selfSymbol = savedSelfSym;
    return funcSymbolPtr;
  }

  lesma::Value* existingFunc =
      scope->lookupFunction("new", paramTypes, FunctionLookupKind::OVERLOAD_IDENTITY);
  if (existingFunc == nullptr) {
    auto normalizeFunctionParamType = [](lesma::Type* type) -> lesma::Type* {
      if (type != nullptr && type->is(BaseType::TY_PTR) && type->getElementType() != nullptr &&
          type->getElementType()->is(BaseType::TY_CLASS)) {
        return type->getElementType();
      }
      return type;
    };
    for (auto* candidate : scope->getSymbols()) {
      if (candidate == nullptr || candidate->getName() != "new" ||
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

  if (existingFunc == nullptr) {
    throw CodegenError(astNode->getSpan(),
                       "Missing typechecked symbol for synthesized constructor");
  }
  existingFunc->setType(loweredTypePtr);
  existingFunc->setLlvmValue(f);
  existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
  existingFunc->setMangledName(mangledName);
  specializedFunctions[mangledName] = existingFunc;
  specializedFunctions[signatureKey] = existingFunc;
  syntheticConstructorBodies.emplace_back(existingFunc, astNode);
  selfSymbol = savedSelfSym;
  return existingFunc;
}

auto Codegen::defineSynthesizedClassConstructor(lesma::Value* ctorSym, const Class* astNode)
    -> void {
  SymbolTable* savedScope = scope;
  scope = ctorSym->getBodyScope();
  if (scope == nullptr) {
    throw CodegenError(astNode->getSpan(), "Synthesized constructor has no body scope");
  }
  currentFunction = ctorSym;
  deferStack.emplace();
  pushDeferBaseline();

  if (ctorSym->getLlvmValue() == nullptr) {
    throw CodegenError(astNode->getSpan(), "Synthesized constructor has no LLVM function");
  }
  auto* f = llvm::cast<Function>(ctorSym->getLlvmValue());

  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);
  setDebugLoc(astNode->getSpan());

  llvm::DIFile* declFile = moduleDiFile;
  unsigned declLine = 1U;
  if (astNode->getNameSpan().isValid() && astNode->getNameSpan().Start.isValid()) {
    unsigned const bid = sourceManager->FindBufferContainingLoc(astNode->getNameSpan().Start);
    if (bid != 0U) {
      declFile = getOrCreateDiFileForBuffer(bid);
    }
    declLine = sourceManager->getLineAndColumn(astNode->getNameSpan().Start).first;
  }

  auto lookupInCurrentScope = [this](const std::string& name) -> Value* {
    for (auto* symbol : scope->getSymbols()) {
      if (symbol->getName() == name) {
        return symbol;
      }
    }
    return nullptr;
  };

  int fieldIndex = 0;
  for (const auto& field : ctorSym->getType()->getFields()) {
    auto* param = f->getArg(fieldIndex);
    std::string const paramName = field->name;

    param->setName(paramName);

    llvm::Value* ptr = builder->CreateAlloca(param->getType(), nullptr, param->getName() + "_ptr");
    llvm::Instruction* storeParam = builder->CreateStore(param, ptr);
    emitParameterDebugDeclare(f, ptr, paramName, static_cast<unsigned>(fieldIndex + 1), declFile,
                              declLine, param->getType(), storeParam);

    if (auto* existingParam = lookupInCurrentScope(paramName);
        existingParam != nullptr && existingParam->getLlvmValue() == nullptr) {
      existingParam->setLlvmValue(ptr);
      existingParam->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    } else {
      auto symbol = std::make_unique<Value>(field->name, field->type, ptr);
      symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      scope->insertSymbol(std::move(symbol));
    }

    fieldIndex++;
  }

  Type* classType = ctorSym->getType()->getFields()[0]->type->getElementType();
  getOrCreateLlvmType(classType);
  auto* structTy = llvm::cast<llvm::StructType>(getOrCreateLlvmType(classType));
  Value* selfSym = scope->lookup("self");
  if (selfSym == nullptr) {
    throw CodegenError(astNode->getSpan(), "Synthesized constructor missing self parameter");
  }
  llvm::Value* selfAlloca = selfSym->getLlvmValue();
  llvm::Value* selfPtr = builder->CreateLoad(builder->getPtrTy(), selfAlloca, "self.ptr");
  // Vtable pointer is initialized at the allocation site for the concrete class. Re-emitting it
  // here would let synthesized base constructors overwrite a derived object's vtable.

  std::vector<Field*> const layoutFields = classType->getFields();
  size_t layoutIdx = 0;
  for (VarDecl* v : astNode->getFields()) {
    if (v->getIsStatic()) {
      continue;
    }
    if (layoutIdx >= layoutFields.size()) {
      break;
    }
    Type* storageTy = layoutFields[layoutIdx]->type;
    unsigned const structIdx =
        TypeUtils::classDataFieldStructIndex(classType, static_cast<unsigned>(layoutIdx));
    llvm::Value* slotPtr = builder->CreateStructGEP(structTy, selfPtr, structIdx,
                                                    v->getIdentifier()->getValue() + ".ptr");
    layoutIdx++;
    if (v->getValue() != nullptr) {
      v->getValue()->accept(*this);
      auto rhs = cast(v->getValue()->getSpan(), result.get(), storageTy);
      builder->CreateStore(rhs->getLlvmValue(), slotPtr);
    } else {
      Value* ps = scope->lookup(v->getIdentifier()->getValue());
      if (ps == nullptr) {
        throw CodegenError(astNode->getSpan(), "Missing parameter for field {}",
                           v->getIdentifier()->getValue());
      }
      getOrCreateLlvmType(storageTy);
      llvm::Value* loaded =
          loadStoredAggregateFieldValue(ps->getLlvmValue(), storageTy,
                                        llvm::Twine(v->getIdentifier()->getValue()).concat(".arg"));
      builder->CreateStore(loaded, slotPtr);
    }
  }

  flushDeferredFramesForReturn();
  builder->CreateRetVoid();

  deferStack.pop();
  deferBaselineStack.pop();

  for (BasicBlock& bb : *f) {
    Instruction* terminator = bb.getTerminator();
    if (terminator != nullptr) {
      continue;
    }
    builder->SetInsertPoint(&bb);
    builder->CreateRetVoid();
  }

  isReturn = false;
  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(astNode->getSpan(), "Invalid synthesized constructor\n{}", verifyOutput);
  }

  scope = savedScope;
  currentFunction = nullptr;
  builder->SetInsertPoint(&topLevelFunc->back());
  builder->SetCurrentDebugLocation(llvm::DebugLoc());
}

auto Codegen::getOrEmitClassVtableGlobal(lesma::Type* classTy, const Class* astNode)
    -> llvm::GlobalVariable* {
  auto it = classVtableGlobals.find(classTy);
  if (it != classVtableGlobals.end()) {
    return it->second;
  }
  if (astNode == nullptr && classTy != nullptr && !classTy->getDisplayName().empty()) {
    if (auto dit = codegenClassAstByDisplayName.find(classTy->getDisplayName());
        dit != codegenClassAstByDisplayName.end()) {
      astNode = dit->second;
    }
  }
  const auto& order = classTy->getClassVtableMethodOrder();
  llvm::Type* const ptrTy = builder->getPtrTy();
  auto* ptrTyTyped = llvm::cast<llvm::PointerType>(ptrTy);
  if (order.empty()) {
    classVtableGlobals[classTy] = nullptr;
    return nullptr;
  }

  llvm::SMRange const emptySpan{};
  std::string const vn = "lesma.vt." + MangleUtils::getTypeMangledName(emptySpan, classTy);
  if (llvm::GlobalVariable* existing = theModule->getGlobalVariable(vn, true)) {
    classVtableGlobals[classTy] = existing;
    return existing;
  }

  auto* arrTy = llvm::ArrayType::get(ptrTy, order.size());
  std::vector<llvm::Constant*> elems(order.size(), llvm::ConstantPointerNull::get(ptrTyTyped));

  if (Type* sup = classTy->getClassSuperclass()) {
    const Class* supAst = nullptr;
    if (auto sit = codegenClassAstByType.find(sup); sit != codegenClassAstByType.end()) {
      supAst = sit->second;
    }
    llvm::GlobalVariable* pg = getOrEmitClassVtableGlobal(sup, supAst);
    if (pg != nullptr && pg->hasInitializer()) {
      if (auto* carr = llvm::dyn_cast<llvm::ConstantArray>(pg->getInitializer())) {
        for (unsigned i = 0; i < carr->getNumOperands() && i < elems.size(); ++i) {
          elems[i] = remapVtableSlotConstant(theModule.get(), ptrTyTyped,
                                             llvm::cast<llvm::Constant>(carr->getOperand(i)));
        }
      }
    }
  }

  if (astNode != nullptr) {
    for (FuncDecl* m : astNode->getMethods()) {
      if (m->getName() == "new") {
        continue;
      }
      Value* rs = m->getResolvedSymbol();
      if (rs == nullptr) {
        continue;
      }
      std::string const methodKey = makeResolvedCallableKey(rs);
      llvm::Constant* fnConst = materializeVtableFunctionPointer(theModule.get(), ptrTyTyped, rs);
      if (fnConst == nullptr) {
        continue;
      }
      for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == methodKey) {
          elems[i] = fnConst;
          break;
        }
      }
    }
  }

  llvm::Constant* init = llvm::ConstantArray::get(arrTy, elems);
  // LinkOnceODR + hidden keeps one definition across ORC JIT modules and survives GlobalDCE better
  // than private linkage when stores reference the vtable by symbol name after inlining.
  auto* gv = new llvm::GlobalVariable(*theModule, arrTy, true,
                                      llvm::GlobalValue::LinkOnceODRLinkage, init, vn);
  gv->setVisibility(llvm::GlobalValue::HiddenVisibility);
  llvm::appendToCompilerUsed(*theModule, {gv});
  classVtableGlobals[classTy] = gv;
  return gv;
}

auto Codegen::emitInitClassVtablePointer(lesma::Type* classTy, llvm::Value* objectPtr) -> void {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS) || objectPtr == nullptr) {
    return;
  }
  const Class* ast = nullptr;
  if (auto ait = codegenClassAstByType.find(classTy); ait != codegenClassAstByType.end()) {
    ast = ait->second;
  }
  if (ast == nullptr && !classTy->getDisplayName().empty()) {
    if (auto dit = codegenClassAstByDisplayName.find(classTy->getDisplayName());
        dit != codegenClassAstByDisplayName.end()) {
      ast = dit->second;
    }
  }
  llvm::GlobalVariable* vtGlobal = getOrEmitClassVtableGlobal(classTy, ast);
  auto* st = llvm::cast<llvm::StructType>(getOrCreateLlvmType(classTy));
  llvm::Value* slot = builder->CreateStructGEP(st, objectPtr, 0, "vptr.slot");
  if (vtGlobal != nullptr) {
    builder->CreateStore(vtGlobal, slot);
  } else {
    builder->CreateStore(llvm::ConstantPointerNull::get(builder->getPtrTy()), slot);
  }
}

auto Codegen::visit(const FuncDecl* node) -> void {
  setDebugLoc(node->getSpan());
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

  if (selfSymbol != nullptr && !node->getIsStatic()) {
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

  lesma::Value* existingFunc =
      scope->lookupFunction(node->getName(), paramTypes, FunctionLookupKind::OVERLOAD_IDENTITY);
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
  if (selfSymbol != nullptr && !currentGenericTypes.empty()) {
    existingFunc = nullptr;
  }
  if (existingFunc != nullptr && existingFunc->getLlvmValue() != nullptr) {
    result = std::make_unique<Value>(*existingFunc);
    return;
  }

  auto mangledName = getMangledName(node->getSpan(), node->getName(), paramTypes,
                                    selfSymbol != nullptr && !node->getIsStatic());
  std::string signatureKey = makeCallableSignatureKey(node->getName(), paramTypes);
  if (!currentGenericTypes.empty()) {
    std::vector<std::string> bindingOrder;
    if (selfSymbol != nullptr && selfSymbol->getType() != nullptr &&
        selfSymbol->getType()->getElementType() != nullptr) {
      bindingOrder = selfSymbol->getType()->getElementType()->getGenericParams();
    }
    if (bindingOrder.empty()) {
      bindingOrder.reserve(currentGenericTypes.size());
      for (const auto& kv : currentGenericTypes) {
        bindingOrder.push_back(kv.first);
      }
      std::sort(bindingOrder.begin(), bindingOrder.end());
    }
    appendGenericBindingSuffix(node->getSpan(), mangledName, bindingOrder, currentGenericTypes);
    appendGenericBindingSuffix(node->getSpan(), signatureKey, bindingOrder, currentGenericTypes);
  }
  if (selfSymbol != nullptr && !currentGenericTypes.empty()) {
    if (auto it = specializedFunctions.find(mangledName); it != specializedFunctions.end() &&
                                                          it->second != nullptr &&
                                                          it->second->getLlvmValue() != nullptr) {
      result = std::make_unique<Value>(*it->second);
      return;
    }
  }
  auto linkage = shouldExport ? Function::ExternalLinkage : Function::PrivateLinkage;
  Value* templateFuncSymbol = node->getResolvedSymbol();

  llvm::Type* llvmReturnType = nullptr;
  if (returnType->is(BaseType::TY_FUNCTION)) {
    llvmReturnType = getFuncValuePairLlvmType();
  } else if (returnType->is(BaseType::TY_PTR) || returnType->is(BaseType::TY_CLASS)) {
    llvmReturnType = builder->getPtrTy();
  } else {
    llvmReturnType = returnType->getLlvmType();
  }
  llvm::FunctionType* funcType =
      FunctionType::get(llvmReturnType, paramLLVMTypes, node->getVarArgs());
  Function* f = Function::Create(funcType, linkage, mangledName, *theModule);
  attachFunctionDebugInfo(f, node->getName(), mangledName, node->getSpan(), linkage, false);
  auto loweredType = std::make_unique<Type>(BaseType::TY_FUNCTION, funcType, std::move(fields));
  loweredType->setReturnType(returnType);
  loweredType->setGenericParams(node->getGenericParams());
  {
    std::vector<std::vector<std::string>> tb;
    tb.reserve(node->getGenericParamDecls().size());
    for (const auto& p : node->getGenericParamDecls()) {
      tb.push_back(p.traitBounds);
    }
    loweredType->setGenericParamTraitBounds(std::move(tb));
  }
  loweredType->setVarArgs(node->getVarArgs());
  if (existingFunc != nullptr) {
    existingFunc->setType(cacheType(std::move(loweredType)));
    existingFunc->setLlvmValue(f);
    existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
    existingFunc->setMangledName(mangledName);
    existingFunc->setDeclarationKind(selfSymbol != nullptr ? ValueDeclarationKind::METHOD
                                                           : ValueDeclarationKind::FUNCTION);
    existingFunc->setStaticMethod(node->getIsStatic());
    if (templateFuncSymbol != nullptr && existingFunc->getBodyScope() == nullptr) {
      existingFunc->setBodyScope(templateFuncSymbol->getBodyScope());
    }
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
  funcSymbol->setDeclarationKind(selfSymbol != nullptr ? ValueDeclarationKind::METHOD
                                                       : ValueDeclarationKind::FUNCTION);
  funcSymbol->setStaticMethod(node->getIsStatic());
  funcSymbol->setExported(node->isExported());
  funcSymbol->setMangledName(mangledName);
  if (templateFuncSymbol != nullptr) {
    funcSymbol->setBodyScope(templateFuncSymbol->getBodyScope());
  }
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

  lesma::Value* existingFunc =
      scope->lookupFunction(node->getName(), paramTypes, FunctionLookupKind::OVERLOAD_IDENTITY);
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
  {
    std::vector<std::vector<std::string>> tb;
    tb.reserve(node->getGenericParamDecls().size());
    for (const auto& p : node->getGenericParamDecls()) {
      tb.push_back(p.traitBounds);
    }
    loweredType->setGenericParamTraitBounds(std::move(tb));
  }
  loweredType->setVarArgs(node->getVarArgs());
  existingFunc->setType(cacheType(std::move(loweredType)));
  existingFunc->setLlvmValue(f);
  existingFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
  existingFunc->setMangledName(node->getName());
}

auto Codegen::visit(const Assignment* node) -> void {
  setDebugLoc(node->getSpan());
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
  if (lhs->getStoresFuncValuePair() && result != nullptr && result->getStoresFuncValuePair()) {
    llvm::StructType* pt = getFuncValuePairLlvmType();
    llvm::Value* rhsAgg = builder->CreateLoad(pt, result->getLlvmValue(), "fnval.assign");
    builder->CreateStore(rhsAgg, lhs->getLlvmValue());
    lhs->setClosureCalleeUsesEnvParameter(result->getClosureCalleeUsesEnvParameter());
    return;
  }
  auto value = cast(node->getSpan(), result.get(),
                    isPtr ? lhs->getType()->getElementType() : lhs->getType());

  setDebugLoc(node->getSpan());
  switch (node->getOperator()) {
  case TokenType::EQUAL:
    builder->CreateStore(value->getLlvmValue(), lhs->getLlvmValue());
    break;
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
  setDebugLoc(node->getSpan());
  if (breakBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot break without being in a loop");
  }

  auto* block = breakBlocks.top();
  isBreak = true;

  finishLoopDeferFrameIfAny();
  builder->CreateBr(block);
}

auto Codegen::visit(const Continue* node) -> void {
  setDebugLoc(node->getSpan());
  if (continueBlocks.empty()) {
    throw CodegenError(node->getSpan(), "Cannot continue without being in a loop");
  }

  auto* block = continueBlocks.top();
  isBreak = true;

  finishLoopDeferFrameIfAny();
  builder->CreateBr(block);
}

auto Codegen::visit(const Pass* node) -> void { (void) node; }

auto Codegen::visit(const Return* node) -> void {
  setDebugLoc(node->getSpan());
  // Check if it's top-level
  if (builder->GetInsertBlock()->getParent() == topLevelFunc) {
    throw CodegenError(node->getSpan(), "Return statements are not allowed at top-level");
  }

  flushDeferredFramesForReturn();

  isReturn = true;

  if (node->getValue() == nullptr) {
    if (currentFunction->getType()->getReturnType()->is(BaseType::TY_VOID)) {
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
    if (result != nullptr && result->getStoresFuncValuePair() &&
        result->getLlvmValue() != nullptr) {
      llvm::StructType* pt = getFuncValuePairLlvmType();
      llvm::Value* toRet = result->getLlvmValue();
      if (result->getCategory() == ValueCategory::ADDRESSABLE_STORAGE) {
        toRet = builder->CreateLoad(pt, toRet, "ret.fnval");
      }
      builder->CreateRet(toRet);
      return;
    }
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
      builder->CreateRet(result->getLlvmValue());
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

auto Codegen::runDeferredStatements(std::vector<Statement*> const& stmts) -> void {
  for (auto it = stmts.rbegin(); it != stmts.rend(); ++it) {
    (*it)->accept(*this);
  }
}

auto Codegen::pushDeferBaseline() -> void { deferBaselineStack.push(deferStack.size()); }

auto Codegen::flushDeferredFramesForReturn() -> void {
  if (deferBaselineStack.empty()) {
    runDeferredStatements(deferStack.top());
    return;
  }
  size_t const baseline = deferBaselineStack.top();
  while (deferStack.size() > baseline) {
    runDeferredStatements(deferStack.top());
    deferStack.pop();
  }
  runDeferredStatements(deferStack.top());
}

auto Codegen::finishLoopDeferFrameIfAny() -> void {
  if (deferBaselineStack.empty()) {
    return;
  }
  size_t const baseline = deferBaselineStack.top();
  if (deferStack.size() > baseline) {
    // Loop defer vectors are registered once during codegen but must run after every dynamic
    // iteration; the frame is popped when leaving visit(While)/visit(ForIn), or by
    // flushDeferredFramesForReturn on return.
    runDeferredStatements(deferStack.top());
  }
}

auto Codegen::visit(const UnimplementedStatement* node) -> void {
  throw CodegenError(node->getSpan(), "{}", node->getMessage());
}

auto Codegen::visit(const ExpressionStatement* node) -> void {
  setDebugLoc(node->getSpan());
  node->getExpression()->accept(*this);
  if (expressionCallsStdlibBaseLesExit(node->getExpression()) && currentFunction != nullptr) {
    isReturn = true;
    setDebugLoc(node->getSpan());
    // Terminate this BB so LLVM is satisfied; further stmts in the same compound emit into a new
    // dead block (stdlib exit does not return).
    lesma::Type* declaredReturnType = currentFunction->getType()->getReturnType();
    if (declaredReturnType == nullptr || declaredReturnType->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      getOrCreateLlvmType(declaredReturnType);
      llvm::Type* llvmRet = declaredReturnType->is(BaseType::TY_CLASS)
                                ? builder->getPtrTy()
                                : declaredReturnType->getLlvmType();
      builder->CreateRet(UndefValue::get(llvmRet));
    }
    llvm::Function* parent = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* dead = BasicBlock::Create(theModule->getContext(), "dead", parent);
    builder->SetInsertPoint(dead);
  }
}

auto Codegen::visit(const Import* node) -> void {
  setDebugLoc(node->getSpan());
  compileModule(node->getSpan(), node->getFilePath(), node->isStd(), node->getAlias(),
                node->getImportAll(), node->getImportScope(), node->getImportedNames());
}

auto Codegen::visit(const TraitDecl* /*node*/) -> void {
  // Trait declarations are typechecking-only; witnesses are emitted per impl site when used.
}

auto Codegen::visit(const Class* node) -> void {
  setDebugLoc(node->getSpan());
  if (!node->getGenericParams().empty()) {
    genericClasses[node->getIdentifier()] = node;
    auto* genericSymbol = scope->lookupStruct(node->getIdentifier());
    if (genericSymbol != nullptr) {
      genericSymbol->setGenericClassTemplate(node);
      // Globals must exist before `Box.tag`-style use that precedes any specialization.
      if (genericSymbol->getType() != nullptr) {
        emitClassStaticFieldGlobals(genericSymbol->getType(), node);
      }
    }
    return;
  }

  lesma::Value* existingStruct = scope->lookupStruct(node->getIdentifier());
  if (existingStruct == nullptr) {
    throw CodegenError(node->getSpan(), "Missing typechecked class symbol for {}",
                       node->getIdentifier());
  }

  lesma::Type* type = existingStruct->getType();
  codegenClassAstByType[type] = node;
  if (!type->getDisplayName().empty()) {
    codegenClassAstByDisplayName.insert({type->getDisplayName(), node});
  }
  if (type->getLlvmType() == nullptr) {
    getOrCreateLlvmType(type);
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
      std::vector<lesma::Type*> constructorParams = buildClassMethodParamTypesForLookup(func);
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
    lesma::Value* synthCtor = declareSynthesizedClassConstructor(node, type, existingStruct);
    existingStruct->setConstructor(synthCtor);
  }

  emitClassStaticFieldGlobals(type, node);

  getOrEmitClassVtableGlobal(type, node);

  selfSymbol = nullptr;
}

auto Codegen::emitClassStaticFieldGlobals(lesma::Type* classTy, const Class* astNode) -> void {
  if (classTy == nullptr || astNode == nullptr) {
    return;
  }
  llvm::SMRange const span = astNode->getSpan();
  for (VarDecl* vd : astNode->getFields()) {
    if (!vd->getIsStatic()) {
      continue;
    }
    Value* sym = vd->getResolvedSymbol();
    if (sym == nullptr) {
      continue;
    }
    lesma::Type* fieldTy = sym->getType();
    getOrCreateLlvmType(fieldTy);
    llvm::Type* storageTy = fieldTy->getLlvmType();
    if (fieldTy->is(BaseType::TY_CLASS)) {
      storageTy = builder->getPtrTy();
    } else if (fieldTy->is(BaseType::TY_PTR) && fieldTy->getElementType() != nullptr &&
               fieldTy->getElementType()->is(BaseType::TY_CLASS)) {
      storageTy = builder->getPtrTy();
    } else if (fieldTy->is(BaseType::TY_FUNCTION) && sym->getStoresFuncValuePair()) {
      storageTy = getFuncValuePairLlvmType();
    }
    std::string classManglePart;
    if (classTy->is(BaseType::TY_CLASS) && !classTy->getGenericParams().empty()) {
      std::string const& dn = classTy->getDisplayName();
      if (dn.empty()) {
        throw CodegenError(span, "Internal error: generic class template missing display name");
      }
      classManglePart = "(tmpl_" + dn + ")";
    } else {
      classManglePart = MangleUtils::getTypeMangledName(span, classTy);
    }
    std::string const gvName = std::string{"lesma.sfs."} + classManglePart + "." + sym->getName();
    if (theModule->getGlobalVariable(gvName, true) != nullptr) {
      continue;
    }
    llvm::Constant* init = llvm::Constant::getNullValue(storageTy);
    auto* gv = new llvm::GlobalVariable(*theModule, storageTy, false,
                                        llvm::GlobalValue::PrivateLinkage, init, gvName);
    sym->setLlvmValue(gv);
    sym->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    if (vd->getValue() != nullptr) {
      vd->getValue()->accept(*this);
      auto* valueResult = result.get();
      const bool funcPairStorage =
          fieldTy->is(BaseType::TY_FUNCTION) && sym->getStoresFuncValuePair();
      if (funcPairStorage) {
        if (valueResult->getLlvmValue() == nullptr && !fieldTy->getGenericParams().empty()) {
          builder->CreateStore(llvm::ConstantAggregateZero::get(getFuncValuePairLlvmType()), gv);
        } else if (valueResult->getLlvmValue() != nullptr &&
                   valueResult->getLlvmValue()->getType() == getFuncValuePairLlvmType()) {
          builder->CreateStore(valueResult->getLlvmValue(), gv);
        } else if (valueResult->getStoresFuncValuePair()) {
          llvm::StructType* pt = getFuncValuePairLlvmType();
          llvm::Value* loaded = builder->CreateLoad(pt, valueResult->getLlvmValue(),
                                                    sym->getName() + ".sfnpair.init");
          builder->CreateStore(loaded, gv);
        } else {
          auto rhs = cast(vd->getValue()->getSpan(), valueResult, fieldTy);
          builder->CreateStore(rhs->getLlvmValue(), gv);
        }
        if (auto* le = dynamic_cast<LambdaExpr*>(vd->getValue())) {
          if (Value* rs = le->getResolvedSymbol(); rs != nullptr) {
            sym->setClosureCalleeUsesEnvParameter(rs->getClosureCalleeUsesEnvParameter());
          }
        } else if (dynamic_cast<FuncCall*>(vd->getValue()) != nullptr &&
                   fieldTy->is(BaseType::TY_FUNCTION)) {
          sym->setClosureCalleeUsesEnvParameter(true);
        }
      } else {
        auto rhs = cast(vd->getValue()->getSpan(), valueResult, fieldTy);
        builder->CreateStore(rhs->getLlvmValue(), gv);
      }
    }
  }
}

auto Codegen::llvmGlobalForClassStaticField(lesma::Type* classTy, const std::string& fieldName)
    -> llvm::Value* {
  // Globals are emitted once on the class template; specialized codegen shells may omit static
  // Field entries, so always resolve storage from the template type.
  Type* templateTy = classTy;
  if (auto it = specializedClassTemplateOf.find(classTy); it != specializedClassTemplateOf.end()) {
    templateTy = it->second;
  }
  Field* tf = TypeUtils::findStaticFieldInClass(templateTy, fieldName);
  if (tf == nullptr || tf->getDeclarationSymbol() == nullptr) {
    return nullptr;
  }
  return tf->getDeclarationSymbol()->getLlvmValue();
}

auto Codegen::visit(const Enum* node) -> void {
  setDebugLoc(node->getSpan());
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

auto Codegen::visit(const FuncCall* node) -> void {
  setDebugLoc(node->getSpan());
  result = genFuncCall(node, {});
}

auto Codegen::visit(const LambdaExpr* node) -> void {
  setDebugLoc(node->getSpan());
  Value* resolved = node->getResolvedSymbol();
  if (resolved == nullptr || resolved->getType() == nullptr ||
      !resolved->getType()->is(BaseType::TY_FUNCTION)) {
    throw CodegenError(node->getSpan(), "Lambda has no resolved function type");
  }
  if (!resolved->getType()->getGenericParams().empty()) {
    genericLambdas[resolved->getName()] = node;
    resolved->setLlvmValue(nullptr);
    result = std::make_unique<Value>(*resolved);
    return;
  }

  auto* fnType = resolved->getType();
  std::vector<Field*> fields = fnType->getFields();
  const std::vector<Value*>& caps = resolved->getClosureCaptureOuters();

  llvm::StructType* envStructTy = nullptr;
  if (!caps.empty()) {
    std::vector<llvm::Type*> envFields;
    envFields.reserve(caps.size());
    for (Value* cap : caps) {
      Type* t = cap->getType();
      getOrCreateLlvmType(t);
      envFields.push_back(t->getLlvmType());
    }
    envStructTy = llvm::StructType::create(theModule->getContext(), envFields,
                                           resolved->getName() + std::string{".env"});
  }

  std::vector<llvm::Type*> paramLLVMTypes;
  paramLLVMTypes.reserve(fields.size() + 1U);
  if (!caps.empty()) {
    paramLLVMTypes.push_back(builder->getPtrTy());
  }
  for (Field* field : fields) {
    getOrCreateLlvmType(field->type);
    paramLLVMTypes.push_back(field->type->getLlvmType());
  }
  Type* retType = fnType->getReturnType();
  if (retType == nullptr) {
    retType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
    fnType->setReturnType(retType);
  }
  getOrCreateLlvmType(retType);
  llvm::Type* llvmReturnType = retType->is(BaseType::TY_CLASS) || retType->is(BaseType::TY_PTR)
                                   ? builder->getPtrTy()
                                   : retType->getLlvmType();
  std::string const lambdaName = resolved->getName();
  llvm::FunctionType* llvmFnType = FunctionType::get(llvmReturnType, paramLLVMTypes, false);
  Function* f =
      Function::Create(llvmFnType, llvm::GlobalValue::PrivateLinkage, lambdaName, *theModule);
  resolved->setLlvmValue(f);
  resolved->setMangledName(lambdaName);
  resolved->setClosureCalleeUsesEnvParameter(!caps.empty());

  llvm::BasicBlock* resumeBlock = builder->GetInsertBlock();
  SymbolTable* savedScope = scope;
  Value* savedCurrentFunction = currentFunction;
  scope = resolved->getBodyScope() != nullptr ? resolved->getBodyScope() : savedScope;
  currentFunction = resolved;
  deferStack.emplace();
  pushDeferBaseline();
  BasicBlock* entry = BasicBlock::Create(theModule->getContext(), "entry", f);
  builder->SetInsertPoint(entry);
  setDebugLoc(node->getSpan());

  unsigned argIdx = 0U;
  if (!caps.empty()) {
    llvm::Argument* envArg = f->getArg(argIdx++);
    envArg->setName("__env");
    llvm::Value* typedEnv = builder->CreateBitCast(
        envArg, llvm::PointerType::get(envStructTy->getContext(), 0U), "env.ptr");
    SymbolTable* body = resolved->getBodyScope();
    for (size_t i = 0; i < caps.size(); ++i) {
      Value* outer = caps[i];
      if (body == nullptr) {
        throw CodegenError(node->getSpan(), "Lambda missing body scope for captures");
      }
      Value* shadow = body->lookupShallow(outer->getName());
      if (shadow == nullptr || shadow->getClosureSlotOuter() != outer) {
        throw CodegenError(node->getSpan(), "Internal error: missing capture shadow for {}",
                           outer->getName());
      }
      llvm::Value* slot = builder->CreateStructGEP(envStructTy, typedEnv, static_cast<unsigned>(i),
                                                   outer->getName());
      Type* outerTy = outer->getType();
      getOrCreateLlvmType(outerTy);
      llvm::Value* loaded = builder->CreateLoad(outerTy->getLlvmType(), slot);
      llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
      llvm::AllocaInst* capAlloca =
          createAllocaInEntry(parentFct, loaded->getType(), outer->getName() + ".cap");
      builder->CreateStore(loaded, capAlloca);
      shadow->setLlvmValue(capAlloca);
      shadow->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    }
  }

  for (size_t i = 0; i < fields.size(); ++i) {
    Field* field = fields[i];
    auto* arg = f->getArg(argIdx++);
    Value* paramSymbol = scope->lookup(field->name);
    if (paramSymbol == nullptr) {
      continue;
    }
    llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
    llvm::AllocaInst* alloca = createAllocaInEntry(parentFct, arg->getType(), field->name);
    builder->CreateStore(arg, alloca);
    paramSymbol->setLlvmValue(alloca);
  }

  if (node->isExpressionBody()) {
    node->getExpressionBody()->accept(*this);
    llvm::Value* rv = result != nullptr ? result->getLlvmValue() : nullptr;
    flushDeferredFramesForReturn();
    if (retType->is(BaseType::TY_VOID)) {
      builder->CreateRetVoid();
    } else {
      if (rv == nullptr) {
        throw CodegenError(node->getSpan(), "Lambda expression body did not produce a value");
      }
      builder->CreateRet(rv);
    }
  } else if (node->getBlockBody() != nullptr) {
    node->getBlockBody()->accept(*this);
    if (builder->GetInsertBlock()->getTerminator() == nullptr) {
      flushDeferredFramesForReturn();
      if (retType->is(BaseType::TY_VOID)) {
        builder->CreateRetVoid();
      } else {
        throw CodegenError(node->getSpan(), "Non-void lambda may reach end without returning");
      }
    }
  }

  std::string verifyOutput;
  llvm::raw_string_ostream oss(verifyOutput);
  if (llvm::verifyFunction(*f, &oss)) {
    throw CodegenError(node->getSpan(), "Invalid lambda function {}\n{}", lambdaName, verifyOutput);
  }
  scope = savedScope;
  currentFunction = savedCurrentFunction;
  deferStack.pop();
  deferBaselineStack.pop();
  builder->SetInsertPoint(resumeBlock);
  builder->SetCurrentDebugLocation(llvm::DebugLoc());

  llvm::Function* resumeFn = resumeBlock->getParent();
  llvm::StructType* pairTy = getFuncValuePairLlvmType();
  llvm::AllocaInst* pairSlot = createAllocaInEntry(resumeFn, pairTy, lambdaName + ".pair");
  llvm::Value* codePtr = builder->CreateBitCast(f, builder->getPtrTy());
  llvm::Value* envStoreVal = llvm::ConstantPointerNull::get(builder->getPtrTy());
  if (!caps.empty()) {
    llvm::Type* i64Ty = builder->getInt64Ty();
    llvm::Value* envSize = llvm::ConstantInt::get(
        i64Ty, theModule->getDataLayout().getTypeAllocSize(envStructTy).getFixedValue());
    envStoreVal = emitMalloc(envSize, lambdaName + ".env");
    llvm::Value* typedEnv =
        builder->CreateBitCast(envStoreVal, llvm::PointerType::get(envStructTy->getContext(), 0U));
    for (size_t i = 0; i < caps.size(); ++i) {
      Value* outer = caps[i];
      if (outer->getLlvmValue() == nullptr) {
        throw CodegenError(node->getSpan(), "Capture '{}' has no LLVM slot at lambda site",
                           outer->getName());
      }
      Type* outerTy = outer->getType();
      getOrCreateLlvmType(outerTy);
      llvm::Value* v = builder->CreateLoad(outerTy->getLlvmType(), outer->getLlvmValue(),
                                           outer->getName() + ".v");
      llvm::Value* slot =
          builder->CreateStructGEP(envStructTy, typedEnv, static_cast<unsigned>(i), "cap.slot");
      builder->CreateStore(v, slot);
    }
  }
  builder->CreateStore(codePtr, builder->CreateStructGEP(pairTy, pairSlot, 0U));
  builder->CreateStore(envStoreVal, builder->CreateStructGEP(pairTy, pairSlot, 1U));

  auto out = std::make_unique<Value>("", fnType, pairSlot);
  out->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  out->setStoresFuncValuePair(true);
  out->setClosureCalleeUsesEnvParameter(!caps.empty());
  result = std::move(out);
}

auto Codegen::visit(const BinaryOp* node) -> void {
  setDebugLoc(node->getSpan());
  node->getLeft()->accept(*this);
  auto left = std::move(result);
  node->getRight()->accept(*this);
  auto right = std::move(result);
  setDebugLoc(node->getSpan());
  lesma::Type* finalType = CodegenTypeUtils::getExtendedType(left->getType(), right->getType());
  if (finalType == nullptr && left->getType()->is(BaseType::TY_ENUM) &&
      right->getType()->is(BaseType::TY_ENUM) && left->getType()->isEqual(right->getType())) {
    finalType = left->getType();
  }

  switch (node->getOperator()) {
  case TokenType::MINUS:
  case TokenType::PLUS:
  case TokenType::STAR:
  case TokenType::SLASH:
  case TokenType::MOD: {
    TokenType const arithOp = node->getOperator();
    if (auto arith = emitPromotedArithmetic(node->getSpan(), arithOp, left, right, finalType)) {
      result = std::move(arith);
      return;
    }
    break;
  }
  case TokenType::POWER:
    if (finalType == nullptr) {
      break;
    }

    if (!right->getType()->is(BaseType::TY_INT) && !right->getType()->isFloatingPoint()) {
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

    if (finalType->isFloatingPoint()) {
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

    if (finalType->isFloatingPoint()) {
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

    if (finalType->isFloatingPoint()) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      llvm::Value* cmp = finalType->isSigned()
                             ? builder->CreateICmpSGT(left->getLlvmValue(), right->getLlvmValue())
                             : builder->CreateICmpUGT(left->getLlvmValue(), right->getLlvmValue());
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), cmp);
      return;
    }

    break;
  case TokenType::GREATER_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->isFloatingPoint()) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOGE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      llvm::Value* cmp = finalType->isSigned()
                             ? builder->CreateICmpSGE(left->getLlvmValue(), right->getLlvmValue())
                             : builder->CreateICmpUGE(left->getLlvmValue(), right->getLlvmValue());
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), cmp);
      return;
    }

    break;
  case TokenType::LESS:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->isFloatingPoint()) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLT(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      llvm::Value* cmp = finalType->isSigned()
                             ? builder->CreateICmpSLT(left->getLlvmValue(), right->getLlvmValue())
                             : builder->CreateICmpULT(left->getLlvmValue(), right->getLlvmValue());
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), cmp);
      return;
    }

    break;
  case TokenType::LESS_EQUAL:
    left = cast(node->getSpan(), left.get(), finalType);
    right = cast(node->getSpan(), right.get(), finalType);

    if (finalType == nullptr) {
      break;
    }

    if (finalType->isFloatingPoint()) {
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())),
          builder->CreateFCmpOLE(left->getLlvmValue(), right->getLlvmValue()));
      return;
    }

    if (finalType->is(BaseType::TY_INT)) {
      llvm::Value* cmp = finalType->isSigned()
                             ? builder->CreateICmpSLE(left->getLlvmValue(), right->getLlvmValue())
                             : builder->CreateICmpULE(left->getLlvmValue(), right->getLlvmValue());
      result = std::make_unique<Value>(
          "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), cmp);
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
  setDebugLoc(node->getSpan());
  node->getLeft()->accept(*this);
  auto listValue = std::move(result);
  node->getIndex()->accept(*this);
  auto indexValue = std::move(result);
  setDebugLoc(node->getSpan());
  if (listValue != nullptr && listValue->getType() != nullptr &&
      listValue->getType()->is(BaseType::TY_TUPLE)) {
    auto* idxLit = dynamic_cast<Literal*>(node->getIndex());
    if (idxLit == nullptr || idxLit->getType() != TokenType::INTEGER) {
      throw CodegenError(node->getIndex()->getSpan(),
                         "Tuple index must be a non-negative integer literal");
    }
    unsigned long idx = 0;
    try {
      idx = static_cast<unsigned long>(std::stoull(idxLit->getValue()));
    } catch (...) {
      throw CodegenError(node->getIndex()->getSpan(), "Invalid tuple index literal");
    }
    std::vector<Field*> const tf = listValue->getType()->getFields();
    if (idx >= tf.size()) {
      throw CodegenError(node->getSpan(), "Invalid index on tuple");
    }
    llvm::Value* agg = listValue->getLlvmValue();
    llvm::Value* ev = builder->CreateExtractValue(agg, static_cast<unsigned>(idx), "tuple.sub");
    result = std::make_unique<Value>("", tf[idx]->type, ev);
    return;
  }
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

auto Codegen::superMethodReceiverMatchesFormalCodegen(Type* formalReceiverClass,
                                                      Type* staticSuperType) const -> bool {
  if (formalReceiverClass == nullptr || staticSuperType == nullptr) {
    return false;
  }
  if (formalReceiverClass->isEqual(staticSuperType)) {
    return true;
  }
  if (auto specIt = specializedClassTemplateOf.find(staticSuperType);
      specIt != specializedClassTemplateOf.end() && formalReceiverClass->isEqual(specIt->second)) {
    return true;
  }
  return false;
}

auto Codegen::emitClassStaticFieldValue(Type* classTy, const std::string& field,
                                        llvm::SMRange unknownFieldSpan,
                                        llvm::SMRange storageDiagSpan)
    -> std::unique_ptr<lesma::Value> {
  Field* sf = TypeUtils::findStaticFieldInClass(classTy, field);
  if (sf == nullptr) {
    if (auto it = specializedClassTemplateOf.find(classTy);
        it != specializedClassTemplateOf.end()) {
      sf = TypeUtils::findStaticFieldInClass(it->second, field);
    }
  }
  if (sf == nullptr) {
    throw CodegenError(unknownFieldSpan, "Unknown static field {} for class {}", field,
                       classTy->getDisplayName().empty() ? "(unknown)" : classTy->getDisplayName());
  }
  llvm::Value* gv = llvmGlobalForClassStaticField(classTy, field);
  if (gv == nullptr) {
    throw CodegenError(storageDiagSpan, "Static field {} has no lowered storage", field);
  }
  lesma::Type* ft = sf->type;
  getOrCreateLlvmType(ft);
  if (isAssignment) {
    lesma::Type* ptrToVal = ft;
    if (ft->is(BaseType::TY_CLASS)) {
      ptrToVal = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), ft));
    } else if (ft->is(BaseType::TY_PTR) && ft->getElementType() != nullptr &&
               ft->getElementType()->is(BaseType::TY_CLASS)) {
      ptrToVal = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), ft));
    }
    auto out = std::make_unique<Value>("", ptrToVal, gv);
    if (ft->is(BaseType::TY_FUNCTION) && sf->getDeclarationSymbol() != nullptr &&
        sf->getDeclarationSymbol()->getStoresFuncValuePair()) {
      out->setStoresFuncValuePair(true);
      out->setClosureCalleeUsesEnvParameter(
          sf->getDeclarationSymbol()->getClosureCalleeUsesEnvParameter());
    }
    return out;
  }
  if (ft->is(BaseType::TY_FUNCTION) && sf->getDeclarationSymbol() != nullptr &&
      sf->getDeclarationSymbol()->getStoresFuncValuePair()) {
    auto slot = std::make_unique<Value>("", ft, gv);
    slot->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    slot->setStoresFuncValuePair(true);
    slot->setClosureCalleeUsesEnvParameter(
        sf->getDeclarationSymbol()->getClosureCalleeUsesEnvParameter());
    return slot;
  }
  llvm::Type* storageTy = ft->getLlvmType();
  if (ft->is(BaseType::TY_CLASS)) {
    storageTy = builder->getPtrTy();
  } else if (ft->is(BaseType::TY_PTR) && ft->getElementType() != nullptr &&
             ft->getElementType()->is(BaseType::TY_CLASS)) {
    storageTy = builder->getPtrTy();
  }
  return std::make_unique<Value>("", ft, builder->CreateLoad(storageTy, gv));
}

auto Codegen::emitClassInstanceDataField(Value* classStructSym, llvm::Value* objectBase,
                                         const std::string& field, llvm::SMRange span,
                                         bool baseMayBeNonPointer)
    -> std::unique_ptr<lesma::Value> {
  lesma::Type* structTy = classStructSym->getType();
  auto index = TypeUtils::findIndexInFields(structTy, field);
  auto* type = TypeUtils::findTypeInFields(structTy, field);
  if (index == -1) {
    throw CodegenError(span, "Could not find field {} in {}", field,
                       structTy->getLlvmType()->getStructName().str());
  }
  llvm::Value* fieldBasePtr = objectBase;
  if (baseMayBeNonPointer && !fieldBasePtr->getType()->isPointerTy()) {
    auto* parentFn = builder->GetInsertBlock()->getParent();
    auto* tempAlloca = createAllocaInEntry(parentFn, structTy->getLlvmType(), field + ".field.tmp");
    builder->CreateStore(fieldBasePtr, tempAlloca);
    fieldBasePtr = tempAlloca;
  }
  unsigned const structIdx =
      TypeUtils::classDataFieldStructIndex(structTy, static_cast<unsigned>(index));
  auto* ptr = builder->CreateStructGEP(structTy->getLlvmType(), fieldBasePtr, structIdx);
  if (isAssignment) {
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), type)), ptr);
  }
  return std::make_unique<Value>("", type, loadStoredAggregateFieldValue(ptr, type));
}

void Codegen::emitClassStaticMethodCall(const DotOp* node, Type* classTy, const FuncCall* method) {
  auto recvHolder = std::make_unique<lesma::Value>("", classTy, static_cast<llvm::Value*>(nullptr));
  std::vector<std::unique_ptr<lesma::Value>> argStorage;
  std::vector<lesma::Value*> args;
  evaluateCallArgValues(method, argStorage, args);
  std::vector<lesma::Type*> explicitTypeArgs;
  evaluateCallExplicitTypeArgs(method, explicitTypeArgs);
  setDebugLoc(node->getSpan());
  result = callMethodByName(node->getSpan(), recvHolder.get(), method->getName(), args,
                            explicitTypeArgs, method->getResolvedSymbol(), method);
}

void Codegen::emitClassInstanceMethodCall(const DotOp* node, lesma::Value* receiver,
                                          const FuncCall* method) {
  std::vector<std::unique_ptr<lesma::Value>> argStorage;
  std::vector<lesma::Value*> args;
  evaluateCallArgValues(method, argStorage, args);
  std::vector<lesma::Type*> explicitTypeArgs;
  evaluateCallExplicitTypeArgs(method, explicitTypeArgs);
  setDebugLoc(node->getSpan());
  result = callMethodByName(node->getSpan(), receiver, method->getName(), args, explicitTypeArgs,
                            method->getResolvedSymbol(), method);
}

void Codegen::lowerDotOpSuperMethodCall(const DotOp* node) {
  auto* method = dynamic_cast<FuncCall*>(node->getRight());
  if (method == nullptr) {
    throw CodegenError(node->getSpan(), "Expected super.method(...)");
  }
  Value* resolved = method->getResolvedSymbol();
  if (builder->GetInsertBlock() != nullptr) {
    if (auto* curFn = builder->GetInsertBlock()->getParent()) {
      if (resolved != nullptr && resolved->getLlvmValue() != nullptr &&
          resolved->getLlvmValue() == curFn) {
        resolved = nullptr;
      }
    }
  }
  std::vector<std::unique_ptr<lesma::Value>> argStorage;
  std::vector<lesma::Value*> args;
  evaluateCallArgValues(method, argStorage, args);
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  std::unique_ptr<lesma::Value> implicitSelfValue;
  bool const chainingCtorWithExplicitReceiver = method->getName() == "new" && !args.empty();
  if (!chainingCtorWithExplicitReceiver) {
    Value* selfSym = scope->lookup("self");
    if (selfSym == nullptr) {
      throw CodegenError(node->getSpan(),
                         "Internal error: super call requires `self` in scope (instance method)");
    }
    implicitSelfValue = materializeSymbolValue(selfSym);
    appendCallableArgument(implicitSelfValue.get(), paramTypes, paramsLLVM);
  }
  for (auto* arg : args) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }
  if (resolved == nullptr || resolved->getLlvmValue() == nullptr) {
    Type* curCls = nullptr;
    if (currentFunction != nullptr) {
      std::vector<Field*> const ff = currentFunction->getType()->getFields();
      if (!ff.empty() && ff[0]->type != nullptr && ff[0]->type->is(BaseType::TY_PTR) &&
          ff[0]->type->getElementType() != nullptr &&
          ff[0]->type->getElementType()->is(BaseType::TY_CLASS)) {
        curCls = ff[0]->type->getElementType();
      }
    }
    if (curCls == nullptr && selfSymbol != nullptr && selfSymbol->getType() != nullptr &&
        selfSymbol->getType()->getElementType() != nullptr) {
      curCls = selfSymbol->getType()->getElementType();
    }
    Type* superTy = curCls != nullptr ? curCls->getClassSuperclass() : nullptr;
    const std::unordered_map<std::string, Type*>* superSeed = nullptr;
    if (superTy != nullptr) {
      superSeed = specializedClassEnvFor(superTy);
    }
    Value* superPick = nullptr;
    if (superTy != nullptr) {
      superPick = scope->lookupSuperClassMethod(
          method->getName(), paramTypes,
          [this, superTy](Type* recvCls) {
            return superMethodReceiverMatchesFormalCodegen(recvCls, superTy);
          },
          superTy, superSeed);
    }
    if (superPick != nullptr) {
      resolved = superPick;
    }
    if (resolved == nullptr || resolved->getLlvmValue() == nullptr) {
      resolved =
          scope->lookupFunction(method->getName(), paramTypes, FunctionLookupKind::VALUE, curCls);
    }
  }
  if (resolved == nullptr || resolved->getLlvmValue() == nullptr) {
    throw CodegenError(node->getSpan(), "Internal error: super call has no lowered method");
  }
  auto fields = resolved->getType()->getFields();
  auto savedGenerics = currentGenericTypes;
  if (auto genIt = specializationEnvs.find(resolved); genIt != specializationEnvs.end()) {
    currentGenericTypes = genIt->second;
  } else if (!fields.empty() && fields[0]->type != nullptr &&
             fields[0]->type->is(BaseType::TY_PTR) &&
             fields[0]->type->getElementType() != nullptr) {
    Type* baseTy = fields[0]->type->getElementType();
    if (const auto* envPtr = specializedClassEnvFor(baseTy); envPtr != nullptr) {
      currentGenericTypes = *envPtr;
    }
  }
  try {
    std::vector<llvm::Value*> finalParams;
    finalParams.reserve(paramsLLVM.size());
    for (size_t i = 0; i < paramsLLVM.size(); ++i) {
      auto paramVal = std::make_unique<Value>("", paramTypes[i], paramsLLVM[i]);
      auto castVal = cast(node->getSpan(), paramVal.get(), fields[i]->type);
      finalParams.push_back(castVal->getLlvmValue());
    }
    lesma::Type* returnTy = resolved->getType()->getReturnType();
    if (returnTy != nullptr) {
      getOrCreateLlvmType(returnTy);
    }
    auto* calleeFn = llvm::cast<llvm::Function>(resolved->getLlvmValue());
    llvm::Value* callResult = builder->CreateCall(calleeFn, finalParams);
    currentGenericTypes = std::move(savedGenerics);
    result = std::make_unique<Value>("", returnTy, callResult);
    if (returnTy != nullptr && returnTy->is(BaseType::TY_FUNCTION)) {
      result->setStoresFuncValuePair(true);
      result->setCategory(ValueCategory::DIRECT_VALUE);
    }
  } catch (...) {
    currentGenericTypes = std::move(savedGenerics);
    throw;
  }
}

auto Codegen::visit(const DotOp* node) -> void {
  setDebugLoc(node->getSpan());
  if (dynamic_cast<SuperExpr*>(node->getLeft()) != nullptr) {
    lowerDotOpSuperMethodCall(node);
    return;
  }

  node->getLeft()->accept(*this);
  auto leftValue = std::move(result);
  setDebugLoc(node->getSpan());
  if (leftValue != nullptr && leftValue->getType() != nullptr &&
      leftValue->getType()->is(BaseType::TY_ARRAY)) {
    auto* call = dynamic_cast<FuncCall*>(node->getRight());
    if (call == nullptr) {
      throw CodegenError(node->getSpan(), "Expected list method call after dot");
    }
    std::vector<std::unique_ptr<lesma::Value>> argStorage;
    std::vector<lesma::Value*> args;
    evaluateCallArgValues(call, argStorage, args);
    std::vector<lesma::Type*> explicitTypeArgs;
    evaluateCallExplicitTypeArgs(call, explicitTypeArgs);
    setDebugLoc(node->getSpan());
    result =
        callMethodByName(node->getSpan(), leftValue.get(), call->getName(), args, explicitTypeArgs);
    return;
  }

  if (leftValue != nullptr && leftValue->getType() != nullptr &&
      leftValue->getType()->is(BaseType::TY_UNION)) {
    auto* method = dynamic_cast<FuncCall*>(node->getRight());
    if (method == nullptr) {
      throw CodegenError(node->getSpan(), "Dot on a union requires a method call");
    }
    std::vector<std::unique_ptr<lesma::Value>> argStorage;
    std::vector<lesma::Value*> args;
    evaluateCallArgValues(method, argStorage, args);
    std::vector<lesma::Type*> explicitTypeArgs;
    evaluateCallExplicitTypeArgs(method, explicitTypeArgs);
    setDebugLoc(node->getSpan());
    result = emitUnionClassMethodDispatch(node->getSpan(), leftValue.get(), method->getName(), args,
                                          explicitTypeArgs);
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
      evaluateCallArgValues(call, argStorage, args);
      std::vector<lesma::Type*> explicitTypeArgs;
      evaluateCallExplicitTypeArgs(call, explicitTypeArgs);
      setDebugLoc(node->getSpan());
      result = callMethodByName(node->getSpan(), leftValue.get(), call->getName(), args,
                                explicitTypeArgs, call->getResolvedSymbol());
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
      if (leftValue->getCategory() == ValueCategory::TYPE_SYMBOL) {
        if (!field.empty()) {
          result = emitClassStaticFieldValue(receiverType, field, node->getRight()->getSpan(),
                                             node->getSpan());
          return;
        }
        if (method != nullptr) {
          emitClassStaticMethodCall(node, receiverType, method);
          return;
        }
      }
      if (!field.empty()) {
        result = emitClassInstanceDataField(cls, leftValue->getLlvmValue(), field,
                                            node->getRight()->getSpan(), true);
        return;
      }
      if (method != nullptr) {
        emitClassInstanceMethodCall(node, leftValue.get(), method);
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

        if (!field.empty()) {
          auto pathIt = importAliasToModulePath.find(left->getValue());
          if (pathIt != importAliasToModulePath.end()) {
            lesma::Type* memTy = nullptr;
            for (size_t i = 0; i < importedModules->size(); ++i) {
              if (normalizeResolvedFilesystemPath(importedModules->at(i)) ==
                  normalizeResolvedFilesystemPath(pathIt->second)) {
                SymbolTable* isc = importedScopes->at(i).get();
                Value* vs = isc->lookup(field);
                if (vs != nullptr && vs->getDeclarationKind() == ValueDeclarationKind::VARIABLE &&
                    vs->isExported()) {
                  memTy = vs->getType();
                  break;
                }
              }
            }
            if (memTy != nullptr) {
              getOrCreateLlvmType(memTy);
              llvm::Type* storageTy = memTy->getLlvmType();
              if (memTy->is(BaseType::TY_CLASS)) {
                storageTy = builder->getPtrTy();
              } else if (memTy->is(BaseType::TY_PTR) && memTy->getElementType() != nullptr &&
                         memTy->getElementType()->is(BaseType::TY_CLASS)) {
                storageTy = builder->getPtrTy();
              }
              std::string const mangled = MangleUtils::getGlobalVariableSymbolName(
                  normalizeResolvedFilesystemPath(pathIt->second), field);
              llvm::GlobalVariable* gv = theModule->getGlobalVariable(mangled, true);
              if (gv == nullptr) {
                gv = new llvm::GlobalVariable(*theModule, storageTy, false,
                                              llvm::GlobalValue::ExternalLinkage, nullptr, mangled);
              }
              if (isAssignment) {
                lesma::Type* ptrToVal = memTy;
                if (memTy->is(BaseType::TY_CLASS)) {
                  ptrToVal = cacheType(
                      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), memTy));
                } else if (memTy->is(BaseType::TY_PTR) &&
                           memTy->getElementType()->is(BaseType::TY_CLASS)) {
                  ptrToVal = cacheType(
                      std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), memTy));
                }
                result = std::make_unique<Value>("", ptrToVal, gv);
                return;
              }
              result = std::make_unique<Value>("", memTy, builder->CreateLoad(storageTy, gv));
              return;
            }
          }
          throw CodegenError(node->getRight()->getSpan(), "Unknown module member '{}'", field);
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
      setDebugLoc(node->getSpan());
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
        if (result->getCategory() == ValueCategory::TYPE_SYMBOL) {
          if (!field.empty()) {
            result = emitClassStaticFieldValue(lesmaType, field, node->getRight()->getSpan(),
                                               node->getSpan());
            return;
          }
          if (method != nullptr) {
            emitClassStaticMethodCall(node, lesmaType, method);
            return;
          }
        }
        if (!field.empty()) {
          result = emitClassInstanceDataField(cls, result->getLlvmValue(), field,
                                              node->getRight()->getSpan(), false);
          return;
        }
        if (method != nullptr) {
          emitClassInstanceMethodCall(node, result.get(), method);
          return;
        }
      }
    }
  }
  throw CodegenError(node->getSpan(), "Unimplemented dot accessor: {}",
                     node->toString(sourceManager.get(), "", true));
}

auto Codegen::visit(const CastOp* node) -> void {
  setDebugLoc(node->getSpan());
  node->getExpression()->accept(*this);
  auto expr = std::move(result);
  node->getType()->accept(*this);
  auto* castType = result->getType();
  setDebugLoc(node->getSpan());
  result = cast(node->getSpan(), expr.get(), castType);
}

auto Codegen::visit(const IsOp* node) -> void {
  setDebugLoc(node->getSpan());
  node->getLeft()->accept(*this);
  auto leftOwner = std::move(result);
  Type* leftType = leftOwner->getType();
  Type* rightType = node->getResolvedRhsType();
  if (rightType == nullptr) {
    throw CodegenError(node->getSpan(), "Internal: `is` expression missing resolved RHS type");
  }

  if (leftType != nullptr && leftType->is(BaseType::TY_UNION)) {
    auto idxOpt = unionVariantIndexOf(leftType, rightType);
    if (!idxOpt.has_value()) {
      throw CodegenError(node->getSpan(), "`is` type is not a member of the union value type");
    }
    getOrCreateLlvmType(leftType);
    llvm::Value* agg = leftOwner->getLlvmValue();
    llvm::Value* tagVal = builder->CreateExtractValue(agg, {0U}, "union.tag");
    llvm::Value* cmp = builder->CreateICmpEQ(
        tagVal, llvm::ConstantInt::get(tagVal->getType(), static_cast<uint64_t>(*idxOpt)));
    llvm::Value* val = nullptr;
    if (node->getOperator() == TokenType::IS) {
      val = cmp;
    } else {
      val = builder->CreateNot(cmp);
    }
    result = std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty())), val);
    return;
  }

  if (leftType != nullptr && leftType->is(BaseType::TY_PTR) &&
      leftType->getElementType() != nullptr && leftType->getElementType()->is(BaseType::TY_CLASS)) {
    leftType = leftType->getElementType();
  }
  if (rightType != nullptr && rightType->is(BaseType::TY_PTR) &&
      rightType->getElementType() != nullptr &&
      rightType->getElementType()->is(BaseType::TY_CLASS)) {
    rightType = rightType->getElementType();
  }

  setDebugLoc(node->getSpan());
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
  setDebugLoc(node->getSpan());
  node->getExpression()->accept(*this);
  auto operand = std::move(result);
  setDebugLoc(node->getSpan());

  llvm::Value* val = nullptr;
  lesma::Type* type = operand->getType();
  std::unique_ptr<lesma::Type> ptrTypeHolder; // Keep owned type alive if created

  if (node->getOperator() == TokenType::MINUS) {
    if (operand->getType()->is(BaseType::TY_INT)) {
      val = builder->CreateNeg(operand->getLlvmValue());
    } else if (operand->getType()->isFloatingPoint()) {
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
  setDebugLoc(node->getSpan());
  lesma::Type* listType = node->getResolvedType();
  if (listType != nullptr && !currentGenericTypes.empty() && typeContainsUnboundGeneric(listType)) {
    listType = substituteTypeForSpecializationEnv(listType, currentGenericTypes);
  }
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
    auto* classHandle = emitMalloc(classSize, "list.obj");
    emitInitClassVtablePointer(listType, classHandle);
    unsigned const bufIdx = TypeUtils::classDataFieldStructIndex(listType, 0U);
    auto* storagePtr =
        builder->CreateStructGEP(structType, classHandle, bufIdx, "list.storage.ptr");

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
        setDebugLoc(node->getSpan());
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
      setDebugLoc(node->getSpan());
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

auto Codegen::visit(const DictLiteral* node) -> void {
  setDebugLoc(node->getSpan());
  lesma::Type* dictType = node->getResolvedType();
  if (dictType == nullptr || !dictType->is(BaseType::TY_CLASS)) {
    throw CodegenError(node->getSpan(), "Dict literal has no resolved dict type");
  }
  auto fields = dictType->getFields();
  if (fields.size() < 2 || fields[0]->type == nullptr || fields[1]->type == nullptr ||
      !fields[0]->type->is(BaseType::TY_ARRAY) || !fields[1]->type->is(BaseType::TY_ARRAY) ||
      fields[0]->type->getElementType() == nullptr ||
      fields[1]->type->getElementType() == nullptr) {
    throw CodegenError(node->getSpan(),
                       "Dict literal resolved to invalid class (expected two __buffer fields)");
  }
  lesma::Type* keysBufferType = fields[0]->type;
  lesma::Type* valsBufferType = fields[1]->type;
  lesma::Type* keyElemType = keysBufferType->getElementType();
  lesma::Type* valElemType = valsBufferType->getElementType();

  auto* structType = cast<llvm::StructType>(getOrCreateLlvmType(dictType));
  auto* classSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* classHandle = emitMalloc(classSize, "dict.obj");
  emitInitClassVtablePointer(dictType, classHandle);
  unsigned const keysIdx = TypeUtils::classDataFieldStructIndex(dictType, 0U);
  unsigned const valsIdx = TypeUtils::classDataFieldStructIndex(dictType, 1U);
  auto* keysFieldPtr = builder->CreateStructGEP(structType, classHandle, keysIdx, "dict.keys.ptr");
  auto* valsFieldPtr = builder->CreateStructGEP(structType, classHandle, valsIdx, "dict.vals.ptr");

  std::vector<Expression*> keys = node->getKeys();
  std::vector<Expression*> values = node->getValues();
  if (keys.size() != values.size()) {
    throw CodegenError(node->getSpan(), "Dict literal key/value count mismatch");
  }
  const size_t pairCount = keys.size();

  auto* keysListStructTy = getOrCreateListStructType(keysBufferType);
  auto* valsListStructTy = getOrCreateListStructType(valsBufferType);
  auto* keysHeader =
      emitMalloc(builder->getInt64(
                     theModule->getDataLayout().getTypeAllocSize(keysListStructTy).getFixedValue()),
                 "dict.keys.header");
  auto* valsHeader =
      emitMalloc(builder->getInt64(
                     theModule->getDataLayout().getTypeAllocSize(valsListStructTy).getFixedValue()),
                 "dict.vals.header");

  llvm::Value* keysDataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
  llvm::Value* valsDataPtr = llvm::ConstantPointerNull::get(builder->getPtrTy());
  if (pairCount != 0U) {
    auto* keysElementLlvmType = getListStoredElementType(keysBufferType);
    auto* valsElementLlvmType = getListStoredElementType(valsBufferType);
    keysDataPtr = emitMalloc(
        builder->getInt64(
            theModule->getDataLayout().getTypeAllocSize(keysElementLlvmType).getFixedValue() *
            pairCount),
        "dict.keys.data");
    valsDataPtr = emitMalloc(
        builder->getInt64(
            theModule->getDataLayout().getTypeAllocSize(valsElementLlvmType).getFixedValue() *
            pairCount),
        "dict.vals.data");

    for (size_t i = 0; i < pairCount; ++i) {
      keys[i]->accept(*this);
      setDebugLoc(node->getSpan());
      auto* keyPtr = builder->CreateGEP(keysElementLlvmType, keysDataPtr, builder->getInt64(i),
                                        "dict.key.elem.ptr");
      builder->CreateStore(getListStoredElementValue(keys[i]->getSpan(), result.get(), keyElemType),
                           keyPtr);

      values[i]->accept(*this);
      setDebugLoc(node->getSpan());
      auto* valPtr = builder->CreateGEP(valsElementLlvmType, valsDataPtr, builder->getInt64(i),
                                        "dict.val.elem.ptr");
      builder->CreateStore(
          getListStoredElementValue(values[i]->getSpan(), result.get(), valElemType), valPtr);
    }
  }

  auto* count = builder->getInt64(pairCount);
  emitStoreListDataPtr(keysBufferType, keysHeader, keysDataPtr);
  emitStoreListLength(keysBufferType, keysHeader, count);
  emitStoreListCapacity(keysBufferType, keysHeader, count);
  emitStoreListDataPtr(valsBufferType, valsHeader, valsDataPtr);
  emitStoreListLength(valsBufferType, valsHeader, count);
  emitStoreListCapacity(valsBufferType, valsHeader, count);
  builder->CreateStore(keysHeader, keysFieldPtr);
  builder->CreateStore(valsHeader, valsFieldPtr);
  result = std::make_unique<Value>("", dictType, classHandle);
}

auto Codegen::visit(const TupleLiteral* node) -> void {
  setDebugLoc(node->getSpan());
  lesma::Type* tupleType = node->getResolvedType();
  if (tupleType == nullptr || !tupleType->is(BaseType::TY_TUPLE)) {
    throw CodegenError(node->getSpan(), "Tuple literal has no resolved tuple type");
  }
  getOrCreateLlvmType(tupleType);
  llvm::Type* structTy = tupleType->getLlvmType();
  auto* st = llvm::cast<llvm::StructType>(structTy);
  llvm::Value* agg = llvm::UndefValue::get(st);
  std::vector<Expression*> const elements = node->getElements();
  std::vector<Field*> const fields = tupleType->getFields();
  if (elements.size() != fields.size()) {
    throw CodegenError(node->getSpan(), "Tuple literal element count mismatch");
  }
  for (size_t i = 0; i < elements.size(); ++i) {
    elements[i]->accept(*this);
    setDebugLoc(node->getSpan());
    llvm::Value* ev = result->getLlvmValue();
    agg = builder->CreateInsertValue(agg, ev, static_cast<unsigned>(i), "tuple");
  }
  result = std::make_unique<Value>("", tupleType, agg);
}

auto Codegen::visit(const Literal* node) -> void {
  setDebugLoc(node->getSpan());
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
      auto* classHandle = emitMalloc(classSize, "str.obj");
      emitInitClassVtablePointer(strClass, classHandle);
      unsigned const strFieldIdx = TypeUtils::classDataFieldStructIndex(strClass, 0U);
      auto* storagePtr =
          builder->CreateStructGEP(structType, classHandle, strFieldIdx, "str.storage.ptr");
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
    if (val->getType() != nullptr && val->getType()->is(BaseType::TY_FUNCTION) &&
        val->getCategory() == ValueCategory::CALLABLE_SYMBOL &&
        val->getDeclarationKind() == ValueDeclarationKind::FUNCTION &&
        val->getLlvmValue() != nullptr && !val->getStoresFuncValuePair()) {
      llvm::StructType* pairTy = getFuncValuePairLlvmType();
      llvm::Function* parentFct = builder->GetInsertBlock()->getParent();
      llvm::AllocaInst* pairSlot =
          createAllocaInEntry(parentFct, pairTy, val->getName() + ".fnval");
      llvm::Value* fnVal = val->getLlvmValue();
      llvm::Value* codePtr = llvm::isa<llvm::Function>(fnVal)
                                 ? builder->CreateBitCast(fnVal, builder->getPtrTy())
                                 : fnVal;
      builder->CreateStore(codePtr, builder->CreateStructGEP(pairTy, pairSlot, 0U));
      builder->CreateStore(llvm::ConstantPointerNull::get(builder->getPtrTy()),
                           builder->CreateStructGEP(pairTy, pairSlot, 1U));
      auto wrapped = std::make_unique<Value>("", val->getType(), pairSlot);
      wrapped->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      wrapped->setStoresFuncValuePair(true);
      wrapped->setClosureCalleeUsesEnvParameter(false);
      result = std::move(wrapped);
    } else if (val->getCategory() == ValueCategory::TYPE_SYMBOL) {
      // Type names in value position (`Cell.method`, `Foo.bar`) are not storage; avoid lowering a
      // generic class template to LLVM when materializing.
      result = std::make_unique<Value>(val->getName(), val->getType(), nullptr);
      result->setCategory(ValueCategory::TYPE_SYMBOL);
    } else {
      result = materializeSymbolValue(val);
    }
  } else {
    throw CodegenError(node->getSpan(), "Unknown literal {}", node->getValue());
  }
}

auto Codegen::visit(const SuperExpr* node) -> void {
  throw CodegenError(node->getSpan(), "`super` must be used as super.method(...)");
}

namespace {
[[nodiscard]] auto isStrBoxLesmaType(lesma::Type* t) -> bool {
  if (t == nullptr) {
    return false;
  }
  lesma::Type* b = t;
  if (t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
    b = t->getElementType();
  }
  return b->is(BaseType::TY_CLASS) && b->getDisplayName() == "str";
}

/// Pointers we may free after copying in \c emitCstrConcatValues: results of \c emitMalloc (format
/// helpers, prior concat buffers). Excludes literals (\c llvm::Constant / globals), loads
/// (e.g. \c str.cstr() storage), and any non-malloc calls.
[[nodiscard]] auto stripPtrValue(llvm::Value* v) -> llvm::Value* {
  while (v != nullptr) {
    if (auto* ce = llvm::dyn_cast<llvm::CastInst>(v)) {
      if (!ce->getType()->isPointerTy()) {
        break;
      }
      v = ce->getOperand(0);
      continue;
    }
    break;
  }
  return v;
}

[[nodiscard]] auto isOwnedMallocCstrBuffer(llvm::Value* v) -> bool {
  if (v == nullptr || llvm::isa<llvm::Constant>(v)) {
    return false;
  }
  v = stripPtrValue(v);
  if (v == nullptr || llvm::isa<llvm::Constant>(v)) {
    return false;
  }
  auto* call = llvm::dyn_cast<llvm::CallBase>(v);
  if (call == nullptr) {
    return false;
  }
  llvm::Function* callee = call->getCalledFunction();
  if (callee == nullptr) {
    return false;
  }
  llvm::StringRef calleeName = callee->getName();
  return std::string_view{calleeName.data(), calleeName.size()} == codegen::runtime::MALLOC;
}
} // namespace

auto Codegen::emitCstrConcatValues(llvm::SMRange span, llvm::Value* a, llvm::Value* b)
    -> llvm::Value* {
  (void) span;
  llvm::Type* i8 = llvm::Type::getInt8Ty(theModule->getContext());
  auto strlenFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::STRLEN},
      llvm::FunctionType::get(builder->getInt64Ty(), {builder->getPtrTy()}, false));
  auto memcpyFn = theModule->getOrInsertFunction(
      std::string{codegen::runtime::MEMCPY},
      llvm::FunctionType::get(builder->getPtrTy(),
                              {builder->getPtrTy(), builder->getPtrTy(), builder->getInt64Ty()},
                              false));
  llvm::Value* la = a;
  llvm::Value* lb = b;
  auto* lenA = builder->CreateCall(strlenFn, {la}, "ip.strcat.lenA");
  auto* lenB = builder->CreateCall(strlenFn, {lb}, "ip.strcat.lenB");
  auto* total =
      builder->CreateAdd(builder->CreateAdd(lenA, lenB), builder->getInt64(1), "ip.strcat.total");
  llvm::Value* buf = emitMalloc(total, "ip.strcat.buf");
  builder->CreateCall(memcpyFn, {buf, la, lenA});
  auto* tail = builder->CreateInBoundsGEP(i8, buf, lenA, "ip.strcat.tail");
  builder->CreateCall(memcpyFn, {tail, lb, lenB});
  auto* endPtr = builder->CreateInBoundsGEP(
      i8, buf, builder->CreateSub(total, builder->getInt64(1)), "ip.strcat.nul");
  builder->CreateStore(llvm::ConstantInt::get(i8, 0), endPtr);

  if (la == lb) {
    if (isOwnedMallocCstrBuffer(la)) {
      emitFree(la);
    }
  } else {
    if (isOwnedMallocCstrBuffer(la)) {
      emitFree(la);
    }
    if (isOwnedMallocCstrBuffer(lb)) {
      emitFree(lb);
    }
  }
  return buf;
}

auto Codegen::emitFormatIntegerToCstr(llvm::SMRange span, llvm::Value* intVal, lesma::Type* intTy)
    -> llvm::Value* {
  (void) span;
  llvm::Value* i64v = intVal;
  if (!intVal->getType()->isIntegerTy(64)) {
    if (intTy != nullptr && intTy->isSigned()) {
      i64v = builder->CreateSExt(intVal, builder->getInt64Ty(), "ip.i64");
    } else {
      i64v = builder->CreateZExt(intVal, builder->getInt64Ty(), "ip.i64");
    }
  }
  llvm::Value* buf = emitMalloc(builder->getInt64(80), "ip.int.buf");
  llvm::Value* fmt = (intTy != nullptr && intTy->isSigned()) ? builder->CreateGlobalString("%lld")
                                                             : builder->CreateGlobalString("%llu");
  auto* snprintfTy = llvm::FunctionType::get(
      builder->getInt32Ty(), {builder->getPtrTy(), builder->getInt64Ty(), builder->getPtrTy()},
      true);
  llvm::FunctionCallee snprintfFn = theModule->getOrInsertFunction("snprintf", snprintfTy);
  builder->CreateCall(snprintfFn, {buf, builder->getInt64(79), fmt, i64v});
  return buf;
}

auto Codegen::emitFormatFloatToCstr(llvm::SMRange span, llvm::Value* floatVal, lesma::Type* floatTy)
    -> llvm::Value* {
  (void) span;
  llvm::Value* dbl = floatVal;
  if (floatTy != nullptr && floatTy->is(BaseType::TY_FLOAT32)) {
    dbl = builder->CreateFPExt(floatVal, builder->getDoubleTy(), "ip.dbl");
  }
  llvm::Value* buf = emitMalloc(builder->getInt64(80), "ip.flt.buf");
  llvm::Value* fmt = builder->CreateGlobalString("%g");
  auto* snprintfTy = llvm::FunctionType::get(
      builder->getInt32Ty(), {builder->getPtrTy(), builder->getInt64Ty(), builder->getPtrTy()},
      true);
  llvm::FunctionCallee snprintfFn = theModule->getOrInsertFunction("snprintf", snprintfTy);
  builder->CreateCall(snprintfFn, {buf, builder->getInt64(79), fmt, dbl});
  return buf;
}

auto Codegen::emitBoxedStrLiteralText(llvm::SMRange span, const std::string& text,
                                      lesma::Type* strClass) -> std::unique_ptr<lesma::Value> {
  if (strClass == nullptr || !strClass->is(BaseType::TY_CLASS)) {
    throw CodegenError(span, "Invalid str class for string interpolation chunk");
  }
  auto fields = strClass->getFields();
  if (fields.empty() || fields.front()->type == nullptr ||
      !fields.front()->type->is(BaseType::TY_STRING)) {
    throw CodegenError(span, "String interpolation chunk: invalid stdlib str class");
  }
  auto* structType = llvm::cast<llvm::StructType>(getOrCreateLlvmType(strClass));
  auto* classSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* classHandle = emitMalloc(classSize, "ip.str.obj");
  emitInitClassVtablePointer(strClass, classHandle);
  unsigned const strFieldIdx = TypeUtils::classDataFieldStructIndex(strClass, 0U);
  auto* storagePtr =
      builder->CreateStructGEP(structType, classHandle, strFieldIdx, "ip.str.storage.ptr");
  llvm::Value* globalStr = builder->CreateGlobalString(text);
  builder->CreateStore(globalStr, storagePtr);
  return std::make_unique<Value>("", strClass, classHandle);
}

auto Codegen::emitBoxedStrWithCstrField(llvm::SMRange span, llvm::Value* nulTerminatedPtr,
                                        lesma::Type* strClass) -> std::unique_ptr<lesma::Value> {
  if (strClass == nullptr || !strClass->is(BaseType::TY_CLASS)) {
    throw CodegenError(span, "Invalid str class for string interpolation");
  }
  auto fields = strClass->getFields();
  if (fields.empty() || fields.front()->type == nullptr ||
      !fields.front()->type->is(BaseType::TY_STRING)) {
    throw CodegenError(span, "String interpolation: invalid stdlib str class");
  }
  auto* structType = llvm::cast<llvm::StructType>(getOrCreateLlvmType(strClass));
  auto* classSize =
      builder->getInt64(theModule->getDataLayout().getTypeAllocSize(structType).getFixedValue());
  auto* classHandle = emitMalloc(classSize, "ip.str.dyn");
  emitInitClassVtablePointer(strClass, classHandle);
  unsigned const strFieldIdx2 = TypeUtils::classDataFieldStructIndex(strClass, 0U);
  auto* storagePtr =
      builder->CreateStructGEP(structType, classHandle, strFieldIdx2, "ip.str.dyn.ptr");
  builder->CreateStore(nulTerminatedPtr, storagePtr);
  return std::make_unique<Value>("", strClass, classHandle);
}

auto Codegen::emitInterpolationExprToCstr(llvm::SMRange span, const Expression* expr,
                                          lesma::Type* exprTy) -> llvm::Value* {
  expr->accept(*this);
  std::unique_ptr<lesma::Value> v = std::move(result);
  setDebugLoc(span);
  if (isStrBoxLesmaType(exprTy)) {
    std::unique_ptr<lesma::Value> cstrV = callMethodByName(span, v.get(), "cstr", {});
    return cstrV->getLlvmValue();
  }
  if (exprTy->is(BaseType::TY_STRING)) {
    return v->getLlvmValue();
  }
  if (exprTy->is(BaseType::TY_INT)) {
    return emitFormatIntegerToCstr(span, v->getLlvmValue(), exprTy);
  }
  if (exprTy->is(BaseType::TY_FLOAT) || exprTy->is(BaseType::TY_FLOAT32)) {
    return emitFormatFloatToCstr(span, v->getLlvmValue(), exprTy);
  }
  if (exprTy->is(BaseType::TY_BOOL)) {
    llvm::Value* trueStr = builder->CreateGlobalString("true");
    llvm::Value* falseStr = builder->CreateGlobalString("false");
    return builder->CreateSelect(v->getLlvmValue(), trueStr, falseStr);
  }
  throw CodegenError(span, "Unsupported string interpolation type");
}

auto Codegen::emitInterpolationExprToBoxedStr(llvm::SMRange span, const Expression* expr,
                                              lesma::Type* exprTy, lesma::Type* strClass)
    -> std::unique_ptr<lesma::Value> {
  if (isStrBoxLesmaType(exprTy)) {
    expr->accept(*this);
    setDebugLoc(span);
    return std::move(result);
  }
  llvm::Value* c = emitInterpolationExprToCstr(span, expr, exprTy);
  return emitBoxedStrWithCstrField(span, c, strClass);
}

auto Codegen::visit(const StringInterpolation* node) -> void {
  setDebugLoc(node->getSpan());
  std::vector<std::string> const& chunks = node->getChunks();
  std::vector<Expression*> exprs = node->getExprs();
  std::vector<Type*> const& exprTys = node->getInterpolatedExprTypes();
  if (exprTys.size() != exprs.size()) {
    throw CodegenError(node->getSpan(), "String interpolation not typechecked");
  }
  lesma::Type* strClass = node->getResolvedStrClassType();

  if (strClass != nullptr && strClass->is(BaseType::TY_CLASS)) {
    std::unique_ptr<Value> acc = emitBoxedStrLiteralText(node->getSpan(), chunks[0], strClass);
    std::string const opPlus = std::string{*OperatorUtils::getBinaryOperatorName(TokenType::PLUS)};
    for (size_t i = 0; i < exprs.size(); ++i) {
      std::unique_ptr<Value> rhs =
          emitInterpolationExprToBoxedStr(node->getSpan(), exprs[i], exprTys[i], strClass);
      setDebugLoc(node->getSpan());
      acc = callMethodByName(node->getSpan(), acc.get(), opPlus, {rhs.get()});
      std::unique_ptr<Value> tail =
          emitBoxedStrLiteralText(node->getSpan(), chunks[i + 1], strClass);
      setDebugLoc(node->getSpan());
      acc = callMethodByName(node->getSpan(), acc.get(), opPlus, {tail.get()});
    }
    result = std::move(acc);
    return;
  }

  llvm::Value* acc = builder->CreateGlobalString(chunks[0]);
  for (size_t i = 0; i < exprs.size(); ++i) {
    llvm::Value* part = emitInterpolationExprToCstr(node->getSpan(), exprs[i], exprTys[i]);
    acc = emitCstrConcatValues(node->getSpan(), acc, part);
    llvm::Value* tailG = builder->CreateGlobalString(chunks[i + 1]);
    acc = emitCstrConcatValues(node->getSpan(), acc, tailG);
  }
  auto* ty = cacheType(std::make_unique<Type>(BaseType::TY_STRING, builder->getPtrTy()));
  result = std::make_unique<Value>("", ty, acc);
}

auto Codegen::visit(const Else* /*node*/) -> void {
  auto* type = cacheType(std::make_unique<Type>(BaseType::TY_BOOL, builder->getInt1Ty()));
  result = std::make_unique<Value>("", type, llvm::ConstantInt::getTrue(theModule->getContext()));
}

auto Codegen::cast(llvm::SMRange span, lesma::Value* val, lesma::Type* type)
    -> std::unique_ptr<lesma::Value> {
  if (type != nullptr && type->is(BaseType::TY_UNION) && val != nullptr &&
      val->getType() != nullptr) {
    const auto& mem = type->getUnionMembers();
    if (val->getType()->is(BaseType::TY_UNION)) {
      lesma::Type* fromU = val->getType();
      const auto& fromMembers = fromU->getUnionMembers();
      std::vector<unsigned> destIdxPerFrom;
      destIdxPerFrom.reserve(fromMembers.size());
      bool canWidenWithMatchingArms = !fromMembers.empty();
      for (Type* fm : fromMembers) {
        std::optional<unsigned> dest;
        for (unsigned j = 0; j < mem.size(); ++j) {
          if (mem[j] != nullptr && mem[j]->isEqual(fm)) {
            dest = j;
            break;
          }
        }
        if (!dest.has_value()) {
          canWidenWithMatchingArms = false;
          break;
        }
        destIdxPerFrom.push_back(*dest);
      }
      if (canWidenWithMatchingArms) {
        getOrCreateLlvmType(fromU);
        getOrCreateLlvmType(type);
        llvm::Value* agg = val->getLlvmValue();
        llvm::Function* func = builder->GetInsertBlock()->getParent();
        auto* fromSt = llvm::cast<llvm::StructType>(fromU->getLlvmType());
        llvm::AllocaInst* srcSlot = createAllocaInEntry(func, fromSt, "union.widen.src");
        builder->CreateStore(agg, srcSlot);
        auto* destSt = llvm::cast<llvm::StructType>(type->getLlvmType());
        llvm::AllocaInst* wrapSlot = createAllocaInEntry(func, destSt, "union.widen.wrap");

        llvm::BasicBlock* mergeBB =
            llvm::BasicBlock::Create(theModule->getContext(), "union.widen.merge", func);
        llvm::BasicBlock* defaultBB =
            llvm::BasicBlock::Create(theModule->getContext(), "union.widen.badtag", func);

        llvm::Value* tagVal = builder->CreateExtractValue(agg, {0U}, "union.widen.tag");
        llvm::Type* fromTagTy = getOrCreateUnionTagLlvmType(fromU);
        auto* sw =
            builder->CreateSwitch(tagVal, defaultBB, static_cast<unsigned>(fromMembers.size()));
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> phiIncomings;
        phiIncomings.reserve(fromMembers.size());

        for (unsigned i = 0; i < fromMembers.size(); ++i) {
          llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(
              theModule->getContext(), "union.widen.case." + std::to_string(i), func);
          sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(fromTagTy, i)), caseBB);
          builder->SetInsertPoint(caseBB);
          Type* memTy = fromMembers[i];
          llvm::Value* loaded = emitUnionPayloadLoadFromSlot(srcSlot, fromU, memTy);
          auto tmp = std::make_unique<lesma::Value>("", memTy, loaded);
          std::unique_ptr<lesma::Value> wrapped =
              emitUnionWrapValueToSlot(span, tmp.get(), type, destIdxPerFrom[i], wrapSlot);
          llvm::Value* outAgg = wrapped->getLlvmValue();
          builder->CreateBr(mergeBB);
          phiIncomings.emplace_back(outAgg, caseBB);
        }

        builder->SetInsertPoint(defaultBB);
        builder->CreateUnreachable();

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(
            type->getLlvmType(), static_cast<unsigned>(phiIncomings.size()), "union.widen.out");
        for (auto const& pr : phiIncomings) {
          phi->addIncoming(pr.first, pr.second);
        }
        return std::make_unique<lesma::Value>("", type, phi);
      }
    }
    for (unsigned i = 0; i < mem.size(); ++i) {
      Type* memI = mem[i];
      if (memI == nullptr) {
        continue;
      }
      if (val->getType()->isEqual(memI)) {
        return emitUnionWrapValue(span, val, type, i);
      }
      if (memI->is(BaseType::TY_CLASS) && val->getType() != nullptr &&
          val->getType()->is(BaseType::TY_PTR) && val->getType()->getElementType() != nullptr &&
          val->getType()->getElementType()->isEqual(memI)) {
        // Class union arms use pointer ABI; *T rvalues already carry the same handle the union
        // stores for T (no LLVM load—val is the handle, not a slot address).
        getOrCreateLlvmType(memI);
        auto tmp = std::make_unique<lesma::Value>("", memI, val->getLlvmValue());
        return emitUnionWrapValue(span, tmp.get(), type, i);
      }
    }
  }
  return CodegenTypeUtils::cast(span, val, type, builder.get());
}

auto Codegen::symbolUsesDirectLlvmValue(const lesma::Value* symbol) const -> bool {
  return symbol != nullptr && symbol->usesDirectLlvmValue();
}

auto Codegen::materializeSymbolValue(lesma::Value* symbol) -> std::unique_ptr<lesma::Value> {
  if (symbol == nullptr) {
    return nullptr;
  }

  if (symbol->getStoresFuncValuePair()) {
    return std::make_unique<Value>(*symbol);
  }

  Type* lesmaTy = symbol->getType();
  if (lesmaTy != nullptr && !currentGenericTypes.empty() && typeContainsUnboundGeneric(lesmaTy)) {
    lesmaTy = substituteTypeForSpecializationEnv(lesmaTy, currentGenericTypes);
  }
  if (lesmaTy == nullptr) {
    throw CodegenError({}, "Symbol {} has no type for materialization", symbol->getName());
  }
  getOrCreateLlvmType(lesmaTy);
  if (lesmaTy->is(BaseType::TY_UNION)) {
    if (std::optional<unsigned> const idx = lookupUnionNarrowVariant(symbol)) {
      Type* memTy = lesmaTy->getUnionMembers().at(*idx);
      getOrCreateLlvmType(memTy);
      llvm::Value* v = emitUnionPayloadLoadFromSlot(symbol->getLlvmValue(), lesmaTy, memTy);
      return std::make_unique<Value>("", memTy, v);
    }
  }
  if (symbolUsesDirectLlvmValue(symbol)) {
    auto out = std::make_unique<Value>(*symbol);
    out->setType(lesmaTy);
    return out;
  }

  llvm::Type* llvmTy = lesmaTy->getLlvmType();
  if (llvmTy == nullptr) {
    throw CodegenError({}, "No LLVM type for symbol {} after specialization", symbol->getName());
  }
  llvm::Value* llvmVal = builder->CreateLoad(llvmTy, symbol->getLlvmValue());
  return std::make_unique<Value>("", lesmaTy, llvmVal);
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
      (!targetType->isFloatingPoint() && !targetType->is(BaseType::TY_INT))) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(compoundOp));
  }
  const bool isFloat = targetType->isFloatingPoint();
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
  if (finalType != nullptr && (finalType->is(BaseType::TY_INT) || finalType->isFloatingPoint())) {
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
      (!targetType->isFloatingPoint() && !targetType->is(BaseType::TY_INT))) {
    throw CodegenError(span, "Invalid operator: {}", NAMEOF_ENUM(op));
  }
  auto* varVal = builder->CreateLoad(targetType->getLlvmType(), lhs->getLlvmValue());
  auto loaded = std::make_unique<Value>("", targetType, varVal);
  auto newVal = emitCompoundAssignArithmetic(span, op, loaded.get(), value);
  builder->CreateStore(newVal->getLlvmValue(), lhs->getLlvmValue());
}

auto Codegen::appendCallableArgument(lesma::Value* arg, std::vector<lesma::Type*>& paramTypes,
                                     std::vector<llvm::Value*>& paramsLLVM) -> void {
  lesma::Type* const originalLesmaType = arg->getType();
  lesma::Type* argType = originalLesmaType;
  llvm::Value* llvmArg = arg->getLlvmValue();
  if (originalLesmaType != nullptr && originalLesmaType->is(BaseType::TY_CLASS)) {
    lesma::Type* ptrType = nullptr;
    for (auto& t : typeCache) {
      if (t->is(BaseType::TY_PTR) && t->getElementType() == originalLesmaType) {
        ptrType = t.get();
        break;
      }
    }
    if (ptrType != nullptr) {
      argType = ptrType;
    } else {
      argType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), originalLesmaType));
    }
    if (llvmArg != nullptr && !llvmArg->getType()->isPointerTy()) {
      llvm::Type* st = getOrCreateLlvmType(originalLesmaType);
      llvm::Value* slot = builder->CreateAlloca(st);
      builder->CreateStore(llvmArg, slot);
      llvmArg = slot;
    }
  }
  paramTypes.push_back(argType);
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

auto Codegen::evaluateCallArgValues(const FuncCall* call,
                                    std::vector<std::unique_ptr<lesma::Value>>& storage,
                                    std::vector<lesma::Value*>& argsOut) -> void {
  storage.clear();
  argsOut.clear();
  if (call == nullptr) {
    return;
  }
  std::vector<Expression*> const operands = call->getArguments();
  storage.reserve(operands.size());
  for (Expression* arg : operands) {
    arg->accept(*this);
    storage.push_back(std::move(result));
    argsOut.push_back(storage.back().get());
  }
}

auto Codegen::evaluateCallExplicitTypeArgs(const FuncCall* call,
                                           std::vector<lesma::Type*>& typesOut) -> void {
  typesOut.clear();
  if (call == nullptr) {
    return;
  }
  for (TypeExpr* ta : call->getExplicitTypeArgs()) {
    ta->accept(*this);
    typesOut.push_back(result->getType());
  }
}

auto Codegen::callNamedFunction(
    llvm::SMRange span, const std::string& functionName,
    const std::vector<lesma::Type*>& paramTypes, const std::vector<llvm::Value*>& paramsLLVM,
    const std::vector<lesma::Type*>& explicitTypeArgs, Value* typecheckCalleeFallback,
    Type* allocatedClassMonomorph,
    const std::vector<std::pair<std::string, lesma::Type*>>* genericBindingHint)
    -> std::unique_ptr<lesma::Value> {
  std::vector<lesma::Type*> localParamTypes = paramTypes;
  std::vector<llvm::Value*> localParamsLLVM = paramsLLVM;
  std::unordered_map<std::string, lesma::Type*> callSiteGenericFallbackStorage;
  if (genericBindingHint != nullptr) {
    for (const auto& kv : *genericBindingHint) {
      if (kv.second != nullptr) {
        callSiteGenericFallbackStorage[kv.first] = kv.second;
      }
    }
  }
  struct ScopedCallSiteGenericFallback {
    Codegen* cg;
    bool const on;
    ScopedCallSiteGenericFallback(Codegen* code, std::unordered_map<std::string, lesma::Type*>* map)
        : cg(code), on(map != nullptr && !map->empty()) {
      if (on) {
        code->pushGenericTypeFallback(map);
      }
    }
    ~ScopedCallSiteGenericFallback() {
      if (on) {
        cg->popGenericTypeFallback();
      }
    }
    ScopedCallSiteGenericFallback(const ScopedCallSiteGenericFallback&) = delete;
    auto operator=(const ScopedCallSiteGenericFallback&) -> ScopedCallSiteGenericFallback& = delete;
    ScopedCallSiteGenericFallback(ScopedCallSiteGenericFallback&&) = delete;
    auto operator=(ScopedCallSiteGenericFallback&&) -> ScopedCallSiteGenericFallback& = delete;
  } scopedCallSiteFallback(this, &callSiteGenericFallbackStorage);

  auto hintedGenericBindings = [&]() -> std::unordered_map<std::string, lesma::Type*> {
    std::unordered_map<std::string, lesma::Type*> env;
    if (genericBindingHint != nullptr) {
      for (const auto& [name, ty] : *genericBindingHint) {
        env[name] = substituteTypeForSpecializationEnv(ty, currentGenericTypes);
      }
    }
    return env;
  };
  // No `self` at the call site: use the hint for free/generic calls instead of inferring only from
  // parameter types (mirrors `buildFunctionSpecializationEnv` when there is no class receiver).
  auto freeCallGenericEnvFromHint = [&]() -> std::unordered_map<std::string, lesma::Type*> {
    if (selfSymbol != nullptr || genericBindingHint == nullptr || genericBindingHint->empty()) {
      return {};
    }
    return hintedGenericBindings();
  };
  // Monomorphize generic class template when typechecker fixed class parameters (e.g. Cell.of(7)).
  if (selfSymbol != nullptr && functionName != "new" && genericBindingHint != nullptr &&
      !genericBindingHint->empty()) {
    lesma::Type* recvTy = selfSymbol->getType();
    if (recvTy != nullptr && recvTy->is(BaseType::TY_CLASS) &&
        !recvTy->getGenericParams().empty()) {
      if (auto git = genericClasses.find(selfSymbol->getName()); git != genericClasses.end()) {
        std::unordered_map<std::string, lesma::Type*> envFromHint = hintedGenericBindings();
        bool complete = true;
        for (const auto& gn : git->second->getGenericParams()) {
          auto it = envFromHint.find(gn);
          if (it == envFromHint.end() || it->second == nullptr) {
            complete = false;
            break;
          }
        }
        if (complete) {
          selfSymbol = specializeClass(git->second, {}, {}, &envFromHint);
        }
      }
    }
  }
  for (auto* explicitTypeArg : explicitTypeArgs) {
    if (explicitTypeArg != nullptr) {
      getOrCreateLlvmType(explicitTypeArg);
    }
  }
  for (auto* pt : localParamTypes) {
    if (pt != nullptr) {
      getOrCreateLlvmType(pt);
    }
  }
  const FuncDecl* genericFuncTemplateForLookup = nullptr;
  if (selfSymbol != nullptr) {
    if (auto cit = genericMethods.find(selfSymbol->getName()); cit != genericMethods.end()) {
      if (auto mit = cit->second.find(functionName); mit != cit->second.end()) {
        genericFuncTemplateForLookup = mit->second;
      }
    }
  } else {
    if (auto git = genericFunctions.find(functionName); git != genericFunctions.end()) {
      genericFuncTemplateForLookup = git->second;
    }
  }
  auto appendGenericBindingsForTemplate = [&](std::string& out) -> void {
    if (genericFuncTemplateForLookup == nullptr) {
      return;
    }
    const auto& genericNames = genericFuncTemplateForLookup->getGenericParams();
    if (genericNames.empty()) {
      return;
    }
    std::unordered_map<std::string, lesma::Type*> env = freeCallGenericEnvFromHint();
    if (env.empty()) {
      env = computeGenericFunctionBindingEnv(genericFuncTemplateForLookup, localParamTypes,
                                             genericNames, explicitTypeArgs);
    }
    appendGenericBindingSuffix(span, out, genericNames, env);
  };
  auto buildFunctionSpecializationEnv = [&](const FuncDecl* templateDecl,
                                            const std::vector<std::string>& genericNames)
      -> std::unordered_map<std::string, lesma::Type*> {
    std::unordered_map<std::string, lesma::Type*> env;
    if (selfSymbol != nullptr && selfSymbol->getType() != nullptr) {
      lesma::Type* selfTy = selfSymbol->getType();
      if (selfTy->is(BaseType::TY_PTR) && selfTy->getElementType() != nullptr) {
        selfTy = selfTy->getElementType();
      }
      if (const auto* classEnv = specializedClassEnvFor(selfTy); classEnv != nullptr) {
        env = *classEnv;
      }
    } else if (genericBindingHint != nullptr) {
      env = hintedGenericBindings();
    }
    auto inferred = computeGenericFunctionBindingEnv(templateDecl, localParamTypes, genericNames,
                                                     explicitTypeArgs);
    env.insert(inferred.begin(), inferred.end());
    for (const auto& [name, ty] : inferred) {
      env[name] = ty;
    }
    return env;
  };
  Value* symbol = nullptr;
  auto* selfSymbolTmp = selfSymbol;
  auto* classSym = scope->lookupStruct(functionName);
  llvm::Value* classPtr = nullptr;

  if (classSym == nullptr || (classSym->getType()->is(BaseType::TY_CLASS) &&
                              classSym->getType()->getLlvmType() == nullptr)) {
    const Class* templateClass = nullptr;
    if (auto git = genericClasses.find(functionName); git != genericClasses.end()) {
      templateClass = git->second;
    } else if (classSym != nullptr && classSym->getGenericClassTemplate() != nullptr) {
      templateClass = static_cast<const Class*>(classSym->getGenericClassTemplate());
    }
    if (templateClass != nullptr) {
      auto monomorphEnvIsConcrete = [this](Type* monomorph) -> bool {
        if (monomorph == nullptr) {
          return false;
        }
        auto it = specializedClassTypeEnvs.find(monomorph);
        if (it == specializedClassTypeEnvs.end()) {
          return false;
        }
        return std::ranges::all_of(it->second,
                                   [](const std::pair<const std::string, lesma::Type*>& e) {
                                     lesma::Type* ty = e.second;
                                     return ty == nullptr || !ty->is(BaseType::TY_GENERIC);
                                   });
      };
      if (allocatedClassMonomorph != nullptr && monomorphEnvIsConcrete(allocatedClassMonomorph)) {
        classSym = emitClassMonomorph(allocatedClassMonomorph, templateClass);
      } else {
        classSym = specializeClass(templateClass, localParamTypes, explicitTypeArgs);
      }
    }
  }

  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    auto* classLlvmType = getOrCreateLlvmType(classSym->getType());
    auto* classSize = builder->getInt64(
        theModule->getDataLayout().getTypeAllocSize(classLlvmType).getFixedValue());
    classPtr = emitMalloc(classSize, functionName + ".obj");
    emitInitClassVtablePointer(classSym->getType(), classPtr);
    localParamsLLVM.insert(localParamsLLVM.begin(), classPtr);
    lesma::Type* selfArgTy = cacheType(
        std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), classSym->getType()));
    localParamTypes.insert(localParamTypes.begin(), selfArgTy);

    selfSymbol = classSym;
    symbol = scope->lookupFunction("new", localParamTypes);
    if (symbol == nullptr && rootScope != nullptr) {
      // Method/template body scopes may not chain to the table where monomorph constructors live.
      symbol = rootScope->lookupFunction("new", localParamTypes);
    }
    if (symbol == nullptr) {
      symbol = classSym->getConstructor();
    }
  } else {
    auto directSignatureKey = makeCallableSignatureKey(functionName, localParamTypes);
    std::string directMangledLookup =
        getMangledName(span, functionName, localParamTypes, selfSymbol != nullptr);
    appendGenericBindingsForTemplate(directSignatureKey);
    appendGenericBindingsForTemplate(directMangledLookup);
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
      if (auto directIt = specializedFunctions.find(directMangledLookup);
          directIt != specializedFunctions.end()) {
        symbol = directIt->second;
      } else {
        symbol = scope->lookupFunction(functionName, localParamTypes);
      }
    }
  }

  if (symbol == nullptr && typecheckCalleeFallback != nullptr &&
      typecheckCalleeFallback->getType() != nullptr &&
      typecheckCalleeFallback->getType()->is(BaseType::TY_FUNCTION) &&
      typecheckCalleeFallback->getName() == functionName &&
      typecheckCalleeFallback->getType()->getGenericParams().empty()) {
    symbol = typecheckCalleeFallback;
  }

  if (symbol == nullptr && genericFuncTemplateForLookup != nullptr) {
    std::vector<std::string> genericNames = genericFuncTemplateForLookup->getGenericParams();
    auto bindingEnv = buildFunctionSpecializationEnv(genericFuncTemplateForLookup, genericNames);
    symbol = specializeFunction(genericFuncTemplateForLookup, localParamTypes, genericNames,
                                explicitTypeArgs, bindingEnv.empty() ? nullptr : &bindingEnv);
  }

  if (symbol == nullptr) {
    throw CodegenError(span, "{} {} not in current scope.",
                       classSym != nullptr ? "Constructor for" : "Function", functionName);
  }
  if (symbol->getLlvmValue() == nullptr) {
    std::string moduleLookupName =
        getMangledName(span, functionName, localParamTypes, selfSymbol != nullptr);
    appendGenericBindingsForTemplate(moduleLookupName);
    if (auto* directFunction = theModule->getFunction(moduleLookupName);
        directFunction != nullptr) {
      symbol->setLlvmValue(directFunction);
    }
  }

  const bool genericLambdaNeedsSpecialize =
      symbol->getOriginLambdaExpr() != nullptr && !symbol->getType()->getGenericParams().empty();
  if (symbol->getType()->getFields().size() == localParamTypes.size() &&
      (symbol->getLlvmValue() == nullptr || genericLambdaNeedsSpecialize)) {
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
      auto bindingEnv = buildFunctionSpecializationEnv(templateDecl, genericNames);
      symbol = specializeFunction(templateDecl, localParamTypes, genericNames, explicitTypeArgs,
                                  bindingEnv.empty() ? nullptr : &bindingEnv);
    } else if (symbol->getOriginLambdaExpr() != nullptr) {
      const LambdaExpr* lamNode = symbol->getOriginLambdaExpr();
      Value* lamSym = lamNode->getResolvedSymbol();
      if (lamSym != nullptr && lamSym->getType() != nullptr &&
          !lamSym->getType()->getGenericParams().empty()) {
        std::vector<std::string> genericNames = lamSym->getType()->getGenericParams();
        std::unordered_map<std::string, lesma::Type*> bindingEnv = freeCallGenericEnvFromHint();
        symbol = specializeLambda(lamNode, localParamTypes, genericNames, explicitTypeArgs,
                                  bindingEnv.empty() ? nullptr : &bindingEnv);
      }
    }
  }

  if (symbol != nullptr && symbol->getType() != nullptr &&
      symbol->getType()->is(BaseType::TY_FUNCTION)) {
    auto ff = symbol->getType()->getFields();
    for (size_t i = 0; i < localParamsLLVM.size() && i < ff.size(); ++i) {
      lesma::Type* formal = ff[i]->type;
      if (formal != nullptr && formal->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
        lesma::Type* actual = localParamTypes[i];
        if (actual->is(BaseType::TY_PTR) && actual->getElementType() != nullptr &&
            actual->getElementType()->is(BaseType::TY_CLASS)) {
          getOrCreateLlvmType(formal);
          localParamsLLVM[i] = emitBoxClassToExistential(formal, actual, localParamsLLVM[i]);
          localParamTypes[i] = formal;
        }
      }
    }
  }

  Type* callableLesmaType = symbol->getType();
  if (callableLesmaType != nullptr) {
    if (genericBindingHint != nullptr && !genericBindingHint->empty()) {
      // Call-site env from typecheck (e.g. `T = int` for `Cell.of(7)`); apply even when the stored
      // callee type does not trip `typeContainsUnboundGeneric`.
      callableLesmaType =
          substituteTypeForSpecializationEnv(callableLesmaType, hintedGenericBindings());
    } else if (!currentGenericTypes.empty() && typeContainsUnboundGeneric(callableLesmaType)) {
      callableLesmaType =
          substituteTypeForSpecializationEnv(callableLesmaType, currentGenericTypes);
    }
  }

  if (callableLesmaType == nullptr ||
      !callableLesmaType->isOneOf({BaseType::TY_CLASS, BaseType::TY_FUNCTION})) {
    throw CodegenError(span, "Symbol {} is not a function or constructor.", functionName);
  }

  if (callableLesmaType->getFields().size() > localParamsLLVM.size()) {
    auto fields = callableLesmaType->getFields();
    for (size_t i = localParamsLLVM.size(); i < fields.size(); ++i) {
      auto* field = fields[i];
      if (field->defaultValue == nullptr) {
        throw CodegenError(
            span, "Something bad happened, lookup found a function with incorrect defaults",
            functionName);
      }
      localParamsLLVM.push_back(field->defaultValue->getLlvmValue());
      localParamTypes.push_back(field->type);
    }
  }

  std::vector<llvm::Value*> callParamsLLVM = localParamsLLVM;
  if (callableLesmaType->is(BaseType::TY_FUNCTION)) {
    std::vector<Field*> const fields = callableLesmaType->getFields();
    if (fields.size() == localParamsLLVM.size()) {
      callParamsLLVM.clear();
      for (size_t i = 0; i < fields.size(); ++i) {
        auto holder = std::make_unique<Value>("", localParamTypes[i], localParamsLLVM[i]);
        auto casted = cast(span, holder.get(), fields[i]->type);
        callParamsLLVM.push_back(casted->getLlvmValue());
      }
    }
  }

  llvm::Value* callableValue = nullptr;
  llvm::Value* loadedClosureEnv = nullptr;
  if (callableLesmaType->is(BaseType::TY_CLASS)) {
    if (symbol->getConstructor() == nullptr ||
        symbol->getConstructor()->getLlvmValue() == nullptr) {
      throw CodegenError(span, "Constructor for {} is declared but not defined", functionName);
    }
    callableValue = symbol->getConstructor()->getLlvmValue();
  } else {
    if (symbol->getStoresFuncValuePair()) {
      if (symbol->getLlvmValue() == nullptr) {
        throw CodegenError(span, "Function value {} is not initialized", functionName);
      }
      llvm::StructType* pairTy = getFuncValuePairLlvmType();
      llvm::Value* pairPtr = symbol->getLlvmValue();
      llvm::Value* fnSlot = builder->CreateStructGEP(pairTy, pairPtr, 0U);
      llvm::Value* envSlot = builder->CreateStructGEP(pairTy, pairPtr, 1U);
      callableValue = builder->CreateLoad(builder->getPtrTy(), fnSlot, functionName + ".code");
      loadedClosureEnv = builder->CreateLoad(builder->getPtrTy(), envSlot, functionName + ".env");
    } else {
      if (symbol->getLlvmValue() == nullptr) {
        throw CodegenError(span, "Function {} is declared but not defined", functionName);
      }
      callableValue = symbol->getLlvmValue();
    }
  }
  llvm::Value* const envArgForCall =
      (symbol->getStoresFuncValuePair() && symbol->getClosureCalleeUsesEnvParameter())
          ? loadedClosureEnv
          : nullptr;

  auto emitIndirectCall = [&](llvm::Value* calleePtr, llvm::Value* envArg) -> llvm::CallInst* {
    std::vector<llvm::Type*> callParamTypes;
    std::vector<llvm::Value*> callArgs;
    if (envArg != nullptr) {
      callParamTypes.push_back(builder->getPtrTy());
      callArgs.push_back(envArg);
    }
    callParamTypes.reserve(callParamTypes.size() + callableLesmaType->getFields().size());
    callArgs.reserve(callArgs.size() + callParamsLLVM.size());
    for (Field* pf : callableLesmaType->getFields()) {
      if (pf->type == nullptr) {
        continue;
      }
      getOrCreateLlvmType(pf->type);
      llvm::Type* plt = pf->type->getLlvmType();
      if (pf->type->is(BaseType::TY_CLASS)) {
        plt = builder->getPtrTy();
      } else if (pf->type->is(BaseType::TY_FUNCTION)) {
        plt = getFuncValuePairLlvmType();
      } else if (pf->type->is(BaseType::TY_PTR)) {
        plt = builder->getPtrTy();
      }
      callParamTypes.push_back(plt);
    }
    callArgs.insert(callArgs.end(), callParamsLLVM.begin(), callParamsLLVM.end());
    Type* retType = callableLesmaType->getReturnType();
    if (retType == nullptr) {
      retType = cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy()));
      symbol->getType()->setReturnType(retType);
    }
    getOrCreateLlvmType(retType);
    llvm::Type* llvmRetType = nullptr;
    if (retType->is(BaseType::TY_FUNCTION)) {
      llvmRetType = getFuncValuePairLlvmType();
    } else if (retType->is(BaseType::TY_CLASS) || retType->is(BaseType::TY_PTR)) {
      llvmRetType = builder->getPtrTy();
    } else {
      llvmRetType = retType->getLlvmType();
    }
    llvm::FunctionType* callTy = llvm::FunctionType::get(llvmRetType, callParamTypes, false);
    return builder->CreateCall(callTy, calleePtr, callArgs);
  };

  llvm::CallInst* callInst = nullptr;
  if (symbol->getStoresFuncValuePair()) {
    llvm::Value* calleePtr = callableValue;
    callInst = emitIndirectCall(calleePtr, envArgForCall);
  } else if (auto* func = llvm::dyn_cast<Function>(callableValue)) {
    if (envArgForCall != nullptr) {
      std::vector<llvm::Value*> withEnv;
      withEnv.push_back(envArgForCall);
      withEnv.insert(withEnv.end(), callParamsLLVM.begin(), callParamsLLVM.end());
      callInst = builder->CreateCall(func, withEnv);
    } else {
      callInst = builder->CreateCall(func, callParamsLLVM);
    }
  } else {
    llvm::Value* calleePtr = callableValue;
    if (symbol->getCategory() == ValueCategory::ADDRESSABLE_STORAGE) {
      calleePtr = builder->CreateLoad(builder->getPtrTy(), callableValue, functionName + ".fn");
    }
    callInst = emitIndirectCall(calleePtr, nullptr);
  }

  if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
    selfSymbol = selfSymbolTmp;
    return std::make_unique<Value>("", classSym->getType(), classPtr);
  }

  selfSymbol = selfSymbolTmp;
  Type* retLesma = callableLesmaType->getReturnType();
  auto callResult = std::make_unique<Value>("", retLesma, callInst);
  if (retLesma != nullptr && retLesma->is(BaseType::TY_FUNCTION)) {
    callResult->setStoresFuncValuePair(true);
    callResult->setCategory(ValueCategory::DIRECT_VALUE);
  }
  return callResult;
}

auto Codegen::isBuiltinListBuiltinMethodName(const std::string& methodName) const -> bool {
  return OperatorUtils::isBuiltinListMethodName(methodName);
}

auto Codegen::classHasSingleBufferStorageField(lesma::Type* classTy) const -> bool {
  if (classTy == nullptr || !classTy->is(BaseType::TY_CLASS)) {
    return false;
  }
  auto fields = classTy->getFields();
  if (fields.size() != 1U) {
    return false;
  }
  return fields.front()->type != nullptr && fields.front()->type->is(BaseType::TY_ARRAY);
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
      if (!classHasSingleBufferStorageField(receiverType)) {
        throw CodegenError(span, "Function {} not in current scope.", methodName);
      }
      auto fields = receiverType->getFields();
      if (!fields.empty() && fields.front()->type != nullptr &&
          fields.front()->type->is(BaseType::TY_ARRAY)) {
        listClassType = receiverType;
        bufferType = fields.front()->type;
        unsigned const bufIdx = TypeUtils::classDataFieldStructIndex(listClassType, 0U);
        auto* storagePtr =
            builder->CreateStructGEP(cast<llvm::StructType>(getOrCreateLlvmType(listClassType)),
                                     receiver->getLlvmValue(), bufIdx, "list.storage.ptr");
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
    builder->CreateStore(getListStoredElementValue(span, args[0], bufferType->getElementType()),
                         elementPtr);
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
    auto* classHandle = emitMalloc(classSize, "list.copy.obj");
    emitInitClassVtablePointer(listClassType, classHandle);
    unsigned const copyBufIdx = TypeUtils::classDataFieldStructIndex(listClassType, 0U);
    auto* storagePtr = builder->CreateStructGEP(cast<llvm::StructType>(classLlvmType), classHandle,
                                                copyBufIdx, "list.copy.storage.ptr");
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
    builder->CreateStore(getListStoredElementValue(span, args[1], bufferType->getElementType()),
                         elementPtr);
    return std::make_unique<Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }

  throw CodegenError(span, "Function {} not in current scope.", methodName);
}

auto Codegen::emitUnionClassMethodDispatch(llvm::SMRange span, lesma::Value* unionValue,
                                           const std::string& methodName,
                                           const std::vector<lesma::Value*>& args,
                                           const std::vector<lesma::Type*>& explicitTypeArgs)
    -> std::unique_ptr<lesma::Value> {
  if (!explicitTypeArgs.empty()) {
    throw CodegenError(span, "Explicit type arguments are not supported on union method calls");
  }
  lesma::Type* unionTy = unionValue->getType();
  if (unionTy == nullptr || !unionTy->is(BaseType::TY_UNION)) {
    throw CodegenError(span, "Internal error: union method dispatch without union type");
  }
  const std::vector<Type*>& members = unionTy->getUnionMembers();
  if (members.empty()) {
    throw CodegenError(span, "Internal error: empty union in method dispatch");
  }
  Type* mem0 = members[0];
  lesma::Type* selfPtr0 =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), mem0));
  std::vector<lesma::Type*> lookupParams;
  lookupParams.push_back(selfPtr0);
  for (lesma::Value* arg : args) {
    Type* t = arg->getType();
    if (t != nullptr && t->is(BaseType::TY_CLASS)) {
      t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), t));
    }
    lookupParams.push_back(t);
  }
  lesma::Value* probeMeth = scope->lookupFunction(methodName, lookupParams);
  if (probeMeth == nullptr || probeMeth->getType() == nullptr ||
      !probeMeth->getType()->is(BaseType::TY_FUNCTION)) {
    throw CodegenError(span, "Internal error: could not resolve union method '{}'", methodName);
  }
  lesma::Type* commonRetTy = probeMeth->getType()->getReturnType();

  llvm::Function* parent = builder->GetInsertBlock()->getParent();
  llvm::Type* st = getOrCreateLlvmType(unionTy);
  auto* structTy = llvm::cast<llvm::StructType>(st);
  llvm::Value* uv = unionValue->getLlvmValue();
  llvm::Value* unionPtr = nullptr;
  if (uv->getType()->isPointerTy()) {
    unionPtr = uv;
  } else {
    llvm::AllocaInst* tmpSlot = createAllocaInEntry(parent, structTy, "union.class.dispatch.slot");
    builder->CreateStore(uv, tmpSlot);
    unionPtr = tmpSlot;
  }

  llvm::Value* tagPtr = builder->CreateStructGEP(structTy, unionPtr, 0U, "union.dispatch.tag.ptr");
  llvm::Type* tagTy = getOrCreateUnionTagLlvmType(unionTy);
  llvm::Value* tagVal = builder->CreateLoad(tagTy, tagPtr, "union.dispatch.tag");

  llvm::BasicBlock* mergeBB =
      llvm::BasicBlock::Create(theModule->getContext(), "union.m.merge", parent);
  llvm::BasicBlock* defBB =
      llvm::BasicBlock::Create(theModule->getContext(), "union.m.bad", parent);

  llvm::SwitchInst* sw =
      builder->CreateSwitch(tagVal, defBB, static_cast<unsigned>(members.size()));
  builder->SetInsertPoint(defBB);
  builder->CreateUnreachable();

  std::vector<llvm::Value*> phiVals;
  std::vector<llvm::BasicBlock*> phiBBs;
  for (unsigned i = 0; i < members.size(); ++i) {
    llvm::BasicBlock* caseBB =
        llvm::BasicBlock::Create(theModule->getContext(), "union.m.case", parent);
    sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(tagTy, i)), caseBB);
    builder->SetInsertPoint(caseBB);
    llvm::Value* payloadVal = emitUnionPayloadLoadFromSlot(unionPtr, unionTy, members[i]);
    auto recv = std::make_unique<lesma::Value>("", members[i], payloadVal);
    std::unique_ptr<lesma::Value> out =
        callMethodByName(span, recv.get(), methodName, args, explicitTypeArgs);
    llvm::BasicBlock* currentBB = builder->GetInsertBlock();
    if (currentBB == nullptr) {
      currentBB = caseBB;
    }
    builder->SetInsertPoint(currentBB);
    if (commonRetTy != nullptr && commonRetTy->is(BaseType::TY_VOID)) {
      builder->CreateBr(mergeBB);
    } else {
      llvm::Value* rv = out->getLlvmValue();
      builder->CreateBr(mergeBB);
      phiVals.push_back(rv);
      phiBBs.push_back(currentBB);
    }
  }

  builder->SetInsertPoint(mergeBB);
  if (commonRetTy != nullptr && commonRetTy->is(BaseType::TY_VOID)) {
    return std::make_unique<lesma::Value>(
        "", cacheType(std::make_unique<Type>(BaseType::TY_VOID, builder->getVoidTy())), nullptr);
  }
  getOrCreateLlvmType(commonRetTy);
  llvm::Type* retLt = commonRetTy->getLlvmType();
  llvm::PHINode* phi =
      builder->CreatePHI(retLt, static_cast<unsigned>(phiVals.size()), "union.m.phi");
  for (unsigned j = 0; j < phiVals.size(); ++j) {
    phi->addIncoming(phiVals[j], phiBBs[j]);
  }
  return std::make_unique<lesma::Value>("", commonRetTy, phi);
}

auto Codegen::callMethodByName(llvm::SMRange span, lesma::Value* receiver,
                               const std::string& methodName,
                               const std::vector<lesma::Value*>& args,
                               const std::vector<lesma::Type*>& explicitTypeArgs,
                               lesma::Value* resolvedCallee, const FuncCall* callSiteForGenericEnv)
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
  if (receiverType->is(BaseType::TY_CLASS) && classHasSingleBufferStorageField(receiverType) &&
      isBuiltinListBuiltinMethodName(methodName)) {
    return callListMethodByName(span, receiver, methodName, args, explicitTypeArgs);
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
  const auto* receiverClassEnv = specializedClassEnvFor(receiverType);
  const std::vector<std::pair<std::string, lesma::Type*>>* callSiteGenericBindings =
      (callSiteForGenericEnv != nullptr && !callSiteForGenericEnv->getGenericBindingEnv().empty())
          ? &callSiteForGenericEnv->getGenericBindingEnv()
          : nullptr;
  auto* savedSelfSymbol = selfSymbol;
  std::vector<lesma::Type*> paramTypes;
  std::vector<llvm::Value*> paramsLLVM;
  lesma::Value* receiverForCall = receiver;
  std::unique_ptr<lesma::Value> receiverAdapter;
  lesma::Value* directMethod = nullptr;
  if (resolvedCallee != nullptr && resolvedCallee->isStaticMethod()) {
    for (auto* arg : args) {
      appendCallableArgument(arg, paramTypes, paramsLLVM);
    }
    directMethod = resolvedCallee;
  } else {
    // Adapt imported/aliased class pointers to the canonical method-owning class type for overload
    // lookup only; the underlying LLVM pointer value is unchanged.
    receiverForCall = receiver;
    if (receiver->getType()->is(BaseType::TY_PTR) &&
        receiver->getType()->getElementType() != nullptr) {
      lesma::Type* elemTy = receiver->getType()->getElementType();
      if (Value* structSym = lookupClassStructSymbol(elemTy); structSym != nullptr &&
                                                              structSym->getType() != nullptr &&
                                                              structSym->getType() != elemTy) {
        lesma::Type* specClassTy = structSym->getType();
        lesma::Type* ptrToSpec =
            cacheType(std::make_unique<Type>(BaseType::TY_PTR, builder->getPtrTy(), specClassTy));
        receiverAdapter = std::make_unique<lesma::Value>("", ptrToSpec, receiver->getLlvmValue());
        receiverForCall = receiverAdapter.get();
      }
    }
    appendCallableArgument(receiverForCall, paramTypes, paramsLLVM);
    for (auto* arg : args) {
      appendCallableArgument(arg, paramTypes, paramsLLVM);
    }
    directMethod = scope->lookupFunction(methodName, paramTypes);
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
    if (receiverClassEnv != nullptr) {
      directMethod = nullptr;
    }
  }
  if (directMethod != nullptr && directMethod->getLlvmValue() != nullptr) {
    // Method symbols may retain template types with TY_GENERIC; call sites are not inside
    // defineFunction(), so currentGenericTypes is often empty. Prefer the env recorded for this
    // specialization, else the receiver’s class env, else the FuncCall’s binding list (static
    // generic methods), so getOrCreateLlvmType / return substitution see concrete `T`.
    auto savedGenerics = currentGenericTypes;
    if (auto genIt = specializationEnvs.find(directMethod); genIt != specializationEnvs.end()) {
      currentGenericTypes = genIt->second;
    } else if (receiverClassEnv != nullptr) {
      currentGenericTypes = *receiverClassEnv;
    } else if (callSiteGenericBindings != nullptr) {
      currentGenericTypes.clear();
      for (const auto& binding : *callSiteGenericBindings) {
        currentGenericTypes[binding.first] = binding.second;
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
      if (returnTy != nullptr && !currentGenericTypes.empty() &&
          typeContainsUnboundGeneric(returnTy)) {
        returnTy = substituteTypeForSpecializationEnv(returnTy, currentGenericTypes);
      }
      if (returnTy != nullptr) {
        getOrCreateLlvmType(returnTy);
      }
      selfSymbol = savedSelfSymbol;
      llvm::Value* callResult = nullptr;
      const auto& vtOrder = receiverType->getClassVtableMethodOrder();
      std::string const methodKey = makeResolvedCallableKey(directMethod);
      unsigned vtableSlot = ~0U;
      for (size_t i = 0; i < vtOrder.size(); ++i) {
        if (vtOrder[i] == methodKey) {
          vtableSlot = static_cast<unsigned>(i);
          break;
        }
      }
      unsigned vtableArrayLen = static_cast<unsigned>(vtOrder.size());
      const bool canUseVirtual = methodName != "new" && vtableSlot != ~0U && !vtOrder.empty() &&
                                 vtableSlot < vtableArrayLen &&
                                 (receiverType->getClassSuperclass() != nullptr ||
                                  receiverType->getClassHasDerivedClass()) &&
                                 !directMethod->isStaticMethod();
      if (canUseVirtual) {
        llvm::Type* const ptrTy = builder->getPtrTy();
        auto* st = llvm::cast<llvm::StructType>(getOrCreateLlvmType(receiverType));
        llvm::Value* recvPtr = receiverForCall->getLlvmValue();
        llvm::Value* vtablePtrAddr = builder->CreateStructGEP(st, recvPtr, 0, "vtable.ptr.ptr");
        llvm::Value* vtablePtr = builder->CreateLoad(ptrTy, vtablePtrAddr, "vtable.ptr");
        auto* arrTy = llvm::ArrayType::get(ptrTy, vtableArrayLen);
        llvm::Value* fnPtrSlot = builder->CreateGEP(
            arrTy, vtablePtr, {builder->getInt64(0), builder->getInt64(vtableSlot)}, "vt.fn.ptr");
        llvm::Value* fnPtrVal = builder->CreateLoad(ptrTy, fnPtrSlot, "vt.fn");
        auto* calleeFn = llvm::cast<llvm::Function>(directMethod->getLlvmValue());
        llvm::FunctionType* ft = calleeFn->getFunctionType();
        llvm::Value* useVirtual = builder->CreateIsNotNull(fnPtrVal, "vt.fn.has.target");
        llvm::Value* directFnPtr =
            builder->CreateBitCast(directMethod->getLlvmValue(), ptrTy, "direct.fn.ptr");
        llvm::Value* selectedFnPtr =
            builder->CreateSelect(useVirtual, fnPtrVal, directFnPtr, "dispatch.fn.ptr");
        llvm::Value* callee = builder->CreateBitCast(selectedFnPtr, calleeFn->getType());
        callResult = builder->CreateCall(llvm::FunctionCallee(ft, callee), finalParams);
      } else {
        callResult =
            builder->CreateCall(llvm::cast<Function>(directMethod->getLlvmValue()), finalParams);
      }
      currentGenericTypes = std::move(savedGenerics);
      return std::make_unique<Value>("", returnTy, callResult);
    } catch (...) {
      currentGenericTypes = std::move(savedGenerics);
      throw;
    }
  }
  selfSymbol = cls;
  auto resultValue = callNamedFunction(span, methodName, paramTypes, paramsLLVM, explicitTypeArgs,
                                       resolvedCallee, nullptr, callSiteGenericBindings);
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

  std::vector<std::unique_ptr<lesma::Value>> posArgStorage;
  std::vector<lesma::Value*> posArgs;
  evaluateCallArgValues(node, posArgStorage, posArgs);
  for (auto* arg : posArgs) {
    appendCallableArgument(arg, paramTypes, paramsLLVM);
  }

  evaluateCallExplicitTypeArgs(node, explicitTypeArgs);

  setDebugLoc(node->getSpan());
  if (isListIntrinsicName(node->getName())) {
    return genListIntrinsicCall(node, paramTypes, paramsLLVM);
  }
  return callNamedFunction(node->getSpan(), node->getName(), paramTypes, paramsLLVM,
                           explicitTypeArgs, node->getResolvedSymbol(),
                           node->getAllocatedClassMonomorph(), &node->getGenericBindingEnv());
}
