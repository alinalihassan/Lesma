#include "Typechecker.h"

#include <memory>
#include <utility>
#include <vector>

#include "liblesma/AST/AST.h"
#include "liblesma/Common/TypeCheckError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Token/TokenType.h"
#include "nameof.hpp"

namespace lesma {

auto Typechecker::cacheType(std::unique_ptr<Type> type) -> Type* {
  typeCache.push_back(std::move(type));
  return typeCache.back().get();
}

auto Typechecker::getExtendedType(Type* left, Type* right) -> Type* {
  if (left->getBaseType() == right->getBaseType()) {
    return left;
  }
  if (left->is(BaseType::TY_VOID)) {
    return right;
  }
  if (right->is(BaseType::TY_VOID)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_INT) && right->is(BaseType::TY_FLOAT)) {
    return right;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_INT)) {
    return left;
  }
  if (left->is(BaseType::TY_FLOAT) && right->is(BaseType::TY_FLOAT)) {
    return left;
  }
  return nullptr;
}

auto Typechecker::isAssignableTo(Type* from, Type* to) -> bool {
  if (to == nullptr) {
    return true;
  }
  if (from == nullptr) {
    return false;
  }
  if (from->isEqual(to)) {
    return true;
  }
  if (from->is(BaseType::TY_VOID)) {
    return true;
  }
  if (to->is(BaseType::TY_INT)) {
    return from->is(BaseType::TY_INT) || from->is(BaseType::TY_FLOAT);
  }
  if (to->is(BaseType::TY_FLOAT)) {
    return from->is(BaseType::TY_INT) || from->is(BaseType::TY_FLOAT);
  }
  if (to->is(BaseType::TY_STRING)) {
    return from->is(BaseType::TY_STRING) ||
           (from->is(BaseType::TY_PTR) && from->getElementType() != nullptr &&
            (from->getElementType()->is(BaseType::TY_INT) ||
             from->getElementType()->is(BaseType::TY_VOID)));
  }
  return false;
}

auto Typechecker::resolveType(const TypeExpr* node) -> Type* {
  if (node->getType() == TokenType::INT_TYPE || node->getType() == TokenType::INT8_TYPE ||
      node->getType() == TokenType::INT16_TYPE || node->getType() == TokenType::INT32_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_INT));
  }
  if (node->getType() == TokenType::FLOAT_TYPE ||
      node->getType() == TokenType::FLOAT32_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_FLOAT));
  }
  if (node->getType() == TokenType::BOOL_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  }
  if (node->getType() == TokenType::STRING_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_STRING));
  }
  if (node->getType() == TokenType::VOID_TYPE) {
    return cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  }
  if (node->getType() == TokenType::PTR_TYPE) {
    node->getElementType()->accept(*this);
    Type* elem = result->getType();
    if (elem->is(BaseType::TY_FUNCTION)) {
      return elem; // Function type is already a pointer in Lesma
    }
    return cacheType(
        std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (node->getType() == TokenType::FUNC_TYPE) {
    node->getReturnType()->accept(*this);
    Type* retType = result->getType();
    std::vector<std::unique_ptr<Field>> fields;
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      fields.push_back(
          std::make_unique<Field>(result->getName(), result->getType()));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr,
                                          std::move(fields));
    funcType->setReturnType(retType);
    return cacheType(std::move(funcType));
  }
  if (node->getType() == TokenType::CUSTOM_TYPE) {
    Type* typ = scope->lookupType(node->getName());
    Value* sym = scope->lookupStruct(node->getName());
    if (typ == nullptr && sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Type not found: {}",
                           node->getName());
    }
    return sym != nullptr ? sym->getType() : typ;
  }
  throw TypeCheckError(node->getSpan(), "Unimplemented type {}",
                      NAMEOF_ENUM(node->getType()));
}

Typechecker::Typechecker()
    : mainFilePath(), getExports() {
  rootScope = std::make_unique<SymbolTable>(nullptr);
  scope = rootScope.get();
}

