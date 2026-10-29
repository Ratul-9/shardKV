# COMMANDS

Every command is run from the project root (`/Users/ratulmukherjee/Applications/shardKV`). Verified on macOS (arm64) at the end of Phase 1. The `<preset>` placeholder is one of: `debug`, `release`, `asan`, `ubsan`, `tsan`.

| Preset | Use it for |
|---|---|
| `debug` | everyday development and tests |
| `release` | **all benchmarks** (`-O3`); never benchmark any other preset |
| `asan` | memory bugs: out-of-bounds, use-after-free (leak checking is not available on macOS) |
| `ubsan` | undefined behaviour: overflow, misaligned access |
| `tsan` | data races (matters from Phase 2 onwards) |

---

## 1. Build

| What | Command |
|---|---|
| Configure a preset (first time, or after editing CMake files) | `cmake --preset <preset>` |
| Build everything in a preset | `cmake --build --preset <preset> -j10` |
| Build only the store tests | `cmake --build --preset debug -j10 --target store_tests` |
| Build only the benchmarks | `cmake --build --preset release -j10 --target store_bench store_latency footprint histogram_bench` |
| Wipe a build and start fresh | `rm -rf build/<preset> && cmake --preset <preset> && cmake --build --preset <preset> -j10` |

The first configure downloads GoogleTest and Google Benchmark (needs internet once).

---

## 2. Run the tests

| What | Command |
|---|---|
| All tests in one preset | `ctest --preset <preset>` |
| Build then test (the usual loop) | `cmake --build --preset debug -j10 && ctest --preset debug` |
| List every test name | `ctest --preset debug -N` |
| Run tests whose name matches a pattern | `ctest --preset debug -R Store` |
| Run one test | `ctest --preset debug -R 'Store.PutThenGet'` |
| Run one test directly with full output | `./build/debug/tests/store_tests --gtest_filter='Store.PutThenGet'` |
| Run the store tests directly | `./build/debug/tests/store_tests` |
| Run the harness tests directly (histogram, open loop) | `./build/debug/tests/common_tests` |
| Run a test many times to catch flaky ones | `./build/debug/tests/store_tests --gtest_repeat=50 --gtest_shuffle` |
| Run the randomised model test with a different seed | edit `kSeed` in `tests/store_test.cpp`, rebuild, run it |

### Run everything, all four configurations (about 1 minute once built)

```sh
for p in debug asan ubsan tsan; do
  echo "== $p"
  cmake --preset $p >/dev/null && cmake --build --preset $p -j10 >/dev/null && ctest --preset $p | grep 'tests passed'
done
```

Expected today: `100% tests passed, 0 tests failed out of 23` four times.

**Rule from the project:** concurrent code is not done until it passes `tsan` and a stress test.

---

## 3. Measure latency and throughput (always the `release` preset)

Build first: `cmake --preset release && cmake --build --preset release -j10`

Before any measurement that goes into `docs/BENCHMARKS.md`, check the machine:
```sh
uptime          # load average should be low (this is 10 cores; aim under ~1)
```
Close the browser, IDE and other apps. The M5 has performance and efficiency cores and macOS cannot pin threads, so results carry noise.

### 3a. Throughput / time per operation (Google Benchmark): `store_bench`

| What | Command |
|---|---|
| Full baseline, as recorded in `BENCHMARKS.md` (B-002) | `./build/release/bench/store_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s` |
| Quick look (single run, noisier) | `./build/release/bench/store_bench --benchmark_min_time=0.1s` |
| List the available benchmarks | `./build/release/bench/store_bench --benchmark_list_tests` |
| One benchmark only (Get hit at 1M keys) | `./build/release/bench/store_bench --benchmark_filter='GetHit/1000000' --benchmark_min_time=0.2s` |
| All Get benchmarks | `./build/release/bench/store_bench --benchmark_filter='Get'` |
| Save results to a file as well as showing them | `./build/release/bench/store_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true 2>&1 \| tee bench/results/<name>.txt` |
| JSON output (for scripts) | `./build/release/bench/store_bench --benchmark_format=json --benchmark_out=bench/results/<name>.json` |

Benchmarks: `KeyGenOnly` (cost of making a key, subtract approximately), `GetHit`, `GetMiss`, `PutOverwrite` (each at 1K, 100K, 1M keys), `PutInsert` (1M new keys). Read the **mean** and **cv** (variation) rows; a CV above ~5% means a noisy run.

### 3b. Latency percentiles (open loop, coordinated-omission safe): `store_latency`

Usage: `store_latency [num_keys] [offered_ops_per_sec] [measured_seconds] [warmup_seconds]`

| What | Command |
|---|---|
| Baseline run as in `BENCHMARKS.md` (B-003), 1M keys | `./build/release/bench/store_latency 1000000 200000 10 2` |
| 1K keys (everything in cache) | `./build/release/bench/store_latency 1000 200000 10 2` |
| 100K keys | `./build/release/bench/store_latency 100000 200000 10 2` |
| Higher offered load | `./build/release/bench/store_latency 1000000 1000000 10 2` |
| Short smoke run | `./build/release/bench/store_latency 100000 200000 2 1` |
| All three sizes, saved | `for n in 1000 100000 1000000; do ./build/release/bench/store_latency $n 200000 10 2; done \| tee bench/results/<name>.txt` |

