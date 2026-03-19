// Copyright (c) 2021-2022 ChilliBits. All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

#include "Type.h"

namespace lesma {
class SymbolTable;

enum class SymbolState : std::uint8_t { DECLARED, INITIALIZED };

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
        llvmValue(other.llvmValue), used(other.used), mutableVar(other.mutableVar),
        signedVar(other.signedVar), exported(other.exported), constructor(other.constructor),
        genericClassTemplate(other.genericClassTemplate), bodyScope(other.bodyScope) {}

  ~Value() = default;
  auto operator=(const Value& other) -> Value& {
    if (this != &other) {
      name = other.name;
      mangledName = other.mangledName;
      ownedType.reset(); // Release any owned type
      type = other.getType();
      state = other.state;
      llvmValue = other.llvmValue;
      used = other.used;
      mutableVar = other.mutableVar;
      signedVar = other.signedVar;
      exported = other.exported;
      constructor = other.constructor;
      genericClassTemplate = other.genericClassTemplate;
      bodyScope = other.bodyScope;
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
  [[nodiscard]] auto getConstructor() const -> lesma::Value* { return constructor; }
  [[nodiscard]] auto getBodyScope() const -> SymbolTable* { return bodyScope; }
  /** Opaque pointer to the Class* AST for generic class templates (used when
   *  specializing imported generics). Codegen interprets this as const Class*.
   */
  [[nodiscard]] auto getGenericClassTemplate() const -> void* { return genericClassTemplate; }
  [[nodiscard]] auto isExported() const -> bool { return exported; }
  [[nodiscard]] auto isUsed() const -> bool { return used; }

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
  auto setExported(bool value) -> void { exported = value; }
  auto setConstructor(lesma::Value* value) -> void { constructor = value; }
  auto setGenericClassTemplate(void* ptr) -> void { genericClassTemplate = ptr; }
  auto setBodyScope(SymbolTable* value) -> void { bodyScope = value; }

  auto toString() const -> std::string {
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
  void* genericClassTemplate = nullptr;
  SymbolTable* bodyScope = nullptr;
};
} // namespace lesma
