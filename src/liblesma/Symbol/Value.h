// Copyright (c) 2021-2022 ChilliBits. All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/raw_ostream.h>

#include "Type.h"

namespace lesma {
class SymbolTable;
class LambdaExpr;

enum class SymbolState : std::uint8_t { DECLARED, INITIALIZED };
enum class ValueCategory : std::uint8_t {
  DIRECT_VALUE,
  ADDRESSABLE_STORAGE,
  CALLABLE_SYMBOL,
  TYPE_SYMBOL,
  MODULE_SYMBOL,
};
enum class ValueDeclarationKind : std::uint8_t {
  UNKNOWN,
  NAMESPACE,
  CLASS,
  ENUM,
  ENUM_MEMBER,
  TYPE,
  TYPE_PARAMETER,
  FUNCTION,
  TRAIT,
  METHOD,
  PARAMETER,
  VARIABLE,
  PROPERTY,
};

/**
 * Entry of a symbol table, representing an individual symbol with all its
 * properties
 */
class Value {
public:
  // Constructors that take ownership of Type
  explicit Value(std::unique_ptr<Type> type)
      : ownedType(std::move(type)), state(SymbolState::INITIALIZED) {}

  Value(std::string name, std::unique_ptr<Type> type)
      : name(std::move(name)), mangledName(name), ownedType(std::move(type)),
        state(SymbolState::INITIALIZED) {}

  Value(std::string name, std::unique_ptr<Type> type, llvm::Value* value)
      : name(std::move(name)), mangledName(name), ownedType(std::move(type)),
        state(SymbolState::INITIALIZED), llvmValue(value) {}

  // Constructors with non-owning Type reference (existing behavior)
  explicit Value(Type* type) : type(type), state(SymbolState::INITIALIZED) {}

  Value(std::string name, Type* type)
      : name(std::move(name)), mangledName(name), type(type), state(SymbolState::INITIALIZED) {}

  Value(std::string name, Type* type, llvm::Value* value)
      : name(std::move(name)), mangledName(name), type(type), state(SymbolState::INITIALIZED),
        llvmValue(value) {}

  Value(std::string name, Type* type, SymbolState state)
      : name(std::move(name)), mangledName(name), type(type), state(state) {}

  Value(std::string name, Type* type, SymbolState state, bool mutableVar, bool signedVar)
      : name(std::move(name)), type(type), state(state), mutableVar(mutableVar),
        signedVar(signedVar) {}

  // Copy constructor - creates a shallow copy with non-owning Type reference
  Value(const Value& other)
      : name(other.name), mangledName(other.mangledName), type(other.getType()), state(other.state),
        llvmValue(other.llvmValue), category(other.category), used(other.used),
        mutableVar(other.mutableVar), signedVar(other.signedVar), exported(other.exported),
        constructor(other.constructor), genericClassTemplate(other.genericClassTemplate),
        bodyScope(other.bodyScope), declarationKind(other.declarationKind),
        declarationSpan(other.declarationSpan), declarationFilePath(other.declarationFilePath),
        privateMember(other.privateMember), memberDeclaredInClass(other.memberDeclaredInClass),
        closureCaptureOuters(other.closureCaptureOuters),
        closureSlotOuter(other.closureSlotOuter), storesFuncValuePair(other.storesFuncValuePair),
        originLambdaExpr(other.originLambdaExpr), arcOwnedValue(other.arcOwnedValue),
        closureCalleeUsesEnvParameter(other.closureCalleeUsesEnvParameter),
        staticMethod(other.staticMethod) {}

