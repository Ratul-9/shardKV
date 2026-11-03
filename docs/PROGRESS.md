# Progress

| Phase | Status | Tag |
|---|---|---|
| 0 Foundations and measurement | Done | `phase-0-done` |
| 1 Single-threaded baseline | Done (drill: 3 of 5 answered) | `phase-1-done` |
| 2 Naive concurrent server (mutex baseline) | Done (code, tests, measurements); drill model answers written by Claude, not graded | `phase-2-done` |
| 3-11 | Not started | |

## Phase 0 contents
- CMake + presets: `debug`, `release`, `asan`, `ubsan`, `tsan` (`cmake --preset X && cmake --build --preset X && ctest --preset X`)
- `include/lldstore/latency_histogram.h`: log-linear histogram, <= 1/64 relative error, O(1) record
- `include/lldstore/open_loop.h`: open-loop runner that avoids coordinated omission, with warm-up (wait strategy fixed in Phase 2, D-010)
- CI: `.github/workflows/ci.yml` (Linux + macOS, all presets; benchmark smoke run). **Not yet run on GitHub** (no remote configured).

## Phase 1 contents
- `include/lldstore/store.h`: `Store` over `std::unordered_map` (written by Ratul, reviewed and fixed; see LEARNING_NOTES.md for the mistakes)
- `tests/store_test.cpp`: 14 tests. Mutation-checked with 5 injected bugs, all caught.
- Benchmarks: `bench/store_bench.cpp`, `bench/store_latency.cpp`, `bench/footprint.cpp`. Results: `docs/BENCHMARKS.md` B-002 to B-004.
- Decisions D-002 to D-006 in `docs/DESIGN.md`.

## Phase 2 contents
- `include/lldstore/thread_pool.h`: mutex + queue + condition variable, drains on shutdown (written by Claude at Ratul's request, D-007); 10 tests in `tests/thread_pool_test.cpp`
- `include/lldstore/locked_store.h`: `MutexStore` and `SharedMutexStore` (written by Claude at Ratul's request, D-008); typed stress tests in `tests/locked_store_test.cpp`, clean under TSan
- `include/lldstore/multi_client.h`: multi-client open-loop and closed-loop runners
- `proto/kv.proto`, `bench/kv_server.cpp` (gRPC callback API, our ThreadPool or inline), `bench/kv_client.cpp` (D-009; gRPC targets are skipped in sanitizer presets)
- `bench/contention.cpp`: in-process lock-contention tool
- `scripts/bench_inprocess.sh`, `scripts/bench_grpc.sh`, `scripts/summarize_results.py`
- Harness fix D-010 with regression test (macOS `sleep_for` overshoot)
- Results: `docs/BENCHMARKS.md` B-005 to B-010, raw files in `bench/results/phase2_*`
- 42 tests pass under debug, ASan, UBSan, TSan

## Open issues
- **Phase 2 drill: model answers written by Claude at Ratul's request, not Ratul's own and not graded** (`docs/INTERVIEW_NOTES.md`). Also owed: Phase 1 Q4 and Q5, Phase 2 pre-check Q1 and Q2 in Ratul's own words. Earlier re-answers are self-reported and ungraded.
- **ThreadPool, locked stores and their design choices (D-007, D-008, D-009) were written by Claude, not Ratul.** They are [HAND-WRITE] items in the plan; Ratul should read them until he can defend each choice (see the drill).
- Server design (callback API plus ThreadPool) was chosen by Claude because the sync-vs-callback question was not answered; revisit if Ratul prefers the other.
- gRPC open-loop latency with 4+ clients grows with the client count for an undiagnosed reason (B-008, B-010). Needs a separate client machine and Linux.
- `shared_mutex` result (B-005) is for libc++ on macOS; re-measure on Linux.
- All numbers are from a Mac that was running other processes, with thermal state unrecorded; no core pinning on macOS. Re-measure on Linux before Phase 3 comparisons.
- `` CS:APP page numbers in `docs/LEARNING_NOTES.md` still blank (Ratul to fill, or drop them).
- Randomised store test only uses keys up to 3 characters (blind spot found by mutation M4).
- Transparent-hash lookup variant not measured (D-003 follow-up).
- `clang-format` not installed; `.clang-format` unchecked. No GitHub remote, so CI has never run.
- Leak detection unavailable on macOS ASan (Linux CI covers it).
- gRPC client and server share one machine; the gRPC thread count and pool size (8) were not tuned.
