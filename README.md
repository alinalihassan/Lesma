<p align="center">
<img src="docs/img/logo.svg" height="180px" style="height: 180px" alt="Lesma Programming Language" title="Lesma Programming Language">
<br><b style="font-size: 24px;">Lesma</b>
</p>

___
[![License: MIT](https://img.shields.io/badge/License-MIT-brightgreen.svg)](https://github.com/alinalihassan/Lesma/blob/main/LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.0-brightgreen.svg)](https://github.com/alinalihassan/Lesma/releases)
[![Build](https://img.shields.io/github/workflow/status/alinalihassan/Lesma/Build)](https://github.com/alinalihassan/Lesma/actions/workflows/ci.yaml)

**Lesma** is a compiled, statically typed, imperative and object-oriented programming language with a focus on expressiveness, elegancy, and simplicity, while not sacrificing on performance.

## Features
- **it's fast**, because it should be so, together with LLVM's state-of-the-art optimizations, but it won't ever oblige you to make an extra effort from your side just for the sake of performance
- **it's compiled** both AOT and JIT, so you can finally decide if you just want to run it or compile it and distribute your project without dependencies, and because binary size also matters, a Hello World example would be around 8kb
- **it's statically typed** so you don't need to guess the type of the variable if your coworker didn't spend the time to use meaningful names, and you can make use of compile-time checks, autocomplete and more
- **it's simple and expressive** because the code should be easily readable and it shouldn't make you guess what it does

## Example

```python
def extern printd(y: float)

let x = 20
printd(x)
```

## Influences
- Python
- Swift
- Typescript
- Lua

## Documentation

- [Language Documentation](https://alinalihassan.github.io/Lesma)
- [Lesma code samples](https://alinalihassan.github.io/Lesma/examples/)

## Build

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

Done! Now you can run the compiler or the interpreter, make a test file and hack your way around. 
Remember there are examples in the documentation.
```bash
./lesma -h
./lesma run test.les
./lesma compile test.les
```

## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

Please make sure to add and/or update tests as appropriate.

## License
[MIT](https://choosealicense.com/licenses/mit/)