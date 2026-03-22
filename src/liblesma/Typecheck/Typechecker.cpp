#include "Typechecker.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include "fmt/format.h"
#include "nameof.hpp"

#include "liblesma/AST/AST.h"
#include "liblesma/Common/TypeCheckError.h"
#include "liblesma/Common/Utils.h"
#include "liblesma/Frontend/Lexer.h"
#include "liblesma/Frontend/Parser.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Token/TokenType.h"

namespace lesma {

namespace {

auto makeGenericDisplaySuffix(const std::vector<std::string>& genericParamNames) -> std::string {
  if (genericParamNames.empty()) {
    return {};
  }
  std::string suffix = "<";
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    if (i > 0U) {
      suffix += ", ";
    }
    suffix += genericParamNames[i];
  }
  suffix += ">";
  return suffix;
}

auto makeSpecializedDisplayName(Type* classTemplate,
                                const std::vector<std::string>& genericParamNames,
                                const std::unordered_map<std::string, Type*>& env) -> std::string {
  if (classTemplate == nullptr) {
    return {};
  }
  std::string baseName = classTemplate->getDisplayName();
  if (baseName.empty()) {
    baseName = classTemplate->toString();
  }
  size_t genericStart = baseName.find('<');
  if (genericStart != std::string::npos) {
    baseName = baseName.substr(0U, genericStart);
  }
  if (genericParamNames.empty()) {
    return baseName;
  }
  std::string result = baseName + "<";
  for (size_t i = 0; i < genericParamNames.size(); ++i) {
    if (i > 0U) {
      result += ", ";
    }
    auto it = env.find(genericParamNames[i]);
    result +=
        it != env.end() && it->second != nullptr ? it->second->toString() : genericParamNames[i];
  }
  result += ">";
  return result;
}

void insertGenericParamSymbols(SymbolTable* genericsScope,
                               const std::vector<lesma::GenericParamDecl>& genericParams,
                               const std::unordered_map<std::string, Type*>& genericTypes,
                               const std::string& mainFilePath) {
  if (genericsScope == nullptr) {
    return;
  }
  for (const auto& param : genericParams) {
    auto it = genericTypes.find(param.name);
    if (it == genericTypes.end() || it->second == nullptr) {
      continue;
    }
    auto genericSymbol = std::make_unique<Value>(param.name, it->second);
    genericSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    genericSymbol->setDeclarationSpan(param.span);
    genericSymbol->setDeclarationFilePath(mainFilePath);
    genericsScope->insertSymbol(std::move(genericSymbol));
  }
}

} // namespace

auto Typechecker::pathLeadsToEndWithoutReturn(const std::vector<Statement*>& statements,
                                              size_t index) -> bool {
  if (index >= statements.size()) {
    return true; // fell off the end
  }
  Statement* s = statements[index];
  if (dynamic_cast<Return*>(s) != nullptr) {
    return false; // this path returns
  }
  if (auto* comp = dynamic_cast<Compound*>(s)) {
    // If inner block always returns, we never fall off it.
    if (!pathLeadsToEndWithoutReturn(comp->getChildren(), 0)) {
      return false;
    }
    return pathLeadsToEndWithoutReturn(statements, index + 1);
  }
  if (auto* ifStmt = dynamic_cast<If*>(s)) {
    const auto& blocks = ifStmt->getBlocks();
    const bool someBranchFallsThrough =
        std::any_of(blocks.begin(), blocks.end(), [this](Compound* block) {
          return pathLeadsToEndWithoutReturn(block->getChildren(), 0);
        });
    // Any branch that falls through continues with the following statements.
    // Without an else-branch, skipping the whole if also falls through.
    if (blocks.size() == 1U || someBranchFallsThrough) {
      return pathLeadsToEndWithoutReturn(statements, index + 1);
    }
    return false; // all branches return
  }
  if (dynamic_cast<While*>(s) != nullptr) {
    // Can skip loop (0 iterations) and reach next statement.
    return pathLeadsToEndWithoutReturn(statements, index + 1);
  }
  // VarDecl, Assignment, ExpressionStatement, Break, Continue, Defer, etc.: fall through
  return pathLeadsToEndWithoutReturn(statements, index + 1);
}

auto Typechecker::blockAlwaysReturns(const Compound* body) -> bool {
  return !pathLeadsToEndWithoutReturn(body->getChildren(), 0);
}

auto Typechecker::cacheType(std::unique_ptr<Type> type) -> Type* {
  typeCache.push_back(std::move(type));
  return typeCache.back().get();
}

auto Typechecker::materializeImportedType(Type* type) -> Type* {
  if (type == nullptr) {
    return nullptr;
  }
  auto existing = importedTypeCopies.find(type);
  if (existing != importedTypeCopies.end()) {
    return existing->second;
  }
  if (type->is(BaseType::TY_CLASS) || type->is(BaseType::TY_ENUM)) {
    auto placeholder =
        std::make_unique<Type>(type->getBaseType(), nullptr, std::vector<std::unique_ptr<Field>>{});
    Type* copy = cacheType(std::move(placeholder));
    importedTypeCopies[type] = copy;
    copy->setDisplayName(type->getDisplayName());
    copy->setGenericParams(type->getGenericParams());
    for (Field* field : type->getFields()) {
      auto fieldCopy = std::make_unique<Field>(field->name, materializeImportedType(field->type));
      fieldCopy->setDeclarationSpan(field->getDeclarationSpan());
      fieldCopy->setDeclarationFilePath(field->getDeclarationFilePath());
      if (Value* declarationSymbol = field->getDeclarationSymbol()) {
        auto symbolCopy =
            std::make_unique<Value>(declarationSymbol->getName(), materializeImportedType(field->type));
        symbolCopy->setCategory(declarationSymbol->getCategory());
        symbolCopy->setDeclarationSpan(declarationSymbol->getDeclarationSpan());
        symbolCopy->setDeclarationFilePath(declarationSymbol->getDeclarationFilePath());
        symbolCopy->setMutable(declarationSymbol->getMutability());
        fieldCopy->setDeclarationSymbol(std::move(symbolCopy));
      }
      copy->addField(std::move(fieldCopy));
    }
    if (auto envIt = specializedTypeEnv.find(type); envIt != specializedTypeEnv.end()) {
      std::unordered_map<std::string, Type*> envCopy;
      for (const auto& [name, envType] : envIt->second) {
        envCopy[name] = materializeImportedType(envType);
      }
      specializedTypeEnv[copy] = std::move(envCopy);
    }
    if (auto tmplIt = specializedTypeToTemplate.find(type);
        tmplIt != specializedTypeToTemplate.end()) {
      specializedTypeToTemplate[copy] = materializeImportedType(tmplIt->second);
      copy->setDisplayName(makeSpecializedDisplayName(
          specializedTypeToTemplate[copy], copy->getGenericParams(), specializedTypeEnv[copy]));
    }
    return copy;
  }
  if (type->is(BaseType::TY_PTR)) {
    Type* copy = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr,
                                                  materializeImportedType(type->getElementType())));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_FUNCTION)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : type->getFields()) {
      auto fieldCopy = std::make_unique<Field>(field->name, materializeImportedType(field->type));
      fieldCopy->setDeclarationSpan(field->getDeclarationSpan());
      fieldCopy->setDeclarationFilePath(field->getDeclarationFilePath());
      if (Value* declarationSymbol = field->getDeclarationSymbol()) {
        auto symbolCopy =
            std::make_unique<Value>(declarationSymbol->getName(), materializeImportedType(field->type));
        symbolCopy->setCategory(declarationSymbol->getCategory());
        symbolCopy->setDeclarationSpan(declarationSymbol->getDeclarationSpan());
        symbolCopy->setDeclarationFilePath(declarationSymbol->getDeclarationFilePath());
        symbolCopy->setMutable(declarationSymbol->getMutability());
        fieldCopy->setDeclarationSymbol(std::move(symbolCopy));
      }
      fields.push_back(std::move(fieldCopy));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(materializeImportedType(type->getReturnType()));
    funcType->setGenericParams(type->getGenericParams());
    funcType->setVarArgs(type->isVarArgs());
    Type* copy = cacheType(std::move(funcType));
    importedTypeCopies[type] = copy;
    return copy;
  }
  if (type->is(BaseType::TY_GENERIC)) {
    Type* copy = cacheType(std::make_unique<Type>(type->getGenericName()));
    importedTypeCopies[type] = copy;
    return copy;
  }
  Type* copy = cacheType(std::make_unique<Type>(type->getBaseType()));
  importedTypeCopies[type] = copy;
  return copy;
}

