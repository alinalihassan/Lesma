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
  - **`src/lsp/`** — `lesma-lsp` (when `LESMA_BUILD_LSP` is ON). Advertises **UTF-8 position encoding** (`PositionEncodingKind::UTF8`); clients must send `Position.character` as UTF-8 code units (bytes) from the line start. Enables vcpkg feature **`lsp`** (**libgit2**, minimal: `pcre2` only, no HTTPS/SSH) for workspace-wide `.les` discovery in Git work trees without shelling out to `git`.
- **`tests/lesma/success/`** — Programs that must compile and run (exit 0).
- **`tests/lesma/failure/`** — Programs that must be rejected (expected to fail).
- **`scripts/run_tests.sh`** — Runs the compiler on all success/failure cases (run + compile for each).
- **`benchmark/`** — Google Benchmark targets plus the **`benchmark suite`** integration harness (`benchmark/SuiteWallClock.cpp`).

---

## Wall-clock benchmark (`benchmark suite`)

The **`benchmark`** executable can run an integration suite that measures **end-to-end wall time** for `lesma run` on each `.les` file (subprocess startup, compile/JIT, and the test program). Enable **`LESMA_BUILD_BENCHMARKS`** in CMake, build the **`benchmark`** target, then run from the **repository root** so paths like `tests/lesma/success/...` resolve:

```bash
./build/Debug/benchmark suite ./build/Debug/lesma \
  --vega-lite-out suite.vl.json \
  --json-out bench.json
```

