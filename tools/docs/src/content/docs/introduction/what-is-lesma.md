---
title: What is Lesma
description: An overview of the language, compiler, and current project goals.
---

Lesma is a compiled, statically typed, imperative, object-oriented programming
language with a focus on expressiveness, simplicity, and performance.

The current compiler is written in C++23 and uses LLVM for parsing, code
generation, optimization, JIT execution, and object-file emission.

## A quick example

```txt
def fibonacci(x: int) -> int
    if x <= 1
        return x
    return fibonacci(x - 1) + fibonacci(x - 2)

print(fibonacci(20))
```

## Current status

Lesma is still evolving. Some syntax and ideas documented here are ahead of the
current implementation, so treat the docs as a mix of supported features and
language direction.

The main repository includes:

- The `lesma` CLI compiler.
- The native `lesma-lsp` language server.
- The standard library in `src/stdlib/`.
- Editor tooling such as the VS Code extension under `tools/vscode`.

## Why build another language?

Lesma was created to explore a language that keeps the ergonomics of a dynamic
language while using static typing and native code generation to improve
correctness and performance.

## Related projects

- [Peregrine](https://github.com/peregrine-lang/Peregrine) shares some
  high-level design goals while targeting a different implementation approach.
- [Lobster](https://github.com/aardappel/lobster) is another language project
  with a strong focus on expressive syntax and practical features.
