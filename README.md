<h1 align="center">
  <img src="tools/docs/src/assets/logo.svg" height="180px" style="height: 180px" alt="Lesma Programming Language" title="Lesma Programming Language">
  <br>
  Lesma
</h1>

<div align="center">

[![License: MIT](https://img.shields.io/github/license/alinalihassan/Lesma?color=yellow)](https://github.com/alinalihassan/Lesma/blob/main/LICENSE.txt)
[![Version](https://img.shields.io/github/v/release/alinalihassan/Lesma?color=blue)](https://github.com/alinalihassan/Lesma/releases)
[![Platform](https://img.shields.io/badge/platforms-%20Linux%20|%20macOS-green.svg?color=lightgrey)](https://github.com/alinalihassan/Lesma/releases)
[![Build](https://img.shields.io/github/actions/workflow/status/alinalihassan/Lesma/ci.yaml?branch=main)](https://github.com/alinalihassan/Lesma/actions/workflows/ci.yaml)

</div>

**Lesma** is a compiled, statically typed, imperative, and object-oriented programming language with a focus on
expressiveness, elegance, and simplicity without sacrificing performance.

## 📝 Features

- 🚀 Fast Compilation: compiling at a rate of ≈230k
  loc/s, [because waiting for code to compile is a thing of the past](https://xkcd.com/303/)
- ⚡ Blazing Fast Execution: because it should be, it's as fast as C, using LLVM's state-of-the-art optimizations, but it
  won't ever oblige you to make an extra effort just for the sake of performance
- 🔬 Statically Typed: because IDE completion is like heaven, while unknown behaviour and runtime exceptions are like
  hell
- 🧑‍🎨 Simple: because the code should be easily readable, and it shouldn't make you guess what it does or take long to
  learn

## ✍️ Example

![Lesma Fibonacci](imgs/lesma_fib.svg)

## 📖 Documentation

- [Official Documentation](https://lesma-lang.com/)
- [Examples](https://github.com/alinalihassan/Lesma/blob/main/tests/lesma)

## Installation

Every Lesma release contains archives with the binary and standard library which you can grab. Alternatively, you can
use the installer script to do all the work for you. The [get-lesma.sh](scripts/get-lesma.sh) script downloads and
installs the latest release.

Run the following in your terminal:

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/alinalihassan/Lesma/main/scripts/get-lesma.sh)"
```

## 🔧 Build

In order to build Lesma, you need Clang, LLVM (17+), and Ninja installed. It's currently only supported on Linux and macOS.
For a more comprehensive guide, and more information on how to install the prerequisites,
read the documentation on [Getting Started](https://lesma-lang.com/getting-started/)

### Prerequisites

**Required:**
- CMake 3.24+
- Ninja
- Clang
- LLVM 17+ (with Clang)

### vcpkg (submodule)

vcpkg is included as a git submodule for dependency management. After cloning Lesma, init and bootstrap it:

```bash
git submodule update --init --recursive
cd vcpkg
./bootstrap-vcpkg.sh  # On Linux/macOS
cd ..
```

### Installing LLVM

#### Option 1: Homebrew (macOS)
```bash
brew install llvm
export LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
export Clang_DIR=$(brew --prefix llvm)/lib/cmake/clang
```

#### Option 2: Package Manager (Linux)
```bash
# Ubuntu/Debian
sudo apt-get install llvm-dev clang libclang-dev

# Or for a specific version (e.g., LLVM 17)
sudo apt-get install llvm-17-dev clang-17 libclang-17-dev
```

#### Option 3: Build LLVM via vcpkg (Any platform)
This option builds LLVM from source using vcpkg. It takes significant time (~1-2 hours) but works on any platform.

```bash
# Configure with LLVM build enabled
cmake . -Bbuild -DLESMA_BUILD_LLVM=ON -G Ninja
cmake --build build
```

### Building Lesma

1. Clone the repository and init the vcpkg submodule
    ```bash
    git clone --recurse-submodules https://github.com/alinalihassan/Lesma
    cd Lesma
    ```
    If you already cloned without `--recurse-submodules`, run `git submodule update --init --recursive` and bootstrap vcpkg (see above).

2. Run CMake to configure and build
    ```bash
    # Using presets (recommended)
    cmake --preset Debug
    cmake --build --preset Debug

    # Or manually
    cmake . -Bbuild -DCMAKE_TOOLCHAIN_FILE="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake" -G Ninja
    cmake --build build
    ```

3. Run tests (optional)
    ```bash
    cd build/Debug  # or build/Release
    ctest --output-on-failure
    ```

## 💬 Contributing

Pull requests are welcome. For major changes, please open an issue to discuss your proposal and what you'd like to
change.

Repository tooling lives under `tools/`:

- `tools/docs` contains the Astro Starlight documentation site.
- `tools/vscode` contains the VS Code extension that launches the native `lesma-lsp` server.

- To keep updated with releases, consider starring the project.
- Check the [code of conduct](CODE_OF_CONDUCT.md) and [contributing guidelines](CONTRIBUTING.md)

## 📎 License

This software is licensed under the [MIT](https://github.com/alinalihassan/Lesma/blob/main/LICENSE.txt)
© [Alin Ali Hassan](https://github.com/alinalihassan).