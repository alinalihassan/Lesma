# Perf Guide

Use a `Release` build for timing work:

```bash
cmake --build --preset Release --target lesma
```

Track `test.les` with `hyperfine` and keep the JSON so averages are comparable before/after changes:

```bash
hyperfine --warmup 3 --runs 15 \
  --export-json hyperfine-test-les.json \
  './build/Release/lesma run test.les'
```

Read the mean from `hyperfine-test-les.json` and compare the same command after each optimization. A change only counts if the mean improves and the compiler still passes tests.

For profiling on macOS, use one of:

```bash
sample "$(pgrep -n lesma)" 2 1 -mayDie -file sample-lesma.txt
xctrace record --template 'Time Profiler' --output lesma.trace --time-limit 3s --launch -- ./build/Release/lesma run test.les
sudo dtrace -n 'profile-997 /pid == $target/ { @[ustack()] = count(); }' -c "./build/Release/lesma run test.les"
```

Typical loop:

1. Benchmark with `hyperfine`.
2. Profile the same command with `sample`, `xctrace`, or `dtrace`.
3. Optimize the hottest safe path.
4. Rebuild, rerun `hyperfine`, and confirm the mean got better.