  ~Value() = default;
  auto operator=(const Value& other) -> Value& {
    if (this != &other) {
      name = other.name;
      mangledName = other.mangledName;
      ownedType.reset(); // Release any owned type
      type = other.getType();
      state = other.state;
      llvmValue = other.llvmValue;
      category = other.category;
      used = other.used;
      mutableVar = other.mutableVar;
      signedVar = other.signedVar;
      exported = other.exported;
      constructor = other.constructor;
      genericClassTemplate = other.genericClassTemplate;
      bodyScope = other.bodyScope;
      declarationKind = other.declarationKind;
      declarationSpan = other.declarationSpan;
      declarationFilePath = other.declarationFilePath;
      privateMember = other.privateMember;
      memberDeclaredInClass = other.memberDeclaredInClass;
      closureCaptureOuters = other.closureCaptureOuters;
      closureSlotOuter = other.closureSlotOuter;
      storesFuncValuePair = other.storesFuncValuePair;
      originLambdaExpr = other.originLambdaExpr;
      arcOwnedValue = other.arcOwnedValue;
      closureCalleeUsesEnvParameter = other.closureCalleeUsesEnvParameter;
      staticMethod = other.staticMethod;
    }
    return *this;
  }
  Value(Value&&) = default;
  auto operator=(Value&&) -> Value& = default;

