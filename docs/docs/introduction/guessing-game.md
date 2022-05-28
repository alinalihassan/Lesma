---
id: guessing-game
title: Programming a Guessing Game
sidebar_position: 4
---

# Programming a Guessing Game

Now that you should have everything set up and you wrote your first Hello World example in Lesma, let's build something more challenging.
We'll write a guessing game together. The rules are simple, the program will generate a number between 1 and 100, 
and it will prompt the player to guess the number. If it's not the right number, we'll give him a hint if it's lower or higher.
If the guess is correct, it will congratulate the player and exit.

## Writing the game

Make a new source file called `guess.les`. There are quite a few functions from the standard library that we want to use.
First, let's say hi to the player once he starts the game. To do that, we'll use the `print` function.

```python
print("Guess the number! 🎰")
```

Afterwards, we would like to decide on a random number, but if we just choose a constant, 
it will be the same value every game, not fun. Let's use the `random` function. This function
takes two numbers as inputs, the lower bound and the upper bound.

```typescript
let secret_number = random(1, 101)
```

We can now focus on the game loop, and we need to ask the user for input. 
We will use the `input` function from standard library, which will prompt the user for an input, 
and return it to us.

```typescript
while true
    let guess: str = input("Please input your guess: ")
```

Don't worry about the infinite loop, we'll get there. Our problem is that `guess` in the above example is a string, 
and we can't compare that in the same way we compare numbers, so we'll need to convert it to an integer.

```typescript
    let guessed_number = strToInt(guess)
```

Now for the last part, we need to compare the player's guess to our secret number. We can do that
with `if` and `else` statements.

```typescript
    if guessed_number > secret_number
        print("Too big!")
    else if guessed_number < secret_number
        print("Too small")
    else
        print("You win!")
        break
```

## Final Result

If you managed to follow along, you should have written code similar to the one below:

```typescript
print("Guess the number! 🎰")

let secret_number = random(1, 101)

while true
    let guess = input("Please input your guess: ")
    let guessed_number = strToInt(guess)

    if guessed_number > secret_number
        print("Too big!")
    else if guessed_number < secret_number
        print("Too small")
    else
        print("You win!")
        break
```

We can run that using Lesma's CLI. Let's give it a shot and see if our game is working.

```shell
lesma run guess.les
```

Here's my trial against the robot.

```shell
Guess the number! 🎰
Please input your guess: 6
Too small
Please input your guess: 47
Too small
Please input your guess: 87
Too big!
Please input your guess: 66
Too small
Please input your guess: 77
Too small
Please input your guess: 80
Too small
Please input your guess: 82
Too big!
Please input your guess: 81
You win!
```