auto Typechecker::substituteInType(Type* t, const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  if (t == nullptr) {
    return nullptr;
  }
  if (t->is(BaseType::TY_GENERIC)) {
    auto it = env.find(t->getGenericName());
    if (it != env.end()) {
      return it->second;
    }
    return t;
  }
  if (t->is(BaseType::TY_PTR) && t->getElementType() != nullptr) {
    Type* elem = substituteInType(t->getElementType(), env);
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (t->is(BaseType::TY_ARRAY) && t->getElementType() != nullptr) {
    Type* elem = substituteInType(t->getElementType(), env);
    return cacheType(std::make_unique<Type>(BaseType::TY_ARRAY, nullptr, elem));
  }
  if (t->is(BaseType::TY_FUNCTION)) {
    std::vector<std::unique_ptr<Field>> fields;
    for (Field* field : t->getFields()) {
      fields.push_back(std::make_unique<Field>(field->name, substituteInType(field->type, env)));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(substituteInType(t->getReturnType(), env));
    funcType->setGenericParams(t->getGenericParams());
    funcType->setVarArgs(t->isVarArgs());
    return cacheType(std::move(funcType));
  }
  return t;
}

auto Typechecker::inferGenericBindings(Type* pattern, Type* actual,
                                       std::unordered_map<std::string, Type*>& bindings,
                                       llvm::SMRange span) -> void {
  if (pattern == nullptr || actual == nullptr) {
    return;
  }
  if (pattern->is(BaseType::TY_GENERIC)) {
    const std::string genericName = pattern->getGenericName();
    auto it = bindings.find(genericName);
    if (it == bindings.end()) {
      bindings[genericName] = actual;
      return;
    }
    if (!actual->isEqual(it->second)) {
      throw TypeCheckError(span, "Conflicting inferred types for generic parameter {}: {} and {}",
                           genericName, it->second->toString(), actual->toString());
    }
    return;
  }
  if (pattern->getBaseType() != actual->getBaseType()) {
    return;
  }
  if (pattern->isOneOf({BaseType::TY_PTR, BaseType::TY_ARRAY})) {
    inferGenericBindings(pattern->getElementType(), actual->getElementType(), bindings, span);
    return;
  }
  if (pattern->is(BaseType::TY_FUNCTION)) {
    auto patternFields = pattern->getFields();
    auto actualFields = actual->getFields();
    for (size_t i = 0; i < patternFields.size() && i < actualFields.size(); ++i) {
      inferGenericBindings(patternFields[i]->type, actualFields[i]->type, bindings, span);
    }
    inferGenericBindings(pattern->getReturnType(), actual->getReturnType(), bindings, span);
  }
}

auto Typechecker::getDeclaredGenericParams(Type* type) const -> const std::vector<std::string>& {
  static const std::vector<std::string> emptyParams;
  if (type == nullptr) {
    return emptyParams;
  }
  Type* key = type;
  auto it = specializedTypeToTemplate.find(type);
  if (it != specializedTypeToTemplate.end()) {
    key = it->second;
  }
  return key->getGenericParams();
}

auto Typechecker::getOrCreateSpecializedClassType(Type* classTemplate,
                                                  const std::vector<std::string>& genericParamNames,
                                                  const std::unordered_map<std::string, Type*>& env)
    -> Type* {
  if (genericParamNames.empty()) {
    return classTemplate;
  }

  std::ostringstream key;
  key << classTemplate->toString();
  for (const auto& name : genericParamNames) {
    auto it = env.find(name);
    if (it != env.end()) {
      key << "|" << it->second->toString();
    }
  }
  std::string keyStr = key.str();
  auto it = specializedClassTypes.find(keyStr);
  if (it != specializedClassTypes.end()) {
    return it->second;
  }
  std::vector<std::unique_ptr<Field>> newFields;
  for (Field* f : classTemplate->getFields()) {
    Type* subst = substituteInType(f->type, env);
    newFields.push_back(std::make_unique<Field>(f->name, subst));
  }
  auto specialized = std::make_unique<Type>(BaseType::TY_CLASS, nullptr, std::move(newFields));
  Type* ptr = cacheType(std::move(specialized));
  specializedClassTypes[keyStr] = ptr;
  specializedTypeEnv[ptr] = env;
  specializedTypeToTemplate[ptr] = classTemplate;
  ptr->setGenericParams(genericParamNames);
  ptr->setDisplayName(makeSpecializedDisplayName(classTemplate, genericParamNames, env));
  return ptr;
}

auto Typechecker::getExtendedType(Type* left, Type* right) -> Type* {
  if (left->isEqual(right)) {
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

auto Typechecker::typecheckBinaryOpResult(TokenType op, Type* leftTy, Type* rightTy,
                                          llvm::SMRange span) -> Type* {
  bool hasGeneric = leftTy->is(BaseType::TY_GENERIC) || rightTy->is(BaseType::TY_GENERIC);
  Type* unified = getExtendedType(leftTy, rightTy);

  switch (op) {
  case TokenType::PLUS:
  case TokenType::MINUS:
  case TokenType::STAR:
  case TokenType::SLASH:
  case TokenType::MOD:
  case TokenType::POWER:
    if (hasGeneric) {
      return leftTy->is(BaseType::TY_GENERIC) ? leftTy : rightTy;
    }
    if (unified == nullptr) {
      throw TypeCheckError(span, "Operator {} not applicable to {} and {}", NAMEOF_ENUM(op),
                           leftTy->toString(), rightTy->toString());
    }
    if (!unified->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw TypeCheckError(span, "Arithmetic operator requires numeric types");
    }
    return unified;
  case TokenType::EQUAL_EQUAL:
  case TokenType::BANG_EQUAL:
  case TokenType::GREATER:
  case TokenType::GREATER_EQUAL:
  case TokenType::LESS:
  case TokenType::LESS_EQUAL:
    if (leftTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS}) ||
        rightTy->isOneOf({BaseType::TY_ENUM, BaseType::TY_CLASS})) {
      if (leftTy != rightTy) {
        throw TypeCheckError(span, "Comparison requires operands of the same enum or class type");
      }
    }
    if (!hasGeneric && unified == nullptr && !leftTy->isEqual(rightTy)) {
      throw TypeCheckError(span, "Comparison requires compatible types: {} and {}",
                           leftTy->toString(), rightTy->toString());
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  case TokenType::AND:
  case TokenType::OR:
    if (!hasGeneric && (leftTy == nullptr || !leftTy->is(BaseType::TY_BOOL) || rightTy == nullptr ||
                        !rightTy->is(BaseType::TY_BOOL))) {
      throw TypeCheckError(span, "Logical operator requires Bool operands");
    }
    return cacheType(std::make_unique<Type>(BaseType::TY_BOOL));
  default:
    throw TypeCheckError(span, "Unsupported binary operator: {}", NAMEOF_ENUM(op));
  }
}

auto Typechecker::isAssignableTo(Type* from, Type* to) -> bool {
  if (to == nullptr) {
    return true;
  }
  if (from == nullptr) {
    return false;
  }
  if (from->is(BaseType::TY_GENERIC) || to->is(BaseType::TY_GENERIC)) {
    return true;
  }
  if (from->isEqual(to)) {
    return true;
  }
  // Void is only assignable to Void
  if (from->is(BaseType::TY_VOID)) {
    return to->is(BaseType::TY_VOID);
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
  if (node->getType() == TokenType::FLOAT_TYPE || node->getType() == TokenType::FLOAT32_TYPE) {
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
    return cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, elem));
  }
  if (node->getType() == TokenType::FUNC_TYPE) {
    node->getReturnType()->accept(*this);
    Type* retType = result->getType();
    std::vector<std::unique_ptr<Field>> fields;
    for (TypeExpr* param : node->getParams()) {
      param->accept(*this);
      fields.push_back(std::make_unique<Field>(result->getName(), result->getType()));
    }
    auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(fields));
    funcType->setReturnType(retType);
    return cacheType(std::move(funcType));
  }
  if (node->getType() == TokenType::CUSTOM_TYPE) {
    auto genericIt = currentGenericTypes.find(node->getName());
    if (genericIt != currentGenericTypes.end()) {
      node->setResolvedSymbol(scope->lookup(node->getName()));
      return genericIt->second;
    }
    Type* typ = scope->lookupType(node->getName());
    Value* sym = scope->lookupStruct(node->getName());
    bool resolvedFromImport = false;
    if ((typ == nullptr && sym == nullptr) ||
        (sym != nullptr && sym->getType()->is(BaseType::TY_IMPORT))) {
      auto importedIt = importedNameToSource.find(node->getName());
      if (importedIt != importedNameToSource.end()) {
        SymbolTable* importScope = getOrTypecheckImport(importedIt->second.first);
        if (importScope != nullptr) {
          sym = importScope->lookupStruct(importedIt->second.second);
          typ = importScope->lookupType(importedIt->second.second);
          resolvedFromImport = true;
        }
      }
    }
    if (typ == nullptr && sym == nullptr) {
      throw TypeCheckError(node->getSpan(),
                           "Type '{}' not found. If you meant a generic type parameter, add it "
                           "to the generic parameter list (e.g. def foo<T>(x: T) -> T).",
                           node->getName());
    }
    node->setResolvedSymbol(sym);
    if (resolvedFromImport) {
      return sym != nullptr ? materializeImportedType(sym->getType())
                            : materializeImportedType(typ);
    }
    return sym != nullptr ? sym->getType() : typ;
  }
  throw TypeCheckError(node->getSpan(), "Unimplemented type {}", NAMEOF_ENUM(node->getType()));
}

Typechecker::Typechecker() : mainFilePath(), getExports() {
  rootScope = std::make_unique<SymbolTable>(nullptr);
  scope = rootScope.get();
}

Typechecker::Typechecker(std::string mainFilePath_, GetExportsFn getExports_)
    : mainFilePath(std::move(mainFilePath_)), getExports(std::move(getExports_)) {
  rootScope = std::make_unique<SymbolTable>(nullptr);
  scope = rootScope.get();
}

void Typechecker::loadImplicitBaseModule() {
  const auto basePath =
      std::filesystem::absolute(std::filesystem::path(getStdDir()) / "base.les").lexically_normal();
  if (!mainFilePath.empty() &&
      std::filesystem::absolute(std::filesystem::path(mainFilePath)).lexically_normal() ==
          basePath) {
    return;
  }

  SymbolTable* baseScope = getOrTypecheckImport(basePath.string());
  if (baseScope == nullptr) {
    return;
  }

  auto* importType = cacheType(std::make_unique<Type>(BaseType::TY_IMPORT));
  for (auto* sym : baseScope->getSymbols()) {
    if (!sym->isExported()) {
      continue;
    }
    const std::string& name = sym->getName();
    if (importedNameToSource.contains(name)) {
      continue;
    }
    auto symbol = std::make_unique<Value>(name, importType);
    symbol->setCategory(ValueCategory::MODULE_SYMBOL);
    scope->insertSymbol(std::move(symbol));
    importedNameToSource[name] = std::make_pair(basePath.string(), name);
  }
}

auto Typechecker::resolveImportPath(const std::string& filepath, bool isStd) const -> std::string {
  if (isStd || mainFilePath.empty()) {
    return filepath;
  }
  return fmt::format("{}/{}", std::filesystem::absolute(mainFilePath).parent_path().string(),
                     filepath);
}

auto Typechecker::getOrTypecheckImport(const std::string& absolutePath) -> SymbolTable* {
  auto it = importedModuleCache.find(absolutePath);
  if (it != importedModuleCache.end()) {
    return it->second != nullptr ? it->second->rootScope.get() : nullptr;
  }
  auto buffer = llvm::MemoryBuffer::getFile(absolutePath);
  if (!buffer) {
    return nullptr;
  }
  auto srcMgr = std::make_shared<llvm::SourceMgr>();
  unsigned const bufferId = srcMgr->AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  auto lexer = std::make_unique<Lexer>(srcMgr);
  lexer->scanAll();
  auto parser = std::make_unique<Parser>(lexer->getTokens());
  parser->parse();
  Compound* ast = parser->getAst();
  if (ast == nullptr) {
    return nullptr;
  }
  Typechecker sub(absolutePath, getExports);
  sub.run(ast);
  auto imported = std::make_shared<ImportedModuleAnalysis>();
  imported->sourceMgr = std::move(srcMgr);
  imported->mainBufferId = bufferId;
  imported->mainFilePath = absolutePath;
  imported->parser = std::move(parser);
  imported->rootScope = sub.takeRootScope();
  imported->typeCache = sub.takeTypeCache();
  imported->index = buildAnalysisIndex(imported->parser != nullptr ? imported->parser->getAst() : nullptr,
                                       imported->sourceMgr.get(), imported->mainBufferId);
  imported->importAliasToPath = sub.takeImportAliasToPath();
  imported->importedNameToSource = sub.takeImportedNameToSource();
  imported->importedModules = sub.takeImportedModules();
  SymbolTable* scopePtr = imported->rootScope.get();
  importedModuleCache[absolutePath] = std::move(imported);
  return scopePtr;
}

auto Typechecker::run(const Compound* ast) -> void {
  loadImplicitBaseModule();
  declarationPass = true;
  ast->accept(*this);
  declarationPass = false;
  ast->accept(*this);
}

auto Typechecker::takeRootScope() -> std::unique_ptr<SymbolTable> {
  scope = nullptr;
  return std::move(rootScope);
}

auto Typechecker::takeTypeCache() -> std::vector<std::unique_ptr<Type>> {
  return std::move(typeCache);
}

auto Typechecker::takeImportAliasToPath() -> ImportAliasMap { return std::move(importAliasToPath); }

auto Typechecker::takeImportedNameToSource() -> ImportedNameSourceMap {
  return std::move(importedNameToSource);
}

auto Typechecker::takeImportedModules()
    -> std::unordered_map<std::string, std::shared_ptr<ImportedModuleAnalysis>> {
  return std::move(importedModuleCache);
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
        throw TypeCheckError(node->getSpan(),
                             "Variable initializer type {} is not assignable "
                             "to declared type {}",
                             initType->toString(), declType->toString());
      }
    } else {
      declType = initType;
    }
  }
  if (declType == nullptr) {
    throw TypeCheckError(node->getSpan(), "Variable {} has no type and no initializer",
                         node->getIdentifier()->getValue());
  }
  // Typechecker does not allocate; we only register the symbol for lookup.
  auto symbol = std::make_unique<Value>(node->getIdentifier()->getValue(), declType,
                                        SymbolState::INITIALIZED);
  symbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
  symbol->setMutable(node->getMutability());
  symbol->setDeclarationSpan(node->getIdentifier()->getSpan());
  symbol->setDeclarationFilePath(mainFilePath);
  node->setResolvedSymbol(symbol.get());
  scope->insertSymbol(std::move(symbol));
}

