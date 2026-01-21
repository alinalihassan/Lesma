// Copyright (c) 2021-2022 ChilliBits. All rights reserved.

#pragma once

#include <cstdint>
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
        explicit Value(Type *type) : state_(SymbolState::INITIALIZED),
                                     type_(type) {}
        Value(std::string name, Type *type) : name_(std::move(name)), mangledName_(name),
                                              state_(SymbolState::INITIALIZED), type_(type) {}
        Value(std::string name, Type *type, llvm::Value *value) : name_(std::move(name)), mangledName_(name),
                                                                  state_(SymbolState::INITIALIZED), type_(type), llvmValue_(value) {}
        Value(std::string name, Type *type, SymbolState state) : name_(std::move(name)), mangledName_(name),
                                                                 state_(state), type_(type) {}
        Value(std::string name, Type *type,
              SymbolState state, bool mutable_, bool signed_) : name_(std::move(name)), state_(state),
                                                                type_(type), mutableVar_(mutable_),
                                                                signedVar_(signed_) {}

        [[nodiscard]] std::string getName() { return name_; }
        [[nodiscard]] std::string getMangledName() { return mangledName_; }
        [[nodiscard]] llvm::Value *getLLVMValue() { return llvmValue_; }
        [[nodiscard]] bool getMutability() const { return mutableVar_; }
        [[nodiscard]] bool getSigned() const { return signedVar_; }
        [[nodiscard]] SymbolState getState() { return state_; }
        [[nodiscard]] Type *getType() { return type_; }
        [[nodiscard]] lesma::Value *getConstructor() { return constructor_; }
        [[nodiscard]] bool isExported() const { return exported_; }
        [[nodiscard]] bool isUsed() const { return used_; }

        void setLLVMValue(llvm::Value *value) { llvmValue_ = value; }
        void setName(std::string value) { name_ = std::move(value); }
        void setMangledName(std::string value) { mangledName_ = std::move(value); }
        void setUsed(bool value) { used_ = value; }
        void setSigned(bool value) { signedVar_ = value; }
        void setMutable(bool value) { mutableVar_ = value; }
        void setExported(bool value) { exported_ = value; }
        void setConstructor(lesma::Value *value) { constructor_ = value; }

        std::string toString() {
            std::string typeStr;
            std::string valueStr;
            llvm::raw_string_ostream rso(typeStr);
            llvm::raw_string_ostream rso2(valueStr);

            if (type_->getLLVMType() != nullptr) {
                type_->getLLVMType()->print(rso);
            }
            if (llvmValue_ != nullptr) {
                llvmValue_->print(rso2);
            }
            return name_ + ": " + typeStr + " = " + valueStr;
        }

    private:
        std::string name_;
        std::string mangledName_;
        SymbolState state_;
        Type *type_;
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
}//namespace lesma