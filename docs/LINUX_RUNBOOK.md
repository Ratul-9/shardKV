# Linux runbook: everything that still has to be RUN

Written 2026-11-16, when the code for all 11 phases existed and built on the Mac, but no test from Phases 3 to 11 had run (except one debug run of the Phase 5 table tests) and no benchmark after Phase 2 had run. Work top to bottom. Each step says what "pass" looks like and what to do if it does not.

Machine: the Ubuntu desktop (i5-12400F, GCC 11.4, kernel 6.8; see `CLAUDE.md`). Keep the Mac for step 4 (arm64 has a weak memory model).

## 0. Prepare (once)

```bash
conda deactivate                      # conda's cmake/python/protobuf leak into builds
cd ~/shardKV && git pull
cmake --version                       # must be >= 3.25
sudo sysctl vm.mmap_rnd_bits=28       # TSan on kernel 6.8 + GCC 11 (persist: /etc/sysctl.d/99-tsan.conf)
lscpu -e=CPU,CORE                     # pick ONE CPU per CORE for PIN_CPUS, e.g. 0,1,2,3,4,5
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # note it; "performance" is best for benchmarks
findmnt -T $HOME                      # the disk the WAL benchmark will use (must NOT be tmpfs)
```

## 1. Build every preset with GCC 11

```bash
for p in debug release asan ubsan tsan; do cmake --preset $p && cmake --build --preset $p -j$(nproc) || break; done
```

**Pass:** no errors. **Likely trouble, because GCC 11 has never compiled Phases 3 to 11:**
- `-Wconversion` or `-Wshadow` warnings that Clang does not give (`-Werror` turns them into errors). Fix with an explicit cast or a rename, never by turning the warning off.
- Constructors constrained with `requires` (in `sharded_store.h`).
- `std::atomic<uint64_t>::wait`/`notify` (the MPSC queues).
- `std::atomic_ref` (a queue test).

Commit each fix with a message naming the GCC error.

## 2. Run the tests, phase by phase, debug first

Do one phase at a time so a failure points at one thing. The exact filters are in `COMMANDS.md`, sections 3h to 3k.

| Order | Command | Pass |
|---|---|---|
| 2a | `ctest --preset debug -R "^(BoundedQueue|ShardedStore)"` | Phase 3, 26 tests |
| 2b | `ctest --preset debug -R "^MpscQueue|^lockfree\."` | Phase 4 |
| 2c | `ctest --preset debug -R "FlatStore|FlatHash|^flat\."` | Phase 5 |
| 2d | `ctest --preset debug -R "Slab|IntrusiveMpsc|^slab\."` and `build/release/tests/zero_alloc_tests` | Phase 6; zero_alloc must say 0 allocations for SlabShardedStore |
| 2e | `ctest --preset debug -R "Crc32c|Codec|LogScan|GroupCommitLog|DurableStore"` | Phase 7, including the kill -9 test |
| 2f | `ctest --preset debug -R "Raft"` then `ctest --preset debug -R "^Cluster\."` | Phase 8 (Cluster prints the failover time) |
| 2g | `ctest --preset debug -R "Epoch|RcuMap"` | Phase 9 |
| 2h | `ctest --preset debug -R Linearizability` | Phase 10 checker |
| 2i | `ctest --preset release -R Chaos --output-on-failure` | 300 simulated chaos runs, 0 failures; it prints run, operation and fault counts |

**When a test fails:** do not change the test to make it pass. Read the failure, find the cause, fix the code, and write the bug up in `docs/DESIGN.md` (what broke, why, the fix). For chaos failures, the seed in the message reproduces the run exactly: `LLDSTORE_CHAOS_SEED=<seed> build/debug/tests/chaos_tests --gtest_filter='*Single*'`.

## 3. Sanitizers

```bash
ctest --preset asan  -j4     # includes the RcuMap naive-free death test (must report heap-use-after-free)
ctest --preset ubsan -j4
ctest --preset tsan  -j2 --timeout 3600     # slow: chaos tests run ~10x slower under TSan
```

