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
- [x] Add dictionaries
- [x] Add lambda functions
- [x] Add comments above classes and functions as documentation in LSP
- [x] Check dictionaries for multiline support
- [x] Type unions
- [x] Type narrowing
- [x] Automatic Reference Counting (no cycles detection yet)
- [x] Add optional types
- [x] Enums v2 (in type unions, enums with methods, traits and associated values)
- [x] Async/await with LLVM coroutines (top-level await, methods, and lambdas; single-threaded drain-to-completion)
- [ ] Cooperative or multithreaded async runtime
- [ ] Try catch and Errors
- [x] Formatter

## Bugs
- [x] When entering newline with curly braces, it should put the ending newline on a separate line
- [ ] We should be able to write "!" to force an optional to be unwrapped, and it would panic/exit with message if it's null
- [x] Importing JSON module takes 400ms, must be from too much LLVM IR
- [ ] Start working on LSP and formatter test suite
- [x] Start figuring out how to profile compiler
- [ ] Move optionals off of null unions sugar and use Option<T> instead, or maybe use both?
- [x] Formatting a file with just an imports and comments deletes some of the comments below it

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