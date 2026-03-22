#pragma once

#include <cstddef>

#include "llvm/ADT/StringRef.h"

namespace lesma::lsp_srv {

/** Map LSP (line, character) with UTF-16 code units to a byte offset in the UTF-8 buffer. */
[[nodiscard]] auto bufferByteOffsetFromLspPosition(llvm::StringRef utf8Text, unsigned line,
                                                 unsigned characterUtf16) -> std::size_t;

} // namespace lesma::lsp_srv
