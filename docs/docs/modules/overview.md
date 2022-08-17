---
id: overview
title: Overview
sidebar_position: 1
---

# Module System

Lesma's module system is still a work in progress. Similar to Python's system,
you can import whole files, or select functions and object from another file.

A module is a file containing Python definitions and statements. 
The filename is the module name with the suffix .les appended

There are a few differences in Lesma's module system compared to other implementations
in languages like Python.

## Imports & Exports

The syntax and semantics are exemplified below.

```python
# tests/lesma/function_call.les
export def fib() -> int
    return 101

export class Animal
    var x: int

    def new()
        self.x = 101

    def getX() -> int
        return self.x
```

```python
# main.les

# Import from standard library
import time

time.sleep(1)

# Import file
import "tests/lesma/function_call.les"

# Module is then given an alias based on filename
function_call.fib()

let x = function_call.Animal()
x.getX()

# Import time in scope
from time import sleep

sleep(1)

let x = Animal()
x.getX()

# Import sleep function with alias
from time import sleep as my_sleep

my_sleep(1)

# Import all functions from module
from time import *

# Import all functions from module with alias
import time as my_time

my_time.sleep(1)
```

## Top-Level Statements

Top-level statements are not executed once imported. This is in contrast to Python where it's executed exactly once. 
Because of this it's not recommended to initialize parts of your module outside of classes and functions.