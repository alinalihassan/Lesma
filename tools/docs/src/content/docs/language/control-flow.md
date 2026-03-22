---
title: Control Flow
description: Branching and looping in Lesma.
---

Control flow determines which code runs and how often it runs.

## If statements

Use `if`, `else if`, and `else` to branch on conditions.

```txt
let number = 3

if number > 2
    print("It's true!")
else if number < 5
    print("This branch is skipped")
else
    print("This branch is also skipped")
```

## While loops

Use `while` when a loop should continue until a condition becomes false.

```txt
var number = 10

while number > 0
    print(number)
    number = number - 1
```

## For loops

Lesma's loop story is still evolving. Older examples describe `for` loops,
ranges, and list iteration, but support for those forms is still incomplete in
the current compiler.
