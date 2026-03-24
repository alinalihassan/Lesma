#include "LspUtf16.h"

namespace lesma::lsp_srv {
namespace {

auto utf8CodepointByteLength(unsigned char lead) -> std::size_t {
  if ((lead & 0x80U) == 0U) {
    return 1U;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2U;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3U;
  }
  if ((lead & 0xF8U) == 0xF0U) {
    return 4U;
  }
  return 1U;
}

auto utf16UnitsForCodepoint(char32_t cp) -> unsigned {
  if (cp <= 0xFFFFU) {
    return 1U;
  }
  return 2U;
}

void decodeUtf8AndAdvance(llvm::StringRef text, std::size_t& i, char32_t& outCp) {
  if (i >= text.size()) {
    outCp = 0;
    return;
  }
  auto const lead = static_cast<unsigned char>(text.substr(i).front());
  std::size_t const len = utf8CodepointByteLength(lead);
  if (i + len > text.size()) {
    outCp = 0xFFFDU;
    i += 1;
    return;
  }
  if (len == 1U) {
    outCp = lead;
    i += 1;
    return;
  }
  if (len == 2U) {
    auto const c1 = static_cast<unsigned char>(text.substr(i + 1U).front());
    if ((c1 & 0xC0U) != 0x80U) {
      outCp = 0xFFFDU;
      i += 1;
      return;
    }
    outCp = (static_cast<char32_t>(lead & 0x1FU) << 6) | static_cast<char32_t>(c1 & 0x3FU);
    i += 2;
    return;
  }
  if (len == 3U) {
    auto const c1 = static_cast<unsigned char>(text.substr(i + 1U).front());
    auto const c2 = static_cast<unsigned char>(text.substr(i + 2U).front());
    if ((c1 & 0xC0U) != 0x80U || (c2 & 0xC0U) != 0x80U) {
      outCp = 0xFFFDU;
      i += 1;
      return;
    }
    outCp = (static_cast<char32_t>(lead & 0x0FU) << 12) | (static_cast<char32_t>(c1 & 0x3FU) << 6) |
            static_cast<char32_t>(c2 & 0x3FU);
    i += 3;
    return;
  }
  auto const c1 = static_cast<unsigned char>(text.substr(i + 1U).front());
  auto const c2 = static_cast<unsigned char>(text.substr(i + 2U).front());
  auto const c3 = static_cast<unsigned char>(text.substr(i + 3U).front());
  if ((c1 & 0xC0U) != 0x80U || (c2 & 0xC0U) != 0x80U || (c3 & 0xC0U) != 0x80U) {
    outCp = 0xFFFDU;
    i += 1;
    return;
  }
  outCp = (static_cast<char32_t>(lead & 0x07U) << 18) | (static_cast<char32_t>(c1 & 0x3FU) << 12) |
          (static_cast<char32_t>(c2 & 0x3FU) << 6) | static_cast<char32_t>(c3 & 0x3FU);
  i += 4;
}

} // namespace

auto bufferByteOffsetFromLspPosition(llvm::StringRef utf8Text, unsigned line,
                                     unsigned characterUtf16) -> std::size_t {
  std::size_t i = 0;
  unsigned currentLine = 0;
  while (i < utf8Text.size() && currentLine < line) {
    if (utf8Text.substr(i).front() == '\n') {
      ++currentLine;
    }
    ++i;
  }
  if (currentLine != line) {
    return utf8Text.size();
  }
  std::size_t const lineStart = i;
  std::size_t lineEnd = lineStart;
  while (lineEnd < utf8Text.size() && utf8Text.substr(lineEnd).front() != '\n') {
    ++lineEnd;
  }

  std::size_t j = lineStart;
  unsigned utf16Before = 0;
  while (j < lineEnd) {
    if (utf16Before == characterUtf16) {
      return j;
    }
    std::size_t const prevJ = j;
    char32_t cp = 0;
    decodeUtf8AndAdvance(utf8Text, j, cp);
    utf16Before += utf16UnitsForCodepoint(cp);
    if (utf16Before > characterUtf16) {
      return prevJ;
    }
  }
  if (utf16Before == characterUtf16) {
    return j;
  }
  return lineEnd;
}

} // namespace lesma::lsp_srv