Typechecker::Typechecker(std::string mainFilePath_, GetExportsFn getExports_)
    : mainFilePath(std::move(mainFilePath_)),
      getExports(std::move(getExports_)) {
  rootScope = std::make_unique<SymbolTable>(nullptr);
  scope = rootScope.get();
}

void Typechecker::registerBaseStubs() {
  // exit(Int) -> Void
  auto exitRet = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
  std::vector<std::unique_ptr<Field>> exitParams;
  exitParams.push_back(
      std::make_unique<Field>("", cacheType(std::make_unique<Type>(BaseType::TY_INT))));
  auto exitType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr,
                                         std::move(exitParams));
  exitType->setReturnType(exitRet);
  scope->insertSymbol(
      std::make_unique<Value>("exit", cacheType(std::move(exitType))));
  // print overloads: (Str)->Void, (Int)->Void, (Float)->Void, (Bool)->Void
  for (BaseType argTy :
       {BaseType::TY_STRING, BaseType::TY_INT, BaseType::TY_FLOAT, BaseType::TY_BOOL}) {
    auto ret = cacheType(std::make_unique<Type>(BaseType::TY_VOID));
    std::vector<std::unique_ptr<Field>> params;
    params.push_back(std::make_unique<Field>("", cacheType(std::make_unique<Type>(argTy))));
    auto printType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr,
                                            std::move(params));
    printType->setReturnType(ret);
    scope->insertSymbol(
        std::make_unique<Value>("print", cacheType(std::move(printType))));
  }
}

auto Typechecker::run(const Compound* ast) -> void {
  registerBaseStubs();
  ast->accept(*this);
}

auto Typechecker::takeRootScope() -> std::unique_ptr<SymbolTable> {
  scope = nullptr;
  return std::move(rootScope);
}

auto Typechecker::takeTypeCache() -> std::vector<std::unique_ptr<Type>> {
  return std::move(typeCache);
}

auto Typechecker::visit(const Statement* /*node*/) -> void {}
auto Typechecker::visit(const Expression* /*node*/) -> void {}
auto Typechecker::visit(const Else* /*node*/) -> void {}

auto Typechecker::visit(const Compound* node) -> void {
  for (Statement* elem : node->getChildren()) {
    elem->accept(*this);
  }
}

auto Typechecker::visit(const VarDecl* node) -> void {
  Type* declType = nullptr;
  if (node->getType() != nullptr) {
    node->getType()->accept(*this);
    declType = result->getType();
  }
  if (node->getValue() != nullptr) {
    node->getValue()->accept(*this);
    Type* initType = result->getType();
    if (declType != nullptr) {
      if (!isAssignableTo(initType, declType)) {
        throw TypeCheckError(
            node->getSpan(),
            "Variable initializer type {} is not assignable to declared type {}",
            initType->toString(), declType->toString());
      }
    } else {
      declType = initType;
    }
  }
  if (declType == nullptr) {
    throw TypeCheckError(node->getSpan(),
                         "Variable {} has no type and no initializer",
                         node->getIdentifier()->getValue());
  }
  // Typechecker does not allocate; we only register the symbol for lookup.
  auto symbol = std::make_unique<Value>(node->getIdentifier()->getValue(),
                                        declType, SymbolState::INITIALIZED);
  symbol->setMutable(node->getMutability());
  scope->insertSymbol(std::move(symbol));
}

auto Typechecker::visit(const If* node) -> void {
  for (Expression* cond : node->getConds()) {
    cond->accept(*this);
    if (result->getType() != nullptr &&
        !result->getType()->is(BaseType::TY_BOOL)) {
      throw TypeCheckError(cond->getSpan(),
                          "Condition must be Bool, got {}",
                          result->getType()->toString());
    }
  }
  for (Compound* block : node->getBlocks()) {
    block->accept(*this);
  }
}

auto Typechecker::visit(const While* node) -> void {
  node->getCond()->accept(*this);
  if (result->getType() != nullptr &&
      !result->getType()->is(BaseType::TY_BOOL)) {
    throw TypeCheckError(node->getCond()->getSpan(),
                        "Condition must be Bool, got {}",
                        result->getType()->toString());
  }
  node->getBlock()->accept(*this);
}

