# Roadmap

- [x] Improve Visitor Pattern
- [x] Migrate to LLVM 16 with custom Value and Type classes
- [x] Fix multiple imports not working (import each file once)
- [x] Add default values in function declarations
- [x] Add operator overloading
- [ ] Add global variables (like 'export let pi = 3.14')
- [x] Add generics
- [x] Add lists
- [ ] Add optional types
- [ ] Add optional parameters in function declarations
- [ ] Add tuples
- [ ] Add ranges (1..4 style)
- [ ] Add dictionaries
- [ ] Add foreach loops
- [ ] Add lambda functions
- [ ] Add inheritance (or traits)
- [ ] Add string interpolation
- [ ] Add multithreading using pthread for now (async/await? ala Spice)
- [ ] Add multiple value return without having to make structs

## LSP
- [ ] rename + prepareRename
- [ ] workspace/symbol
- [ ] documentHighlight
- [ ] codeAction for obvious fixes
- [ ]documentLink for import targets
- [ ]richer semantic tokens
- [ ]folding/selection ranges
- [ ]formatting

## Bugs

- [ ] Nested dot access is not implemented yet (for example `holder.payload.value`)
- [ ] Dereferencing a pointer loaded from a class field does not work correctly at runtime/codegen