# Roadmap

- [x] Improve Visitor Pattern
- [x] Migrate to LLVM 16 with custom Value and Type classes
- [x] Fix multiple imports not working (import each file once)
- [x] Add default values in function declarations
- [x] Add operator overloading
- [x] Add global variables (like 'export let pi = 3.14')
- [x] Add generics
- [x] Add lists
- [x] Add optional parameters in function declarations
- [x] Add tuples
- [x] Add ranges (range(1, 4) style)
- [x] Add foreach loops
- [x] Add traits
- [x] Add string interpolation
- [x] Add multiple value return without having to make structs
- [x] Dataclasses (implicit constructors)
- [ ] Add optional types
- [x] Add dictionaries
- [ ] Add lambda functions
- [ ] Add try catch
- [ ] Add multithreading using pthread for now (async/await? ala Spice)
- [ ] Add comments above classes and functions as documentation in LSP
- [x] Check dictionaries for multiline support

## LSP
- [ ] rename + prepareRename
- [ ] workspace/symbol
- [ ] documentHighlight
- [ ] codeAction for obvious fixes
- [ ] documentLink for import targets
- [ ] richer semantic tokens
- [ ] folding/selection ranges
- [ ] formatting

## Compiler warnings

Semantic diagnostics are emitted in the typechecker; warnings are suppressed for stdlib sources. The CLI can hide diagnostics with `--no-warnings`.

- [x] Unreachable code — statement after `return` / `break` / `continue` in the same block
- [x] Unused variable
- [x] Unused parameter
- [x] Unused import
- [x] Shadowing — local declaration hides an outer binding
- [x] Import shadowing — same as above when the outer binding is an import
- [x] Empty `if` / `else` branch body
- [x] Empty `while` body
- [x] Empty `for-in` body
- [x] Trivial condition — `if` / `while` on constant `true` / `false`
- [x] Lossy implicit conversion — e.g. `int` ↔ `float` / `float32`
- [x] Unused class field on non-exported class
- [x] Unused class method on non-exported class
- [x] `unimplemented` / stub statement — warning then error