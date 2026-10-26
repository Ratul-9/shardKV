# Progress

| Phase | Status | Tag |
|---|---|---|
| 0 Foundations and measurement | Done | `phase-0-done` |
| 1 Single-threaded baseline Done (drill: 3 of 5 answered) | `phase-1-done` |
| 2-11 | Not started | |

## Phase 0 contents
- CMake + presets: `debug`, `release`, `asan`, `ubsan`, `tsan` (`cmake --preset X && cmake --build --preset X && ctest --preset X`)
- `include/lldstore/latency_histogram.h`: log-linear histogram, <= 1/64 relative error, O(1) record
- `include/lldstore/open_loop.h`: open-loop runner that avoids coordinated omission, with warm-up
- CI: `.github/workflows/ci.yml` (Linux + macOS, all presets; benchmark smoke run). **Not yet run on GitHub** (no remote configured).

## Phase 1 contents
- `include/lldstore/store.h`: `Store` over `std::unordered_map` (written by Ratul, reviewed and fixed; see LEARNING_NOTES.md for the mistakes)
- `tests/store_test.cpp`: 14 tests (examples, edge cases, rehashing, randomised model test). Pass under debug, ASan, UBSan, TSan. Mutation-checked with 5 injected bugs, all caught.
- Benchmarks: `bench/store_bench.cpp` (throughput), `bench/store_latency.cpp` (open-loop percentiles), `bench/footprint.cpp` (bytes per entry). Results in `docs/BENCHMARKS.md` (B-002 to B-004), raw output in `bench/results/`.
- Decisions D-002 to D-006 in `docs/DESIGN.md`.

## Open issues
- Retry Phase 1 drill Q4 and Q5 (`docs/INTERVIEW_NOTES.md`). Earlier re-answers are self-reported and ungraded.
- `pp. ___` CS:APP page numbers in `docs/LEARNING_NOTES.md` still blank (Ratul to fill, or drop them).
- Baseline measured on a not-quiet machine (load ~2-2.6), no core pinning on macOS. Re-measure on Linux before Phase 3 comparisons.
- Randomised store test only uses keys up to 3 characters (blind spot found by mutation M4).
- Transparent-hash lookup variant not measured (D-003 follow-up).
- `clang-format` not installed; `.clang-format` unchecked. No GitHub remote, so CI has never run.
- Leak detection unavailable on macOS ASan (Linux CI covers it).
- Single-threaded load generator; Phase 2 needs a multi-client one.
