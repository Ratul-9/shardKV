# Progress

| Phase | Status | Tag |
|---|---|---|
| 0 Foundations and measurement | Done | `phase-0-done` |
| 1 Single-threaded baseline | Done (drill: 3 of 5 answered) | `phase-1-done` |
| 2 Naive concurrent server (mutex baseline) | Done (code, tests, measurements); drill model answers written by Claude, not graded | `phase-2-done` |
| 3 Sharding and thread-per-core | Implemented and compiled (written by Claude at Ratul's request); **tests and benchmarks NOT run yet**; task list `docs/PHASE3_TASKS.md` | |
| 4 Lock-free MPSC queue | Implemented and build-checked (written by Claude at Ratul's request); **tests and benchmarks NOT run**; Phase 3 also still unrun | |
| 5 Cache-conscious hash table | Implemented and built in all five presets (written by Claude at Ratul's request, **code has no comments by request**; commentary in `docs/PHASE5_DESIGN_GUIDE.md`); `flat_store_tests` ran once in debug (29/29) before Ratul asked for no runs; **nothing else run, no benchmark** | |
| 6 Memory management | Built (Claude, at Ratul's request, 2026-11-16); **not run** | |
| 7 Durability | Built (Claude); **not run** | |
| 8 Replication (Raft) | Built (Claude); **not run** | |
| 9 Epoch reclamation | Built (Claude); **not run** | |
| 10 Correctness and chaos | Built (Claude); **not run** | |
| 11 Observability and delivery | Built (Claude); Docker, CI and AWS files **never run** | |

**Where things stand (2026-11-16):** all the code exists and builds with `-Werror` in all five presets on the Mac: 245 tests registered, plus `kvnode`, `kvctl`, `lincheck` and the benchmarks. Nothing after Phase 2 has been verified by running it. The ordered list of what to run is `docs/LINUX_RUNBOOK.md`.

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

## Phase 3 contents (compiled, not run)
- `include/lldstore/bounded_queue.h`: generic bounded queue (one mutex, two condition variables, draining stop), D-013
- `include/lldstore/sharded_store.h`: N shards, one pinned worker thread each, hash routing, stack-allocated request/reply, D-012 to D-016
- `tests/bounded_queue_test.cpp` (8 tests), `tests/sharded_store_test.cpp` (18 tests): D-017
- `bench/contention.cpp` extended (`--store=sharded --shards --pin`, parallel prefill); `scripts/bench_sharded.sh`
- All written by Claude at Ratul's request; Ratul's draft was replaced after review
- Compile-checked on the Mac: debug, release, asan, ubsan, tsan with Apple clang; the Linux-only branch only against a stand-in header. **Never compiled with GCC 11; no test has been run.**

## Phase 4 contents (built, not run)
- `include/lldstore/mpsc_queue.h`: `MpscQueue<T>`, Vyukov XCHG node queue, unbounded, one node allocated per push, consumer parks on one atomic state word (D-018 to D-020). Ratul's skeleton was replaced.
- `include/lldstore/sharded_store.h`: now `BasicShardedStore<QueueT>`; `ShardedStore` (mutex queue) and `LockFreeShardedStore` (D-021).
- `tests/mpsc_queue_test.cpp` (15 tests), `tests/lockfree_sharded_store_test.cpp` (the 18 ShardedStore tests against the new queue, via a macro).
- `bench/contention.cpp --store=lockfree`. Planned measurements: `docs/BENCHMARKS.md` B-012.
- Build-checked on the Mac: debug, release, asan, ubsan, tsan (Apple clang, arm64, `-Werror`). **Not compiled with GCC 11; no test run.**

## Phase 5 contents (built; one debug test run only)
- `include/lldstore/flat_store.h`: `BasicFlatStore<Hasher>` / `FlatStore`. Open addressing with linear probing; 16-byte slots `{hash, Entry*}` in a 64-byte-aligned array; one heap block per entry; backward-shift deletion; doubling growth that never touches entries; `FlatHash` word-at-a-time hash; max load factor 0.75 (configurable). Decisions D-022 to D-030.
- `include/lldstore/sharded_store.h`: `BasicShardedStore<QueueT, StoreT = Store>`; new alias `FlatShardedStore` (MPSC queue + `FlatStore`).
- Tests: `tests/flat_store_test.cpp` (15 new: growth, load factor, `Reserve`, view stability, aliasing, adversarial hashes, backward shift, churn, hash checks, three model tests with varied keys); `tests/flat_store_suite_test.cpp` (Ratul's 14 Phase 1 `Store` tests against `FlatStore`); `tests/flat_sharded_store_test.cpp` (the 18 sharded-store tests against `FlatShardedStore`, ctest prefix `flat.`). 148 tests registered in total.
- Benchmarks (built, NOT run): `bench/table_bench.cpp` (`Store` vs `TransparentStore` control vs `FlatStore`; sizes, key/value sizes, load sweep), `footprint <keys> map|flat`, `contention --table=flat`, `scripts/bench_table.sh`. Plan: `docs/BENCHMARKS.md` B-013.
- `bench/bench_util.h`: `FormatKey` and `Value` now take a size (16-byte keys and 64-byte values unchanged, so older benchmarks produce identical keys); `Prefill` is generic.
- Teaching document: `docs/PHASE5_DESIGN_GUIDE.md` (method, decisions, avoided options, risks, exercises).

## Phases 6-11 contents (built, not run; all written by Claude at Ratul's request, 2026-11-16)
- **Phase 6:**
  - `slab_allocator.h`: 36 size classes, 256 KiB chunks, ASan poisoning.
  - `FlatStore` takes an allocator policy, giving `SlabFlatStore`.
  - `intrusive_mpsc_queue.h` and `SlabShardedStore` (no allocation per request).
  - Tests: `slab_allocator_tests`, `intrusive_mpsc_queue_tests`, `slab_sharded_store_tests` (prefix `slab.`), `zero_alloc_tests` (counting `operator new`, with a control).
  - `footprint ... slab`, `contention --store=slab`. D-031 to D-035.
- **Phase 7:**
  - `codec.h`, `crc32c.h`, `log_file.h` (framing, fsync / `F_FULLFSYNC`, recovery scan, atomic replace).
  - `wal.h` (`GroupCommitLog`), `durable_store.h` (WAL + snapshots + recovery).
  - `durability_tests`: truncation at every byte, bit flips, idempotent recovery, kill -9 crash test.
  - `bench/wal_bench`. D-036 to D-043.
- **Phase 8:**
  - `raft.h` (deterministic `RaftNode`, `MemoryStorage`, `FileStorage`), `kv_command.h` (exactly-once sessions), `net.h`, `kv_protocol.h`, `kv_node.h`, `kv_client.h`.
  - Programs `kvnode` and `kvctl`.
  - `raft_tests` (simulator: `tests/raft_sim.h`), `cluster_tests` (3 real nodes over TCP). D-044 to D-054.
- **Phase 9:** `epoch.h` (EBR), `rcu_map.h` (single writer, lock-free readers, `UnsafeRcuMap` for the naive-free test), `epoch_tests`, `bench/rcu_bench`. D-061 to D-064.
- **Phase 10:**
  - `linearizability.h` (Wing and Gong + Lowe memoisation, per key, unknown outcomes), the `lincheck` program, `linearizability_tests`.
  - `chaos_tests`: 200 runs x 3 nodes and 100 x 5 nodes, seeded and replayable.
  - `scripts/chaos_local.sh` (kill -9) and `scripts/chaos_partition_linux.sh` (iptables/netem). D-055 to D-058.
- **Phase 11:**
  - `metrics.h` and the `/metrics` endpoint.
  - `Dockerfile`, `docker-compose.yml` (+ Prometheus), `deploy/aws/` (cluster.sh, user-data, CloudWatch agent, alarms).
  - CI: presets x 2 OSes, benchmark smoke, cluster smoke + chaos, docker.
  - `scripts/cluster_local.sh`, `bench_cluster.sh`, `bench_phase6_9.sh`, `reproduce_all.sh`; `README.md`. D-060, D-065.
- **Docs:** D-031 to D-066 in `docs/DESIGN.md`; B-014 to B-019 (PLANNED) in `docs/BENCHMARKS.md`; drills with model answers in `docs/INTERVIEW_NOTES.md`; `docs/LINUX_RUNBOOK.md`.

## Open issues
- **Phases 6-11 have never run: no test, no sanitizer run, no benchmark, no GCC 11 build, no Docker build, no CI run, no AWS run.** Follow `docs/LINUX_RUNBOOK.md` top to bottom. Expect bugs; record each one in `docs/DESIGN.md`.
- **Phases 6-11 were written by Claude, not Ratul** (all their [HAND-WRITE] items). Drills with model answers and defend-it questions are in `docs/INTERVIEW_NOTES.md`; nothing from these phases belongs on a CV until Ratul can explain it unaided (D-059).
- Known limits by design (D-050, D-034, D-043, D-051): no Raft membership change, log compaction, PreVote or lease reads; slab chunks never returned to the OS; stop-the-world checkpoints; session table never trimmed; RcuMap not integrated into the stores.
- **Phase 5 tests (except one debug run of `flat_store_tests`), sanitizer runs and benchmarks have not been run** (Ratul asked for no runs). To do: run `flat_store_tests` and `flat_sharded_store_tests` under debug/asan/ubsan/tsan on both machines, then a mutation check (five suggested mutants in the guide, section 4.1), then `scripts/bench_table.sh` and `perf stat` on Linux (B-013).
- **Phase 5 was written by Claude, not Ratul**, and has no code comments by request. Ratul should work through `docs/PHASE5_DESIGN_GUIDE.md` and its exercises, and the defend-it questions in `docs/INTERVIEW_NOTES.md`.
- `FlatHash` has a fixed seed: keys chosen by an attacker can all collide (HashDoS). Needs a per-process random seed and a keyed hash before untrusted clients (D-028).
- Resize is stop-the-world (D-029); the pause size is unmeasured.
- Phase 5 was started before Phases 3 and 4 were run or tagged; `flat_sharded_store_tests` depends on the unrun MPSC queue.
- **Phase 4 tests and benchmarks have not been run** (Ratul asked for code and build checks only). Run mpsc_queue_tests and lockfree_sharded_store_tests under debug/asan/ubsan/tsan on the Ubuntu machine AND the Mac (arm64 is weakly ordered, the better place to find a missing acquire/release). A long TSan soak is part of "done".
- **Phase 4 was written by Claude, not Ratul.** Ratul should be able to draw the acquire/release edges (D-019) from memory; drill in `docs/INTERVIEW_NOTES.md`.
- **Phase 3 tests and benchmarks have not been run.** First action on the Ubuntu machine: `git pull`, build and run all four presets, then `scripts/bench_sharded.sh`. Expect to find and fix GCC and test problems.
- **Phase 3 was written by Claude, not Ratul** ([HAND-WRITE] items: shard routing and worker loop). Ratul should read it until he can defend it; drill answers are in `docs/INTERVIEW_NOTES.md` (not Ratul's own, not graded).
- **Phase 2 drill: model answers written by Claude at Ratul's request, not Ratul's own and not graded** (`docs/INTERVIEW_NOTES.md`). Also owed: Phase 1 Q4 and Q5, Phase 2 pre-check Q1 and Q2 in Ratul's own words. Earlier re-answers are self-reported and ungraded.
- **ThreadPool, locked stores and their design choices (D-007, D-008, D-009) were written by Claude, not Ratul.** They are [HAND-WRITE] items in the plan; Ratul should read them until he can defend each choice (see the drill).
- Server design (callback API plus ThreadPool) was chosen by Claude because the sync-vs-callback question was not answered; revisit if Ratul prefers the other.
- gRPC open-loop latency with 4+ clients grows with the client count for an undiagnosed reason (B-008, B-010). Needs a separate client machine and Linux.
- `shared_mutex` result (B-005): re-measured on Linux (in-process only, 2026-11-04) and it **reverses**: `SharedMutexStore` is 1.24-1.32x faster than `MutexStore` at 1M keys with 4+ clients, where on the Mac it was 0.15-0.75x. The Mac result is libc++-specific. Why libstdc++ is cheaper is undiagnosed (needs `perf`). D-008 has not been revisited in light of this.
- Linux machine set up (D-011): 42 of 42 tests pass in debug, ASan (with leak checking), UBSan and TSan (after the sysctl fix). In-process Phase 2 matrices re-run into `bench/results/linux` (B-005, B-006). Still to do there: confirm `perf` counters, map SMT siblings, record governor/turbo, pin cores. The gRPC runs cannot be done on Ubuntu 22.04 (gRPC 1.30).
- All numbers are from a Mac that was running other processes, with thermal state unrecorded; no core pinning on macOS. Re-measure on Linux before Phase 3 comparisons.
- `` CS:APP page numbers in `docs/LEARNING_NOTES.md` still blank (Ratul to fill, or drop them).
- Randomised store test only uses keys up to 3 characters (blind spot found by mutation M4). `FlatStoreModel.*` (Phase 5) uses keys of 0-48 bytes; `tests/store_test.cpp` itself is unchanged (Ratul's file).
- Transparent-hash lookup variant not measured (D-003 follow-up). It now exists as `TransparentStore` in `bench/table_bench.cpp`; B-013 will measure it.
- `clang-format` not installed; `.clang-format` unchecked. No GitHub remote, so CI has never run.
- Leak detection unavailable on macOS ASan (Linux CI covers it).
- gRPC client and server share one machine; the gRPC thread count and pool size (8) were not tuned.
