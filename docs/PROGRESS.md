# Progress

| Phase | Status | Tag |
|---|---|---|
| 0 Foundations and measurement | Built, awaiting interview drill | `phase-0-done` |
| 1 Single-threaded baseline | Not started | |
| 2-11 | Not started | |

## Phase 0 contents
- CMake + presets: `debug`, `release`, `asan`, `ubsan`, `tsan` (`cmake --preset X && cmake --build --preset X && ctest --preset X`)
- `include/lldstore/latency_histogram.h`: log-linear histogram, <= 1/64 relative error, O(1) record
- `include/lldstore/open_loop.h`: open-loop runner that avoids coordinated omission, with warm-up
- Tests: 9 passing under debug, ASan, UBSan, TSan (macOS arm64)
- Benchmark: `bench/histogram_bench.cpp` (record cost, percentile cost, clock cost)
- CI: `.github/workflows/ci.yml` (Linux + macOS, all presets; benchmark smoke run). **Not yet run on GitHub** (no remote configured).

## Open issues
- `clang-format` not installed; `.clang-format` exists but formatting is unchecked. `brew install clang-format` when ready.
- The open-loop runner is single-threaded. A multi-client version is needed in Phase 2.
- Leak detection (LSan) is not available in Apple clang on macOS; ASan here does not check leaks. Linux CI does.
- Phase 0 benchmark numbers were taken with load average ~9 on the machine, so they are a smoke check only and are not recorded in `docs/BENCHMARKS.md`.
