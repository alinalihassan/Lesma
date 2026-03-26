#include <benchmark/benchmark.h>
#include <string_view>

#include "SuiteWallClock.h"

auto main(int argc, char** argv) -> int {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (argc >= 2 && std::string_view(argv[1]) == "suite") {
    return lesma::runSuiteWallClock(argc, argv);
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
