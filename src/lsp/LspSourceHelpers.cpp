#include "LspSourceHelpers.h"

namespace lesma::lsp_srv {

auto getOffsetFromSMLoc(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMLoc loc) -> unsigned {
  if (srcMgr == nullptr) {
    return 0U;
  }
  auto const* buf = srcMgr->getMemoryBuffer(bufferId);
  if (buf == nullptr) {
    return 0U;
  }
  return static_cast<unsigned>(loc.getPointer() - buf->getBufferStart());
}

auto smRangesEqual(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange lhs,
                   llvm::SMRange rhs) -> bool {
  if (!lhs.isValid() || !rhs.isValid()) {
    return false;
  }
  if (srcMgr == nullptr) {
    return lhs.Start == rhs.Start && lhs.End == rhs.End;
  }
  return getOffsetFromSMLoc(srcMgr, bufferId, lhs.Start) ==
             getOffsetFromSMLoc(srcMgr, bufferId, rhs.Start) &&
         getOffsetFromSMLoc(srcMgr, bufferId, lhs.End) ==
             getOffsetFromSMLoc(srcMgr, bufferId, rhs.End);
}

} // namespace lesma::lsp_srv
