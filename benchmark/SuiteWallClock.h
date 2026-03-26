#pragma once

namespace lesma {

/// Wall-clock integration suite (runs `lesma run` per test, optional JSON). Invoked when
/// argv[1] is "suite"; argv[0] is the program path, remaining args are suite flags/positionals.
[[nodiscard]] auto runSuiteWallClock(int argc, char** argv) -> int;

} // namespace lesma
