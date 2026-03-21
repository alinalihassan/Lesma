---
title: Functions
description: Define functions, parameters, return values, and extern calls.
---

Functions are declared with `def`.

```txt
def fibonacci(x: int) -> int
    if x <= 1
        return x
    return fibonacci(x - 1) + fibonacci(x - 2)

print(fibonacci(8))
```

## Parameters

Function parameters require explicit types.

```txt
def greet(name: str)
    print(name)
```

## Return values

Use `->` to declare a return type and `return` to produce a value.

```txt
def add(x: int, y: int) -> int
    return x + y
```

## Extern functions

Extern functions expose C-style APIs to Lesma code.

```txt
def extern sqrt(x: float) -> float

print(sqrt(4.0))
```

## Varargs

Extern functions can also accept a variable number of arguments.

```txt
def extern printf(fmt: str, ...)

printf("Hello %s!\n", "Lesma")
```

## Implementation notes

Some features discussed in older Lesma examples, such as default parameter
values and string interpolation, are still incomplete or evolving.
