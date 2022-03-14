# TODO List

## Bug fixes
- [x] Parser expects exactly one newline at the end of the file
- [x] Disallow return in top level
- [x] Allow return void
- [x] Fix symbols with same name but different function signatures (mangled and non-manged) overlapping, it should issue a warning and the newest one replace the old one
- [x] Fix being able to call function without respective arguments, but throw error
- [x] Fix arguments not being usable (not stored in LLVM IR)
- [ ] Fix import error while reading file being caught by Utils function instead of module, which could show where import was stated
- [ ] Fix indentation on the second line after line continuation not working

## Refactoring
- [x] Replace SourceLocation by Span
- [x] Highlight errors line
- [x] Add name mangling

## Features
- [x] Add parenthesis support
- [ ] Add analyzer phase (add tree walker interface?)
- [x] Add type inference
- [ ] Add operator overloading
- [ ] Add varargs
- [ ] Add pointers/references?
- [ ] Add strings
- [ ] Add lists
- [ ] Add tuples
- [ ] Add ranges (1..4 style)
- [ ] Add dictionaries
- [ ] Add foreach loops
- [ ] Add lambda functions
- [ ] Add classes
- [ ] Add enums
- [x] Add unit tests
- [x] Add module system
- [x] Add python-style syntax
- [ ] Add generics
- [ ] Add inheritance (or traits)
- [ ] Add string interpolation
- [ ] Add standard library (things like print, input, etc)