- **`--json-out`** — Full payload: `tests[]` with **`milliseconds`** per row, **`aggregate`** with **`total_wall_milliseconds`** and **`mean_milliseconds_per_test`**, plus embedded **`vegaLite`** for charting.
- **`--vega-lite-out`** — Same chart as a **standalone Vega-Lite v5** JSON file (handy for `vl2svg` / other tooling).
- **`--suite`** — `success` (default), `failure`, or `both` (default success-only matches `tests/lesma/success/` file count; `both` matches the combined count used by `run_tests.sh`).
- **`--opt`** — Optimization level **0–3** forwarded as `lesma run -O…` (default **3**).
- **`--gha-benchmark-json`** — Writes [github-action-benchmark](https://github.com/benchmark-action/github-action-benchmark)-style JSON with totals in **milliseconds**.

**Static SVG chart** (works with `npx` alone; Node **`vl2png`** usually needs native **canvas**/Cairo). **`vl2svg` writes the SVG to stdout**—redirect to a file so it does not flood the terminal:

```bash
npx -p vega-lite vl2svg suite.vl.json > chart.svg
```

**PNG** without Node canvas: use **[vl-convert](https://github.com/vega/vl-convert)** (`vl-convert vl2png -i suite.vl.json -o chart.png`) or similar; see `--help` on `benchmark suite` for a short reminder.

---

## Compilation pipeline

1. **Driver** (`Driver/Driver.cpp`) — Reads source (file or string), creates `SourceMgr`, adds the main buffer, then runs:
2. **Lexer** — Scans the buffer into tokens. Uses LLVM `SourceMgr`; **buffer IDs are 1-based**: the “current” buffer ID is `srcMgr->getNumBuffers()` (not `getNumBuffers() - 1`).
3. **Parser** — Builds an AST from tokens (visitor-style).
4. **Codegen** — Walks the AST and emits LLVM IR; handles imports by compiling other modules and merging symbols. Can output object files or run via JIT.

When reporting errors, the Driver and Codegen use `showInline()` in `Common/Utils.cpp` with a **buffer ID**: the main file’s ID is the value returned by `AddNewSourceBuffer()` (stored in Driver as `mainBufferId`). For imported modules, Codegen uses the `fileId` returned when that module’s buffer was added. Using the wrong ID (e.g. 0 when IDs are 1-based) triggers LLVM’s `isValidBufferID` assertion in `getMemoryBuffer()`.

---

## How to compile the project

- **Prerequisites:** A **C++23** compiler (Clang from the same LLVM generation as the libraries is recommended), **CMake 3.24+**, **Ninja**, **LLVM 21**, and **LLD**. On macOS, `brew install cmake ninja llvm@21 lld@21` and set `LLVM_DIR` / `LLD_DIR` to those prefixes’ CMake config paths (see [apt.llvm.org](https://apt.llvm.org/) or your distro for Linux). Lesma vendors **vcpkg** under `vcpkg/`; clone with submodules and run `./bootstrap-vcpkg.sh` inside `vcpkg` once so CMake can use `vcpkg.json` (fmt, nameof, CLI11, libgit2 for LSP, etc.). **LLVM and LLD** are discovered with `find_package` and are **not** supplied by that manifest. Optional: `-DLESMA_BUILD_LLVM=ON` enables the vcpkg `build-llvm` feature and builds LLVM from source (slow).
- **Configure (example):** From the repo root, prefer **`cmake --preset Debug`** (uses the in-tree vcpkg toolchain). Alternatively, pass the toolchain explicitly:
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

After any change to the compiler or tests, **rebuild first**, then run the Lesma test suite so that success cases still pass and failure cases are still rejected. Before finishing a feature, once tests are passing, run the Lesma formatter on the touched `.les` directory or directories and then run the tests again so formatting changes are validated too.

- **Commands (from repo root):** Build with the Debug preset, then run tests:
  ```bash
  cmake --build --preset Debug && ./scripts/run_tests.sh
  ```
  You must compile before running tests; otherwise the test script may run an outdated binary and results will be misleading.
- **Formatting pass before signoff:** If the change touches Lesma source files or `.les` tests, run `./build/Debug/lesma fmt <dir-or-file>` on the affected directory or file after the first green test run, then rerun `./scripts/run_tests.sh` (or the relevant test command) to verify the formatted result still passes.
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

- **When ASan points at a crash or bad stack frame:** Use **`lldb`** to get a precise backtrace, inspect the crashing instruction, and check the live values/types that reached codegen or runtime. A typical flow is:
  ```bash
  lldb -- build/Debug_Asan/lesma run tests/lesma/success/list_methods_alias_copy.les
  ```
  Then use `run`, `bt`, `frame variable`, and `up` / `down`. This is usually worth doing after sanitizer output narrows the failing path, especially for recursive specialization bugs, invalid LLVM values, or crashes that happen before ASan can explain ownership clearly.

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

- **Consult the source of truth first:** Before writing or editing any C++ code, read the repo-root **`.clang-format`** and **`.clang-tidy`** and follow those files as the authoritative style/lint configuration for the current change. Do not rely on memory or generic LLVM/C++ habits when the project config says otherwise.
- **Formatting and lint:** `.clang-format` and `.clang-tidy` define formatting and many clang-tidy checks (e.g. `modernize-*`, `readability-*`, `cppcoreguidelines-*`). Naming: `camelBack` for variables/functions/parameters/members, `CamelCase` for classes/enums, `UPPER_CASE` for global constants. Integer literal suffixes are uppercase (e.g. `0U`).
- **No anonymous namespaces:** Do not add helpers in anonymous namespaces. They are easy to add without checking how similar logic is already organized on the relevant class, and they drift from project conventions. Put file-local helpers on the owning class as **private** members (use `static` when the helper does not need `this`). Before adding any helper, search the codebase for an existing place to extend.
- **Helpers in classes:** Prefer **private methods** on the class over free functions at namespace scope. When a helper is only used by one class, add it as a private member so the style stays consistent and the API is clearer.
- **Includes:** Include order and grouping follow `.clang-format` (e.g. standard library, then LLVM, then project `liblesma/`).

---

## Summary

- **Pipeline:** Source → Lexer → Parser → Codegen (Driver + SourceMgr, then Lexer, Parser, Backend).
- **Buffer IDs:** LLVM `SourceMgr` uses 1-based buffer IDs; use `getNumBuffers()` as the ID for the last-added buffer; store `AddNewSourceBuffer()`’s return value for the main file in error reporting.
- **Build:** CMake (presets use the in-tree vcpkg toolchain); build the `lesma` target.
- **Validation:** Always run `scripts/run_tests.sh <path-to-lesma>` and ensure 0 failures.
- **Wall-clock benchmark:** With `LESMA_BUILD_BENCHMARKS`, `./build/Debug/benchmark suite … --json-out` / `--vega-lite-out`; timings in JSON are **milliseconds**; SVG via `npx -p vega-lite vl2svg suite.vl.json > chart.svg`.
- **Memory:** Use the **Debug_Asan** preset (AddressSanitizer + LeakSanitizer) on macOS and Linux; Valgrind is Linux-only and not supported on Apple Silicon.
- **C++ style:** Follow `.clang-format` and `.clang-tidy`; no anonymous namespaces—use private (static) class members instead of anonymous-namespace helpers.
