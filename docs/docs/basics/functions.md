---
id: functions
title: Functions
sidebar_position: 4
---

# Functions

Functions are the basic building blocks of Lesma, and many times they are used behind the scenes,
but you've also seen one of them, `print`. Here's an example function definition to compute fibbonaci.

```python
def fibonacci(x: int) -> int
    if x <= 1
        return x
    return fibonacci(x - 1) + fibonacci(x - 2)

print(fibonacci(8)) # Prints 21
```

We define functions with the keyword `def`, followed by the function name and a set of parenthesis.
The body block is then defined by a level of indentation. 

We can call any defined function, like `print`, by entering its name followed by a set of parenthesis.

## Parameters

We can define functions to receive parameters when we call them. In the example above,
`x` is our parameter in the function named `fibonacci`. 

Unlike variable definitions, function parameters require us to specify the type.

We can also define default values for those parameters in a similar way we assign values to variables.
Simply by adding an `=` and a value after the operator

:::danger

Default parameter values are not implemented yet. Additionally, string interpolation is not implemented.

:::

```python
def hello(name: str = "Mark")
    print("Hello \{name}!")

hello() # Prints Hello Mark!
```

## Return values

Functions can return values to the caller of the function. The type of the return value is
defined in the function signature after the parameters, delimited by an arrow (`->`).
Values are returned using the `return` keyword followed by a value of the corresponding type.

:::tip

A function that doesn't have a return type is expected to return nothing.

:::


## Extern functions

Extern functions are functions that we import from the C family of languages. It's Lesma's
_Foreign Function Interface_ or _FFI_.

Let's say we want to get the square root of a number, and we don't know how to implement the function ourselves.
If you are sufficiently familiar with C, you can use the math library from there!

```python
def extern sqrt(x: float) -> float

print(sqrt(4.0)) # Prints 2
```

## Varargs

Extern functions can have a variable amount of arguments, and to support that, you can
use an ellipsis(`...`). For example `printf` uses a variable amount of parameters, let's see how
we can define that.

```python
def extern printf(fmt: str, ...)

printf("Hello %s and %s!\n", "Mark", "Sussie") # Prints Hello Mark and Sussie!
```