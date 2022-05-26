---
id: types
title: Types
sidebar_position: 3
---

# Types

Every value in Lesma has a type, which tells the compiler what sort of data it is.
It helps Lesma to figure out what it can expect out of it, and it helps you to know what you can do with it.

There are two types of data, _primitives_ and _compound_.

## Primitives

Primitive types are just values, such as integers, strings and booleans.
They don't have any methods or fields or anything like that, but they have built-in operators
for things like comparisons.

### Booleans

Booleans, using the `bool` type, can only two values, `true` or `false`.

```js
let x: bool = true
```

### Numeric

Numeric literals can be either integers, or floating points, using `int` and `float` types respectively. Currently, Lesma assumes all numeric values to be signed. Both types are 64 bits.

```js
let x: int = 5
let pi: float = 3.14
```

### String Literals

Strings, using the `str` type, are enclosed in double-quotes `"`. They can contain both ASCII and UTF-8 characters.

```js
let hello: str = "Hello World!"
```

## Compounds

Compound types are constructs that store more data or details than just one value.

### Enums

Enumerations, also referred as _enums_, allow you to define a type with a limited amount of options.

:::info

Unlike the other compound types, _enums_ have built-in equality and inequality operators.
You can use `==` and `!=`.

:::

```js
enum Color
  RED
  GREEN
  BLUE

x = Color.RED

if x == Color.RED
  print("It's Red!")
```

### Classes

Classes contain constructors, fields and methods. They offer more powerful features when it comes to representing objects.

```python
class Animal
  var x: int

  def new(x: int)
    self.x = x

  def getX() -> int
    return self.x

var z = Animal(101)
print(z.getX()) # Prints 101
```