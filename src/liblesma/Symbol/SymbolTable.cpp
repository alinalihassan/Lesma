#include "SymbolTable.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

using namespace lesma;

namespace {

[[nodiscard]] auto sameResolvedModulePath(const std::string& a, const std::string& b) -> bool {
  if (a.empty() || b.empty()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path pa = std::filesystem::absolute(a, ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path pb = std::filesystem::absolute(b, ec);
  if (ec) {
    return false;
  }
  if (pa.lexically_normal() == pb.lexically_normal()) {
    return true;
  }
  return std::filesystem::equivalent(pa, pb, ec) && !ec;
}

} // namespace

Field::Field(std::string n, Type* t) : name(std::move(n)), type(t) {}

Field::Field(std::string n, Type* t, std::unique_ptr<Value> defVal)
    : name(std::move(n)), type(t), defaultValue(std::move(defVal)) {}

Field::~Field() = default;

Field::Field(Field&&) noexcept = default;
auto Field::operator=(Field&&) noexcept -> Field& = default;
auto Field::setDeclarationSymbol(std::unique_ptr<Value> value) -> void {
  declarationSymbol = std::move(value);
}

/**
 * Insert a new symbol into the current symbol table. If it is a parameter,
 * append its name to the paramNames vector
 *
 * @param symbol Symbol Table Entry (takes ownership)
 */
auto SymbolTable::insertSymbol(std::unique_ptr<Value> symbol) -> void {
  auto name = symbol->getName();
  symbols.emplace(std::move(name), std::move(symbol));
}

/**
 * Insert a new type into the current symbol table.
 *
 * @param name Name of the type
 * @param type Type to insert (takes ownership)
 */
auto SymbolTable::insertType(const std::string& name, std::unique_ptr<Type> type) -> void {
  types.insert_or_assign(name, std::move(type));
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if
 * possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
namespace {
// Match rank for overload resolution: higher = better. Candidates are compared
// lexicographically by per-parameter rank vectors (earlier position breaks
// ties; then shorter vector wins).
constexpr int RANK_EXACT = 3;     // concrete-typed parameter match
constexpr int RANK_GENERIC = 2;   // formal is generic (with consistent binding)
/// \c *Derived actual vs \c *Base formal (still a valid VALUE lookup, but less specific than
/// matching the same class under the pointer).
constexpr int RANK_PTR_SUBCLASS = 2;
constexpr int RANK_DEFAULTED = 1; // caller omitted, default applies
constexpr int RANK_VARARG = 0;    // extra args absorbed by vararg

// Returns true if ranksA is strictly better than ranksB (lexicographic; then
// shorter wins when prefix equal).
auto rankVectorBetter(const std::vector<int>& ranksA, const std::vector<int>& ranksB) -> bool {
  size_t n = std::min(ranksA.size(), ranksB.size());
  for (size_t i = 0; i < n; ++i) {
    if (ranksA[i] != ranksB[i]) {
      return ranksA[i] > ranksB[i];
    }
  }
  return ranksA.size() < ranksB.size();
}

auto superCallSelfPointerMatches(Type* formalTy, Type* argTy, Type* staticSuperType) -> bool {
  if (formalTy == nullptr || argTy == nullptr || staticSuperType == nullptr) {
    return false;
  }
  if (!formalTy->is(BaseType::TY_PTR) || !argTy->is(BaseType::TY_PTR)) {
    return false;
  }
  Type* argCls = argTy->getElementType();
  if (argCls == nullptr || !argCls->is(BaseType::TY_CLASS)) {
    return false;
  }
  for (Type* t = argCls; t != nullptr; t = t->getClassSuperclass()) {
    if (t->isEqual(staticSuperType)) {
      return true;
    }
  }
  return false;
}

auto receiverClassTypeForMethodFn(Type* fnTy) -> Type* {
  if (fnTy == nullptr || !fnTy->is(BaseType::TY_FUNCTION)) {
    return nullptr;
  }
  std::vector<Field*> const fields = fnTy->getFields();
  if (fields.empty()) {
    return nullptr;
  }
  Type* first = fields[0]->type;
  if (first != nullptr && first->is(BaseType::TY_PTR) && first->getElementType() != nullptr &&
      first->getElementType()->is(BaseType::TY_CLASS)) {
    return first->getElementType();
  }
  return nullptr;
}

auto typeContainsGeneric(Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (type->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (type->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    return typeContainsGeneric(type->getElementType());
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    for (Field* field : type->getFields()) {
      if (typeContainsGeneric(field->type)) {
        return true;
      }
    }
    return typeContainsGeneric(type->getReturnType());
  }
  return false;
}

[[nodiscard]] auto rankForMatchedOverloadParam(Type* formalTy, Type* argTy) -> int {
  if (formalTy != nullptr && argTy != nullptr && formalTy->is(BaseType::TY_PTR) &&
      argTy->is(BaseType::TY_PTR) && formalTy->getElementType() != nullptr &&
      argTy->getElementType() != nullptr && formalTy->getElementType()->is(BaseType::TY_CLASS) &&
      argTy->getElementType()->is(BaseType::TY_CLASS)) {
    Type* formalCls = formalTy->getElementType();
    Type* argCls = argTy->getElementType();
    if (!argCls->isEqual(formalCls)) {
      return RANK_PTR_SUBCLASS;
    }
    return RANK_EXACT;
  }
  return typeContainsGeneric(formalTy) ? RANK_GENERIC : RANK_EXACT;
}

auto matchGenericParameter(Type* formalTy, Type* argTy,
                           std::unordered_map<std::string, Type*>& genericBindings,
                           FunctionLookupKind lookupKind) -> bool {
  if (formalTy == nullptr || argTy == nullptr) {
    return formalTy == argTy;
  }
  if (formalTy->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
    const std::string& want = formalTy->getDisplayName();
    Type* cls = argTy;
    if (argTy->is(BaseType::TY_PTR) && argTy->getElementType() != nullptr &&
        argTy->getElementType()->is(BaseType::TY_CLASS)) {
      cls = argTy->getElementType();
    }
    if (cls->is(BaseType::TY_CLASS)) {
      for (const auto& n : cls->getImplTraitNames()) {
        if (n == want) {
          return true;
        }
      }
      return false;
    }
    if (argTy->is(BaseType::TY_TRAIT_EXISTENTIAL)) {
      return formalTy->getDisplayName() == argTy->getDisplayName();
    }
    return false;
  }
  if (formalTy->is(BaseType::TY_PTR) && argTy->is(BaseType::TY_PTR) &&
      formalTy->getElementType() != nullptr && argTy->getElementType() != nullptr &&
      formalTy->getElementType()->is(BaseType::TY_CLASS) &&
      argTy->getElementType()->is(BaseType::TY_CLASS)) {
    Type* fromCls = argTy->getElementType();
    Type* toCls = formalTy->getElementType();
    if (lookupKind == FunctionLookupKind::OVERLOAD_IDENTITY) {
      return fromCls->isEqual(toCls);
    }
    if (fromCls->isEqual(toCls)) {
      return true;
    }
    for (Type* t = fromCls; t != nullptr; t = t->getClassSuperclass()) {
      if (t->isEqual(toCls)) {
        return true;
      }
    }
    return false;
  }
  if (formalTy->is(BaseType::TY_GENERIC)) {
    std::string const& genericName = formalTy->getGenericName();
    auto bindingIt = genericBindings.find(genericName);
    if (bindingIt != genericBindings.end()) {
      return argTy->isEqual(bindingIt->second);
    }
    if (argTy->is(BaseType::TY_GENERIC) && formalTy->getGenericName() != argTy->getGenericName()) {
      return false;
    }
    genericBindings[genericName] = argTy;
    return true;
  }
  if (formalTy->getBaseType() != argTy->getBaseType()) {
    return false;
  }
  if (formalTy->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    return matchGenericParameter(formalTy->getElementType(), argTy->getElementType(),
                                 genericBindings, lookupKind);
  }
  if (formalTy->is(BaseType::TY_FUNCTION)) {
    if (!formalTy->functionGenericSignatureEqual(argTy)) {
      return false;
    }
    auto formalFields = formalTy->getFields();
    auto argFields = argTy->getFields();
    if (formalFields.size() != argFields.size()) {
      return false;
    }
    for (size_t i = 0; i < formalFields.size(); ++i) {
      if (!matchGenericParameter(formalFields[i]->type, argFields[i]->type, genericBindings,
                                 lookupKind)) {
        return false;
      }
    }
    return matchGenericParameter(formalTy->getReturnType(), argTy->getReturnType(), genericBindings,
                               lookupKind);
  }
  return formalTy->isEqual(argTy);
}

auto selectBestFunctionTypeMatchImpl(const std::vector<Type*>& candidateFunctionTypes,
                                     const std::vector<Type*>& paramTypes) -> Type* {
  Type* bestCandidate = nullptr;
  std::vector<int> bestRanks;

  for (Type* funcTy : candidateFunctionTypes) {
    if (funcTy == nullptr || !funcTy->is(BaseType::TY_FUNCTION)) {
      continue;
    }

    bool paramsMatch = true;
    std::vector<int> candidateRanks;
    std::unordered_map<std::string, Type*> genericBindings;
    std::vector<Field*> funcParamFields = funcTy->getFields();
    size_t const numParams = std::max(funcParamFields.size(), paramTypes.size());

    for (size_t i = 0; i < numParams; ++i) {
      if (i < funcParamFields.size() && i < paramTypes.size()) {
        Type* formalTy = funcParamFields[i]->type;
        Type* argTy = paramTypes[i];
        if (argTy->is(BaseType::TY_GENERIC) && !typeContainsGeneric(formalTy)) {
          paramsMatch = false;
          break;
        }
        if (!matchGenericParameter(formalTy, argTy, genericBindings, FunctionLookupKind::VALUE)) {
          paramsMatch = false;
          break;
        }
        candidateRanks.push_back(rankForMatchedOverloadParam(formalTy, argTy));
      } else if (i < funcParamFields.size() && funcParamFields[i]->defaultValue != nullptr) {
        candidateRanks.push_back(RANK_DEFAULTED);
      } else if (i >= funcParamFields.size()) {
        if (funcTy->isVarArgs()) {
          candidateRanks.push_back(RANK_VARARG);
          break;
        }
        paramsMatch = false;
        break;
      } else {
        paramsMatch = false;
        break;
      }
    }

    if (!paramsMatch) {
      continue;
    }

    bool candidateWins = bestCandidate == nullptr || rankVectorBetter(candidateRanks, bestRanks);
    if (candidateWins) {
      bestRanks = std::move(candidateRanks);
      bestCandidate = funcTy;
    }
  }

  return bestCandidate;
}

auto selectBestFunctionTypeMatchTailImpl(const std::vector<Type*>& candidateFunctionTypes,
                                         const std::vector<Type*>& paramTypesAfterSelf) -> Type* {
  Type* bestCandidate = nullptr;
  std::vector<int> bestRanks;

  for (Type* funcTy : candidateFunctionTypes) {
    if (funcTy == nullptr || !funcTy->is(BaseType::TY_FUNCTION)) {
      continue;
    }

    std::vector<Field*> funcParamFields = funcTy->getFields();
    if (funcParamFields.empty()) {
      continue;
    }

    bool paramsMatch = true;
    std::vector<int> candidateRanks;
    std::unordered_map<std::string, Type*> genericBindings;
    size_t const formalCount = funcParamFields.size() > 0 ? funcParamFields.size() - 1U : 0U;
    size_t const numParams = std::max(formalCount, paramTypesAfterSelf.size());

    for (size_t j = 0; j < numParams; ++j) {
      size_t const i = j + 1U;
      if (i < funcParamFields.size() && j < paramTypesAfterSelf.size()) {
        Type* formalTy = funcParamFields[i]->type;
        Type* argTy = paramTypesAfterSelf[j];
        if (argTy->is(BaseType::TY_GENERIC) && !typeContainsGeneric(formalTy)) {
          paramsMatch = false;
          break;
        }
        if (!matchGenericParameter(formalTy, argTy, genericBindings, FunctionLookupKind::VALUE)) {
          paramsMatch = false;
          break;
        }
        candidateRanks.push_back(rankForMatchedOverloadParam(formalTy, argTy));
      } else if (i < funcParamFields.size() && funcParamFields[i]->defaultValue != nullptr) {
        candidateRanks.push_back(RANK_DEFAULTED);
      } else if (i >= funcParamFields.size()) {
        if (funcTy->isVarArgs()) {
          candidateRanks.push_back(RANK_VARARG);
          break;
        }
        paramsMatch = false;
        break;
      } else {
        paramsMatch = false;
        break;
      }
    }

    if (!paramsMatch) {
      continue;
    }

    bool candidateWins = bestCandidate == nullptr || rankVectorBetter(candidateRanks, bestRanks);
    if (candidateWins) {
      bestRanks = std::move(candidateRanks);
      bestCandidate = funcTy;
    }
  }

  return bestCandidate;
}
} // namespace

namespace lesma {

auto selectBestFunctionTypeMatch(const std::vector<Type*>& candidateFunctionTypes,
                                 const std::vector<Type*>& paramTypes) -> Type* {
  return selectBestFunctionTypeMatchImpl(candidateFunctionTypes, paramTypes);
}

auto selectBestFunctionTypeMatchTail(const std::vector<Type*>& candidateFunctionTypes,
                                     const std::vector<Type*>& paramTypesAfterSelf) -> Type* {
  return selectBestFunctionTypeMatchTailImpl(candidateFunctionTypes, paramTypesAfterSelf);
}

} // namespace lesma

auto SymbolTable::lookupFunction(const std::string& name, std::vector<lesma::Type*> paramTypes,
                                 FunctionLookupKind lookupKind,
                                 Type* excludeFormalReceiverClass) -> Value* {
  auto range = symbols.equal_range(name);
  Value* bestCandidate = nullptr;
  std::vector<int> bestRanks;

  for (auto it = range.first; it != range.second; ++it) {
    if (!it->second->getType()->is(BaseType::TY_FUNCTION)) {
      continue;
    }
    if (excludeFormalReceiverClass != nullptr) {
      Type* recvCls = receiverClassTypeForMethodFn(it->second->getType());
      if (recvCls != nullptr && recvCls->isEqual(excludeFormalReceiverClass)) {
        continue;
      }
    }

    bool paramsMatch = true;
    std::vector<int> candidateRanks;
    std::unordered_map<std::string, Type*> genericBindings;
    std::vector<Field*> funcParamTypes = it->second->getType()->getFields();
    size_t const numParams = std::max(funcParamTypes.size(), paramTypes.size());

    for (size_t i = 0; i < numParams; ++i) {
      if (i < funcParamTypes.size() && i < paramTypes.size()) {
        Type* formalTy = funcParamTypes[i]->type;
        Type* argTy = paramTypes[i];
        if (argTy->is(BaseType::TY_GENERIC) && !typeContainsGeneric(formalTy)) {
          paramsMatch = false;
          break; // argument must be concrete
        }
        if (!matchGenericParameter(formalTy, argTy, genericBindings, lookupKind)) {
          paramsMatch = false;
          break;
        }
        candidateRanks.push_back(rankForMatchedOverloadParam(formalTy, argTy));
      } else if (i < funcParamTypes.size() && funcParamTypes[i]->defaultValue != nullptr) {
        candidateRanks.push_back(RANK_DEFAULTED);
      } else if (i >= funcParamTypes.size()) {
        if (it->second->getType()->isVarArgs()) {
          candidateRanks.push_back(RANK_VARARG);
          break;
        }
        paramsMatch = false;
        break;
      } else {
        paramsMatch = false;
        break;
      }
    }

    if (!paramsMatch) {
      continue;
    }

    bool candidateWins = bestCandidate == nullptr || rankVectorBetter(candidateRanks, bestRanks);
    if (!candidateWins && bestCandidate != nullptr && candidateRanks.size() == bestRanks.size()) {
      bool const ranksEqual =
          std::equal(candidateRanks.begin(), candidateRanks.end(), bestRanks.begin());
      if (ranksEqual) {
        Value* candSym = it->second.get();
        if (candSym->getLlvmValue() != nullptr && bestCandidate->getLlvmValue() == nullptr) {
          candidateWins = true;
        }
      }
    }
    if (candidateWins) {
      bestRanks = std::move(candidateRanks);
      bestCandidate = it->second.get();
    }
  }

  if (bestCandidate != nullptr) {
    return bestCandidate;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookupFunction(name, paramTypes, lookupKind, excludeFormalReceiverClass);
}

auto SymbolTable::lookupSuperClassMethod(
    const std::string& name, std::vector<lesma::Type*> paramTypes,
    const std::function<bool(Type* formalReceiverClass)>& receiverMatches,
    Type* staticSuperclassType,
    const std::unordered_map<std::string, Type*>* seedGenericBindings) -> Value* {
  auto range = symbols.equal_range(name);
  Value* bestCandidate = nullptr;
  std::vector<int> bestRanks;

  for (auto it = range.first; it != range.second; ++it) {
    if (!it->second->getType()->is(BaseType::TY_FUNCTION)) {
      continue;
    }
    Type* recvCls = receiverClassTypeForMethodFn(it->second->getType());
    if (recvCls == nullptr || !receiverMatches(recvCls)) {
      continue;
    }

    bool paramsMatch = true;
    std::vector<int> candidateRanks;
    std::unordered_map<std::string, Type*> genericBindings;
    if (seedGenericBindings != nullptr) {
      genericBindings = *seedGenericBindings;
    }
    std::vector<Field*> funcParamTypes = it->second->getType()->getFields();
    size_t const numParams = std::max(funcParamTypes.size(), paramTypes.size());

    for (size_t i = 0; i < numParams; ++i) {
      if (i < funcParamTypes.size() && i < paramTypes.size()) {
        Type* formalTy = funcParamTypes[i]->type;
        Type* argTy = paramTypes[i];
        if (argTy->is(BaseType::TY_GENERIC) && !typeContainsGeneric(formalTy)) {
          paramsMatch = false;
          break;
        }
        if (i == 0 && staticSuperclassType != nullptr &&
            superCallSelfPointerMatches(formalTy, argTy, staticSuperclassType)) {
          candidateRanks.push_back(rankForMatchedOverloadParam(formalTy, argTy));
          continue;
        }
        if (!matchGenericParameter(formalTy, argTy, genericBindings, FunctionLookupKind::VALUE)) {
          paramsMatch = false;
          break;
        }
        candidateRanks.push_back(rankForMatchedOverloadParam(formalTy, argTy));
      } else if (i < funcParamTypes.size() && funcParamTypes[i]->defaultValue != nullptr) {
        candidateRanks.push_back(RANK_DEFAULTED);
      } else if (i >= funcParamTypes.size()) {
        if (it->second->getType()->isVarArgs()) {
          candidateRanks.push_back(RANK_VARARG);
          break;
        }
        paramsMatch = false;
        break;
      } else {
        paramsMatch = false;
        break;
      }
    }

    if (!paramsMatch) {
      continue;
    }

    bool candidateWins = bestCandidate == nullptr || rankVectorBetter(candidateRanks, bestRanks);
    if (!candidateWins && bestCandidate != nullptr && candidateRanks.size() == bestRanks.size()) {
      bool const ranksEqual =
          std::equal(candidateRanks.begin(), candidateRanks.end(), bestRanks.begin());
      if (ranksEqual) {
        Value* candSym = it->second.get();
        if (candSym->getLlvmValue() != nullptr && bestCandidate->getLlvmValue() == nullptr) {
          candidateWins = true;
        }
      }
    }
    if (candidateWins) {
      bestRanks = std::move(candidateRanks);
      bestCandidate = it->second.get();
    }
  }

  if (bestCandidate != nullptr && bestCandidate->getLlvmValue() == nullptr) {
    Value* withLlvm = nullptr;
    std::vector<int> withLlvmRanks;
    for (auto it = range.first; it != range.second; ++it) {
      Value* cand = it->second.get();
      if (cand->getLlvmValue() == nullptr) {
        continue;
      }
      if (!cand->getType()->is(BaseType::TY_FUNCTION)) {
        continue;
      }
      Type* recvCls2 = receiverClassTypeForMethodFn(cand->getType());
      if (recvCls2 == nullptr || !receiverMatches(recvCls2)) {
        continue;
      }
      bool paramsMatch2 = true;
      std::vector<int> candidateRanks2;
      std::unordered_map<std::string, Type*> genericBindings2;
      if (seedGenericBindings != nullptr) {
        genericBindings2 = *seedGenericBindings;
      }
      std::vector<Field*> funcParamTypes2 = cand->getType()->getFields();
      size_t const numParams2 = std::max(funcParamTypes2.size(), paramTypes.size());
      for (size_t i = 0; i < numParams2; ++i) {
        if (i < funcParamTypes2.size() && i < paramTypes.size()) {
          Type* formalTy = funcParamTypes2[i]->type;
          Type* argTy = paramTypes[i];
          if (argTy->is(BaseType::TY_GENERIC) && !typeContainsGeneric(formalTy)) {
            paramsMatch2 = false;
            break;
          }
          if (i == 0 && staticSuperclassType != nullptr &&
              superCallSelfPointerMatches(formalTy, argTy, staticSuperclassType)) {
            candidateRanks2.push_back(rankForMatchedOverloadParam(formalTy, argTy));
            continue;
          }
          if (!matchGenericParameter(formalTy, argTy, genericBindings2, FunctionLookupKind::VALUE)) {
            paramsMatch2 = false;
            break;
          }
          candidateRanks2.push_back(rankForMatchedOverloadParam(formalTy, argTy));
        } else if (i < funcParamTypes2.size() && funcParamTypes2[i]->defaultValue != nullptr) {
          candidateRanks2.push_back(RANK_DEFAULTED);
        } else if (i >= funcParamTypes2.size()) {
          if (cand->getType()->isVarArgs()) {
            candidateRanks2.push_back(RANK_VARARG);
            break;
          }
          paramsMatch2 = false;
          break;
        } else {
          paramsMatch2 = false;
          break;
        }
      }
      if (!paramsMatch2) {
        continue;
      }
      bool wins2 = withLlvm == nullptr || rankVectorBetter(candidateRanks2, withLlvmRanks);
      if (wins2) {
        withLlvmRanks = std::move(candidateRanks2);
        withLlvm = cand;
      }
    }
    if (withLlvm != nullptr &&
        !rankVectorBetter(bestRanks, withLlvmRanks) &&
        !rankVectorBetter(withLlvmRanks, bestRanks)) {
      bestCandidate = withLlvm;
    }
  }

  if (bestCandidate != nullptr) {
    return bestCandidate;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookupSuperClassMethod(name, paramTypes, receiverMatches, staticSuperclassType,
                                        seedGenericBindings);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if
 * possible. When multiple symbols share the same name (e.g. overloaded
 * functions), one match is returned; use lookupFunction for overload
 * resolution. When typecheck stubs and codegen definitions coexist (same name),
 * prefer the definition (getLlvmValue() != nullptr).
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookup(const std::string& name) -> Value* {
  auto [it, end] = symbols.equal_range(name);
  Value* typeNominal = nullptr;
  Value* importStub = nullptr;
  Value* fallback = nullptr;
  for (auto i = it; i != end; ++i) {
    Value* v = i->second.get();
    if (v->getLlvmValue() != nullptr) {
      return v; // Prefer symbol that has LLVM value (from codegen)
    }
    if (v->getCategory() == ValueCategory::TYPE_SYMBOL && v->getType() != nullptr &&
        v->getType()->isOneOf(
            {BaseType::TY_CLASS, BaseType::TY_ENUM, BaseType::TY_TRAIT_EXISTENTIAL})) {
      typeNominal = v;
      continue;
    }
    if (v->getCategory() == ValueCategory::MODULE_SYMBOL && v->getType() != nullptr &&
        v->getType()->is(BaseType::TY_IMPORT)) {
      importStub = v;
      continue;
    }
    if (fallback == nullptr) {
      fallback = v;
    }
  }
  if (typeNominal != nullptr) {
    return typeNominal;
  }
  if (importStub != nullptr && fallback != nullptr && fallback->getType() != nullptr &&
      !fallback->getType()->is(BaseType::TY_IMPORT)) {
    Type* stubTy = importStub->getType();
    if (stubTy == nullptr || !stubTy->is(BaseType::TY_IMPORT) ||
        !sameResolvedModulePath(stubTy->getDeclarationFilePath(),
                                fallback->getDeclarationFilePath())) {
      return fallback;
    }
  }
  if (importStub != nullptr) {
    return importStub;
  }
  if (fallback != nullptr) {
    return fallback;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookup(name);
}

auto SymbolTable::lookupShallow(const std::string& name) -> Value* {
  auto [it, end] = symbols.equal_range(name);
  Value* fallback = nullptr;
  for (auto i = it; i != end; ++i) {
    Value* v = i->second.get();
    if (v->getLlvmValue() != nullptr) {
      return v;
    }
    if (fallback == nullptr) {
      fallback = v;
    }
  }
  return fallback;
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if
 * possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupStruct(const std::string& name) -> Value* {
  Value* nameFallback = nullptr;
  for (const auto& [key, sym] : symbols) {
    if (!sym->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
      continue;
    }
    auto* llvmTy = sym->getType()->getLlvmType();
    if (llvmTy != nullptr) {
      if (auto* st = llvm::dyn_cast<llvm::StructType>(llvmTy);
          st != nullptr && st->hasName() && st->getName() == name) {
        return sym.get();
      }
    }
    // Specialized generics and imports: LLVM struct name may not match Lesma display spelling;
    // symbols may also have no LLVM StructType yet (imported / not yet codegen'd).
    Type* stTy = sym->getType();
    if (!stTy->getDisplayName().empty() && stTy->getDisplayName() == name) {
      return sym.get();
    }
    if (key == name) {
      if (llvmTy != nullptr) {
        return sym.get();
      }
      // Typechecker inserts class/enum symbols by name with no LLVM type yet
      nameFallback = sym.get();
    }
  }
  if (nameFallback != nullptr) {
    return nameFallback;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookupStruct(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if
 * possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupType(const std::string& name) -> Type* {
  // Check owned types first
  auto it = types.find(name);
  if (it != types.end()) {
    return it->second.get();
  }

  // Check referenced (imported) types
  auto refIt = typeRefs.find(name);
  if (refIt != typeRefs.end()) {
    return refIt->second;
  }

  // Check parent scope
  if (parent == nullptr) {
    return nullptr;
  }
  return parent->lookupType(name);
}

/**
 * Insert a non-owning type reference into the current symbol table.
 * Used for imported types that are owned by another scope.
 *
 * @param name Name of the type
 * @param type Pointer to type (caller must ensure Type outlives this
 * SymbolTable)
 */
auto SymbolTable::insertTypeRef(const std::string& name, Type* type) -> void {
  typeRefs.insert_or_assign(name, type);
}

auto SymbolTable::releaseOwnedTypesInto(std::vector<std::unique_ptr<Type>>& dest) -> void {
  for (auto it = types.begin(); it != types.end();) {
    const std::string& name = it->first;
    Type* raw = it->second.get();
    typeRefs.insert_or_assign(name, raw);
    dest.push_back(std::move(it->second));
    it = types.erase(it);
  }
  for (auto& [childName, child] : children) {
    (void) childName;
    child->releaseOwnedTypesInto(dest);
  }
}

/**
 * Create a child leaf for the tree of symbol tables and return it
 *
 * @param blockName Name of the child scope
 * @return Newly created child table
 */
auto SymbolTable::createChildBlock(const std::string& blockName) -> SymbolTable* {
  int idx = 1;
  while (children.contains(blockName + std::to_string(idx))) {
    idx++;
  }
  auto key = blockName + std::to_string(idx);
  auto child = std::make_unique<SymbolTable>(this);
  auto* childPtr = child.get();
  children.emplace(std::move(key), std::move(child));
  return childPtr;
}

/**
 * Navigate to parent table of the current one in the tree structure
 *
 * @return Pointer to the parent symbol table
 */
auto SymbolTable::getParent() -> SymbolTable* { return parent; }

/**
 * Navigate to a child table of the current one in the tree structure
 *
 * @param scopeId Name of the child scope
 * @return Pointer to the child symbol table
 */
auto SymbolTable::getChild(const std::string& scopeId) -> SymbolTable* {
  if (children.empty()) {
    return nullptr;
  }
  auto it = children.find(scopeId);
  if (it == children.end()) {
    return nullptr;
  }
  return it->second.get();
}
