#pragma once

#include <cstddef>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SMLoc.h>
#include <llvm/Support/SourceMgr.h>

#include "liblesma/AST/AST.h"

namespace lesma::lsp_srv {

/** With LSP UTF-8 position encoding: \p line is 0-based, \p characterUtf8 is 0-based UTF-8 code
 * units (bytes) from the start of that line. Returns a byte offset into \p utf8Text. */
[[nodiscard]] auto bufferByteOffsetFromLspUtf8Position(llvm::StringRef utf8Text, unsigned line,
                                                       unsigned characterUtf8) -> std::size_t;

/** Byte offset of \p loc within \p bufferId (0 if \p loc is invalid, not in that buffer, or the
 * buffer is missing). */
[[nodiscard]] auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc)
    -> unsigned;

/** True if both ranges are valid and refer to the same source extent in \p bufferId. */
[[nodiscard]] auto smRangesEqual(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange lhs,
                                 llvm::SMRange rhs) -> bool;

/** Extracts a slash-star-star doc comment immediately above the declaration line (blank lines
 * between the comment block and the declaration are skipped). Lines are normalized and joined
 * with Markdown hard breaks (`  \\n`) so each source line renders on its own line in LSP
 * Markdown (hover, completion docs). Plain non-doc block comments are ignored. */
[[nodiscard]] auto extractBlockCommentDocumentationAboveDecl(llvm::StringRef buffer,
                                                             std::size_t declarationByteOffset)
    -> std::string;

/** If there is no slash-star-star doc block, collects contiguous slash-slash line comments
 * immediately above the declaration (blank lines between those comments and the declaration are
 * skipped). Each line's comment prefix and one following space are stripped; lines are joined
 * with Markdown hard breaks. */
[[nodiscard]] auto extractLineCommentDocumentationAboveDecl(llvm::StringRef buffer,
                                                            std::size_t declarationByteOffset)
    -> std::string;

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
