#pragma once

namespace lesma {

/// Wall-clock integration suite (runs `lesma run` per test, optional JSON). Timings include
/// subprocess startup, full compile/JIT, and the test program's own execution time. Use
/// `--vega-lite-out` for a standalone Vega-Lite chart spec. Prefer `npx -p vega-lite vl2svg` for
/// static output without Cairo; PNG via Node `vl2png` usually needs `canvas` + system libs, or use
/// `vl-convert vl2png`. Invoked when argv[1] is "suite"; argv[0] is the program path,
/// remaining args are suite flags/positionals.
[[nodiscard]] auto runSuiteWallClock(int argc, char** argv) -> int;

} // namespace lesma
