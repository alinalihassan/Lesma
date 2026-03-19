# AGENTS.md — Lesma project guide for AI agents

This file gives future agents (and humans) a quick reference for how the Lesma compiler works, how to build it, and how to validate changes.

---

## What Lesma is

Lesma is a compiled, statically typed, imperative, object-oriented language that targets LLVM. The compiler is C++23 and uses LLVM for code generation, optimization, JIT, and object-file emission. The standard library is written in Lesma (`.les` files in `src/stdlib/`).

---

## Project layout

- **`src/`** — Compiler and stdlib
  - **`src/cli/main.cpp`** — CLI entry point (when `LESMA_BUILD_CLI` is on).
  - **`src/liblesma/`** — Core library:
    - **`Common/`** — Utils, logging, errors (`LesmaError`, `CodegenError` in `Backend/CodegenError.h`).
    - **`Frontend/`** — Lexer, Parser (tokens → AST).
    - **`AST/`** — AST node definitions and visitor interface.
    - **`Token/`** — Token types and `Token` class.
    - **`Symbol/`** — Symbol table, `Type`, `Value`, `TypeUtils`.
    - **`Backend/`** — Codegen (AST → LLVM IR), `MangleUtils`, `CodegenTypeUtils`, linking/JIT.
  - **`src/stdlib/`** — Lesma standard library (e.g. `base.les`, `math.les`, `time.les`).
- **`tests/lesma/success/`** — Programs that must compile and run (exit 0).
- **`tests/lesma/failure/`** — Programs that must be rejected (expected to fail).
- **`scripts/run_tests.sh`** — Runs the compiler on all success/failure cases (run + compile for each).

---

## Compilation pipeline

1. **Driver** (`Driver/Driver.cpp`) — Reads source (file or string), creates `SourceMgr`, adds the main buffer, then runs:
2. **Lexer** — Scans the buffer into tokens. Uses LLVM `SourceMgr`; **buffer IDs are 1-based**: the “current” buffer ID is `srcMgr->getNumBuffers()` (not `getNumBuffers() - 1`).
3. **Parser** — Builds an AST from tokens (visitor-style).
4. **Codegen** — Walks the AST and emits LLVM IR; handles imports by compiling other modules and merging symbols. Can output object files or run via JIT.

When reporting errors, the Driver and Codegen use `showInline()` in `Common/Utils.cpp` with a **buffer ID**: the main file’s ID is the value returned by `AddNewSourceBuffer()` (stored in Driver as `mainBufferId`). For imported modules, Codegen uses the `fileId` returned when that module’s buffer was added. Using the wrong ID (e.g. 0 when IDs are 1-based) triggers LLVM’s `isValidBufferID` assertion in `getMemoryBuffer()`.

---

## How to compile the project

- **Prerequisites:** CMake 3.24+, Ninja, Clang, LLVM 17+, and vcpkg (with Lesma’s `vcpkg.json`). vcpkg is typically used as a submodule; bootstrap it and use the vcpkg toolchain when configuring.
- **Configure (example):** From the repo root, using the vcpkg toolchain and a build directory such as `build` or `build/Debug`:
  ```bash
  cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLESMA_BUILD_CLI=ON \
    -DLESMA_BUILD_TESTS=ON
  ```
  If the project already uses a multi-config generator, the binary may live under `build/Debug/lesma` (or similar); use whatever path your tree uses.
- **Build:**
  ```bash
  cmake --build build
  ```
  Or build only the CLI: `cmake --build build --target lesma` (adjust if your build dir is `build/Debug`).
- The **compiler binary** is the `lesma` executable (e.g. `build/lesma` or `build/Debug/lesma`). The script `scripts/run_tests.sh` takes the path to this binary as its first argument.

---

## Always run tests after making changes

After any change to the compiler or tests, **rebuild first**, then run the Lesma test suite so that success cases still pass and failure cases are still rejected.

- **Commands (from repo root):** Build with the Debug preset, then run tests:
  ```bash
  cmake --build --preset Debug && ./scripts/run_tests.sh
  ```
  You must compile before running tests; otherwise the test script may run an outdated binary and results will be misleading.
- The script auto-detects the compiler: it looks for `build/Debug/lesma` then `build/lesma`. You can still pass the path explicitly: `./scripts/run_tests.sh build/Debug/lesma`.
- **What it does:** For each `.les` file in `tests/lesma/success/` it runs `lesma run` and `lesma compile` and expects exit code 0. For each file in `tests/lesma/failure/` it expects the compiler to fail (non-zero exit).
- **Success criterion:** The script should report **0 failures** and 44 successes (or the current total number of tests). Any failing test should be fixed before considering the change complete.

