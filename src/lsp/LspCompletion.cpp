#include "LspCompletion.h"
#include "LspUtf16.h"

#include <cctype>

#include "llvm/Support/SourceMgr.h"

#include "liblesma/AST/AST.h"
#include "liblesma/Symbol/SymbolTable.h"
#include "liblesma/Symbol/Type.h"
#include "liblesma/Symbol/TypeUtils.h"
#include "liblesma/Symbol/Value.h"
#include "liblesma/Token/TokenType.h"

namespace lesma::lsp_srv {
namespace {

using namespace lesma;

auto positionToOffset(llvm::StringRef ref, unsigned line, unsigned character) -> unsigned {
  return static_cast<unsigned>(
      bufferByteOffsetFromLspPosition(ref, line, character));
}

auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) -> unsigned {
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (!buf) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

auto isIdentChar(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/**
 * When the cursor is immediately after `.` on the same line, and the LHS is a single
 * identifier (e.g. `holder.`), return that identifier. Used when the document does not
 * parse as a complete DotOp (no RHS yet).
 */
auto extractSimpleLhsBeforeDot(llvm::StringRef text, unsigned offset) -> std::string {
  if (offset == 0) {
    return {};
  }
  if (text[static_cast<size_t>(offset) - 1U] != '.') {
    return {};
  }
  size_t dot = static_cast<size_t>(offset) - 1U;
  if (dot == 0) {
    return {};
  }
  size_t idStart = dot;
  while (idStart > 0 && isIdentChar(text[idStart - 1U])) {
    --idStart;
  }
  if (idStart >= dot) {
    return {};
  }
  // LHS must be one identifier only (no nested `a.b.` yet)
  for (size_t i = idStart; i < dot; ++i) {
    if (text[i] == '.') {
      return {};
    }
  }
  return std::string(text.data() + idStart, dot - idStart);
}

void collectDotOpsFromExpr(Expression* e, std::vector<DotOp*>& out);
void collectDotOpsFromStmt(Statement* s, std::vector<DotOp*>& out);

void collectDotOpsFromExpr(Expression* e, std::vector<DotOp*>& out) {
  if (e == nullptr) {
    return;
  }
  if (auto* d = dynamic_cast<DotOp*>(e)) {
    collectDotOpsFromExpr(d->getLeft(), out);
    collectDotOpsFromExpr(d->getRight(), out);
    out.push_back(d);
    return;
  }
  if (auto* fc = dynamic_cast<FuncCall*>(e)) {
    for (Expression* a : fc->getArguments()) {
      collectDotOpsFromExpr(a, out);
    }
    return;
  }
  if (auto* bin = dynamic_cast<BinaryOp*>(e)) {
    collectDotOpsFromExpr(bin->getLeft(), out);
    collectDotOpsFromExpr(bin->getRight(), out);
    return;
  }
  if (auto* un = dynamic_cast<UnaryOp*>(e)) {
    collectDotOpsFromExpr(un->getExpression(), out);
    return;
  }
  if (auto* castOp = dynamic_cast<CastOp*>(e)) {
    collectDotOpsFromExpr(castOp->getExpression(), out);
    return;
  }
  if (auto* isOp = dynamic_cast<IsOp*>(e)) {
    collectDotOpsFromExpr(isOp->getLeft(), out);
    return;
  }
}

void collectDotOpsFromStmt(Statement* s, std::vector<DotOp*>& out) {
  if (s == nullptr) {
    return;
  }
  if (auto* es = dynamic_cast<ExpressionStatement*>(s)) {
    collectDotOpsFromExpr(es->getExpression(), out);
  } else if (auto* vd = dynamic_cast<VarDecl*>(s)) {
    if (vd->getValue() != nullptr) {
      collectDotOpsFromExpr(vd->getValue(), out);
    }
  } else if (auto* asn = dynamic_cast<Assignment*>(s)) {
    collectDotOpsFromExpr(asn->getLeftHandSide(), out);
    collectDotOpsFromExpr(asn->getRightHandSide(), out);
  } else if (auto* iff = dynamic_cast<If*>(s)) {
    for (Expression* c : iff->getConds()) {
      collectDotOpsFromExpr(c, out);
    }
    for (Compound* b : iff->getBlocks()) {
      collectDotOpsFromStmt(b, out);
    }
  } else if (auto* w = dynamic_cast<While*>(s)) {
    collectDotOpsFromExpr(w->getCond(), out);
    collectDotOpsFromStmt(w->getBlock(), out);
  } else if (auto* r = dynamic_cast<Return*>(s)) {
    if (r->getValue() != nullptr) {
      collectDotOpsFromExpr(r->getValue(), out);
    }
  } else if (auto* d = dynamic_cast<Defer*>(s)) {
    collectDotOpsFromStmt(d->getStatement(), out);
  } else if (auto* comp = dynamic_cast<Compound*>(s)) {
    for (Statement* ch : comp->getChildren()) {
      collectDotOpsFromStmt(ch, out);
    }
  } else if (auto* fd = dynamic_cast<FuncDecl*>(s)) {
    if (fd->getBody() != nullptr) {
      collectDotOpsFromStmt(fd->getBody(), out);
    }
  } else if (auto* cls = dynamic_cast<Class*>(s)) {
    for (FuncDecl* m : cls->getMethods()) {
      if (m->getBody() != nullptr) {
        collectDotOpsFromStmt(m->getBody(), out);
      }
    }
  }
}

auto resolveExprType(Expression* e, SymbolTable* root) -> Type* {
  if (e == nullptr) {
    return nullptr;
  }
  if (auto* lit = dynamic_cast<Literal*>(e)) {
    if (lit->getType() == TokenType::IDENTIFIER) {
      Value* v = root->lookup(lit->getValue());
      return v != nullptr ? v->getType() : nullptr;
    }
    return nullptr;
  }
  if (auto* dot = dynamic_cast<DotOp*>(e)) {
    Type* base = resolveExprType(dot->getLeft(), root);
    if (base == nullptr) {
      return nullptr;
    }
    if (base->is(BaseType::TY_PTR) && base->getElementType() != nullptr) {
      base = base->getElementType();
    }
    if (!base->is(BaseType::TY_CLASS) && !base->is(BaseType::TY_ENUM)) {
      return nullptr;
    }
    if (auto* rlit = dynamic_cast<Literal*>(dot->getRight())) {
      if (rlit->getType() == TokenType::IDENTIFIER) {
        return TypeUtils::findTypeInFields(base, rlit->getValue());
      }
    }
    return nullptr;
  }
  return nullptr;
}

void appendMembersForClassLike(Type* baseClass, Compound* ast, SymbolTable* root,
                                 std::vector<std::string>& out) {
  if (baseClass == nullptr) {
    return;
  }
  if (baseClass->is(BaseType::TY_PTR) && baseClass->getElementType() != nullptr) {
    baseClass = baseClass->getElementType();
  }
  if (!baseClass->is(BaseType::TY_CLASS) && !baseClass->is(BaseType::TY_ENUM)) {
    return;
  }
  for (Field* f : baseClass->getFields()) {
    out.push_back(f->name);
  }
  if (!baseClass->is(BaseType::TY_CLASS)) {
    return;
  }
  for (Statement* stmt : ast->getChildren()) {
    auto* cls = dynamic_cast<Class*>(stmt);
    if (cls == nullptr) {
      continue;
    }
    Type* ct = root->lookupType(cls->getIdentifier());
    if (ct != nullptr && ct->isEqual(baseClass)) {
      for (FuncDecl* m : cls->getMethods()) {
        out.push_back(m->getName());
      }
      return;
    }
  }
}

auto findDotOpForCompletion(Compound* ast, unsigned offset, llvm::SourceMgr* srcMgr, unsigned bufferId)
    -> DotOp* {
  std::vector<DotOp*> dots;
  for (Statement* stmt : ast->getChildren()) {
    collectDotOpsFromStmt(stmt, dots);
  }
  DotOp* best = nullptr;
  unsigned bestRightStart = 0;
  for (DotOp* d : dots) {
    Expression* r = d->getRight();
    if (r == nullptr) {
      continue;
    }
    llvm::SMRange rspan = r->getSpan();
    if (!rspan.isValid()) {
      continue;
    }
    unsigned rs = getOffsetFromSMLoc(srcMgr, bufferId, rspan.Start);
    unsigned re = getOffsetFromSMLoc(srcMgr, bufferId, rspan.End);
    if (offset >= rs && offset <= re && (!best || rs >= bestRightStart)) {
      best = d;
      bestRightStart = rs;
    }
  }
  return best;
}

} // namespace

auto memberCompletionNames(const AnalysisResult& result, unsigned line, unsigned character)
    -> std::vector<std::string> {
  std::vector<std::string> names;
  // Do not require `!hasErrors()`: we still want member names when the file has unrelated
  // diagnostics, as long as typecheck produced a scope.
  if (result.parser == nullptr || result.rootScope == nullptr) {
    return names;
  }
  Compound* ast = result.parser->getAst();
  if (ast == nullptr) {
    return names;
  }
  llvm::SourceMgr* srcMgr = result.sourceMgr.get();
  unsigned const bufferId = result.mainBufferId;
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return names;
  }
  llvm::StringRef text = buf->getBuffer();
  unsigned const offset = positionToOffset(text, line, character);

  SymbolTable* root = result.rootScope.get();

  DotOp* dot = findDotOpForCompletion(ast, offset, srcMgr, bufferId);
  Type* baseType = nullptr;
  if (dot != nullptr) {
    baseType = resolveExprType(dot->getLeft(), root);
  } else {
    std::string lhs = extractSimpleLhsBeforeDot(text, offset);
    if (!lhs.empty()) {
      Value* v = root->lookup(lhs);
      if (v != nullptr) {
        baseType = v->getType();
      }
    }
  }

  if (baseType == nullptr) {
    return names;
  }
  appendMembersForClassLike(baseType, ast, root, names);
  return names;
}

} // namespace lesma::lsp_srv
