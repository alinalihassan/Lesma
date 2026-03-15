# Agents

## Cursor Cloud specific instructions

### Overview
Lesma is a compiled, statically typed programming language. The compiler (`lesmac`) is written in C++23 and uses LLVM 21 as its backend. Build commands are in the `Makefile` and CMake presets in `CMakePresets.json`.

### System Dependencies
The project requires LLVM 21 (with Clang and LLD dev libraries), CMake 3.24+, Ninja, and Clang. The VM snapshot has these pre-installed along with the pre-built LLVM 21 release at `/opt/llvm-21`.

### Building
The project must be built using the pre-built LLVM 21 from `/opt/llvm-21` with `lld-21` as the linker. The apt-packaged LLVM 21 shared library (`libLLVM.so.21.1`) causes segfaults due to static/shared library conflicts. Always use these cmake flags:

```bash
CC=clang-21 CXX=clang++-21 cmake --preset Debug \
  -DLLVM_DIR=/opt/llvm-21/lib/cmake/llvm \
  -DLLD_DIR=/opt/llvm-21/lib/cmake/lld \
  -DClang_DIR=/opt/llvm-21/lib/cmake/clang \
  -DCMAKE_PREFIX_PATH=/opt/llvm-21 \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld-21" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld-21"

cmake --build --preset Debug -j2
```

**Important:** LTO linking with pre-built LLVM 21 is slow (~12 minutes). Use `-j2` to avoid OOM during linking.

### Running
- JIT mode: `./build/Debug/lesma run <file>.les`
- AOT compile mode has a pre-existing libc linking issue on `dev` branch.

### Testing
- `ctest --test-dir build/Debug --output-on-failure`
- Some GTest unit tests have pre-existing "Unexpected character" failures on `dev` (6/20 fail).
- The `test_lesma_sources` integration test (shell script at `scripts/run_tests.sh`) has pre-existing failures.

### Linting
- Format: `make format` (uses `clang-format`)
- Lint: `make lint` (uses `run-clang-tidy`, requires a build first for `compile_commands.json`)
- Lint is slow (~2 min) as it runs clang-tidy on all sources.

### Standard Library
The Lesma stdlib is copied to `~/.lesma/stdlib/` during cmake configuration. If you see import errors, verify it exists.
