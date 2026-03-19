#include "SymbolTable.h"

#include <algorithm>
#include <cstddef>
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

Field::Field(std::string n, Type* t) : name(std::move(n)), type(t) {}

Field::Field(std::string n, Type* t, std::unique_ptr<Value> defVal)
    : name(std::move(n)), type(t), defaultValue(std::move(defVal)) {}

Field::~Field() = default;

Field::Field(Field&&) noexcept = default;
auto Field::operator=(Field&&) noexcept -> Field& = default;

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
} // namespace

auto SymbolTable::lookupFunction(const std::string& name, std::vector<lesma::Type*> paramTypes)
    -> Value* {
  auto range = symbols.equal_range(name);
  Value* bestCandidate = nullptr;
  std::vector<int> bestRanks;
  bool bestHasValue = false;

  for (auto it = range.first; it != range.second; ++it) {
    if (!it->second->getType()->is(BaseType::TY_FUNCTION)) {
      continue;
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
        if (formalTy->is(BaseType::TY_GENERIC)) {
          std::string const& genericName = formalTy->getGenericName();
          auto bindingIt = genericBindings.find(genericName);
          if (bindingIt != genericBindings.end()) {
            if (!argTy->isEqual(bindingIt->second)) {
              paramsMatch = false;
              break; // repeated generic must match previously bound type
            }
          } else {
            if (argTy->is(BaseType::TY_GENERIC)) {
              paramsMatch = false;
              break; // argument must be concrete
            }
            genericBindings[genericName] = argTy;
          }
          candidateRanks.push_back(RANK_GENERIC);
          continue;
        }
        if (argTy->is(BaseType::TY_GENERIC)) {
          paramsMatch = false;
          break; // argument must be concrete
        }
        if (!formalTy->isEqual(argTy)) {
          paramsMatch = false;
          break;
        }
        candidateRanks.push_back(RANK_EXACT);
      } else if (i < funcParamTypes.size() && funcParamTypes[i]->defaultValue != nullptr) {
        candidateRanks.push_back(RANK_DEFAULTED);
      } else if (i >= funcParamTypes.size()) {
        auto* llvmTy = it->second->getType()->getLlvmType();
        if (llvmTy != nullptr && llvmTy->isFunctionVarArg()) {
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

    bool hasValue = (it->second->getLlvmValue() != nullptr);
    bool candidateWins =
        bestCandidate == nullptr || rankVectorBetter(candidateRanks, bestRanks) ||
        (!rankVectorBetter(bestRanks, candidateRanks) && hasValue && !bestHasValue);
    if (candidateWins) {
      bestRanks = std::move(candidateRanks);
      bestCandidate = it->second.get();
      bestHasValue = hasValue;
    }
  }

  if (bestCandidate != nullptr) {
    return bestCandidate;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookupFunction(name, paramTypes);
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
  Value* withValue = nullptr;
  for (auto i = it; i != end; ++i) {
    Value* v = i->second.get();
    if (v->getLlvmValue() != nullptr) {
      return v; // Prefer symbol that has LLVM value (from codegen)
    }
    if (withValue == nullptr) {
      withValue = v;
    }
  }
  if (withValue != nullptr) {
    return withValue;
  }

  if (parent == nullptr) {
    return nullptr;
  }

  return parent->lookup(name);
}

/**
 * Check if a symbol exists in the current or any parent scope and return it if
 * possible
 *
 * @param name Name of the desired symbol
 * @return Desired symbol / nullptr if the symbol was not found
 */
auto SymbolTable::lookupStruct(const std::string& name) -> Value* {
  for (const auto& [key, sym] : symbols) {
    if (!sym->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
      continue;
    }
    auto* llvmTy = sym->getType()->getLlvmType();
    if (llvmTy != nullptr) {
      if (llvm::cast<llvm::StructType>(llvmTy)->getName() == name) {
        return sym.get();
      }
    } else if (key == name) {
      // Typechecker inserts class/enum symbols by name with no LLVM type yet
      return sym.get();
    }
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
