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
    enum class SymbolState : std::uint8_t {
        DECLARED,
        INITIALIZED
    };

    /**
     * Entry of a symbol table, representing an individual symbol with all its properties
     */
    class Value {
    public:
        // Constructors that take ownership of Type
        explicit Value(std::unique_ptr<Type> type)
            : ownedType_(std::move(type)), state_(SymbolState::INITIALIZED) {}

        Value(std::string name, std::unique_ptr<Type> type)
            : name_(std::move(name)), mangledName_(name_), ownedType_(std::move(type)),
              state_(SymbolState::INITIALIZED) {}

        Value(std::string name, std::unique_ptr<Type> type, llvm::Value *value)
            : name_(std::move(name)), mangledName_(name_), ownedType_(std::move(type)),
              state_(SymbolState::INITIALIZED), llvmValue_(value) {}

        // Constructors with non-owning Type reference (existing behavior)
        explicit Value(Type *type) : type_(type), state_(SymbolState::INITIALIZED) {}

        Value(std::string name, Type *type)
            : name_(std::move(name)), mangledName_(name_), type_(type), state_(SymbolState::INITIALIZED) {}

        Value(std::string name, Type *type, llvm::Value *value)
            : name_(std::move(name)), mangledName_(name_), type_(type),
              state_(SymbolState::INITIALIZED), llvmValue_(value) {}

        Value(std::string name, Type *type, SymbolState state)
            : name_(std::move(name)), mangledName_(name_), type_(type), state_(state) {}

        Value(std::string name, Type *type, SymbolState state, bool mutableVar, bool signedVar)
            : name_(std::move(name)), type_(type), state_(state),
              mutableVar_(mutableVar), signedVar_(signedVar) {}

        // Copy constructor - creates a shallow copy with non-owning Type reference
        Value(const Value &other)
            : name_(other.name_), mangledName_(other.mangledName_),
              type_(other.getType()), state_(other.state_),
              llvmValue_(other.llvmValue_), used_(other.used_), mutableVar_(other.mutableVar_),
              signedVar_(other.signedVar_), exported_(other.exported_), constructor_(other.constructor_) {}

        ~Value() = default;
        auto operator=(const Value &other) -> Value & {
            if (this != &other) {
                name_ = other.name_;
                mangledName_ = other.mangledName_;
                ownedType_.reset();// Release any owned type
                type_ = other.getType();
                state_ = other.state_;
                llvmValue_ = other.llvmValue_;
                used_ = other.used_;
                mutableVar_ = other.mutableVar_;
                signedVar_ = other.signedVar_;
                exported_ = other.exported_;
                constructor_ = other.constructor_;
            }
            return *this;
        }
        Value(Value &&) = default;
        auto operator=(Value &&) -> Value & = default;

        [[nodiscard]] auto getName() -> std::string { return name_; }
        [[nodiscard]] auto getMangledName() -> std::string { return mangledName_; }
        [[nodiscard]] auto getLLVMValue() -> llvm::Value * { return llvmValue_; }
        [[nodiscard]] auto getMutability() const -> bool { return mutableVar_; }
        [[nodiscard]] auto getSigned() const -> bool { return signedVar_; }
        [[nodiscard]] auto getState() -> SymbolState { return state_; }
        [[nodiscard]] auto getType() const -> Type * { return ownedType_ ? ownedType_.get() : type_; }
        [[nodiscard]] auto getConstructor() -> lesma::Value * { return constructor_; }
        [[nodiscard]] auto isExported() const -> bool { return exported_; }
        [[nodiscard]] auto isUsed() const -> bool { return used_; }

        auto setLLVMValue(llvm::Value *value) -> void { llvmValue_ = value; }
        auto setName(std::string value) -> void { name_ = std::move(value); }
        auto setMangledName(std::string value) -> void { mangledName_ = std::move(value); }
        auto setUsed(bool value) -> void { used_ = value; }
        auto setSigned(bool value) -> void { signedVar_ = value; }
        auto setMutable(bool value) -> void { mutableVar_ = value; }
        auto setExported(bool value) -> void { exported_ = value; }
        auto setConstructor(lesma::Value *value) -> void { constructor_ = value; }

        auto toString() -> std::string {
            std::string typeStr;
            std::string valueStr;
            llvm::raw_string_ostream rso(typeStr);
            llvm::raw_string_ostream rso2(valueStr);

            auto *type = getType();
            if (type != nullptr && type->getLLVMType() != nullptr) {
                type->getLLVMType()->print(rso);
            }
            if (llvmValue_ != nullptr) {
                llvmValue_->print(rso2);
            }
            return name_ + ": " + typeStr + " = " + valueStr;
        }

    private:
        std::string name_;
        std::string mangledName_;
        std::unique_ptr<Type> ownedType_;// Owned Type (when Value creates its own)
        Type *type_ = nullptr;           // Non-owning reference (when Type is owned elsewhere)
        SymbolState state_ = SymbolState::DECLARED;
        llvm::Value *llvmValue_ = nullptr;
        // For analysis
        bool used_ = false;
        // For variables
        bool mutableVar_ = false;
        // For integers
        bool signedVar_ = true;
        // For functions
        bool exported_ = false;
        // For classes
        lesma::Value *constructor_ = nullptr;
    };
}// namespace lesma
