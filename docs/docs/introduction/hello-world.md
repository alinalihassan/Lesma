---
id: hello-world
title: Hello World!
sidebar_position: 3
---

# Hello World!

Now that you’ve installed Lesma, we can start writing our first Lesma program! You can use either a terminal or an editor like [Visual Studio Code](https://code.visualstudio.com/) to create the folder and the files in this tutorial.

## Writing a Lesma Program

Make a new source file called `main.les`. All Lesma files will have the extension `.les`.

```py
print("Hello World!👋")
```

## Running The Program

To be able to use Lesma you need to either refer to the binary file we created in the [Getting Started](getting-started) section, or add the folder to the PATH environment variable.

```bash
lesma run main.les
```

It should print `Hello World!` in the console. If it did, congrats, you wrote your first Lesma program.