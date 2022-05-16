// Copyright (c) 2021-2022 ChilliBits. All rights reserved.

#pragma once

#include <string>
#include <utility>

#include "SymbolType.h"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace lesma {
    enum SymbolState { DECLARED,
                       INITIALIZED };

    /**
     * Entry of a symbol table, representing an individual symbol with all its properties
     */
    class SymbolTableEntry {
    public:
        SymbolTableEntry(std::string name, const SymbolType &type) : name(std::move(name)), state(INITIALIZED),
                                                                     type(type), mutable_(false),
                                                                     signed_(true) {}
        SymbolTableEntry(std::string name, const SymbolType &type, SymbolState state) : name(std::move(name)), state(state),
                                                                                        type(type), mutable_(false),
                                                                                        signed_(true) {}
        SymbolTableEntry(std::string name, const SymbolType &type,
                         SymbolState state, bool mutable_, bool signed_) : name(std::move(name)), state(state),
                                                                           type(type), mutable_(mutable_),
                                                                           signed_(signed_) {}

        [[nodiscard]] std::string getName() { return name; }
        [[nodiscard]] llvm::Value *getLLVMValue() { return llvmValue; }
        [[nodiscard]] llvm::Type *getLLVMType() { return llvmType; }
        [[nodiscard]] bool getMutability() const { return mutable_; }
        [[nodiscard]] bool getSigned() const { return signed_; }
        [[nodiscard]] SymbolState getState() { return state; }
        [[nodiscard]] SymbolType getType() { return type; }
        [[nodiscard]] bool isUsed() { return used; }

        void setLLVMValue(llvm::Value *value) { llvmValue = value; }
        void setLLVMType(llvm::Type *type_) { llvmType = type_; }
        void setUsed(bool used_) { used = used_; }
        void setSigned(bool signed__) { signed_ = signed__; }
        void setMutable(bool mutable__) { mutable_ = mutable__; }

        std::string toString() {
            std::string type_str, value_str;
            llvm::raw_string_ostream rso(type_str), rso2(value_str);
            llvmType->print(rso);
            llvmValue->print(rso2);
            return name + ": " + type_str + " = " + value_str;
        }

    private:
        std::string name;
        SymbolState state;
        SymbolType type;
        llvm::Value *llvmValue = nullptr;
        llvm::Type *llvmType = nullptr;
        bool mutable_;
        bool signed_;
        bool used = false;
    };
}//namespace lesma