auto Typechecker::visit(const If* node) -> void {
  for (Expression* cond : node->getConds()) {
    cond->accept(*this);
    if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
        !result->getType()->is(BaseType::TY_GENERIC)) {
      throw TypeCheckError(cond->getSpan(), "Condition must be Bool, got {}",
                           result->getType()->toString());
    }
  }
  for (Compound* block : node->getBlocks()) {
    block->accept(*this);
  }
}

auto Typechecker::visit(const While* node) -> void {
  node->getCond()->accept(*this);
  if (result->getType() != nullptr && !result->getType()->is(BaseType::TY_BOOL) &&
      !result->getType()->is(BaseType::TY_GENERIC)) {
    throw TypeCheckError(node->getCond()->getSpan(), "Condition must be Bool, got {}",
                         result->getType()->toString());
  }
  node->getBlock()->accept(*this);
}

auto Typechecker::visit(const Import* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto importType = cacheType(std::make_unique<Type>(BaseType::TY_IMPORT));
  const std::string resolvedPath = resolveImportPath(node->getFilePath(), node->isStd());
  auto addImportSymbol = [this, &importType](const std::string& name) {
    if (!name.empty()) {
      auto symbol = std::make_unique<Value>(name, importType);
      symbol->setCategory(ValueCategory::MODULE_SYMBOL);
      scope->insertSymbol(std::move(symbol));
    }
  };
  if (node->getImportAll() && getExports) {
    std::vector<std::string> names = getExports(node->getFilePath(), node->isStd(), mainFilePath);
    for (const std::string& name : names) {
      addImportSymbol(name);
      importedNameToSource[name] = std::make_pair(resolvedPath, name);
    }
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName);
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  if (node->getImportedNames().empty()) {
    std::string aliasName =
        node->getAlias().empty() ? getBasename(node->getFilePath()) : node->getAlias();
    addImportSymbol(aliasName);
    importAliasToPath[aliasName] = resolvedPath;
    return;
  }
  for (const ImportedNameBinding& binding : node->getImportedNames()) {
    const std::string& name = binding.name;
    const std::string& alias = binding.alias;
    const std::string localName = alias.empty() ? name : alias;
    addImportSymbol(localName);
    importedNameToSource[localName] = std::make_pair(resolvedPath, name);
  }
}