**Pass:** everything passes and no sanitizer report appears. A TSan report in `MpscQueue`, `IntrusiveMpscQueue`, `EpochManager` or `RcuMap` is the most important kind of finding in this project; keep the full report.

## 4. The Mac (arm64, weak memory model)

```bash
git pull && for p in debug tsan; do cmake --build --preset $p -j10 && ctest --preset $p -R "Mpsc|Intrusive|Epoch|RcuMap|ShardedStore" ; done
build/tsan/tests/epoch_tests --gtest_repeat=20
build/tsan/tests/intrusive_mpsc_queue_tests --gtest_repeat=20
```

A missing acquire/release can pass on x86 and fail here. (The Mac cannot pin threads, and its Linux-only tests skip.)

## 5. Mutation checks (do the tests catch bugs?)

For each of these, make the change, run the named test, confirm it FAILS, then revert. Record the outcome in `docs/DESIGN.md` (as D-006 did for Phase 1).

1. `src/raft.cpp` `AdvanceCommit`: remove the `term_at(n) != term()` check. Expect chaos runs to fail.
2. `src/raft.cpp` `HandleVoteRequest`: skip `CandidateLogIsUpToDate`. Expect chaos runs to fail.
3. `src/durable_store.cpp` `Put`: call `WaitDurable` *before* `Append`'s record is synced (e.g. remove the wait). Expect `DurableStoreCrash` to fail, maybe only sometimes; run it with `--gtest_repeat=20`.
4. `src/log_file.cpp` `ScanLogBytes`: continue after a bad CRC instead of `break`. Expect `LogScan.AFlippedBit...` to fail.
5. `include/lldstore/epoch.h` `Reclaim`: free at `epoch + 1` instead of `+ 2`. Expect `EpochManager.NothingIsFreed...` to fail.
6. `src/linearizability.cpp` `StepModel`: make Get always succeed. Expect most checker tests to fail.

## 6. Benchmarks (release, quiet machine: close the browser)

```bash
export RESULTS_DIR=bench/results/linux PIN_CPUS=0,1,2,3,4,5 WAL_DIR=$HOME/walbench
scripts/bench_sharded.sh 3        # B-011 (Phases 3-4)
scripts/bench_table.sh 3          # B-013 (Phase 5)
scripts/bench_phase6_9.sh 3       # B-014 to B-017 (Phases 6, 7, 9)
scripts/bench_cluster.sh 3        # B-018 (Phase 8, one machine)
```

Or all of it in one go: `scripts/reproduce_all.sh` (takes a few hours). Then for each file: `scripts/summarize_results.py <file>`. Paste the results into a session, and the B-entries get rewritten from PLANNED to measured, each prediction confirmed or refuted. Never edit a number by hand.

Also: `perf stat -e cache-misses,branch-misses build/release/bench/table_bench ...` for B-013 (Phase 5), if `perf` works (`sudo sysctl kernel.perf_event_paranoid=1`).

## 7. Real-process chaos (Phase 10)

```bash
RUNS=20 scripts/chaos_local.sh 60                 # kill -9; every history must be linearizable
sudo -E scripts/chaos_partition_linux.sh 120      # partitions + 2 ms delay + 1% loss (root; read the script first)
```

Keep every failing history file: it is the reproduction.

## 8. Delivery (Phase 11)

```bash
docker compose up -d --build && docker compose run --rm client status
docker compose run --rm client put hello world && docker compose stop node1 && docker compose run --rm client get hello
curl -s localhost:9101/metrics | head
docker compose down -v
```

Then push to GitHub so CI runs for the first time (`.github/workflows/ci.yml`). The AWS cluster (`deploy/aws/README.md`) is last: set a billing alarm first, and run `cluster.sh down` the same day.

## 9. Close the phases

For each phase whose tests pass and whose measurements are recorded, do the following, then tag `phase-N-done`:
- Update `docs/PROGRESS.md`.
- Rewrite the B-entry with the real numbers.
- Answer the drill in your own words in `docs/INTERVIEW_NOTES.md`.

Do not tag a phase whose tests were never run.
