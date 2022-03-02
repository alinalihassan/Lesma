<p align="center">
<img src="docs/img/logo.svg" height="180px" style="height: 180px" alt="Lesma Programming Language" title="Lesma Programming Language">
<br><b style="font-size: 32px;">Lesma</b>
</p>

<div align="center">

[![License: MIT](https://img.shields.io/github/license/alinalihassan/Lesma?color=brightgreen)](https://github.com/alinalihassan/Lesma/blob/main/LICENSE)
[![Version](https://img.shields.io/github/v/release/alinalihassan/Lesma?color=brightgreen)](https://github.com/alinalihassan/Lesma/releases)
[![Build](https://img.shields.io/github/workflow/status/alinalihassan/Lesma/Build)](https://github.com/alinalihassan/Lesma/actions/workflows/ci.yaml)

</div>

**Lesma** is a compiled, statically typed, imperative and object-oriented programming language with a focus on expressiveness, elegancy, and simplicity, while not sacrificing on performance.

## 📝 Features
- 🚀 Fast Compilation: compiling at a rate of ≈230k loc/s, [because waiting for code to compile is a thing of the past](https://xkcd.com/303/)
- ⚡ Blazing Fast Execution: because it should be, it's as fast as C, working together with LLVM's state-of-the-art optimizations, but it won't ever oblige you to make an extra effort just for the sake of performance
- 🔬 Statically Typed: because IDE completion is like heaven, while unknown behaviour and runtime exceptions are like hell
- 🧑‍🎨 Simple: because the code should be easily readable, and it shouldn't make you guess what it does or take long to learn

## ✍️ Example

```python
def fibonacci(x: int)
    if x <= 1
        return x
    return fibonacci(x - 1) + fibonacci(x - 2)

fibonacci(20)
```

## 📖 Documentation

- [Language Documentation](https://alinalihassan.github.io/pyLesma)
- [Lesma code samples](https://alinalihassan.github.io/pyLesma/examples/)

## 🔧 Build

In order to build Lesma, you need Clang installed and LLVM 13. It's currently tested only on Linux and macOS.

Clone the repo:
```shell
git clone https://github.com/alinalihassan/Lesma
```

Build source
```shell
mkdir build
cd build
cmake ..
make -j8
```

Run tests
```shell
ctest
```

Run Lesma source files
```bash
./lesma -h
./lesma test.les
```

## 💬 Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

Please make sure to add and/or update tests as appropriate.

## ⚠️ License
This software is licensed under the [MIT](https://github.com/alinalihassan/Lesma/blob/main/LICENSE) © [Alin Ali Hassan](https://github.com/alinalihassan).