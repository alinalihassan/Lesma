---
title: Variables
description: Declare mutable and immutable bindings.
---

Variables are introduced with assignment.

## Assignment

If you omit the type, Lesma infers it from the assigned value.

```txt
let x = 5
```

## Mutability

Lesma distinguishes between immutable and mutable bindings:

```txt
# Immutable
let y = 7

# Mutable
var x = 5

x = 42  # Allowed
y = 42  # Error
```

Use `let` when the value should not change and `var` when you need mutation.
