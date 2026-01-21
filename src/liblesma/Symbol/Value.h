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
        explicit Value(Type *type) : state(SymbolState::INITIALIZED),
                                     type(type) {}
        Value(std::string name, Type *type) : name(std::move(name)), mangledName(name),
                                              state(SymbolState::INITIALIZED), type(type) {}
        Value(std::string name, Type *type, llvm::Value *value) : name(std::move(name)), mangledName(name),
                                                                  state(SymbolState::INITIALIZED), type(type), llvmValue(value) {}
        Value(std::string name, Type *type, SymbolState state) : name(std::move(name)), mangledName(name),
                                                                 state(state), type(type) {}
        Value(std::string name, Type *type,
              SymbolState state, bool mutable_, bool signed_) : name(std::move(name)), state(state),
                                                                type(type), mutableVar(mutable_),
                                                                signedVar(signed_) {}

        [[nodiscard]] std::string getName() { return name; }
        [[nodiscard]] std::string getMangledName() { return mangledName; }
        [[nodiscard]] llvm::Value *getLLVMValue() { return llvmValue; }
        [[nodiscard]] bool getMutability() const { return mutableVar; }
        [[nodiscard]] bool getSigned() const { return signedVar; }
        [[nodiscard]] SymbolState getState() { return state; }
        [[nodiscard]] Type *getType() { return type; }
        [[nodiscard]] lesma::Value *getConstructor() { return constructor; }
        [[nodiscard]] bool isExported() const { return exported; }
        [[nodiscard]] bool isUsed() const { return used; }

        void setLLVMValue(llvm::Value *value) { llvmValue = value; }
        void setName(std::string name_) { name = std::move(name_); }
        void setMangledName(std::string name_) { mangledName = std::move(name_); }
        void setUsed(bool used_) { used = used_; }
        void setSigned(bool signed_) { mutableVar = signed_; }
        void setMutable(bool mutable_) { mutableVar = mutable_; }
        void setExported(bool exported_) { exported = exported_; }
        void setConstructor(lesma::Value *constructor_) { constructor = constructor_; }

        std::string toString() {
            std::string typeStr;
            std::string valueStr;
            llvm::raw_string_ostream rso(typeStr);
            llvm::raw_string_ostream rso2(valueStr);

            if (type->getLLVMType() != nullptr) {
                type->getLLVMType()->print(rso);
            }
            if (llvmValue != nullptr) {
                llvmValue->print(rso2);
            }
            return name + ": " + typeStr + " = " + valueStr;
        }

    private:
        std::string name;
        std::string mangledName;
        SymbolState state;
        Type *type;
        llvm::Value *llvmValue = nullptr;
        // For analysis
        bool used = false;
        // For variables
        bool mutableVar = false;
        // For integers
        bool signedVar = true;
        // For functions
        bool exported = false;
        // For classes
        lesma::Value *constructor = nullptr;
    };
}//namespace lesma