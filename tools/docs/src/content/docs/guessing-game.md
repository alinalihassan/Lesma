---
title: Guessing Game
description: Build a small command-line game to practice Lesma syntax.
---

This walkthrough builds a small guessing game that:

- picks a random number between 1 and 100
- asks the player for guesses
- prints hints until the user wins

## Start with output

```txt
print("Guess the number!")
```

## Generate the secret number

```txt
let secret_number = random(1, 101)
```

## Read guesses in a loop

```txt
while true
    let guess = input("Please input your guess: ")
```

## Convert the input and compare it

```txt
    let guessed_number = strToInt(guess)

    if guessed_number > secret_number
        print("Too big!")
    else if guessed_number < secret_number
        print("Too small!")
    else
        print("You win!")
        break
```

## Final program

```txt
print("Guess the number!")

let secret_number = random(1, 101)

while true
    let guess = input("Please input your guess: ")
    let guessed_number = strToInt(guess)

    if guessed_number > secret_number
        print("Too big!")
    else if guessed_number < secret_number
        print("Too small!")
    else
        print("You win!")
        break
```

Run it with:

```bash
lesma run guess.les
```
