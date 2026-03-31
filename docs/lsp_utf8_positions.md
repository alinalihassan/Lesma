# LSP and UTF-8 positions

Lesma LSP advertises `PositionEncodingKind::UTF8`. Clients must send `Position.character` as **UTF-8 code units (bytes)** from the start of the line, not Unicode code points or UTF-16 code units.

Regression coverage: run `tests/lesma/success/utf8_string_literal.les` and ensure diagnostics (if any) align with byte offsets in editors that use UTF-8 columns.
