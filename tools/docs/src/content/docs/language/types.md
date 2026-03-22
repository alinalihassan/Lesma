---
title: Types
description: Primitive and compound data types in Lesma.
---

Every value in Lesma has a type. Types let the compiler reason about valid
operations and help editor tooling provide better feedback.

## Primitive types

Primitive values include booleans, integers, floating-point numbers, and
strings.

```txt
let ok: bool = true
let count: int = 5
let pi: float = 3.14
let message: str = "Hello"
```

## Compound types

Compound types carry more structure than a single primitive value.

### Enums

Enums define a fixed set of options.

```txt
enum Color
    RED
    GREEN
    BLUE

let selected = Color.RED
```

### Classes

Classes group fields, constructors, and methods together.

```txt
class Animal
    var x: int

    def new(x: int)
        self.x = x

    def getX() -> int
        return self.x

let pet = Animal(101)
print(pet.getX())
```