auto Typechecker::visit(const Import* node) -> void {
  auto importType = cacheType(std::make_unique<Type>(BaseType::TY_IMPORT));
  auto addImportSymbol = [this, &importType](const std::string& name) {
    if (!name.empty()) {
      scope->insertSymbol(std::make_unique<Value>(name, importType));
    }
  };
  if (node->getImportAll() && getExports) {
    std::vector<std::string> names = getExports(node->getFilePath(),
                                                node->isStd(), mainFilePath);
    for (const std::string& name : names) {
      addImportSymbol(name);
    }
    std::string aliasName = node->getAlias().empty()
                                ? getBasename(node->getFilePath())
                                : node->getAlias();
    addImportSymbol(aliasName);
    return;
  }
  if (node->getImportedNames().empty()) {
    std::string aliasName = node->getAlias().empty()
                                ? getBasename(node->getFilePath())
                                : node->getAlias();
    addImportSymbol(aliasName);
    return;
  }
  for (const auto& [name, alias] : node->getImportedNames()) {
    addImportSymbol(alias.empty() ? name : alias);
  }
}

auto Typechecker::visit(const Enum* node) -> void {
  auto type = std::make_unique<Type>(BaseType::TY_ENUM, nullptr,
                                     std::vector<std::unique_ptr<Field>>{});
  Type* typePtr = type.get();
  for (const std::string& field : node->getValues()) {
    type->addField(std::make_unique<Field>(field, typePtr));
  }
  scope->insertType(node->getIdentifier(), std::move(type));
  scope->insertSymbol(
      std::make_unique<Value>(node->getIdentifier(), typePtr));
}

auto Typechecker::visit(const Class* node) -> void {
  std::vector<std::unique_ptr<Field>> fields;
  for (VarDecl* field : node->getFields()) {
    if (field->getType() != nullptr) {
      field->getType()->accept(*this);
    } else if (field->getValue() != nullptr) {
      field->getValue()->accept(*this);
    } else {
      throw TypeCheckError(field->getSpan(), "Class field has no type");
    }
    fields.push_back(std::make_unique<Field>(
        field->getIdentifier()->getValue(), result->getType()));
  }
  auto type = std::make_unique<Type>(BaseType::TY_CLASS, nullptr,
                                     std::move(fields));
  Type* typePtr = type.get();
  scope->insertType(node->getIdentifier(), std::move(type));
  scope->insertSymbol(
      std::make_unique<Value>(node->getIdentifier(), typePtr));

  Type* classTypePtr = scope->lookupType(node->getIdentifier());
  auto* selfPtrType =
      cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classTypePtr));
  for (FuncDecl* func : node->getMethods()) {
    currentClassType = classTypePtr;
    SymbolTable* methodScope = scope->createChildBlock("method");
    scope = methodScope;
    scope->insertSymbol(
        std::make_unique<Value>("self", selfPtrType));
    func->accept(*this);
    scope = scope->getParent();
    currentClassType = nullptr;
  }
}

