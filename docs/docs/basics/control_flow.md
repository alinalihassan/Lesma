---
id: control_flow
title: Control Flow
sidebar_position: 6
---

# Control Flow

The ability to run some code depending on if a condition is true, or run some code repeatedly while a condition is true,
are basic building blocks in most programming languages. 
The most common constructs that let you control the flow of execution are if expressions and loops.

## If statement

An if statement allows you to branch your code depending on conditions. 
You provide a condition and then state, “If this condition is met, run this block of code. If the condition is not met, do not run this block of code.”

```python
let number = 3

if number > 2
    print("It's true!")
else if number < 5
    print("It's true but never reached")
else
    print("Never reached either")
```

All if expressions start with the keyword `if`, followed by a condition. In this case, the condition checks whether the variable number has a value larger than 2.
We place the code to execute if the condition is true immediately after the condition with an indented block.

Optionally, we can also include `else if` and `else` statements, which we chose to do here, to give the program an alternative block of code to execute should the condition evaluate to false. 
If you don’t provide an else expression and the condition is false, the program will just skip the if block and move on to the next bit of code.

## Loops

It’s often useful to execute a block of code more than once. For this task, 
Lesma provides several loops, which will run through the code inside the loop body to the end and then start immediately back at the beginning.

Lesma has two kinds of loops: while and for. Let’s try both of them.

### While Loops

A program will often need to evaluate a condition within a loop. While the condition is true, the loop runs. 
When the condition ceases to be true, the program calls break, stopping the loop.

```python
var number = 10
while number > 0
    print(number)
```

### For loops

For loops are using to iterate through collections of elements.

:::danger

For loops, lists and ranges are not implemented. None of the examples below will work.

:::

```python
let my_numbers = [0, 1, 2, 3, 4, 5]
for elem in my_numbers
    print(elem)
```

We can do the same with ranges. Specify the starting number, two dots (`..`) and upper limit.
```python
for elem in 0..6
    print(elem)
```