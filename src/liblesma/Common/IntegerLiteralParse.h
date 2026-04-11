#pragma once

#include <optional>
#include <string_view>

namespace lesma {

/** Parse a Lesma integer literal (decimal, or `0b` / `0o` / `0x` with optional `B`/`O`/`X`). */
[[nodiscard]] auto parseLesmaIntegerLiteral(std::string_view s) -> std::optional<long long>;

} // namespace lesma
