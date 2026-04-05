#include "LspSourceHelpers.h"

#include <algorithm>
#include <vector>

namespace lesma::lsp_srv {

namespace {

[[nodiscard]] auto lineIndexAtOffset(llvm::StringRef buf, std::size_t offset) -> unsigned {
  offset = std::min(offset, buf.size());
  unsigned line = 0;
  for (std::size_t i = 0; i < offset; ++i) {
    if (buf[i] == '\n') {
      ++line;
    }
  }
  return line;
}

[[nodiscard]] auto lineBoundsByIndex(llvm::StringRef buf, unsigned lineIdx, std::size_t& outStart,
                                     std::size_t& outEndExcl) -> bool {
  unsigned cur = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= buf.size(); ++i) {
    bool const atEnd = (i == buf.size());
    if (atEnd || buf[i] == '\n') {
      if (cur == lineIdx) {
        outStart = start;
        outEndExcl = i;
        return true;
      }
      ++cur;
      start = i + 1;
      if (atEnd) {
        break;
      }
    }
  }
  return false;
}

[[nodiscard]] auto trimmedLineEndsBlockComment(llvm::StringRef line) -> bool {
  return line.ltrim(" \t\r").contains("*/");
}

[[nodiscard]] auto normalizeBlockCommentLine(llvm::StringRef line, bool isFirst, bool isLast)
    -> std::string {
  llvm::StringRef t = line.ltrim(" \t\r");
  if (isFirst) {
    size_t const start = t.find("/*");
    if (start != llvm::StringRef::npos) {
      t = t.drop_front(start + 2);
    }
  }
  if (isLast) {
    size_t const end = t.rfind("*/");
    if (end != llvm::StringRef::npos) {
      t = t.take_front(end);
    }
  }
  t = t.ltrim(" \t");
  if (t.starts_with("*")) {
    t = t.drop_front(1).ltrim(" \t");
  }
  return std::string(t);
}

[[nodiscard]] auto lineCommentBody(llvm::StringRef line) -> std::string {
  llvm::StringRef t = line.ltrim(" \t\r");
  if (!t.starts_with("//")) {
    return {};
  }
  t = t.drop_front(2);
  if (t.starts_with(" ")) {
    t = t.drop_front(1);
  }
  return std::string(t);
}

[[nodiscard]] auto isLineCommentLine(llvm::StringRef line) -> bool {
  return line.ltrim(" \t\r").starts_with("//");
}

} // namespace

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
  if (srcMgr == nullptr || !loc.isValid()) {
    return 0U;
  }
  if (srcMgr->FindBufferContainingLoc(loc) != bufferId) {
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
  if (!lhs.Start.isValid() || !lhs.End.isValid() || !rhs.Start.isValid() || !rhs.End.isValid()) {
    return false;
  }
  if (srcMgr == nullptr) {
    return lhs.Start == rhs.Start && lhs.End == rhs.End;
  }
  auto const sameBuffer = [&](llvm::SMLoc loc) -> bool {
    return srcMgr->FindBufferContainingLoc(loc) == bufferId;
  };
  if (!sameBuffer(lhs.Start) || !sameBuffer(lhs.End) || !sameBuffer(rhs.Start) ||
      !sameBuffer(rhs.End)) {
    return false;
  }
  return getOffsetFromSMLoc(srcMgr, bufferId, lhs.Start) ==
             getOffsetFromSMLoc(srcMgr, bufferId, rhs.Start) &&
         getOffsetFromSMLoc(srcMgr, bufferId, lhs.End) ==
             getOffsetFromSMLoc(srcMgr, bufferId, rhs.End);
}

auto extractBlockCommentDocumentationAboveDecl(llvm::StringRef buffer,
                                               std::size_t declarationByteOffset) -> std::string {
  if (buffer.empty() || declarationByteOffset > buffer.size()) {
    return {};
  }
  unsigned const declLine = lineIndexAtOffset(buffer, declarationByteOffset);
  if (declLine == 0U) {
    return {};
  }
  int scan = static_cast<int>(declLine) - 1;
  while (scan >= 0) {
    std::size_t lineStart = 0;
    std::size_t lineEndExcl = 0;
    if (!lineBoundsByIndex(buffer, static_cast<unsigned>(scan), lineStart, lineEndExcl)) {
      return {};
    }
    llvm::StringRef const lineText = buffer.slice(lineStart, lineEndExcl);
    if (lineText.trim().empty()) {
      --scan;
      continue;
    }
    if (!trimmedLineEndsBlockComment(lineText)) {
      return {};
    }
    std::vector<std::string> linesBottomToTop;
    int c = scan;
    int startLine = scan;
    while (c >= 0) {
      std::size_t ls = 0;
      std::size_t le = 0;
      if (!lineBoundsByIndex(buffer, static_cast<unsigned>(c), ls, le)) {
        break;
      }
      llvm::StringRef const lt = buffer.slice(ls, le);
      linesBottomToTop.push_back(std::string(lt));
      llvm::StringRef const trimmed = lt.ltrim(" \t\r");
      if (trimmed.contains("/**")) {
        startLine = c;
        break;
      }
      if (trimmed.contains("/*")) {
        return {};
      }
      --c;
    }
    if (linesBottomToTop.empty()) {
      return {};
    }
    std::reverse(linesBottomToTop.begin(), linesBottomToTop.end());
    std::string out;
    for (size_t i = 0; i < linesBottomToTop.size(); ++i) {
      if (i != 0U) {
        out += "  \n";
      }
      out += normalizeBlockCommentLine(linesBottomToTop[i], static_cast<int>(i) == 0,
                                       startLine + static_cast<int>(i) == scan);
    }
    return out;
  }
  return {};
}

auto extractLineCommentDocumentationAboveDecl(llvm::StringRef buffer,
                                              std::size_t declarationByteOffset) -> std::string {
  if (buffer.empty() || declarationByteOffset > buffer.size()) {
    return {};
  }
  unsigned const declLine = lineIndexAtOffset(buffer, declarationByteOffset);
  if (declLine == 0U) {
    return {};
  }
  int scan = static_cast<int>(declLine) - 1;
  while (scan >= 0) {
    std::size_t lineStart = 0;
    std::size_t lineEndExcl = 0;
    if (!lineBoundsByIndex(buffer, static_cast<unsigned>(scan), lineStart, lineEndExcl)) {
      return {};
    }
    llvm::StringRef const lineText = buffer.slice(lineStart, lineEndExcl);
    if (lineText.trim().empty()) {
      --scan;
      continue;
    }
    if (!isLineCommentLine(lineText)) {
      return {};
    }
    std::vector<std::string> linesBottomToTop;
    int c = scan;
    while (c >= 0) {
      std::size_t ls = 0;
      std::size_t le = 0;
      if (!lineBoundsByIndex(buffer, static_cast<unsigned>(c), ls, le)) {
        break;
      }
      llvm::StringRef const lt = buffer.slice(ls, le);
      if (lt.trim().empty()) {
        break;
      }
      if (!isLineCommentLine(lt)) {
        break;
      }
      linesBottomToTop.push_back(lineCommentBody(lt));
      --c;
    }
    if (linesBottomToTop.empty()) {
      return {};
    }
    std::reverse(linesBottomToTop.begin(), linesBottomToTop.end());
    std::string out;
    for (size_t i = 0; i < linesBottomToTop.size(); ++i) {
      if (i != 0U) {
        out += "  \n";
      }
      out += linesBottomToTop[i];
    }
    return out;
  }
  return {};
}

} // namespace lesma::lsp_srv
