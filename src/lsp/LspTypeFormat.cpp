#include "LspTypeFormat.h"

#include <functional>
#include <string>
#include <vector>

#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/Value.h"

namespace lesma::lsp_srv {
using namespace lesma;

namespace {
auto formatFunctionTypeAsLesma(Type* type, SymbolTable* rootScope) -> std::string;
}

auto getTypeName(Type* type, SymbolTable* rootScope) -> std::string {
  if (type == nullptr || rootScope == nullptr) {
    return "";
  }
  if (type->is(BaseType::TY_CLASS) || type->is(BaseType::TY_ENUM)) {
    std::function<Value*(SymbolTable*)> findTypeSymbol = [&](SymbolTable* scope) -> Value* {
      if (scope == nullptr) {
        return nullptr;
      }
      for (Value* sym : scope->getSymbols()) {
        if (sym->getCategory() == ValueCategory::TYPE_SYMBOL && sym->getType() == type) {
          return sym;
        }
      }
      return findTypeSymbol(scope->getParent());
    };
    Value* typeSymbol = findTypeSymbol(rootScope);
    if (typeSymbol != nullptr) {
      return typeSymbol->getName();
    }
  }
  if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr) {
    Type* elementType = type->getElementType();
    // Values are often *T at the type level; source syntax uses T for class/enum (no leading *).
    if (elementType->is(BaseType::TY_CLASS) || elementType->is(BaseType::TY_ENUM)) {
      std::string elementName = getTypeName(elementType, rootScope);
      if (!elementName.empty()) {
        return elementName;
      }
    }
  }
  return "";
}

auto formatTypeName(Type* type, SymbolTable* rootScope) -> std::string {
  if (type == nullptr) {
    return "?";
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    return formatFunctionTypeAsLesma(type, rootScope);
  }
  std::string namedType = getTypeName(type, rootScope);
  if (!namedType.empty()) {
    return namedType;
  }
  if (type->is(BaseType::TY_PTR) && type->getElementType() != nullptr) {
    Type* elem = type->getElementType();
    if (elem->is(BaseType::TY_CLASS) || elem->is(BaseType::TY_ENUM)) {
      return formatTypeName(elem, rootScope);
    }
    return "*" + formatTypeName(elem, rootScope);
  }
  if (type->is(BaseType::TY_ARRAY) && type->getElementType() != nullptr) {
    return "list<" + formatTypeName(type->getElementType(), rootScope) + ">";
  }
  if (type->is(BaseType::TY_INT)) {
    const unsigned w = type->getIntWidth();
    if (w == 64U) {
      return type->isSigned() ? "int" : "uint";
    }
    return (type->isSigned() ? "int" : "uint") + std::to_string(w);
  }
  if (type->is(BaseType::TY_FLOAT)) {
    return "float";
  }
  if (type->is(BaseType::TY_FLOAT32)) {
    return "float32";
  }
  if (type->is(BaseType::TY_STRING)) {
    return "cstr";
  }
  if (type->is(BaseType::TY_BOOL)) {
    return "bool";
  }
  if (type->is(BaseType::TY_VOID)) {
    return "void";
  }
  return type->toString();
}

namespace {

auto formatFunctionTypeAsLesma(Type* type, SymbolTable* rootScope) -> std::string {
  if (type == nullptr || !type->is(BaseType::TY_FUNCTION)) {
    return {};
  }
  std::string out = "func(";
  std::vector<Field*> const fields = type->getFields();
  size_t const paramOffset =
      (!fields.empty() && fields[0] != nullptr && fields[0]->name == "self") ? 1U : 0U;
  bool first = true;
  for (size_t i = paramOffset; i < fields.size(); ++i) {
    Field* field = fields[i];
    if (field == nullptr || field->type == nullptr) {
      continue;
    }
    if (!first) {
      out += ", ";
    }
    first = false;
    out += formatTypeName(field->type, rootScope);
  }
  if (type->isVarArgs()) {
    if (!first) {
      out += ", ";
    }
    out += "...";
  }
  out += ")";
  Type* const ret = type->getReturnType();
  if (ret != nullptr && !ret->is(BaseType::TY_VOID)) {
    out += " -> " + formatTypeName(ret, rootScope);
  }
  return out;
}

} // namespace

auto formatBufferArrayTypeName(Type* arrayType, SymbolTable* rootScope) -> std::string {
  if (arrayType == nullptr || !arrayType->is(BaseType::TY_ARRAY) ||
      arrayType->getElementType() == nullptr) {
    return "?";
  }
  return "__buffer<" + formatTypeName(arrayType->getElementType(), rootScope) + ">";
}

} // namespace lesma::lsp_srv