auto Typechecker::visit(const Enum* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto type =
      std::make_unique<Type>(BaseType::TY_ENUM, nullptr, std::vector<std::unique_ptr<Field>>{});
  Type* typePtr = type.get();
  type->setDisplayName(node->getIdentifier());
  std::vector<std::string> const values = node->getValues();
  std::vector<llvm::SMRange> const& valueSpans = node->getValueSpans();
  for (size_t i = 0; i < values.size(); ++i) {
    auto field = std::make_unique<Field>(values[i], typePtr);
    if (i < valueSpans.size()) {
      field->setDeclarationSpan(valueSpans[i]);
      field->setDeclarationFilePath(mainFilePath);
      auto memberSymbol = std::make_unique<Value>(values[i], typePtr);
      memberSymbol->setCategory(ValueCategory::DIRECT_VALUE);
      memberSymbol->setDeclarationSpan(valueSpans[i]);
      memberSymbol->setDeclarationFilePath(mainFilePath);
      field->setDeclarationSymbol(std::move(memberSymbol));
    }
    type->addField(std::move(field));
  }
  scope->insertType(node->getIdentifier(), std::move(type));
  auto enumSymbol = std::make_unique<Value>(node->getIdentifier(), typePtr);
  enumSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
  enumSymbol->setExported(node->isExported());
  enumSymbol->setDeclarationSpan(node->getNameSpan());
  enumSymbol->setDeclarationFilePath(mainFilePath);
  scope->insertSymbol(std::move(enumSymbol));
  node->setResolvedSymbol(scope->lookupStruct(node->getIdentifier()));
}

