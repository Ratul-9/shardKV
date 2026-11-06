# CLAUDE.md: read this first in every new session

Project: `lldstore`, a low-latency sharded key-value store in C++20. Owner: Ratul (final-year B.Tech, preparing for SDE interviews). The goal is not just working code: Ratul must be able to defend every design decision in an interview.

## Read in this order
1. `PROJECT_CONTEXT.md`: the rules and the 11-phase plan (teacher mode, one phase at a time, baseline before optimising).
2. `docs/PROGRESS.md`: where we are and the open issues.
3. `docs/DESIGN.md`: every decision and what was rejected. `docs/BENCHMARKS.md`: every measurement. `COMMANDS.md`: how to build, test and benchmark.
4. `docs/LEARNING_NOTES.md` and `docs/INTERVIEW_NOTES.md`: what Ratul has learned, answered and got wrong.

## Working rules
- Start each phase with a 5-line recap and 2-3 check questions; end it with a 5-question drill; keep `PROGRESS`, `DESIGN`, `BENCHMARKS`, `INTERVIEW_NOTES`, `LEARNING_NOTES` (and `REFERENCES`) updated; commit and tag `phase-N-done`.
- [HAND-WRITE] items are Ratul's to write. If he explicitly asks Claude to write one, remind him of his goal once, then do it: record authorship ("written by Claude at Ratul's request") in `DESIGN.md` and `PROGRESS.md`, comment the non-obvious lines, and add "defend it" questions to the drill. Model answers written for him are labelled as not his own and not graded.
- No optimisation without a before/after measurement. Every number needs hardware, commit hash and command. Never invent or round up results; say when something is undiagnosed.
- CS:APP references are chapter/section only (no page numbers). Amdahl's law is CS:APP section 1.9.1.
- Verify before claiming: run the build and tests; do not trust a result that did not change when the input changed.

## Environments
| Machine | Facts |
|---|---|
| Mac (Phases 0-2) | Apple M5, 10 cores (4 performance + 6 efficiency), macOS, Apple clang 21 + libc++. No thread pinning, no `perf`. gRPC and Protobuf installed with Homebrew. |
| Ubuntu desktop (from Phase 3) | Intel i5-12400F, 6 cores / 12 threads (no efficiency cores), 1 socket, 1 NUMA node, L1d 48 KiB and L2 1.25 MiB per core, L3 18 MiB, 15 GiB RAM, bare metal. Ubuntu 22.04.5, kernel 6.8, GCC 11.4 + libstdc++. |

Linux notes:
- Run `conda deactivate` first; conda's CMake/Python/protobuf can leak into the build.
- CMake 3.25 or newer is required (Ubuntu 22.04's apt version is too old).
- TSan crashes at startup on this kernel with GCC 11 unless `sudo sysctl vm.mmap_rnd_bits=28` is set (persist it in `/etc/sysctl.d/99-tsan.conf`).
- Ubuntu 22.04's gRPC (1.30) is too old for the callback API: the gRPC server and client targets are skipped automatically.
- GCC 11 is stricter than Clang about some warnings (`-Werror` is on). Already fixed: `std::bit_width` conversion, `-Wdangling-else` before gtest macros.
- Keep results from each machine apart: `export RESULTS_DIR=bench/results/linux` before running `scripts/bench_*.sh`.
- Pull before starting work and push when done, because the Mac and Ubuntu share the GitHub repo.

## Quick commands
`cmake --preset debug && cmake --build --preset debug -j$(nproc) && ctest --preset debug` (also presets `release`, `asan`, `ubsan`, `tsan`). At the end of Phase 2, 42 tests passed in all four non-release presets on both machines. Phase 3 added 26 tests (8 `BoundedQueue`, 18 `ShardedStore`) that have been compiled but NOT run yet; the first thing to do on the Ubuntu machine is run them (see `docs/PHASE3_TASKS.md`).
