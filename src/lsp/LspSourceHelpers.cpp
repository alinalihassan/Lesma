#include "LspSourceHelpers.h"

namespace lesma::lsp_srv {

auto bufferByteOffsetFromLspUtf8Position(llvm::StringRef utf8Text, unsigned line,
                                         unsigned characterUtf8) -> std::size_t {
  std::size_t i = 0;
  unsigned currentLine = 0;
  while (i < utf8Text.size() && currentLine < line) {
    if (utf8Text[i] == '\n') {
      ++currentLine;
    }
    ++i;
  }
  if (currentLine != line) {
    return utf8Text.size();
  }
  std::size_t const lineStart = i;
  std::size_t lineEnd = lineStart;
  while (lineEnd < utf8Text.size() && utf8Text[lineEnd] != '\n') {
    ++lineEnd;
  }
  std::size_t const lineByteLen = lineEnd - lineStart;
  std::size_t const col = characterUtf8;
  if (col > lineByteLen) {
    return lineEnd;
  }
  return lineStart + col;
}

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

auto smRangesEqual(llvm::SourceMgr* srcMgr, unsigned bufferId, llvm::SMRange lhs, llvm::SMRange rhs)
    -> bool {
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
