#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "llvm/Support/SourceMgr.h"
#include <llvm/Support/SMLoc.h>

#include "liblesma/Token/TokenType.h"

namespace lesma {
struct Token {
  std::string lexeme;
  TokenType type = TokenType::NULL_TOKEN;
  llvm::SMRange span;

  Token() = default;
  Token(const TokenType& type, std::string lexeme, llvm::SMRange span)
      : lexeme(std::move(lexeme)), type(type), span(span) {}

  [[nodiscard]] auto getStart() const -> llvm::SMLoc { return span.Start; }
  [[nodiscard]] auto getEnd() const -> llvm::SMLoc { return span.End; };

  static auto getIdentifierType(const std::string& identifier, Token* lastTok)
      -> TokenType;
  [[nodiscard]] auto dump(const std::shared_ptr<llvm::SourceMgr>& srcMgr) const
      -> std::string;

  auto operator==(const Token& rhs) const -> bool {
    return (lexeme == rhs.lexeme) && (type == rhs.type) &&
           (span.Start.getPointer() == rhs.span.Start.getPointer()) &&
           (span.End.getPointer() == rhs.span.End.getPointer());
  }
  auto operator!=(const Token& rhs) const -> bool { return !operator==(rhs); }

  friend auto operator<<(std::ostream& os, const Token& tok) -> std::ostream& {
    os << *tok.span.Start.getPointer() << " - " << *tok.span.End.getPointer();
    return os;
  }
};
} // namespace lesma