---

## Memory and leak checking

The project uses **AddressSanitizer (ASan)** and **LeakSanitizer (LSan)** for memory error and leak detection. These work on **macOS (including Apple Silicon)** and Linux with Clang; no Valgrind is required on macOS.

- **Recommended on all platforms (including Apple Silicon):** Build with the **Debug_Asan** preset and run the test suite. Any leak or use-after-free will be reported when the process exits or when it occurs.
  ```bash
  cmake --preset Debug_Asan
  cmake --build --preset Debug_Asan
  ASAN_OPTIONS=detect_container_overflow=0 ./scripts/run_tests.sh build/Debug_Asan/lesma
  ```
  (On macOS with system LLVM, `ASAN_OPTIONS=detect_container_overflow=0` avoids a false positive in LLVM’s static initializers. CI sets this automatically.) Or with ctest (when `LESMA_BUILD_TESTS` is ON): `ctest --test-dir build/Debug_Asan --output-on-failure`.

- **Option without presets:** Configure with `-DLESMA_SANITIZE_ADDRESS=ON` and `-DCMAKE_BUILD_TYPE=Debug`, then build and run the same tests.

- **macOS only — quick leak check:** You can run Apple’s `leaks` tool on any built binary (no recompile needed):
  ```bash
  leaks --atExit -- build/Debug/lesma run tests/lesma/success/hello.les
  ```

- **Valgrind:** Valgrind does **not** support Apple Silicon reliably (experimental builds can crash). On **Linux** you can optionally run the test suite under Valgrind for extra coverage:
  ```bash
  valgrind --leak-check=full --error-exitcode=1 ./scripts/run_tests.sh build/Debug/lesma
  ```
  CI runs a **Memory (ASan)** job on both `ubuntu-latest` and `macos-latest` using the Debug_Asan preset; fix any sanitizer failures before merging.

---

## IDE / clangd (VS Code, Cursor, etc.)

CMake sets `CMAKE_EXPORT_COMPILE_COMMANDS ON`, but the database is written under your **build directory** (e.g. `build/Debug/compile_commands.json`), not the repo root. **clangd** only auto-discovers it if you symlink it to the root or configure a path.

- The repo includes **`.clangd`** pointing at `build/Debug` for the default CMake preset. After `cmake --preset Debug` (and at least one build so targets exist), **reload the window** or restart clangd so it picks up flags (LLVM, vcpkg, lsp-framework includes).
- If you use another build folder, edit `.clangd`’s `CompilationDatabase` or run:  
  `ln -sf build/Debug/compile_commands.json compile_commands.json`  
  (root `compile_commands.json` is gitignored.)
- Spurious **`module_odr_violation_*`** diagnostics in system headers on macOS are suppressed in `.clangd`; they are a known libc++/clangd interaction, not Lesma bugs.
- **Namespace note:** Lesma’s document store lives in `lesma::lsp_srv` so it does not nest a `lsp` namespace beside the global **`::lsp`** types from lsp-framework.

---

## C++ style

The codebase follows consistent C++ style. Respect it when editing.

- **Formatting and lint:** `.clang-format` and `.clang-tidy` define formatting and many clang-tidy checks (e.g. `modernize-*`, `readability-*`, `cppcoreguidelines-*`). Naming: `camelBack` for variables/functions/parameters/members, `CamelCase` for classes/enums, `UPPER_CASE` for global constants. Integer literal suffixes are uppercase (e.g. `0U`).
- **Helpers in classes:** Prefer **private methods** on the class over free functions in an anonymous namespace. When a helper is only used by one class, add it as a private member so the style stays consistent and the API is clearer.
- **Includes:** Include order and grouping follow `.clang-format` (e.g. standard library, then LLVM, then project `liblesma/`).

---

## Summary

- **Pipeline:** Source → Lexer → Parser → Codegen (Driver + SourceMgr, then Lexer, Parser, Backend).
- **Buffer IDs:** LLVM `SourceMgr` uses 1-based buffer IDs; use `getNumBuffers()` as the ID for the last-added buffer; store `AddNewSourceBuffer()`’s return value for the main file in error reporting.
- **Build:** CMake + vcpkg toolchain; build the `lesma` target.
- **Validation:** Always run `scripts/run_tests.sh <path-to-lesma>` and ensure 0 failures.
- **Memory:** Use the **Debug_Asan** preset (AddressSanitizer + LeakSanitizer) on macOS and Linux; Valgrind is Linux-only and not supported on Apple Silicon.
- **C++ style:** Follow `.clang-format` and `.clang-tidy`; use private methods instead of anonymous-namespace helpers where the helper belongs to a class.
