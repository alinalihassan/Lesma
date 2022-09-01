---
id: getting-started
title: Getting Started
sidebar_position: 2
---

# Getting Started

There are multiple ways in which you can get started. You can play with the language online in the playground,
get the installer and play on your machine, or build the whole project yourself.

## Playground

You can now try the [Playground](http://playground.lesma-lang.com/) and browse through the many examples provided.
It's using the same editor engine as Visual Studio Code, and has the best support for Lesma.

## Using the installer

You can also just run the installer, and it will get the binary with all the dependencies 
and standard library installed for you. Paste the following in your Terminal:

```shell
bash -c "$(curl -fsSL https://raw.githubusercontent.com/alinalihassan/Lesma/main/scripts/get-lesma.sh)"
```

## Building from source

You can also choose to build from source, if you'd rather not, just skip this part.

To do that, uou'll need certain tools installed on your machine before attempting to build the project. 
Mainly, you'll need a C++ compiler, CMake and LLVM 14 installed. 
Since the project is being developed and tested with Clang, I would suggest you use the same.

### Installing Prerequisites

#### On Ubuntu

Install system dependencies
```bash
sudo apt install zlib1g-dev
```

Install CMake
```bash
sudo apt install cmake
```

Install LLVM & Clang
```bash
# Run the script in the repository
./setup-llvm.sh
```

Set Clang symlink to Clang 14

```bash
sudo ln -s /usr/bin/clang-14 /usr/bin/clang
sudo ln -s /usr/bin/clang++-14 /usr/bin/clang++
```

#### On macOS

MacOS already has Apple Clang installed which should be good enough as a C++ compiler.

Install system dependencies
```bash
xcode-select --install
```

Install CMake
```bash
brew install cmake
```

Install LLVM
```bash
brew install llvm@14

echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zshrc
echo 'export LDFLAGS="-L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib"' >> ~/.zshrc
echo 'export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"' >> ~/.zshrc
```

### Build

You can now build the compiler yourself! Let's try it out.

Clone the repository
```bash
git clone https://github.com/alinalihassan/Lesma
```

Create a build directory for the generated output
```bash
mkdir build
```

Build the project
```bash
cmake -B build
cmake --build build
```

## Using Lesma

You should now see a few more files in the build directory, **lesma** among them. We can use it to run our Lesma sources.

Let's make a lesma input file that contains the Hello World example.
```bash
echo "print(\"Hello World\")" >> hello.les
```

Check how to use the CLI and available options

```bash
lesma --help
```

Run one of the Lesma test files
```bash
lesma run hello.les
```

Compile a Lesma file to binary/executable
```bash
lesma compile hello.les
```