#pragma once

#include <optional>
#include <string_view>

#include "liblesma/Token/TokenType.h"

namespace lesma::OperatorUtils {

inline constexpr std::string_view SUBSCRIPT_GET_NAME = "__operator_subscript";
inline constexpr std::string_view SUBSCRIPT_SET_NAME = "__operator_subscript_set";

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

} // namespace lesma::OperatorUtils
