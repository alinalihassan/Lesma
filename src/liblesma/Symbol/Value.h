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
      : name(std::move(name)), mangledName(name), type(type),
        state(SymbolState::INITIALIZED) {}

  Value(std::string name, Type* type, llvm::Value* value)
      : name(std::move(name)), mangledName(name), type(type),
        state(SymbolState::INITIALIZED), llvmValue(value) {}

  Value(std::string name, Type* type, SymbolState state)
      : name(std::move(name)), mangledName(name), type(type), state(state) {}

  Value(std::string name, Type* type, SymbolState state, bool mutableVar,
        bool signedVar)
      : name(std::move(name)), type(type), state(state), mutableVar(mutableVar),
        signedVar(signedVar) {}

  // Copy constructor - creates a shallow copy with non-owning Type reference
  Value(const Value& other)
      : name(other.name), mangledName(other.mangledName), type(other.GetType()),
        state(other.state), llvmValue(other.llvmValue), used(other.used),
        mutableVar(other.mutableVar), signedVar(other.signedVar),
        exported(other.exported), constructor(other.constructor) {}

  ~Value() = default;
  auto operator=(const Value& other) -> Value& {
    if (this != &other) {
      name = other.name;
      mangledName = other.mangledName;
      ownedType.reset(); // Release any owned type
      type = other.GetType();
      state = other.state;
      llvmValue = other.llvmValue;
      used = other.used;
      mutableVar = other.mutableVar;
      signedVar = other.signedVar;
      exported = other.exported;
      constructor = other.constructor;
    }
    return *this;
  }
  Value(Value&&) = default;
  auto operator=(Value&&) -> Value& = default;

  [[nodiscard]] auto GetName() -> std::string { return name; }
  [[nodiscard]] auto GetMangledName() -> std::string { return mangledName; }
  [[nodiscard]] auto GetLlvmValue() -> llvm::Value* { return llvmValue; }
  [[nodiscard]] auto GetMutability() const -> bool { return mutableVar; }
  [[nodiscard]] auto GetSigned() const -> bool { return signedVar; }
  [[nodiscard]] auto GetState() -> SymbolState { return state; }
  [[nodiscard]] auto GetType() const -> Type* {
    return ownedType ? ownedType.get() : type;
  }
  [[nodiscard]] auto GetConstructor() -> lesma::Value* { return constructor; }
  [[nodiscard]] auto IsExported() const -> bool { return exported; }
  [[nodiscard]] auto IsUsed() const -> bool { return used; }

  auto SetLlvmValue(llvm::Value* value) -> void { llvmValue = value; }
  auto SetName(const std::string& value) -> void { name = value; }
  auto SetMangledName(const std::string& value) -> void { mangledName = value; }
  auto SetUsed(bool value) -> void { used = value; }
  auto SetSigned(bool value) -> void { signedVar = value; }
  auto SetMutable(bool value) -> void { mutableVar = value; }
  auto SetExported(bool value) -> void { exported = value; }
  auto SetConstructor(lesma::Value* value) -> void { constructor = value; }

  auto ToString() -> std::string {
    std::string typeStr;
    std::string valueStr;
    llvm::raw_string_ostream rso(typeStr);
    llvm::raw_string_ostream rso2(valueStr);

    auto* type = GetType();
    if (type != nullptr && type->GetLlvmType() != nullptr) {
      type->GetLlvmType()->print(rso);
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
  Type* type = nullptr; // Non-owning reference (when Type is owned elsewhere)
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
};
} // namespace lesma