auto Typechecker::visit(const FuncDecl* node) -> void {
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  if (currentClassType != nullptr) {
    Type* selfPtr =
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, currentClassType));
    paramFields.push_back(std::make_unique<Field>("self", selfPtr));
    paramTypes.push_back(selfPtr);
  }
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(),
                          "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    // Match codegen: function params use pointer-to-class so lookup matches
    if (paramType->is(BaseType::TY_CLASS)) {
      paramType = cacheType(
          std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramType));
    }
    paramTypes.push_back(paramType);
    std::unique_ptr<Field> field;
    if (param->defaultVal != nullptr) {
      field = std::make_unique<Field>(param->name, paramType,
                                      std::make_unique<Value>(paramType));
    } else {
      field = std::make_unique<Field>(param->name, paramType);
    }
    paramFields.push_back(std::move(field));
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr,
                                        std::move(paramFields));
  funcType->setReturnType(returnType);
  Type* funcTypePtr = cacheType(std::move(funcType));
  auto funcSymbol =
      std::make_unique<Value>(node->getName(), funcTypePtr);
  funcSymbol->setExported(node->isExported());
  SymbolTable* insertScope = (currentClassType != nullptr) ? scope->getParent() : scope;
  insertScope->insertSymbol(std::move(funcSymbol));

  SymbolTable* child = scope->createChildBlock("function");
  scope = child;
  const size_t paramOffset = (currentClassType != nullptr) ? 1u : 0u;
  for (size_t i = 0; i < node->getParameters().size(); ++i) {
    Parameter* param = node->getParameters()[i];
    scope->insertSymbol(
        std::make_unique<Value>(param->name, paramTypes[paramOffset + i]));
  }
  currentFunction = insertScope->lookupFunction(node->getName(), paramTypes);
  inTopLevel = false;
  node->getBody()->accept(*this);
  scope = scope->getParent();
  currentFunction = nullptr;
  inTopLevel = true;
}

auto Typechecker::visit(const ExternFuncDecl* node) -> void {
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(),
                          "Parameter {} has no type", param->name);
    }
    paramTypes.push_back(result->getType());
    paramFields.push_back(
        std::make_unique<Field>(param->name, result->getType()));
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr,
                                         std::move(paramFields));
  funcType->setReturnType(returnType);
  Type* funcTypePtr = cacheType(std::move(funcType));
  auto funcSymbol =
      std::make_unique<Value>(node->getName(), funcTypePtr);
  funcSymbol->setExported(node->isExported());
  scope->insertSymbol(std::move(funcSymbol));
}

auto Typechecker::visit(const Assignment* node) -> void {
  if (auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide())) {
    Value* sym = scope->lookup(lit->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Variable not found: {}",
                           lit->getValue());
    }
    if (!sym->getMutability()) {
      throw TypeCheckError(node->getSpan(),
                          "Cannot assign to immutable variable {}",
                          lit->getValue());
    }
    node->getRightHandSide()->accept(*this);
    if (!isAssignableTo(result->getType(), sym->getType())) {
      throw TypeCheckError(node->getSpan(),
                          "Cannot assign type {} to variable of type {}",
                          result->getType()->toString(),
                          sym->getType()->toString());
    }
    return;
  }
  if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    Type* lhsType = result->getType();
    Type* targetType = lhsType != nullptr && lhsType->is(BaseType::TY_PTR)
                           ? lhsType->getElementType()
                           : lhsType;
    node->getRightHandSide()->accept(*this);
    if (targetType != nullptr &&
        !isAssignableTo(result->getType(), targetType)) {
      throw TypeCheckError(node->getSpan(),
                          "Cannot assign type {} to field of type {}",
                          result->getType()->toString(),
                          targetType->toString());
    }
    return;
  }
  throw TypeCheckError(node->getSpan(), "Invalid assignment target");
}

auto Typechecker::visit(const Break* node) -> void {
  (void)node;
  // We don't track break targets in typecheck; Codegen will enforce.
}

auto Typechecker::visit(const Continue* node) -> void {
  (void)node;
}

auto Typechecker::visit(const Return* node) -> void {
  if (currentFunction == nullptr) {
    throw TypeCheckError(node->getSpan(),
                        "Return not allowed outside function");
  }
  Type* expected = currentFunction->getType()->getReturnType();
  if (node->getValue() == nullptr) {
    if (expected == nullptr || !expected->is(BaseType::TY_VOID)) {
      throw TypeCheckError(node->getSpan(),
                          "Return type does not match: expected {}, got void",
                          expected != nullptr ? expected->toString() : "?");
    }
    return;
  }
  node->getValue()->accept(*this);
  if (!isAssignableTo(result->getType(), expected)) {
    throw TypeCheckError(node->getSpan(),
                        "Return type does not match: expected {}, got {}",
                        expected->toString(), result->getType()->toString());
  }
}