auto Typechecker::visit(const Class* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* outerScope = scope;
  SymbolTable* classGenericScope = outerScope->createChildBlock("class_generics");
  scope = classGenericScope;
  node->setGenericScope(classGenericScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(classGenericScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);

  Type* classTypePtr = outerScope->lookupType(node->getIdentifier());
  if (declarationPass) {
    std::vector<std::unique_ptr<Field>> fields;
    for (VarDecl* field : node->getFields()) {
      Type* fieldType = nullptr;
      if (field->getType() != nullptr) {
        field->getType()->accept(*this);
        fieldType = result->getType();
      }
      if (field->getValue() != nullptr) {
        field->getValue()->accept(*this);
        Type* initType = result->getType();
        if (fieldType != nullptr) {
          if (!isAssignableTo(initType, fieldType)) {
            throw TypeCheckError(
                field->getSpan(),
                "Class field initializer type {} is not assignable to declared type {}",
                initType->toString(), fieldType->toString());
          }
        } else {
          fieldType = initType;
        }
      }
      if (fieldType == nullptr) {
        throw TypeCheckError(field->getSpan(), "Class field has no type");
      }
      auto fieldEntry = std::make_unique<Field>(field->getIdentifier()->getValue(), fieldType);
      fieldEntry->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldEntry->setDeclarationFilePath(mainFilePath);
      auto fieldSymbol = std::make_unique<Value>(field->getIdentifier()->getValue(), fieldType,
                                                 SymbolState::INITIALIZED);
      fieldSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      fieldSymbol->setMutable(field->getMutability());
      fieldSymbol->setDeclarationSpan(field->getIdentifier()->getSpan());
      fieldSymbol->setDeclarationFilePath(mainFilePath);
      field->setResolvedSymbol(fieldSymbol.get());
      fieldEntry->setDeclarationSymbol(std::move(fieldSymbol));
      fields.push_back(std::move(fieldEntry));
    }
    auto type = std::make_unique<Type>(BaseType::TY_CLASS, nullptr, std::move(fields));
    type->setDisplayName(node->getIdentifier() +
                         makeGenericDisplaySuffix(node->getGenericParams()));
    classTypePtr = type.get();
    outerScope->insertType(node->getIdentifier(), std::move(type));
    auto classSymbol = std::make_unique<Value>(node->getIdentifier(), classTypePtr);
    classSymbol->setCategory(ValueCategory::TYPE_SYMBOL);
    classSymbol->setExported(node->isExported());
    classSymbol->setDeclarationSpan(node->getNameSpan());
    classSymbol->setDeclarationFilePath(mainFilePath);
    outerScope->insertSymbol(std::move(classSymbol));
    node->setResolvedSymbol(outerScope->lookupStruct(node->getIdentifier()));
  }

  classTypePtr->setGenericParams(node->getGenericParams());
  auto* selfPtrType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classTypePtr));
  SymbolTable* savedMethodInsertScope = currentMethodInsertScope;
  for (FuncDecl* func : node->getMethods()) {
    currentClassType = classTypePtr;
    currentMethodInsertScope = outerScope;
    SymbolTable* methodScope = scope->createChildBlock("method");
    scope = methodScope;
    auto selfSymbol = std::make_unique<Value>("self", selfPtrType);
    selfSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
    scope->insertSymbol(std::move(selfSymbol));
    func->accept(*this);
    scope = scope->getParent();
    currentClassType = nullptr;
  }
  currentMethodInsertScope = savedMethodInsertScope;

  scope = outerScope;
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::visit(const FuncDecl* node) -> void {
  auto savedGenerics = currentGenericTypes;
  SymbolTable* genericsScope = scope->createChildBlock("generics");
  scope = genericsScope;
  node->setGenericScope(genericsScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(genericsScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  if (currentClassType != nullptr) {
    Type* selfPtr = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, currentClassType));
    paramFields.push_back(std::make_unique<Field>("self", selfPtr));
    paramTypes.push_back(selfPtr);
  }
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    // Match codegen: function params use pointer-to-class so lookup matches
    if (paramType->is(BaseType::TY_CLASS)) {
      paramType = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, paramType));
    }
    if (param->defaultVal != nullptr) {
      param->defaultVal->accept(*this);
      if (!isAssignableTo(result->getType(), paramType)) {
        throw TypeCheckError(param->defaultVal->getSpan(),
                             "Default value type {} is not assignable to parameter type {}",
                             result->getType()->toString(), paramType->toString());
      }
    }
    paramTypes.push_back(paramType);
    std::unique_ptr<Field> field;
    if (param->defaultVal != nullptr) {
      field = std::make_unique<Field>(param->name, paramType, std::make_unique<Value>(paramType));
    } else {
      field = std::make_unique<Field>(param->name, paramType);
    }
    paramFields.push_back(std::move(field));
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  Type* funcTypePtr = cacheType(std::move(funcType));
  funcTypePtr->setGenericParams(node->getGenericParams());
  SymbolTable* insertScope =
      currentMethodInsertScope != nullptr ? currentMethodInsertScope : scope->getParent();
  Value* funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes);

  if (declarationPass) {
    if (funcSymbol == nullptr) {
      auto declaredFunc = std::make_unique<Value>(node->getName(), funcTypePtr);
      declaredFunc->setCategory(ValueCategory::CALLABLE_SYMBOL);
      declaredFunc->setExported(node->isExported());
      declaredFunc->setDeclarationSpan(node->getNameSpan());
      declaredFunc->setDeclarationFilePath(mainFilePath);
      insertScope->insertSymbol(std::move(declaredFunc));
      funcSymbol = insertScope->lookupFunction(node->getName(), paramTypes);
      // Set resolvedSymbol immediately after we get the symbol for this exact overload
      if (funcSymbol != nullptr) {
        node->setResolvedSymbol(funcSymbol);
      }
    } else {
      funcSymbol->setType(funcTypePtr);
      funcSymbol->setExported(node->isExported());
      funcSymbol->setDeclarationSpan(node->getNameSpan());
      funcSymbol->setDeclarationFilePath(mainFilePath);
      // Set resolvedSymbol for existing symbol (this exact overload)
      node->setResolvedSymbol(funcSymbol);
    }

    if (funcSymbol != nullptr && funcSymbol->getBodyScope() == nullptr) {
      SymbolTable* child = scope->createChildBlock("function");
      funcSymbol->setBodyScope(child);
      SymbolTable* savedScopePtr = scope;
      scope = child;
      const size_t paramOffset = (currentClassType != nullptr) ? 1U : 0U;
      for (size_t i = 0; i < node->getParameters().size(); ++i) {
        Parameter* param = node->getParameters()[i];
        auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[paramOffset + i]);
        paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
        paramSymbol->setDeclarationSpan(param->nameSpan);
        paramSymbol->setDeclarationFilePath(mainFilePath);
        param->setResolvedSymbol(paramSymbol.get());
        scope->insertSymbol(std::move(paramSymbol));
      }
      scope = savedScopePtr;
    }
  } else {
    currentFunction = funcSymbol;
    if (currentFunction == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found during definition pass: {}",
                           node->getName());
    }
    // Set resolvedSymbol in definition pass (should already be set in declaration pass, but ensure
    // it)
    node->setResolvedSymbol(currentFunction);
    scope = currentFunction->getBodyScope();
    inTopLevel = false;
    node->getBody()->accept(*this);
    Type* funcReturnType = currentFunction->getType()->getReturnType();
    if (funcReturnType != nullptr && !funcReturnType->is(BaseType::TY_VOID) &&
        !blockAlwaysReturns(node->getBody())) {
      throw TypeCheckError(node->getSpan(), "Non-void function may reach end without returning");
    }
    scope = scope->getParent();
    currentFunction = nullptr;
    inTopLevel = true;
  }
  scope = scope->getParent(); // pop generics scope so generic param names are not visible to outer
                              // lookups
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::visit(const ExternFuncDecl* node) -> void {
  if (!declarationPass) {
    return;
  }
  auto savedGenerics = currentGenericTypes;
  SymbolTable* genericsScope = scope->createChildBlock("generics");
  scope = genericsScope;
  node->setGenericScope(genericsScope);
  for (const auto& param : node->getGenericParamDecls()) {
    auto* genericType = cacheType(std::make_unique<Type>(param.name));
    currentGenericTypes[param.name] = genericType;
  }
  insertGenericParamSymbols(genericsScope, node->getGenericParamDecls(), currentGenericTypes,
                            mainFilePath);
  node->getReturnType()->accept(*this);
  Type* returnType = result->getType();
  std::vector<std::unique_ptr<Field>> paramFields;
  std::vector<Type*> paramTypes;
  for (Parameter* param : node->getParameters()) {
    if (param->type != nullptr) {
      param->type->accept(*this);
    } else {
      throw TypeCheckError(node->getSpan(), "Parameter {} has no type", param->name);
    }
    Type* paramType = result->getType();
    if (param->defaultVal != nullptr) {
      param->defaultVal->accept(*this);
      if (!isAssignableTo(result->getType(), paramType)) {
        throw TypeCheckError(param->defaultVal->getSpan(),
                             "Default value type {} is not assignable to parameter type {}",
                             result->getType()->toString(), paramType->toString());
      }
    }
    paramTypes.push_back(paramType);
    if (param->defaultVal != nullptr) {
      paramFields.push_back(
          std::make_unique<Field>(param->name, paramType, std::make_unique<Value>(paramType)));
    } else {
      paramFields.push_back(std::make_unique<Field>(param->name, paramType));
    }
  }
  auto funcType = std::make_unique<Type>(BaseType::TY_FUNCTION, nullptr, std::move(paramFields));
  funcType->setReturnType(returnType);
  funcType->setVarArgs(node->getVarArgs());
  Type* funcTypePtr = cacheType(std::move(funcType));
  funcTypePtr->setGenericParams(node->getGenericParams());
  Value* existingFunc = scope->getParent()->lookupFunction(node->getName(), paramTypes);
  if (existingFunc == nullptr) {
    auto funcSymbol = std::make_unique<Value>(node->getName(), funcTypePtr);
    funcSymbol->setCategory(ValueCategory::CALLABLE_SYMBOL);
    funcSymbol->setExported(node->isExported());
    funcSymbol->setDeclarationSpan(node->getNameSpan());
    funcSymbol->setDeclarationFilePath(mainFilePath);
    scope->getParent()->insertSymbol(
        std::move(funcSymbol)); // insert into enclosing scope, not generics
    existingFunc = scope->getParent()->lookupFunction(node->getName(), paramTypes);
  } else {
    existingFunc->setType(funcTypePtr);
    existingFunc->setExported(node->isExported());
    existingFunc->setDeclarationSpan(node->getNameSpan());
    existingFunc->setDeclarationFilePath(mainFilePath);
  }
  if (existingFunc != nullptr) {
    node->setResolvedSymbol(existingFunc);
  }
  if (existingFunc != nullptr && existingFunc->getBodyScope() == nullptr) {
    SymbolTable* child = scope->createChildBlock("extern_function");
    existingFunc->setBodyScope(child);
    SymbolTable* savedScopePtr = scope;
    scope = child;
    for (size_t i = 0; i < node->getParameters().size(); ++i) {
      Parameter* param = node->getParameters()[i];
      auto paramSymbol = std::make_unique<Value>(param->name, paramTypes[i]);
      paramSymbol->setCategory(ValueCategory::ADDRESSABLE_STORAGE);
      paramSymbol->setDeclarationSpan(param->nameSpan);
      paramSymbol->setDeclarationFilePath(mainFilePath);
      param->setResolvedSymbol(paramSymbol.get());
      scope->insertSymbol(std::move(paramSymbol));
    }
    scope = savedScopePtr;
  }
  scope = scope->getParent(); // pop generics scope
  currentGenericTypes = std::move(savedGenerics);
}

