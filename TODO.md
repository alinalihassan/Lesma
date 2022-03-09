# TODO List

## Bug fixes
- [x] Parser expects exactly one newline at the end of the file
- [x] Disallow return in top level
- [x] Allow return void
- [ ] Fix symbols with same name but different function signatures (mangled and non-manged) overlapping, it should issue a warning and the newest one replace the old one

## Refactoring
- [ ] Replace SourceLocation by Span
- [ ] Highlight errors line
- [x] Add name mangling

## Features
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
- [ ] Add unit tests
- [x] Add module system
- [x] Add python-style syntax
- [ ] Add generics
- [ ] Add inheritance (or traits)
- [ ] Add string interpolation
- [ ] Add standard library (things like print, input, etc)