  [[nodiscard]] auto getName() const -> std::string { return name; }
  [[nodiscard]] auto getMangledName() const -> std::string { return mangledName; }
  [[nodiscard]] auto getLlvmValue() const -> llvm::Value* { return llvmValue; }
  [[nodiscard]] auto getMutability() const -> bool { return mutableVar; }
  [[nodiscard]] auto getSigned() const -> bool { return signedVar; }
  [[nodiscard]] auto getState() const -> SymbolState { return state; }
  [[nodiscard]] auto getType() const -> Type* { return ownedType ? ownedType.get() : type; }
  [[nodiscard]] auto getCategory() const -> ValueCategory { return category; }
  [[nodiscard]] auto getConstructor() const -> lesma::Value* { return constructor; }
  [[nodiscard]] auto getBodyScope() const -> SymbolTable* { return bodyScope; }
  [[nodiscard]] auto getDeclarationKind() const -> ValueDeclarationKind { return declarationKind; }
  /** Opaque pointer to the Class* AST for generic class templates (used when
   *  specializing imported generics). Codegen interprets this as const Class*.
   */
  [[nodiscard]] auto getGenericClassTemplate() const -> const void* { return genericClassTemplate; }
  [[nodiscard]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto isUsed() const -> bool { return used; }
  /** Declaration location for LSP go-to-definition. */
  [[nodiscard]] auto getDeclarationSpan() const -> llvm::SMRange { return declarationSpan; }
  [[nodiscard]] auto getDeclarationFilePath() const -> const std::string& {
    return declarationFilePath;
  }

  auto setLlvmValue(llvm::Value* value) -> void { llvmValue = value; }
  auto setName(const std::string& value) -> void { name = value; }
  auto setMangledName(const std::string& value) -> void { mangledName = value; }
  auto setType(Type* value) -> void {
    ownedType.reset();
    type = value;
  }
  auto setUsed(bool value) -> void { used = value; }
  auto setSigned(bool value) -> void { signedVar = value; }
  auto setMutable(bool value) -> void { mutableVar = value; }
  auto setCategory(ValueCategory value) -> void { category = value; }
  auto setExported(bool value) -> void { exported = value; }
  auto setConstructor(lesma::Value* value) -> void { constructor = value; }
  auto setGenericClassTemplate(const void* ptr) -> void { genericClassTemplate = ptr; }
  auto setBodyScope(SymbolTable* value) -> void { bodyScope = value; }
  auto setDeclarationKind(ValueDeclarationKind value) -> void { declarationKind = value; }
  auto setDeclarationSpan(llvm::SMRange span) -> void { declarationSpan = span; }
  auto setDeclarationFilePath(std::string path) -> void { declarationFilePath = std::move(path); }
  /** Class field or method: visible only in methods of the declaring class (not subclasses). */
  [[nodiscard]] auto isPrivateMember() const -> bool { return privateMember; }
  [[nodiscard]] auto getMemberDeclaredInClass() const -> Type* { return memberDeclaredInClass; }
  auto setPrivateMember(bool value) -> void { privateMember = value; }
  auto setMemberDeclaredInClass(Type* classType) -> void { memberDeclaredInClass = classType; }
  [[nodiscard]] auto usesAddressableStorage() const -> bool {
    return category == ValueCategory::ADDRESSABLE_STORAGE;
  }
  [[nodiscard]] auto usesDirectLlvmValue() const -> bool { return !usesAddressableStorage(); }

  /** Outer bindings captured by a lambda (synthetic __lambda_N symbol); order matches env layout. */
  [[nodiscard]] auto getClosureCaptureOuters() const -> const std::vector<Value*>& {
    return closureCaptureOuters;
  }
  auto clearClosureCaptureOuters() -> void { closureCaptureOuters.clear(); }
  auto pushClosureCaptureOuterIfNew(Value* outer) -> void {
    if (outer == nullptr) {
      return;
    }
    for (Value* v : closureCaptureOuters) {
      if (v == outer) {
        return;
      }
    }
    closureCaptureOuters.push_back(outer);
  }
  /** Lambda body shadow slot; non-null ⇒ load this binding from the closure env. */
  [[nodiscard]] auto getClosureSlotOuter() const -> Value* { return closureSlotOuter; }
  auto setClosureSlotOuter(Value* outer) -> void { closureSlotOuter = outer; }
  /** Function values stored as `{ code*, env* }` (env null when no captures). */
  [[nodiscard]] auto getStoresFuncValuePair() const -> bool { return storesFuncValuePair; }
  auto setStoresFuncValuePair(bool v) -> void { storesFuncValuePair = v; }
  /** Variable initialized from a lambda AST; used to specialize generic lambdas at call sites. */
  [[nodiscard]] auto getOriginLambdaExpr() const -> const LambdaExpr* { return originLambdaExpr; }
  auto setOriginLambdaExpr(const LambdaExpr* expr) -> void { originLambdaExpr = expr; }
  /** True when this SSA value currently owns a +1 ARC reference and may be moved into storage. */
  [[nodiscard]] auto getArcOwnedValue() const -> bool { return arcOwnedValue; }
  auto setArcOwnedValue(bool v) -> void { arcOwnedValue = v; }

  /** Indirect call must pass closure env as first argument (lambda with captures). */
  [[nodiscard]] auto getClosureCalleeUsesEnvParameter() const -> bool {
    return closureCalleeUsesEnvParameter;
  }
  auto setClosureCalleeUsesEnvParameter(bool v) -> void { closureCalleeUsesEnvParameter = v; }

  [[nodiscard]] auto isStaticMethod() const -> bool { return staticMethod; }
  auto setStaticMethod(bool v) -> void { staticMethod = v; }

  [[nodiscard]] auto toString() const -> std::string {
    std::string typeStr;
    std::string valueStr;
    llvm::raw_string_ostream rso(typeStr);
    llvm::raw_string_ostream rso2(valueStr);

    auto* type = getType();
    if (type != nullptr && type->getLlvmType() != nullptr) {
      type->getLlvmType()->print(rso);
    }
    if (llvmValue != nullptr) {
      llvmValue->print(rso2);
    }
    return name + ": " + typeStr + " = " + valueStr;
  }

private:
  std::string name;
  std::string mangledName;
  std::unique_ptr<Type> ownedType; // Owned Type (when Value creates its own)
  Type* type = nullptr;            // Non-owning reference (when Type is owned elsewhere)
  SymbolState state = SymbolState::DECLARED;
  llvm::Value* llvmValue = nullptr;
  ValueCategory category = ValueCategory::DIRECT_VALUE;
  // For analysis
  bool used = false;
  // For variables
  bool mutableVar = false;
  // For integers
  bool signedVar = true;
  // For functions
  bool exported = false;
  // For classes
  lesma::Value* constructor = nullptr;
  const void* genericClassTemplate = nullptr;
  SymbolTable* bodyScope = nullptr;
  ValueDeclarationKind declarationKind = ValueDeclarationKind::UNKNOWN;
  // For LSP: declaration location
  llvm::SMRange declarationSpan;
  std::string declarationFilePath;
  bool privateMember = false;
  Type* memberDeclaredInClass = nullptr;
  std::vector<Value*> closureCaptureOuters;
  Value* closureSlotOuter = nullptr;
  bool storesFuncValuePair = false;
  const LambdaExpr* originLambdaExpr = nullptr;
  bool arcOwnedValue = false;
  bool closureCalleeUsesEnvParameter = false;
  bool staticMethod = false;
};
} // namespace lesma