auto Typechecker::compoundToBinaryOp(TokenType op) -> std::optional<TokenType> {
  switch (op) {
  case TokenType::PLUS_EQUAL:
    return TokenType::PLUS;
  case TokenType::MINUS_EQUAL:
    return TokenType::MINUS;
  case TokenType::STAR_EQUAL:
    return TokenType::STAR;
  case TokenType::SLASH_EQUAL:
    return TokenType::SLASH;
  case TokenType::MOD_EQUAL:
    return TokenType::MOD;
  case TokenType::POWER_EQUAL:
    return TokenType::POWER;
  default:
    return std::nullopt;
  }
}

auto Typechecker::visit(const Assignment* node) -> void {
  TokenType assignOp = node->getOperator();
  std::optional<TokenType> binaryOp = compoundToBinaryOp(assignOp);

  if (auto* lit = dynamic_cast<Literal*>(node->getLeftHandSide())) {
    Value* sym = scope->lookup(lit->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Variable not found: {}", lit->getValue());
    }
    if (!sym->getMutability()) {
      throw TypeCheckError(node->getSpan(), "Cannot assign to immutable variable {}",
                           lit->getValue());
    }
    Type* lhsType = sym->getType();
    node->getRightHandSide()->accept(*this);
    Type* rhsType = result->getType();
    if (binaryOp.has_value()) {
      Type* resultType = typecheckBinaryOpResult(*binaryOp, lhsType, rhsType, node->getSpan());
      if (!isAssignableTo(resultType, lhsType)) {
        throw TypeCheckError(
            node->getSpan(),
            "Compound assignment result type {} is not assignable to variable of type {}",
            resultType->toString(), lhsType->toString());
      }
    } else {
      if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      }
      if (!isAssignableTo(rhsType, lhsType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to variable of type {}",
                             rhsType->toString(), lhsType->toString());
      }
    }
    return;
  }
  if (dynamic_cast<DotOp*>(node->getLeftHandSide()) != nullptr) {
    node->getLeftHandSide()->accept(*this);
    Type* lhsType = result->getType();
    Type* targetType = lhsType;
    node->getRightHandSide()->accept(*this);
    Type* rhsType = result->getType();
    if (binaryOp.has_value()) {
      Type* resultType = typecheckBinaryOpResult(*binaryOp, targetType, rhsType, node->getSpan());
      if (targetType != nullptr && !isAssignableTo(resultType, targetType)) {
        throw TypeCheckError(
            node->getSpan(),
            "Compound assignment result type {} is not assignable to field of type {}",
            resultType->toString(), targetType->toString());
      }
    } else {
      if (assignOp != TokenType::EQUAL) {
        throw TypeCheckError(node->getSpan(), "Unsupported assignment operator: {}",
                             NAMEOF_ENUM(assignOp));
      }
      if (targetType != nullptr && !isAssignableTo(rhsType, targetType)) {
        throw TypeCheckError(node->getSpan(), "Cannot assign type {} to field of type {}",
                             rhsType->toString(), targetType->toString());
      }
    }
    return;
  }
  throw TypeCheckError(node->getSpan(), "Invalid assignment target");
}

auto Typechecker::visit(const Break* node) -> void {
  (void) node;
  // We don't track break targets in typecheck; Codegen will enforce.
}

auto Typechecker::visit(const Continue* node) -> void { (void) node; }

auto Typechecker::visit(const Return* node) -> void {
  if (currentFunction == nullptr) {
    throw TypeCheckError(node->getSpan(), "Return not allowed outside function");
  }
  Type* expected = currentFunction->getType()->getReturnType();
  if (node->getValue() == nullptr) {
    if (expected == nullptr || !expected->is(BaseType::TY_VOID)) {
      throw TypeCheckError(node->getSpan(), "Return type does not match: expected {}, got void",
                           expected != nullptr ? expected->toString() : "?");
    }
    return;
  }
  node->getValue()->accept(*this);
  if (!isAssignableTo(result->getType(), expected)) {
    throw TypeCheckError(node->getSpan(), "Return type does not match: expected {}, got {}",
                         expected->toString(), result->getType()->toString());
  }
}