Output: min, p50, p90, p99, p999, max and mean in **nanoseconds**. The macOS clock ticks every ~41.7 ns, so values like 0 or 42 are one tick. If `achieved` is below `offered`, the system could not keep up and the percentiles describe an overloaded run.

### 3c. Memory footprint: `footprint`

| What | Command |
|---|---|
| 1M keys (use this row) | `./build/release/bench/footprint 1000000` |
| 100K keys | `./build/release/bench/footprint 100000` |
| All sizes, saved | `for n in 1000 100000 1000000; do ./build/release/bench/footprint $n; done \| tee bench/results/<name>.txt` |

Prints requested bytes per entry, allocations per entry, and RSS delta per entry (payload is 80 B: 16-byte key + 64-byte value). Always run each size in a fresh process.

### 3d. Measurement-harness overhead: `histogram_bench`

| What | Command |
|---|---|
| Cost of recording a sample, computing a percentile, reading the clock | `./build/release/bench/histogram_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s` |

---

## 4. Record a result (project rule: no number without context)

Every measurement that goes into `docs/BENCHMARKS.md` needs hardware, commit, exact command, results and interpretation. Get the pieces:

```sh
git rev-parse --short HEAD          # commit hash (commit your code first so it is meaningful)
uptime                              # machine load at the time
sysctl -n machdep.cpu.brand_string  # chip
sw_vers -productVersion             # macOS version
clang++ --version | head -1         # compiler
```

Raw output goes in `bench/results/` (create descriptive names, e.g. `phase2_mutex_16_clients.txt`). Never put a number on the CV that is not in `docs/BENCHMARKS.md`.

---

## 5. Compare before and after an optimisation

1. Commit the current code. Run the baseline commands in section 3 on the `release` preset, saved to `bench/results/<phase>_before.txt`.
2. Make the change; run the tests in all four presets (section 2).
3. Run the same commands again, saved to `bench/results/<phase>_after.txt`, on the same machine state (check `uptime`).
4. Optionally diff two Google Benchmark JSON files with the tool shipped in the benchmark source: `python3 build/release/_deps/benchmark-src/tools/compare.py benchmarks old.json new.json`.
5. Record both results in `docs/BENCHMARKS.md`. If the change did not help, say so and keep the simpler code.

---

## 6. Git

| What | Command |
|---|---|
| What changed | `git status --short` and `git diff` |
| Commit history | `git log --oneline` |
| Phase tags | `git tag` (currently `phase-0-done`, `phase-1-done`) |
| Commit | `git add -A && git commit -m "message"` |
| Tag a finished phase | `git tag phase-N-done` |
| See the code as it was at a phase | `git show phase-1-done:include/lldstore/store.h` |
| Check out an old phase to re-run its benchmarks (then return) | `git checkout phase-1-done` … `git checkout main` |

---

## 7. Debugging and inspection

| What | Command |
|---|---|
| A test crashed: run it under lldb | `lldb -- ./build/debug/tests/store_tests --gtest_filter='Store.X'` then `run`, `bt` |
| Memory error details | run the failing test from the `asan` build: `./build/asan/tests/store_tests` |
| Data race details (Phase 2+) | `./build/tsan/tests/<test binary>` |
| See the exact compile command for a file | `grep -A2 store_test build/debug/compile_commands.json` |
| Machine facts (cores, caches, memory) | `sysctl hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu hw.l1dcachesize hw.l2cachesize hw.memsize` |
| Is something else hogging the CPU? | `ps -Ao pcpu,comm -r \| head` |

`perf` and `numactl` are Linux-only and not available on this Mac. They come in with the Linux VM or EC2 from Phase 3.

---

## 8. Project documents

| File | Contains |
|---|---|
| `PROJECT_CONTEXT.md` | the rules and the 11-phase plan |
| `docs/TOPICS.md` | topic primer with CS:APP chapter map |
| `docs/DESIGN.md` | decision log (D-001 onwards) |
| `docs/BENCHMARKS.md` | every measurement with context |
| `docs/PROGRESS.md` | phase status and open issues |
| `docs/INTERVIEW_NOTES.md` | graded interview drills |
| `docs/LEARNING_NOTES.md` | knowledge database: topics, insights, mistakes, code references |

---

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| Build seems to use old code after a very quick edit | Make compares timestamps at one-second resolution. Wait a second and rebuild, or `touch` the file. |
| IDE shows red errors that the build does not | The IDE is parsing without C++20 or the project include paths; trust `cmake --build`. |
| `cmake --preset X` fails fetching dependencies | Check your internet connection (needed once for GoogleTest and Google Benchmark). |
| Benchmark numbers jump between runs | Check `uptime`, close other apps, use `--benchmark_repetitions=5` and compare the mean, not one run. |
| A benchmark result looks impossibly fast | The compiler may have removed the work; results must be consumed with `benchmark::DoNotOptimize`. |
| A sanitizer preset fails to link | Delete that build folder (`rm -rf build/<preset>`) and configure again; never mix sanitizers in one build. |

---

*Add new commands here as phases introduce them (the gRPC server and multi-client load generator in Phase 2, Linux/`perf` commands from Phase 3, crash and chaos test runners in Phases 7 and 10).*
