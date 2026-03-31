# Syntax not in Lesma (edge-case catalog)

The following are intentionally unsupported today; do not expect parse or typecheck errors to suggest otherwise.

- **Chained comparisons** such as `a < b < c` (Python-style). Use `a < b && b < c` (with possible duplication of `b` evaluation).
- **Bitwise and shift operators** such as `&`, `|`, `^`, `<<`, `>>`, `~`. Use arithmetic or intrinsics where available.

If these are added later, specify evaluation order, short-circuiting (for chains), and LLVM poison rules for shifts.
