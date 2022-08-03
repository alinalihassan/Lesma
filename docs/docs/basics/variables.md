---
id: variables
title: Variables
sidebar_position: 2
---

# Variables

Like most imperative programming languages, Lesma has variables.

## Assignment

Variables can be assigned using '=' operator.

:::tip

If a type is not specified, it's inferred. In the example below, x will get assigned as an integer.

:::

```js
let x = 5
```

## Mutability (Var vs Let)

Variables can be either mutable, which means that they can change their value over time, or immutable, having the same value after assignment.

You need to specify the mutability when you declare a variable.

```python
# Mutable 
var x = 5

# Immutable
let y = 7

x = 42 # Works just fine
y = 42 # Raises an error
```