auto Typechecker::visit(const Defer* node) -> void { node->getStatement()->accept(*this); }

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
  if (!node->getExplicitTypeArgs().empty()) {
    Value* classSym = scope->lookup(node->getName());
    if (classSym != nullptr && classSym->getType()->is(BaseType::TY_CLASS)) {
      node->setResolvedSymbol(classSym);
      Type* classType = classSym->getType();
      const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
      std::vector<Type*> explicitTypes;
      for (TypeExpr* texpr : node->getExplicitTypeArgs()) {
        texpr->accept(*this);
        explicitTypes.push_back(result->getType());
      }
      if (explicitTypes.size() != genericParamNames.size()) {
        throw TypeCheckError(node->getSpan(),
                             "Explicit type argument count {} does not match "
                             "generic class parameter count {}",
                             explicitTypes.size(), genericParamNames.size());
      }
      std::unordered_map<std::string, Type*> env;
      for (size_t i = 0; i < genericParamNames.size(); ++i) {
        env[genericParamNames[i]] = explicitTypes[i];
      }
      if (!argTypes.empty()) {
        Type* ptrToClass = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
        std::vector<Type*> constructorParamTypes = {ptrToClass};
        constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(), argTypes.end());
        Value* constructor = scope->lookupFunction("new", constructorParamTypes);
        if (constructor == nullptr) {
          throw TypeCheckError(node->getSpan(),
                               "Constructor not found for {} with given type arguments",
                               node->getName());
        }
        auto ctorParams = constructor->getType()->getFields();
        for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
          Type* expected = substituteInType(ctorParams[i]->type, env);
          if (expected != nullptr && !argTypes[i - 1]->isEqual(expected)) {
            throw TypeCheckError(node->getSpan(),
                                 "Argument type {} does not match explicit parameter type {}",
                                 argTypes[i - 1]->toString(), expected->toString());
          }
        }
      }
      Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
      result = std::make_unique<Value>(specialized);
      return;
    }
  }

  Value* callee = scope->lookupFunction(node->getName(), argTypes);
  if (callee == nullptr) {
    Value* sym = scope->lookup(node->getName());
    SymbolTable* importedScope = nullptr;
    std::string importedName;
    if (sym == nullptr || sym->getType()->is(BaseType::TY_IMPORT)) {
      auto importedIt = importedNameToSource.find(node->getName());
      if (importedIt != importedNameToSource.end()) {
        importedScope = getOrTypecheckImport(importedIt->second.first);
        importedName = importedIt->second.second;
        if (importedScope != nullptr) {
          sym = importedScope->lookup(importedName);
          callee = importedScope->lookupFunction(importedName, argTypes);
        }
      }
    }
    if (callee != nullptr) {
      node->setResolvedSymbol(callee);
      auto* funcType = callee->getType();
      Type* retType = materializeImportedType(funcType->getReturnType());
      result = std::make_unique<Value>(retType);
      return;
    }
    if (sym != nullptr) {
      node->setResolvedSymbol(sym);
      if (sym->getType()->is(BaseType::TY_CLASS)) {
        Type* classType =
            importedScope != nullptr ? materializeImportedType(sym->getType()) : sym->getType();
        if (!node->getArguments().empty()) {
          const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
          Type* ptrToClass =
              cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
          std::vector<Type*> constructorParamTypes = {ptrToClass};
          for (Type* t : argTypes) {
            constructorParamTypes.push_back(t);
          }
          Value* constructor = importedScope != nullptr
                                   ? importedScope->lookupFunction("new", constructorParamTypes)
                                   : scope->lookupFunction("new", constructorParamTypes);
          if (constructor != nullptr) {
            std::unordered_map<std::string, Type*> env;
            auto* funcType = constructor->getType();
            auto ctorParams = funcType->getFields();
            for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
              inferGenericBindings(ctorParams[i]->type, argTypes[i - 1], env, node->getSpan());
            }
            Type* specialized = getOrCreateSpecializedClassType(classType, genericParamNames, env);
            result = std::make_unique<Value>(
                importedScope != nullptr ? materializeImportedType(specialized) : specialized);
            return;
          }
        }
        result = std::make_unique<Value>(classType);
        return;
      }
      if (sym->getType()->is(BaseType::TY_ENUM)) {
        result = std::make_unique<Value>(
            importedScope != nullptr ? materializeImportedType(sym->getType()) : sym->getType());
        return;
      }
      if (sym->getType()->is(BaseType::TY_IMPORT)) {
        result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
        return;
      }
    }
    throw TypeCheckError(node->getSpan(), "Function not found: {}", node->getName());
  }
  if (!callee->getType()->is(BaseType::TY_FUNCTION)) {
    throw TypeCheckError(node->getSpan(), "Not a function: {}", node->getName());
  }
  node->setResolvedSymbol(callee);

  auto* funcType = callee->getType();
  auto fields = funcType->getFields();

  if (!node->getExplicitTypeArgs().empty()) {
    std::vector<Type*> explicitTypes;
    for (TypeExpr* texpr : node->getExplicitTypeArgs()) {
      texpr->accept(*this);
      explicitTypes.push_back(result->getType());
    }
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(funcType);
    if (explicitTypes.size() != genericParamNames.size()) {
      throw TypeCheckError(node->getSpan(),
                           "Explicit type argument count {} does not match "
                           "generic parameter count {}",
                           explicitTypes.size(), genericParamNames.size());
    }
    std::unordered_map<std::string, Type*> explicitSubst;
    for (size_t i = 0; i < genericParamNames.size(); ++i) {
      explicitSubst[genericParamNames[i]] = explicitTypes[i];
    }
    for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
      Type* expected = substituteInType(fields[i]->type, explicitSubst);
      if (expected != nullptr && !argTypes[i]->isEqual(expected)) {
        throw TypeCheckError(node->getSpan(),
                             "Argument type {} does not match explicit "
                             "parameter type {}",
                             argTypes[i]->toString(), expected->toString());
      }
    }
    if (node->getName() == "new" && !fields.empty() && fields[0]->type->is(BaseType::TY_PTR)) {
      result = std::make_unique<Value>(
          substituteInType(fields[0]->type->getElementType(), explicitSubst));
    } else {
      Type* retType = substituteInType(funcType->getReturnType(), explicitSubst);
      result = std::make_unique<Value>(retType);
    }
    return;
  }

  std::unordered_map<std::string, Type*> localGenericTypes;
  for (size_t i = 0; i < fields.size() && i < argTypes.size(); ++i) {
    inferGenericBindings(fields[i]->type, argTypes[i], localGenericTypes, node->getSpan());
  }
  // Constructor call: result type is the specialized class type (receiver)
  if (node->getName() == "new" && !funcType->getFields().empty() &&
      funcType->getFields()[0]->type->is(BaseType::TY_PTR)) {
    Type* classType = funcType->getFields()[0]->type->getElementType();
    const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(classType);
    Type* specialized =
        getOrCreateSpecializedClassType(classType, genericParamNames, localGenericTypes);
    result = std::make_unique<Value>(specialized);
  } else {
    Type* retType = funcType->getReturnType();
    if (retType != nullptr && retType->is(BaseType::TY_GENERIC)) {
      auto it = localGenericTypes.find(retType->getGenericName());
      if (it != localGenericTypes.end()) {
        retType = it->second;
      }
    }
    result = std::make_unique<Value>(retType);
  }
}

