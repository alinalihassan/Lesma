#pragma once

#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include "liblesma/AST/AST.h"

namespace lesma::lsp_srv {

/** Byte offset of \p loc within the given buffer (0 if buffer missing). */
[[nodiscard]] auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc)
    -> unsigned;

/** True if both ranges are valid and refer to the same source extent in \p bufferId. */
[[nodiscard]] auto smRangesEqual(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange lhs,
                                 llvm::SMRange rhs) -> bool;

template <typename FuncDeclT, typename ClassT>
struct InnermostFuncAtOffset {
  FuncDeclT* func = nullptr;
  ClassT* enclosingClass = nullptr;
};

template <typename FuncDeclT, typename ClassT>
void considerFuncForInnermost(FuncDeclT* func, ClassT* cls, unsigned targetOffset,
                              llvm::SourceMgr* srcMgr, unsigned bufferId,
                              InnermostFuncAtOffset<FuncDeclT, ClassT>& best, unsigned& bestLen) {
  if (func == nullptr || func->getBody() == nullptr) {
    return;
  }
  llvm::SMRange const span = func->getBody()->getSpan();
  if (!span.isValid()) {
    return;
  }
  unsigned const start = getOffsetFromSMLoc(srcMgr, bufferId, span.Start);
  unsigned const end = getOffsetFromSMLoc(srcMgr, bufferId, span.End);
  if (targetOffset < start || targetOffset >= end) {
    return;
  }
  unsigned const len = end - start;
  if (best.func == nullptr || len < bestLen) {
    best.func = func;
    best.enclosingClass = cls;
    bestLen = len;
  }
}

template <typename FuncDeclT, typename ClassT>
void scanCompoundForFuncsForInnermost(lesma::Compound* compound, ClassT* cls, unsigned targetOffset,
                                      llvm::SourceMgr* srcMgr, unsigned bufferId,
                                      InnermostFuncAtOffset<FuncDeclT, ClassT>& best,
                                      unsigned& bestLen) {
  if (compound == nullptr) {
    return;
  }
  for (lesma::Statement* stmt : compound->getChildren()) {
    if (auto* f = dynamic_cast<FuncDeclT*>(stmt)) {
      considerFuncForInnermost(f, cls, targetOffset, srcMgr, bufferId, best, bestLen);
      scanCompoundForFuncsForInnermost(f->getBody(), cls, targetOffset, srcMgr, bufferId, best,
                                       bestLen);
    } else if (auto* klass = dynamic_cast<lesma::Class*>(stmt)) {
      auto* clsPtr = static_cast<ClassT*>(klass);
      for (lesma::FuncDecl* method : klass->getMethods()) {
        auto* methodAsFuncT = dynamic_cast<FuncDeclT*>(method);
        if (methodAsFuncT == nullptr) {
          continue;
        }
        considerFuncForInnermost(methodAsFuncT, clsPtr, targetOffset, srcMgr, bufferId, best,
                                 bestLen);
        scanCompoundForFuncsForInnermost(method->getBody(), clsPtr, targetOffset, srcMgr, bufferId,
                                         best, bestLen);
      }
    }
  }
}

template <typename FuncDeclT, typename ClassT>
[[nodiscard]] auto findInnermostFuncContainingAst(lesma::Compound* ast, unsigned targetOffset,
                                                  llvm::SourceMgr* srcMgr, unsigned bufferId)
    -> InnermostFuncAtOffset<FuncDeclT, ClassT> {
  InnermostFuncAtOffset<FuncDeclT, ClassT> best;
  unsigned bestLen = 0U;
  scanCompoundForFuncsForInnermost(ast, static_cast<ClassT*>(nullptr), targetOffset, srcMgr,
                                   bufferId, best, bestLen);
  return best;
}

} // namespace lesma::lsp_srv
