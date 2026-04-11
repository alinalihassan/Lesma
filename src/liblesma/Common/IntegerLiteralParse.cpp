#include "liblesma/Common/IntegerLiteralParse.h"

#include <climits>
#include <cstddef>
#include <string_view>

namespace lesma {

namespace {

[[nodiscard]] auto digitValueBase2(char c) -> int {
  if (c == '0' || c == '1') {
    return c - '0';
  }
  return -1;
}

[[nodiscard]] auto digitValueBase8(char c) -> int {
  if (c >= '0' && c <= '7') {
    return c - '0';
  }
  return -1;
}

[[nodiscard]] auto digitValueBase10(char c) -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  return -1;
}

[[nodiscard]] auto digitValueBase16(char c) -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

[[nodiscard]] auto parseMagnitude(std::string_view digits, int base) -> std::optional<long long> {
  if (digits.empty()) {
    return std::nullopt;
  }
  auto digitAt = [base](char c) -> int {
    switch (base) {
    case 2:
      return digitValueBase2(c);
    case 8:
      return digitValueBase8(c);
    case 10:
      return digitValueBase10(c);
    case 16:
      return digitValueBase16(c);
    default:
      return -1;
    }
  };
  unsigned long long acc = 0;
  for (char c : digits) {
    int const v = digitAt(c);
    if (v < 0) {
      return std::nullopt;
    }
    auto const baseU = static_cast<unsigned long long>(base);
    auto const vU = static_cast<unsigned long long>(v);
    if (acc > (static_cast<unsigned long long>(LLONG_MAX) - vU) / baseU) {
      return std::nullopt;
    }
    acc = acc * baseU + vU;
  }
  if (acc > static_cast<unsigned long long>(LLONG_MAX)) {
    return std::nullopt;
  }
  return static_cast<long long>(acc);
}

} // namespace

auto parseLesmaIntegerLiteral(std::string_view s) -> std::optional<long long> {
  if (s.empty()) {
    return std::nullopt;
  }
  if (s.size() >= 2 && s[0] == '0') {
    char const q = s[1];
    if (q == 'b' || q == 'B') {
      return parseMagnitude(s.substr(2), 2);
    }
    if (q == 'o' || q == 'O') {
      return parseMagnitude(s.substr(2), 8);
    }
    if (q == 'x' || q == 'X') {
      return parseMagnitude(s.substr(2), 16);
    }
  }
  return parseMagnitude(s, 10);
}

} // namespace lesma