auto Typechecker::visit(const BinaryOp* node) -> void {
  node->getLeft()->accept(*this);
  std::unique_ptr<Value> left = std::move(result);
  node->getRight()->accept(*this);
  std::unique_ptr<Value> right = std::move(result);
  Type* resultType = typecheckBinaryOpResult(node->getOperator(), left->getType(), right->getType(),
                                             node->getSpan());
  result = std::make_unique<Value>(resultType);
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
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_IMPORT)) {
    if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
      std::string alias = result->getName();
      auto pathIt = importAliasToPath.find(alias);
      if (pathIt != importAliasToPath.end()) {
        SymbolTable* importScope = getOrTypecheckImport(pathIt->second);
        if (importScope != nullptr) {
          std::vector<Type*> argTypes;
          for (Expression* arg : fc->getArguments()) {
            arg->accept(*this);
            Type* t = result->getType();
            if (t != nullptr && t->is(BaseType::TY_CLASS)) {
              t = cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, t));
            }
            argTypes.push_back(t);
          }
          Value* func = importScope->lookupFunction(fc->getName(), argTypes);
          if (func == nullptr) {
            Value* sym = importScope->lookup(fc->getName());
            if (sym != nullptr && sym->getType()->is(BaseType::TY_FUNCTION)) {
              func = sym;
            } else if (sym != nullptr && sym->getType()->is(BaseType::TY_CLASS)) {
              fc->setResolvedSymbol(sym);
              Type* classType = materializeImportedType(sym->getType());
              const std::vector<std::string>& genericParamNames =
                  getDeclaredGenericParams(classType);
              if (!fc->getExplicitTypeArgs().empty()) {
                std::vector<Type*> explicitTypes;
                for (TypeExpr* texpr : fc->getExplicitTypeArgs()) {
                  texpr->accept(*this);
                  explicitTypes.push_back(result->getType());
                }
                if (explicitTypes.size() != genericParamNames.size()) {
                  throw TypeCheckError(node->getSpan(),
                                       "Explicit type argument count {} does not match generic "
                                       "class parameter count {}",
                                       explicitTypes.size(), genericParamNames.size());
                }
                std::unordered_map<std::string, Type*> env;
                for (size_t i = 0; i < genericParamNames.size(); ++i) {
                  env[genericParamNames[i]] = explicitTypes[i];
                }
                if (!argTypes.empty()) {
                  Type* ptrToClass =
                      cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
                  std::vector<Type*> constructorParamTypes = {ptrToClass};
                  constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(),
                                               argTypes.end());
                  Value* constructor = importScope->lookupFunction("new", constructorParamTypes);
                  if (constructor == nullptr) {
                    throw TypeCheckError(node->getSpan(),
                                         "Constructor not found for {} with given type arguments",
                                         fc->getName());
                  }
                  auto ctorParams = constructor->getType()->getFields();
                  for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
                    Type* expected = substituteInType(ctorParams[i]->type, env);
                    if (expected != nullptr && !argTypes[i - 1]->isEqual(expected)) {
                      throw TypeCheckError(
                          node->getSpan(),
                          "Argument type {} does not match explicit parameter type {}",
                          argTypes[i - 1]->toString(), expected->toString());
                    }
                  }
                }
                result = std::make_unique<Value>(
                    getOrCreateSpecializedClassType(classType, genericParamNames, env));
                return;
              }
              if (!fc->getArguments().empty()) {
                Type* ptrToClass =
                    cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, classType));
                std::vector<Type*> constructorParamTypes = {ptrToClass};
                constructorParamTypes.insert(constructorParamTypes.end(), argTypes.begin(),
                                             argTypes.end());
                Value* constructor = importScope->lookupFunction("new", constructorParamTypes);
                if (constructor != nullptr) {
                  std::unordered_map<std::string, Type*> env;
                  auto* funcType = constructor->getType();
                  auto ctorParams = funcType->getFields();
                  for (size_t i = 1; i < ctorParams.size() && i - 1 < argTypes.size(); ++i) {
                    inferGenericBindings(ctorParams[i]->type, argTypes[i - 1], env,
                                         node->getSpan());
                  }
                  result = std::make_unique<Value>(materializeImportedType(
                      getOrCreateSpecializedClassType(classType, genericParamNames, env)));
                  return;
                }
              }
              result = std::make_unique<Value>(materializeImportedType(classType));
              return;
            } else if (sym != nullptr && sym->getType()->is(BaseType::TY_ENUM)) {
              result = std::make_unique<Value>(materializeImportedType(sym->getType()));
              return;
            }
          }
          if (func != nullptr) {
            fc->setResolvedSymbol(func);
            Type* retType = func->getType()->getReturnType();
            result = std::make_unique<Value>(
                retType != nullptr ? materializeImportedType(retType)
                                   : cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
            return;
          }
        }
      }
      for (Expression* arg : fc->getArguments()) {
        arg->accept(*this);
      }
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_VOID)));
    return;
  }
  if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
    base = base->getElementType();
  }
  if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
    throw TypeCheckError(node->getSpan(), "Dot operator requires class or enum type, got {}",
                         base->toString());
  }
  if (auto* fc = dynamic_cast<FuncCall*>(node->getRight())) {
    std::vector<Type*> argTypes;
    for (Expression* arg : fc->getArguments()) {
      arg->accept(*this);
      argTypes.push_back(result->getType());
    }
    Type* receiverForLookup = base;
    auto templateIt = specializedTypeToTemplate.find(base);
    if (templateIt != specializedTypeToTemplate.end()) {
      receiverForLookup = templateIt->second;
    }
    Type* selfType =
        receiverForLookup->is(BaseType::TY_PTR)
            ? receiverForLookup
            : cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, receiverForLookup));
    std::vector<Type*> methodArgTypes = {selfType};
    methodArgTypes.insert(methodArgTypes.end(), argTypes.begin(), argTypes.end());
    std::unordered_map<std::string, Type*> methodTypeEnv;
    auto specializedIt = specializedTypeEnv.find(base);
    if (specializedIt != specializedTypeEnv.end()) {
      methodTypeEnv = specializedIt->second;
    }
    Value* method = scope->lookupFunction(fc->getName(), methodArgTypes);
    if (method == nullptr) {
      for (auto& [_, cachedModule] : importedModuleCache) {
        if (cachedModule == nullptr || cachedModule->rootScope == nullptr) {
          continue;
        }
        method = cachedModule->rootScope->lookupFunction(fc->getName(), methodArgTypes);
        if (method != nullptr) {
          break;
        }
      }
    }
    if (method == nullptr) {
      throw TypeCheckError(node->getSpan(), "Function not found: {}", fc->getName());
    }
    fc->setResolvedSymbol(method);
    auto* methodType = method->getType();
    if (!fc->getExplicitTypeArgs().empty()) {
      std::vector<Type*> explicitTypes;
      explicitTypes.reserve(fc->getExplicitTypeArgs().size());
      for (TypeExpr* texpr : fc->getExplicitTypeArgs()) {
        explicitTypes.push_back(resolveType(texpr));
      }
      const std::vector<std::string>& genericParamNames = getDeclaredGenericParams(methodType);
      if (explicitTypes.size() != genericParamNames.size()) {
        throw TypeCheckError(node->getSpan(),
                             "Explicit type argument count {} does not match "
                             "generic parameter count {}",
                             explicitTypes.size(), genericParamNames.size());
      }
      for (size_t i = 0; i < genericParamNames.size(); ++i) {
        methodTypeEnv[genericParamNames[i]] = explicitTypes[i];
      }
      auto fields = methodType->getFields();
      for (size_t i = 0; i < fields.size() && i < methodArgTypes.size(); ++i) {
        Type* expected = substituteInType(fields[i]->type, methodTypeEnv);
        if (expected != nullptr && !methodArgTypes[i]->isEqual(expected)) {
          throw TypeCheckError(node->getSpan(),
                               "Argument type {} does not match explicit "
                               "parameter type {}",
                               methodArgTypes[i]->toString(), expected->toString());
        }
      }
    }
    Type* retType = methodType->getReturnType();
    if (!methodTypeEnv.empty() && retType != nullptr) {
      retType = substituteInType(retType, methodTypeEnv);
    }
    result = std::make_unique<Value>(retType);
    return;
  }
  if (dynamic_cast<Literal*>(node->getRight()) == nullptr) {
    throw TypeCheckError(node->getSpan(), "Expected field name or method call after dot");
  }
  auto* rightLit = dynamic_cast<Literal*>(node->getRight());
  Field* field = TypeUtils::findFieldInFields(base, rightLit->getValue());
  if (field == nullptr || field->type == nullptr) {
    throw TypeCheckError(node->getSpan(), "Unknown field: {}", rightLit->getValue());
  }
  if (field->getDeclarationSymbol() != nullptr) {
    rightLit->setResolvedSymbol(field->getDeclarationSymbol());
  }
  result = std::make_unique<Value>(field->type);
}

auto Typechecker::visit(const CastOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* from = result->getType();
  node->getType()->accept(*this);
  Type* to = result->getType();
  if (!isAssignableTo(from, to)) {
    throw TypeCheckError(node->getSpan(), "Cannot cast from {} to {}", from->toString(),
                         to->toString());
  }
  result = std::make_unique<Value>(to);
}

auto Typechecker::visit(const IsOp* node) -> void {
  node->getLeft()->accept(*this);
  node->getRight()->accept(*this);
  result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
}

auto Typechecker::visit(const UnaryOp* node) -> void {
  node->getExpression()->accept(*this);
  Type* operand = result->getType();
  switch (node->getOperator()) {
  case TokenType::MINUS:
    if (operand != nullptr && operand->is(BaseType::TY_GENERIC)) {
      result = std::make_unique<Value>(operand);
      break;
    }
    if (operand == nullptr || !operand->isOneOf({BaseType::TY_INT, BaseType::TY_FLOAT})) {
      throw TypeCheckError(node->getSpan(), "Unary minus requires numeric type");
    }
    result = std::make_unique<Value>(operand);
    break;
  case TokenType::BANG:
  case TokenType::NOT:
    if (operand != nullptr && operand->is(BaseType::TY_GENERIC)) {
      result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
      break;
    }
    if (operand == nullptr || !operand->is(BaseType::TY_BOOL)) {
      throw TypeCheckError(node->getSpan(), "Unary not requires Bool type");
    }
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::AMPERSAND:
    if (operand == nullptr) {
      throw TypeCheckError(node->getSpan(), "Address-of requires a value");
    }
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, operand)));
    break;
  case TokenType::STAR:
    if (operand == nullptr || !operand->is(BaseType::TY_PTR)) {
      throw TypeCheckError(node->getSpan(), "Dereference requires pointer type");
    }
    result = std::make_unique<Value>(operand->getElementType() != nullptr
                                         ? operand->getElementType()
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
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_INT)));
    break;
  case TokenType::DOUBLE:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_FLOAT)));
    break;
  case TokenType::STRING:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_STRING)));
    break;
  case TokenType::BOOL:
  case TokenType::TRUE_:
  case TokenType::FALSE_:
    result = std::make_unique<Value>(cacheType(std::make_unique<Type>(BaseType::TY_BOOL)));
    break;
  case TokenType::NIL:
    result = std::make_unique<Value>(
        cacheType(std::make_unique<Type>(BaseType::TY_PTR, nullptr, nullptr)));
    break;
  case TokenType::IDENTIFIER: {
    Value* sym = scope->lookup(node->getValue());
    if (sym == nullptr) {
      throw TypeCheckError(node->getSpan(), "Unknown name: {}", node->getValue());
    }
    node->setResolvedSymbol(sym);
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
