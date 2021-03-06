---
id: getting-started
title: Getting Started
sidebar_position: 2
---

# Getting Started

There's currently no sharable binaries of the compiler, and you'll have to build it yourself.

## Prerequisites

You'll need certain tools installed on your machine before attempting to build the project. Mainly, you'll need a C++ compiler, CMake and LLVM 13 installed. Since the project is being developed and tested with Clang, I would suggest you use the same.

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
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 13 all
```

Set Clang symlink to Clang 13

```bash
sudo ln -s /usr/bin/clang-13 /usr/bin/clang
sudo ln -s /usr/bin/clang++-13 /usr/bin/clang++
```

#### On MacOS

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
brew install llvm@13

echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zshrc
echo 'export LDFLAGS="-L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib"' >> ~/.zshrc
echo 'export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"' >> ~/.zshrc
```

## Build

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

Check how to use the CLI and available options
```bash
./build/lesma --help
```

Run one of the Lesma test files
```bash
./build/lesma run tests/lesma/inference.les
```

Compile a Lesma file to binary/executable
```bash
./build/lesma compile tests/lesma/inference.les
```