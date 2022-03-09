---
id: literals
title: Literals
sidebar_position: 2
---

# Literals

Literals are primitive values like booleans, numbers, etc.

## Booleans

There are only two values, `true` or `false`.

```js
let x = true
```

## Numeric

Numeric literals can be either integers, or floating points, using `int` and `float` types respectively. Currently, Lesma assumes all numeric values to be signed. Both types are 64 bits.

```js
let x: int = 5
let pi: float = 3.14
```

## String Literals

Strings, using the `string` type, are enclosed in double-quotes `"`. They can contain both ASCII and UTF-8 characters.

:::danger
String are not currently implemented!
:::

```js
let hello: string = "Hello World!"
```