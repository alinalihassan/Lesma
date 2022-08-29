# TODO List

## Bug fixes
- [x] Parser expects exactly one newline at the end of the file
- [x] Disallow return in top level
- [x] Allow return void
- [x] Fix symbols with same name but different function signatures (mangled and non-manged) overlapping, it should issue a warning and the newest one replace the old one
- [x] Fix being able to call function without respective arguments, but throw error
- [x] Fix arguments not being usable (not stored in LLVM IR)
- [x] Fix import error while reading file being caught by Utils function instead of module, which could show where import was stated.
- [x] Fix strings escaping all forward slashes, cannot use any escape sequences
- [x] Fix showInline errors showing the wrong file
- [x] Fix class import behavior not working properly
- [x] Fix type comparison across modules (string class test doesn't work because strlen is defined in base.les
  and class is defined in its own file, thus cast doesn't see that i64's pointer is i64 from another module)
- [ ] Fix not being able to handle nested dot accessors
- [ ] Fix error reporting not handling multiline errors well
- [ ] Fix indentation on the second line after line continuation not working
- [ ] Fix imported modules/files skip already imported modules (such as base.les)
- [ ] Fix unused extern functions get optimized (even without optimized) and we have to make dummy functions
- [ ] Fix CI assets versioning having the old version in the name
- [ ] Fix CI assets not showing cpu architecture (arm vs x86)

## Refactoring
- [x] Replace SourceLocation by Span
- [x] Highlight errors line
- [x] Add name mangling
- [ ] Improve standard library search, it doesn't work when using installers

## Features
- [x] Add parenthesis support
- [ ] Add analyzer phase (add tree walker interface?)
- [ ] Add optional parameters in function declarations
- [x] Add type inference
- [ ] Add operator overloading
- [x] Add varargs
- [x] Add pointers/references?
- [x] Add strings
- [ ] Add generics
- [ ] Add lists
- [ ] Add tuples
- [ ] Add ranges (1..4 style)
- [ ] Add dictionaries
- [ ] Add foreach loops
- [ ] Add lambda functions
- [x] Add classes
- [x] Add enums
- [x] Add unit tests
- [x] Add module system
- [x] Add python-style syntax
- [ ] Add inheritance (or traits)
- [ ] Add string interpolation
- [x] Add standard library (things like print, input, etc)
- [ ] Add multithreading using pthread for now (async/await? ala Spice)
- [ ] Add multiple return without having to make structs
- [ ] Fix this not working. If else doesn't return 
```python
def test(x: int) -> int
    if x > 5
        return 5
    else
        return 7
```