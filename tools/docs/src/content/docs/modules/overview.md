---
title: Modules Overview
description: Importing and exporting code across Lesma files.
---

Lesma's module system is still evolving, but the current design supports both
whole-file imports and importing specific symbols into scope.

## Importing and exporting

```txt
# helpers.les
export def fib() -> int
    return 101

export class Animal
    var x: int

    def new()
        self.x = 101

    def getX() -> int
        return self.x
```

```txt
# main.les
import "helpers.les"

print(helpers.fib())

let animal = helpers.Animal()
print(animal.getX())
```

You can also import from the standard library:

```txt
import time

time.sleep(1)
```

Or bring selected names into local scope:

```txt
from time import sleep

sleep(1)
```

## Top-level statements

Unlike Python, imported modules are not intended to execute top-level statements
as an initialization mechanism. Prefer functions and classes for module setup.