auto Typechecker::visit(const Defer* node) -> void {
  node->getStatement()->accept(*this);
}

auto Typechecker::visit(const UnimplementedStatement* node) -> void {
  throw TypeCheckError(node->getSpan(), "{}", node->getMessage());
}

auto Typechecker::visit(const ExpressionStatement* node) -> void {
  node->getExpression()->accept(*this);
}

auto Typechecker::visit(const FuncCall* node) -> void {
  std::vector<Type*> argTypes;
  for (Expression* arg : node->getArguments()) {
    arg->accept(*this);
    Type* t = result->getType();
    if (t->is(BaseType::TY_CLASS)) {
      t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
    }
    argTypes.push_back(t);
  }
  Value* callee = scope->lookupFunction(node->getName(), argTypes);
  if (callee == nullptr) {
    Value* sym = scope->lookup(node->getName());
    if (sym != nullptr) {
      if (sym->getType()->isOneOf({BaseType::TY_CLASS, BaseType::TY_ENUM})) {
        result = std::make_unique<Value>(sym->getType());
        return;
      }
      if (sym->getType()->is(BaseType::TY_IMPORT)) {
        result = std::make_unique<Value>(
            cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
        return;
      }
    }
    throw TypeCheckError(node->getSpan(), "Function not found: {}",
                         node->getName());
  }
  if (!callee->getType()->is(BaseType::TY_FUNCTION)) {
    throw TypeCheckError(node->getSpan(), "Not a function: {}",
                         node->getName());
  }
  // Constructor call: result type is the class type (receiver), not return type
  if (node->getName() == "new" && !callee->getType()->getFields().empty() &&
      callee->getType()->getFields()[0]->type->is(BaseType::TY_PTR)) {
    result = std::make_unique<Value>(
        callee->getType()->getFields()[0]->type->getElementType());
  } else {
    result = std::make_unique<Value>(callee->getType()->getReturnType());
  }
}

auto Typechecker::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  std::unique_ptr<Value> left = std::move(result);
  node->getRight()->accept(*this);
  std::unique_ptr<Value> right = std::move(result);
  Type* leftTy = left->getType();
  Type* rightTy = right->getType();
  Type* unified = getExtendedType(leftTy, rightTy);

  switch (node->getOperator()) {
  case TokenType::PLUS:
  case TokenType::MINUS:
  case TokenType::STAR:
  case TokenType::SLASH:
  case TokenType::MOD:
    if (unified == nullptr) {
      throw TypeCheckError(
          node->getSpan(),
          "Operator {} not applicable to {} and {}",
          NAMEOF_ENUM(node->getOperator()), leftTy->toString(),
          rightTy->toString());
    }
    if (!unified->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw TypeCheckError(node->getSpan(),
                          "Arithmetic operator requires numeric types");
    }
    result = std::make_unique<Value>(unified);
    break;
  case TokenType::EQUAL_EQUAL:
  case TokenType::BANG_EQUAL:
  case TokenType::GREATER:
  case TokenType::GREATER_EQUAL:
  case TokenType::LESS:
  case TokenType::LESS_EQUAL:
    if (unified == nullptr && !leftTy->isEqual(rightTy)) {
      throw TypeCheckError(node->getSpan(),
                          "Comparison requires compatible types: {} and {}",
                          leftTy->toString(), rightTy->toString());
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::AND:
  case TokenType::OR:
    if (leftTy == nullptr || !leftTy->is(BaseType::TY_BOOL) || rightTy == nullptr ||
        !rightTy->is(BaseType::TY_BOOL)) {
      throw TypeCheckError(node->getSpan(),
                          "Logical operator requires Bool operands");
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported binary operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
  }
}

auto Typechecker::visit(const DotOp* node) -> void {
  node->getLeft()->accept(*this);
  Type* base = result->getType();
  if (base == nullptr) {
    throw TypeCheckError(node->getSpan(), "Dot operator on unknown type");
  }
  if (base->is(BaseType::TY_VOID)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
      }
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_IMPORT)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
      }
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    throw TypeCheckError(node->getSpan(),
                         "Dot operator requires class or enum type, got {}",
                         base->toString());
  }
  if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
    std::vector<Type*> argTypes;
    for (Expression* arg : fc->getArguments()) {
      arg->accept(*this);
      argTypes.push_back(result->getType());
    }
    Type* selfType = base->is(BaseType::TY_PTR) ? base
                    : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, base));
    std::vector<Type*> methodArgTypes = {selfType};
    methodArgTypes.insert(methodArgTypes.end(), argTypes.begin(), argTypes.end());
    Value* method = scope->lookupFunction(fc->getName(), methodArgTypes);
    if (method == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found: {}",
                           fc->getName());
    }
    result = std::make_unique<Value>(method->getType()->getReturnType());
    return;
  }
  if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
    throw TypeCheckError(node->getSpan(), "Expected field name or method call after dot");
  }
  auto* rightLit = dynamic_cast<Literal*>(node->getRight());
  Type* fieldType = TypeUtils::findTypeInFields(base, rightLit->getValue());
  if (fieldType == nullptr) {
    throw TypeCheckError(node->getSpan(), "Unknown field: {}",
                         rightLit->getValue());
  }
  result = std::make_unique<Value>(fieldType);
}

