---
id: what-is-lesma
title: What is Lesma
sidebar_position: 1
---

# What is Lesma

Lesma is a compiled, statically typed, imperative and object-oriented programming language with a focus on expressiveness, elegancy, and simplicity, while not sacrificing on performance.

Here's a sample:

```py
def fibonacci(x: int) -> int
    if x <= 1
        return x
    return fibonacci(x - 1) + fibonacci(x - 2)

print(fibonacci(20))
```

:::caution
Many features described in this document **have not been implemented**. Most of them will have a hint around to suggest if they haven't been implemented in the reference C++ compiler.
:::

## Why Another Programming Language?

I have created Lesma mainly because I wanted an alternative to Python that would be much faster, that is statically typed to reduce the amount of runtime exceptions, but use type inference to make it feel like a dynamically typed language, instead of writing out mypy type hints to get autocomplete in IDEs.


## Similar Projects

There are quite a few other projects that are more mature and I can vouch for, and if any of the following descriptions seem interesting to you, please check them out.

- **[Peregrine](https://github.com/peregrine-lang/Peregrine)** is a similar programming language developed by a great team, with a similar syntax. Both projects are meant to become simple, fast and easy to use programming languages, but the two main differences are that Peregrine is meant to be a systems programming language, Lesma being a general purpose, and Peregrine is transpiled to C++/JS, while Lesma is using LLVM as an intermediate representation, the compiler being able to JIT or compile to object code by itself, not needing another compiler like GCC/Clang to compile to binary.
- **[Lobster](https://github.com/aardappel/lobster)** is a general purpose language, but its current implementation is biased towards games and other graphical things, with plenty of “batteries included” functionality. It's a very mature projects, but it's creator is tailoring it for game development. Still, a very impressive language in terms of features.