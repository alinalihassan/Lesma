#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "liblesma/Token/TokenType.h"

namespace lesma::OperatorUtils {

inline constexpr std::string_view SUBSCRIPT_GET_NAME = "__operator_subscript";
inline constexpr std::string_view SUBSCRIPT_SET_NAME = "__operator_subscript_set";

/** Methods lowered on list-like `__buffer<T>` receivers (codegen + LSP). */
inline constexpr std::array<std::string_view, 7U> BUILTIN_LIST_METHOD_NAMES{
    "len", "clear", "push", "pop", "copy", SUBSCRIPT_GET_NAME, SUBSCRIPT_SET_NAME};

[[nodiscard]] constexpr auto isBuiltinListMethodName(std::string_view methodName) noexcept -> bool {
  return std::ranges::any_of(BUILTIN_LIST_METHOD_NAMES,
                             [methodName](std::string_view name) { return name == methodName; });
}

inline auto getBinaryOperatorName(TokenType op) -> std::optional<std::string_view> {
  switch (op) {
  case TokenType::PLUS:
    return "__operator_plus";
  case TokenType::MINUS:
    return "__operator_minus";
  case TokenType::STAR:
    return "__operator_multiply";
  case TokenType::SLASH:
    return "__operator_divide";
  case TokenType::MOD:
    return "__operator_modulo";
  case TokenType::POWER:
    return "__operator_power";
  case TokenType::AMPERSAND:
    return "__operator_bitwise_and";
  case TokenType::PIPE:
    return "__operator_bitwise_or";
  case TokenType::XOR:
    return "__operator_bitwise_xor";
  case TokenType::SHIFT_LEFT:
    return "__operator_shift_left";
  case TokenType::SHIFT_RIGHT:
    return "__operator_shift_right";
  case TokenType::EQUAL_EQUAL:
    return "__operator_equal";
  case TokenType::BANG_EQUAL:
    return "__operator_not_equal";
  case TokenType::GREATER:
    return "__operator_greater";
  case TokenType::GREATER_EQUAL:
    return "__operator_greater_equal";
  case TokenType::LESS:
    return "__operator_less";
  case TokenType::LESS_EQUAL:
    return "__operator_less_equal";
  default:
    return std::nullopt;
  }
}

inline auto getUnaryOperatorName(TokenType op) -> std::optional<std::string_view> {
  switch (op) {
  case TokenType::MINUS:
    return "__operator_minus";
  case TokenType::BANG:
  case TokenType::NOT:
    return "__operator_not";
  case TokenType::TILDE:
    return "__operator_bitwise_not";
  default:
    return std::nullopt;
  }
}

inline auto isOverloadableBinaryOperator(TokenType op) -> bool {
  return getBinaryOperatorName(op).has_value();
}

inline auto isOverloadableUnaryOperator(TokenType op) -> bool {
  return getUnaryOperatorName(op).has_value();
}

inline auto isOverloadableDeclarationToken(TokenType op) -> bool {
  return isOverloadableBinaryOperator(op) || isOverloadableUnaryOperator(op);
}

/** Source spelling for a mangled operator method name (for LSP / docs). */
[[nodiscard]] inline auto surfaceSpellingForMangledOperator(std::string_view mangled)
    -> std::optional<std::string_view> {
  if (mangled == SUBSCRIPT_GET_NAME) {
    return "operator []";
  }
  if (mangled == SUBSCRIPT_SET_NAME) {
    return "operator []=";
  }
  if (mangled == "__operator_plus") {
    return "operator +";
  }
  if (mangled == "__operator_minus") {
    return "operator -";
  }
  if (mangled == "__operator_multiply") {
    return "operator *";
  }
  if (mangled == "__operator_divide") {
    return "operator /";
  }
  if (mangled == "__operator_modulo") {
    return "operator %";
  }
  if (mangled == "__operator_power") {
    return "operator **";
  }
  if (mangled == "__operator_bitwise_and") {
    return "operator &";
  }
  if (mangled == "__operator_bitwise_or") {
    return "operator |";
  }
  if (mangled == "__operator_bitwise_xor") {
    return "operator ^";
  }
  if (mangled == "__operator_shift_left") {
    return "operator <<";
  }
  if (mangled == "__operator_shift_right") {
    return "operator >>";
  }
  if (mangled == "__operator_equal") {
    return "operator ==";
  }
  if (mangled == "__operator_not_equal") {
    return "operator !=";
  }
  if (mangled == "__operator_greater") {
    return "operator >";
  }
  if (mangled == "__operator_greater_equal") {
    return "operator >=";
  }
  if (mangled == "__operator_less") {
    return "operator <";
  }
  if (mangled == "__operator_less_equal") {
    return "operator <=";
  }
  if (mangled == "__operator_not") {
    return "operator not";
  }
  if (mangled == "__operator_bitwise_not") {
    return "operator ~";
  }
  return std::nullopt;
}

} // namespace lesma::OperatorUtils
