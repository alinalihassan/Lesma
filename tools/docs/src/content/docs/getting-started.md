---
title: Getting Started
description: Install Lesma quickly or build the compiler and language server from source.
---

There are two main ways to get started with Lesma:

- Use the installer script to get a release build quickly.
- Build the compiler and `lesma-lsp` from source.

## Quick install

Run the official installer script:

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/alinalihassan/Lesma/main/scripts/get-lesma.sh)"
```

That installs the `lesma` binary and standard library for local use.

## Build from source

### Prerequisites

- CMake 3.24+
- Ninja
- C++23 compiler (Clang recommended)
- LLVM 17+
- lld

Lesma uses `vcpkg` as a submodule for dependency management.

### Clone the repository

```bash
git clone --recurse-submodules https://github.com/alinalihassan/Lesma
cd Lesma
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

### Configure and build

The Debug preset is the easiest way to build the compiler, tests, benchmarks,
and `lesma-lsp`.

```bash
cmake --preset Debug
cmake --build --preset Debug
```

You can also configure manually with the vcpkg toolchain if needed:

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLESMA_BUILD_CLI=ON \
  -DLESMA_BUILD_LSP=ON \
  -DLESMA_BUILD_TESTS=ON
cmake --build build
```

### Run the test suite

After building, run the Lesma source tests:

```bash
./scripts/run_tests.sh build/Debug/lesma
```

If your build output is in a different directory, pass that `lesma` path
instead.

## Using the CLI

Show the available CLI commands:

```bash
lesma --help
```

Run a Lesma source file:

```bash
lesma run hello.les
```

Compile a Lesma source file:

```bash
lesma compile hello.les
```

## Editor support

The repository includes a VS Code extension under `tools/vscode`. It uses the
native `lesma-lsp` binary, so after building from source you can point the
extension's `lesma.compilerPath` setting at your `lesma` executable and it will
launch `lesma-lsp` from the same directory.