auto Typechecker::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* from = result->getType();
  node->getType()->accept(*this);
  Type* to = result->getType();
  if (!isAssignableTo(from, to)) {
    throw TypeCheckError(node->getSpan(),
                         "Cannot cast from {} to {}",
                         from->toString(), to->toString());
  }
  result = std::make_unique<Value>(to);
}

auto Typechecker::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  node->getRight()->accept(*this);
  result = std::make_unique<Value>(
      cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
}

auto Typechecker::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* operand = result->getType();
  switch (node->getOperator()) {
  case TokenType::MINUS:
    if (operand == nullptr || !operand->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw TypeCheckError(node->getSpan(),
                          "Unary minus requires numeric type");
    }
    result = std::make_unique<Value>(operand);
    break;
  case TokenType::BANG:
  case TokenType::NOT:
    if (operand == nullptr || !operand->is(BaseType::TY_BOOL)) {
      throw TypeCheckError(node->getSpan(),
                          "Unary not requires Bool type");
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::AMPERSAND:
    if (operand == nullptr) {
      throw TypeCheckError(node->getSpan(), "Address-of requires a value");
    }
    result = std::make_unique<Value>(cacheType(
        std::make_unique<Type>(BaseType::TY_PTR, nullptr, operand)));
    break;
  case TokenType::STAR:
    if (operand == nullptr || !operand->is(BaseType::TY_PTR)) {
      throw TypeCheckError(node->getSpan(),
                          "Dereference requires pointer type");
    }
    result = std::make_unique<Value>(
        operand->getElementType() != nullptr ? operand->getElementType()
                                             : cacheType(std::make_unique<Type>(BaseType::TY_INT)));
    break;
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported unary operator: {}",
                         NAMEOF_ENUM(node->getOperator()));
  }
}

auto Typechecker::visit(const Literal* node) -> void {
  switch (node->getType()) {
  case TokenType::INTEGER:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_INT)));
    break;
  case TokenType::DOUBLE:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_FLOAT)));
    break;
  case TokenType::STRING:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    break;
  case TokenType::BOOL:
  case TokenType::TRUE_:
  case TokenType::FALSE_:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::NIL:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, nullptr)));
    break;
  case TokenType::IDENTIFIER: {
    Value* sym = scope->lookup(node->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unknown name: {}",
                           node->getValue());
    }
    result = std::make_unique<Value>(*sym);
    break;
  }
  default:
    throw TypeCheckError(node->getSpan(), "Unsupported literal type: {}",
                         NAMEOF_ENUM(node->getType()));
  }
}

auto Typechecker::visit(const TypeExpr* node) -> void {
  Type* type = resolveType(node);
  result = std::make_unique<Value>(type);
}

} // namespace lesma
