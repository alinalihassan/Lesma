---
title: Literals
description: Primitive literal values in Lesma.
---

Literals are primitive values such as booleans, numbers, and strings.

## Booleans

Booleans use the `bool` type and can be either `true` or `false`.

```txt
let x: bool = true
```

## Numbers

Numeric literals can be integers or floating-point values.

```txt
let x: int = 5
let pi: float = 3.14
```

Currently, Lesma treats numeric values as signed, and the built-in `int` and
`float` types are both 64-bit.

## Strings

Strings use the `str` type and are enclosed in double quotes.

```txt
let hello: str = "Hello World!"
```
