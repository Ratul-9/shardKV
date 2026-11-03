# Benchmarks

Every measurement: hardware, commit, command, results, interpretation. Raw output is in `bench/results/`. CV numbers come only from this file.

## Environment (Phases 0-1)

| Item | Value |
|---|---|
| Machine | Mac17,3, Apple M5 (4 performance + 6 efficiency cores, 10 total), 16 GB RAM |
| OS | macOS 27.0.1 (Darwin 27.0.0), arm64 |
| Compiler | Apple clang 21.0.0, `-O3 -DNDEBUG -g` (`release` preset), C++20 |
| Caches (reported) | L1D 64 KiB, L1I 128 KiB, L2 6 MiB per core group |
| Core pinning | **None.** macOS has no thread-to-core pinning API; threads can land on performance or efficiency cores. |
| Machine state | Not quiet: load average ~2.0-2.6 on 10 cores; VS Code, a browser and Claude Code running. Power adapter state and thermal state not recorded (`pmset` reported no thermal warnings). |
| Timer | `std::chrono::steady_clock` on macOS ticks at 24 MHz, so **timings below ~42 ns are quantised to 41.7 ns steps** (latency values of 0 and 42 ns are one tick, not exact). |

These are not x86 and not Linux numbers, and they are not comparable with the final AWS results.

---

## B-001: Harness overhead (Phase 0)

- **Commit:** `0eef24b` (harness code unchanged since `26d815b`)
- **Command:** `./build/release/bench/histogram_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s`
- **Raw:** `bench/results/phase0_histogram_bench.txt`

| Benchmark | Mean (5 runs) | CV |
|---|---|---|
| `LatencyHistogram::Record` | 1.19 ns | 0.09% |
| `ValueAtPercentile(99.9)` over 1M samples | 299 ns | 0.12% |
| `steady_clock::now()` | 12.5 ns | 1.36% |

**Interpretation:** recording a sample is cheap enough to ignore. Reading the clock (12.5 ns) is the real measurement cost: each open-loop sample pays one clock read, and the clock tick is 41.7 ns, so anything the store does in under ~40 ns cannot be resolved individually by the open-loop tool. The throughput benchmark (Google Benchmark amortises the clock over many iterations) does not have this limit.

---

## B-002: Phase 1 baseline, single-threaded `Store` (std::unordered_map<std::string,std::string>)

**These are the numbers every later phase must beat.**

- **Commit:** `0eef24b`
- **Workload:** keys are 16 bytes (`key:` + 12 digits), values 64 bytes, uniform random key choice, store prefilled with `N` keys, single thread, no I/O.
- **Command:** `./build/release/bench/store_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s`
- **Raw:** `bench/results/phase1_store_bench.txt`
- **Time per operation, mean of 5 runs (coefficient of variation in brackets). Includes ~11 ns of key generation, see below.**

| Operation | N=1,000 | N=100,000 | N=1,000,000 |
|---|---|---|---|
| Key generation only (no store) | 11.2 ns (2.1%) | 11.1 ns (0.2%) | 11.2 ns (1.8%) |
| Get, key present | 14.2 ns (0.1%) | 25.3 ns (2.9%) | 84.3 ns (5.0%) |
| Get, key absent | 19.1 ns (0.2%) | 30.7 ns (1.4%) | 70.6 ns (0.9%) |
| Put, overwrite existing | 16.8 ns (1.5%) | 32.4 ns (2.2%) | 126 ns (4.6%) |
| Put, insert new key (empty store, 1M inserts, includes rehashing) | n/a | n/a | 144 ns (10.8%) |

Equivalent single-thread throughput (1 / mean time, key generation included): Get hit about 70M ops/s at N=1K, about 40M at 100K, about 12M at 1M.

**Interpretation**
- **Latency grows with working-set size**, as expected: 14 ns when everything is in L1/L2, 84 ns at 1M entries (about 150 MB, far beyond the 6 MB L2). The 1M figure is the one to compare against later phases.
- **Subtracting the key-generation cost is approximate, not exact.** Key generation and the lookup are independent instruction chains, so the CPU overlaps them. Do not report "store cost = total minus 11 ns" as a precise figure.
- **A miss at 1M (71 ns) is faster than a hit (84 ns)** because many lookups end at an empty bucket and never touch a node or compare keys. At 1K the reverse holds (19 vs 14 ns): there a miss walks past colliding nodes and a hit is found quickly. Both are consistent with a chained hash table.
- **Overwrite is slower than Get** (126 vs 84 ns at 1M): it pays the Get-like lookup plus a write and possibly a value reallocation.
- **Noise:** CV is under 3% for most cases, 5% for 1M Get hit and 10.8% for the insert benchmark (which includes `malloc` growth and page faults). The machine was not quiet, so treat differences under ~10% as unresolved.

---

## B-003: Phase 1 baseline, open-loop latency percentiles

- **Commit:** `0eef24b`
- **Workload:** 90% Get / 10% Put, offered rate 200,000 ops/s (all achieved), 2 s warm-up, 10 s measured (2,000,000 operations), single thread, latency measured from the intended start time (open loop, so safe against coordinated omission).
- **Command:** `./build/release/bench/store_latency <keys> 200000 10 2`
- **Raw:** `bench/results/phase1_store_latency.txt`
- Histogram error is up to 1.6%; values are upper bounds of buckets.

| Keys | p50 | p90 | p99 | p999 | max | mean |
|---|---|---|---|---|---|---|
| 1,000 | 42 ns | 42 ns | 209 ns | 36.4 us | 0.87 ms | 287 ns |
| 100,000 | 251 ns | 503 ns | 1.17 us | 37.9 us | 1.24 ms | 607 ns |
| 1,000,000 | 375 ns | 543 ns | 879 ns | 44.5 us | 1.64 ms | 814 ns |
| 1,000,000 (repeat run) | 459 ns | 631 ns | 919 ns | 49.7 us | 1.01 ms | 734 ns |

**Interpretation**
- **p50 here is higher than the throughput benchmark** (375-459 ns vs 84 ns at 1M keys). This is not a contradiction. The open-loop run is paced: the thread waits about 5 us between operations, so the core may drop to a lower clock and the cache contents are colder, while the throughput benchmark runs hot in a tight loop. Both are real; they answer different questions. The paced one is closer to a lightly loaded server.
- **The 1K-key p50/p90 of 42 ns is one clock tick**, so the true value is below the timer resolution.
- **The tail is not the store.** p999 of about 36-50 us and max of 1-1.6 ms on a 100 ns operation point at the operating system: scheduler preemption, other processes, possible core migration or clock changes. The store has no pauses of that size. Expect this noise floor to appear in every later phase on this machine; reduce it only with a quiet machine or Linux with pinned cores.
- Run-to-run variation at 1M keys is visible (p50 375 vs 459 ns), so p50 differences below roughly 25% are not meaningful here.

---

## B-004: Phase 1 baseline, memory footprint

- **Commit:** `0eef24b`
- **Command:** `./build/release/bench/footprint <keys>` (one fresh process per size)
- **Raw:** `bench/results/phase1_footprint.txt`
- **Payload per entry:** 16-byte key + 64-byte value = 80 B.

| Keys | Requested bytes / entry | Allocations / entry | RSS delta / entry |
|---|---|---|---|
| 1,000 | 148.8 B (1.86x payload) | 2.01 | 262 B (3.28x) |
| 100,000 | 144.2 B (1.80x) | 2.00 | 195.5 B (2.44x) |
| 1,000,000 | 149.2 B (1.86x) | 2.00 | 203.9 B (2.55x) |

**Interpretation**
- **Two heap allocations per entry:** the hash node (which holds the 16-byte key inline in libc++'s small-string buffer) and the 64-byte value, which is too large for the inline buffer.
- **Requested bytes are about 1.8x the payload, and RSS about 2.5x**: the gap is the allocator's per-block overhead, size-class rounding, and the bucket array. RSS is the number to compare against later phases (Phase 6 target: lower than this).
- **The 1,000-key RSS figure is inflated** by fixed process costs and allocator warm-up spread over few entries; use the 100K and 1M rows.
- **The key fits in libc++'s small-string buffer** (22 bytes on this platform). On libstdc++ (Linux/EC2) the buffer is 15 bytes, so the same 16-byte key would take a third allocation. Footprint will differ on Linux; the AWS baseline must be re-measured, not assumed equal.
- Requested-byte counts come from a replaced `operator new`; they do not include allocator overhead, which only RSS shows.

---

## Phase 2 environment and method (applies to B-005 to B-010)

- Same hardware and toolchain as above. Run on 2026-11-03 with only Claude Code and VS Code open (WhatsApp, Spotify and others closed). A previous attempt with WhatsApp running was discarded.
- Load average at the start of the closed-loop runs: 2.2 (not silent). It climbs to 15-27 *during* the 64-client runs because the benchmark's own threads count towards the load average, so the end-of-run figure is not background load. Charger and thermal state were not recorded; the user reported the Mac heating up.
- The client threads and the store (or server) share the same 10 cores (4 performance + 6 efficiency). With 16 and 64 clients the machine is oversubscribed. No core pinning is possible on macOS.
- Workload: 90% Get / 10% Put, 16-byte keys, 64-byte values, uniform random keys. Every Get copies the value into a reused buffer (so the unlocked `none` reference does the same work as the locked stores).
- Repetitions: 3 per closed-loop cell; tables show the median with (min-max). Open-loop cells are single runs.
- Scripts: `scripts/bench_inprocess.sh`, `scripts/bench_grpc.sh`; tables made by `scripts/summarize_results.py`. Raw output: `bench/results/phase2_*.txt`, each with its commit hash in the header.
- Closed-loop results: commit `3de99c5`. Open-loop results: re-run at commit `4e748d1` after a harness fix (see B-009); the pre-fix open-loop files are kept as `*_before_sleepfix.txt`.

---

## B-005: Phase 2, in-process lock contention, closed loop (maximum throughput)

- **Tool:** `contention --mode=closed` (no network), 4 s measured after 1 s warm-up, 3 repetitions. `none` is the bare single-threaded `Store` (valid only with 1 client).
- **Command:** `scripts/bench_inprocess.sh 3 closed`
- **Raw:** `bench/results/phase2_inprocess_closed.txt`

Throughput in ops/s, median of 3 (min-max):

| 1,000,000 keys | 1 client | 4 clients | 16 clients | 64 clients |
|---|---|---|---|---|
| `none` (no lock, 1 thread) | 5.31M (5.16-5.45) | | | |
| `MutexStore` | 4.78M (4.56-4.88) | 1.90M (1.88-1.91) | 2.20M (2.20-2.22) | 2.23M (2.13-2.24) |
| `SharedMutexStore` | 3.45M (3.44-3.46) | 1.42M (1.42-1.44) | 0.54M (0.53-0.54) | 0.33M (0.32-0.33) |

| 1,000 keys | 1 client | 4 clients | 16 clients | 64 clients |
|---|---|---|---|---|
| `none` (no lock, 1 thread) | 47.24M (45.27-48.01) | | | |
| `MutexStore` | 40.44M (40.04-44.45) | 13.31M (12.02-13.53) | 9.44M (8.83-9.59) | 9.20M (9.06-9.72) |
| `SharedMutexStore` | 33.45M (31.63-34.03) | 4.61M (4.55-4.70) | 1.94M (1.93-1.98) | 1.62M (1.60-1.68) |

**Interpretation**
- **Adding clients makes the mutex store slower, not faster.** 1M keys: 4.78M ops/s with 1 client, 1.90M with 4 (a 60% drop), then a plateau near 2.2M. 1K keys: 40.4M, then 13.3M, then 9.4M and 9.2M. Spread is under 10% in every mutex cell, so these differences are real.
- **Why (Amdahl plus lock cost):** almost the entire operation (hash, lookup, copy) runs inside the lock; only key generation (~11 ns of a ~21 ns operation at 1K keys) is outside, so even perfect scaling would give at most about 2x at 1K keys. The measured result is below 1x because the lock itself costs more than the work it protects once threads contend: the lock word's cache line moves between cores and blocked threads sleep and wake. (The ~11 ns share comes from the Phase 1 key-generation measurement; it is an estimate.)
- **Uncontended lock cost:** one client with `MutexStore` is 10% slower than no lock at 1M keys (4.78M vs 5.31M) and 14% slower at 1K keys (40.4M vs 47.2M).
- **The `shared_mutex` hypothesis is falsified for this workload and platform.** `SharedMutexStore` was slower than `MutexStore` at every client count: 0.72x with 1 client, 0.75x with 4, 0.25x with 16 and 0.15x with 64 at 1M keys (0.83x, 0.35x, 0.21x, 0.18x at 1K keys). The expectation that "90% reads means readers run in parallel and it wins" does not hold when the critical section is about 100 ns or less.
- **Platform caveat for `shared_mutex`:** on this machine libc++'s `std::shared_mutex` is built from an internal `std::mutex` plus two condition variables (confirmed in the SDK header `shared_mutex`, lines 158-160), so every `lock_shared()` briefly takes that internal mutex. That is heavier than a bare reader-count update and makes the gap larger than it may be elsewhere. Do not generalise: re-measure on Linux with libstdc++ (which uses `pthread_rwlock`) before quoting the ratio.
- **The plateau at 16 and 64 clients** is the machine saturated (10 cores): more threads than cores adds no throughput.

---

## B-006: Phase 2, in-process lock contention, open loop (latency at a fixed offered rate)

- **Tool:** `contention --mode=open`, 6 s measured after 1 s warm-up, single run per cell, 1M keys, 90% Get / 10% Put. Offered rate is the TOTAL, split evenly across clients. Latency is measured from each operation's intended start time.
- **Commit:** `4e748d1` (after the sleep fix in B-009). **Command:** `scripts/bench_inprocess.sh 1 open`
- **Raw:** `bench/results/phase2_inprocess_open.txt`; pre-fix run for comparison: `phase2_inprocess_open_before_sleepfix.txt`.

At 200,000 ops/s offered (about 10% of the mutex store's capacity; every cell achieved 200,000):

| Store | Clients | p50 | p99 | p999 | max |
|---|---|---|---|---|---|
| `none` | 1 | 503 ns | 839 ns | 7.5 us | 460 us |
| `MutexStore` | 1 | 543 ns | 839 ns | 6.3 us | 215 us |
| `SharedMutexStore` | 1 | 503 ns | 839 ns | 8.3 us | 1.30 ms |
| `MutexStore` | 4 | 459 ns | 879 ns | 721 us | 4.62 ms |
| `SharedMutexStore` | 4 | 459 ns | 879 ns | 729 us | 9.16 ms |
| `MutexStore` | 16 | 543 ns | 26.4 us | 2.23 ms | 8.20 ms |
| `SharedMutexStore` | 16 | 631 ns | 50.7 us | 397 us | 2.72 ms |
| `MutexStore` | 64 | 1.6 us | 145 us | 393 us | 8.19 ms |
| `SharedMutexStore` | 64 | 2.8 us | 4.78 ms | 10.2 ms | 19.5 ms |

At 2,000,000 ops/s offered (more than the store can sustain with 4 or more clients, so the system is overloaded):

| Store | Clients | achieved ops/s | p50 |
|---|---|---|---|
| `MutexStore` | 4 / 16 / 64 | 1.15M / 1.63M / 1.69M | 2.5 s / 0.83 s / 0.79 s |
| `SharedMutexStore` | 4 / 16 / 64 | 1.14M / 0.49M / 0.29M | 2.6 s / 10.6 s / 20.4 s |

**Interpretation**
- **With one client the lock costs almost nothing in latency:** p50 543 ns (mutex) vs 503 ns (no lock), and p99 is identical (839 ns).
- **At low load, medians stay sub-microsecond up to 16 clients; tails do not.** At 64 clients `MutexStore` has p99 145 us but `SharedMutexStore` has p99 4.8 ms and p999 10.2 ms at the same 10% load. That agrees with the throughput result (B-005): the shared lock is much more expensive per operation, and with 64 threads on 10 cores a lock holder that gets descheduled stalls the others.
- **p999 and max in the milliseconds at 4 clients are identical for both stores** (about 0.72 ms, 4.6-9 ms), so they are operating-system scheduling noise on this machine, not lock behaviour. Single runs: do not read differences of a few tens of percent in tails.
- **The 2M ops/s rows are overload tests, not tail latencies.** Offered load exceeds capacity, so a backlog grows for the whole window and open-loop latency (charged from the intended time) reaches seconds. Their useful content is the achieved rate: it matches B-005's ordering (`SharedMutexStore` collapses to 0.29M at 64 clients).
- **The sleep fix removed a harness artifact:** at 16 clients, `MutexStore` p99 was 20.7 ms before the fix and is 26 us after.

---

## B-007: Phase 2, gRPC end to end, closed loop (maximum throughput)

- **Setup:** `kv_server` and `kv_client` on the same machine over loopback TCP. Each client is a thread with its own channel (own connection) and one outstanding request. 1M keys prefilled. 4 s measured after 1 s warm-up, 3 repetitions, 0 RPC errors in all runs.
- **Configurations:** `mutex 0` = `MutexStore`, handlers run inline on gRPC's own threads. `mutex 8` = `MutexStore` with our `ThreadPool` of 8 workers. `shared 8` = `SharedMutexStore` with the pool of 8.
- **Command:** `scripts/bench_grpc.sh 3 closed`
- **Raw:** `bench/results/phase2_grpc_closed.txt`

Throughput in ops/s, median of 3 (min-max):

| Server | 1 client | 4 clients | 16 clients | 64 clients |
|---|---|---|---|---|
| `mutex`, inline (workers=0) | 15.5K (15.4-15.6) | 32.0K (31.7-32.9) | 51.8K (50.4-53.1) | 52.3K (50.9-54.6) |
| `mutex`, pool of 8 | 14.4K (14.2-14.5) | 24.8K (24.5-25.7) | 40.2K (40.0-40.3) | 40.0K (39.9-40.2) |
| `shared`, pool of 8 | 13.1K (13.1-13.1) | 23.7K (23.2-24.0) | 40.1K (39.0-40.5) | 40.6K (40.2-40.6) |

**Interpretation**
- **At the service level the lock is not the bottleneck.** The server tops out near 52K requests/s, while the in-process mutex store alone handles about 2.2M ops/s. So the store lock accounts for roughly 2% of the cost of a request; the rest is gRPC (serialisation, HTTP/2, system calls) and the client sharing the same cores.
- **`mutex 8` and `shared 8` are within noise of each other** at 4, 16 and 64 clients (differences under 3%); at 1 client `shared` is about 9% lower (13.1K vs 14.4K, tight ranges), which is consistent with its higher uncontended cost in B-005. The in-process gap of 4-7x between the two stores is invisible here.
- **The thread-pool hop costs throughput:** with the pool, 22-23% less than inline at 4, 16 and 64 clients (24.8K vs 32.0K, 40.2K vs 51.8K, 40.0K vs 52.3K) and 7% less at 1 client. Handing the work to another thread adds a queue operation, a wake-up and a context switch to every request, and on this saturated 10-core machine nothing is gained in return because the store work is tiny.
- **Scaling:** throughput rises with clients up to 16 (the machine saturates), then flat at 64.
- **Limits:** client and server share cores, so the ceiling reflects both. Loopback only. No core pinning.

---

## B-008: Phase 2, gRPC end to end, open loop (latency at 5,000 ops/s offered)

- **Setup:** as B-007. Offered load is 5,000 requests/s TOTAL (well below every configuration's capacity), split across clients. 6 s measured after 1 s warm-up, single run per cell, 0 RPC errors.
- **Commit:** `4e748d1`. **Command:** `scripts/bench_grpc.sh 1 open`. **Raw:** `bench/results/phase2_grpc_open.txt` (pre-fix: `phase2_grpc_open_before_sleepfix.txt`).

| Server | Clients | p50 | p99 | p999 | max |
|---|---|---|---|---|---|
| `mutex`, inline | 1 | 65.0 us | 73.7 us | 97.3 us | 124 us |
| `mutex`, pool of 8 | 1 | 68.6 us | 81.9 us | 107.5 us | 144 us |
| `shared`, pool of 8 | 1 | 67.6 us | 87.0 us | 178 us | 1.56 ms |
| `mutex`, inline | 4 | 373 us | 447 us | 573 us | 1.26 ms |
| `mutex`, pool of 8 | 4 | 389 us | 471 us | 582 us | 1.28 ms |
| `shared`, pool of 8 | 4 | 377 us | 500 us | 9.83 ms | 12.2 ms |
| `mutex`, inline | 16 | 926 us | 1.28 ms | 16.5 ms | 18.9 ms |
| `mutex`, pool of 8 | 16 | 942 us | 1.21 ms | 1.49 ms | 1.84 ms |
| `shared`, pool of 8 | 16 | 950 us | 1.20 ms | 1.34 ms | 1.66 ms |
| `mutex`, inline | 64 | 2.69 ms | 3.41 ms | 3.67 ms | 3.97 ms |
| `mutex`, pool of 8 | 64 | 2.72 ms | 3.41 ms | 3.60 ms | 3.83 ms |
| `shared`, pool of 8 | 64 | 2.72 ms | 3.38 ms | 3.67 ms | 4.05 ms |

**Interpretation**
- **The clean number is the 1-client row: a request/response round trip over loopback takes about 65-69 us at p50 and 74-87 us at p99** for all three servers. The pool hop adds about 4 us at p50 (65.0 vs 68.6 us); this is a single run, so treat it as "a few microseconds".
- **The three configurations are indistinguishable at every client count** (p50 within 3% of each other), so the lock and the pool do not drive these latencies. Isolated tail spikes (4 clients `shared`: p999 9.8 ms; 16 clients inline: p999 16.5 ms) occur in one configuration or another without a pattern and are operating-system noise in single runs.
- **Latency still grows with the number of clients (65 us, 0.37 ms, 0.93 ms, 2.7 ms) although offered load is constant at 5,000/s.** This is NOT a server property and is undiagnosed: it persisted after the harness fix (B-009; the pre-fix values were 1.28 ms at 16 clients and 4.46 ms at 64), it is identical across servers, and the in-process control (B-010) shows the load generator's own wake-up lateness explains only about 0.1 ms of it at 64 clients. The remaining cause is somewhere in the gRPC client/server and operating-system path for many sparsely used connections on one machine (idle wake-ups are the leading suspect, unproven). Do not quote the 4, 16 and 64 client rows as server latency. Re-measure on Linux with the client on a separate machine.

---

## B-009: Harness finding: `sleep_for` overshoot biased the first open-loop results (fixed)

- **What was wrong:** the open-loop runner slept until 100 us before each deadline, then spun. Measured on this Mac (`sleep_for` called 100 times per size, idle machine): the overshoot is proportional to the requested sleep, about 25-30% of it.

| `sleep_for` requested | median overshoot |
|---|---|
| 100 us | 31 us |
| 200 us | 57 us |
| 800 us | 213 us |
| 3.2 ms | 816 us |
| 12.7 ms | 3.19 ms |

  A fixed 100 us margin is far too small for long waits, so every operation started late and the lateness was charged to latency (latency is measured from the intended start). It also meant that for intervals under 200 us the runner never slept (pure spinning), so 16 clients at 200K ops/s kept 16 threads spinning on 10 cores and starved each other.
- **Effect on the first results (kept in `*_before_sleepfix.txt`):** gRPC open-loop latency at a constant 5,000 ops/s offered rose with the client count (p50 65 us, then ~0.4 ms, ~1.3 ms, ~4.4 ms for 1, 4, 16, 64 clients) and was identical across all three server configurations. It matched about 0.35 x the per-client interval (200 us, 800 us, 3.2 ms, 12.8 ms), which pointed at the load generator and not the server. The in-process 16-client tails (p99 about 21-29 ms at only 10% of capacity) were the same bug.
- **Fix (commit `4e748d1`):** sleep for half of the remaining time, repeat, and spin only below 50 us. The overshoot of a half-length sleep cannot pass the deadline as long as the platform's overshoot stays under 50% of the request (it is 25-31% here). A regression test, `OpenLoop.LongIntervalsDoNotAddSleepOvershootToLatency`, failed on the old code and passes on the new.
- **What this does not change:** closed-loop throughput (B-005, B-007) has no waiting and is unaffected. The Phase 1 numbers are unaffected: its intervals are 5 us, which are spun both before and after.

---

## B-010: Control: how much of the sparse-traffic latency is the load generator? (in process)

- **Question:** at 5,000 ops/s over many clients (12.8 ms between requests per client at 64 clients), how much latency does the load generator alone add?
- **Method:** `contention --store=mutex --mode=open --rate=5000 --seconds=4 --keys=100000`, no network, commit `4e748d1`, run by hand (machine load average 6.5, not quiet). **Raw:** `bench/results/phase2_inprocess_sparse_control.txt`.

| Clients | p50 | p99 | p999 |
|---|---|---|---|
| 1 | 1.1 us | 4.5 us | 18 us |
| 4 | 7.6 us | 60 us | 92 us |
| 16 | 11 us | 112 us | 4.9 ms |
| 64 | 90 us | 500 us | 8.7 ms |

**Interpretation:** even with no network, per-operation latency grows with the number of sparse clients (1 us to 90 us at p50). That is wake-up lateness of threads that have been asleep: a floor of the load generator on this machine. It is about 30 times smaller than the 2.7 ms seen through gRPC at 64 clients, so it cannot explain the gRPC result. It also means open-loop latencies below roughly 0.1 ms with 64 sparse clients on this machine include generator wake-up noise. Single run, loaded machine: use as an order of magnitude only.

---

## Not yet measured / known limits
- No Linux or x86 results yet (needed from Phase 3 for pinning and `perf`). The `shared_mutex` ratio in B-005 is specific to libc++ on macOS and must be re-measured on Linux before it is quoted.
- gRPC open-loop latency with 4 or more clients grows with the client count for an undiagnosed reason (B-008, B-010). Client and server share one machine and one set of cores; a separate client machine is needed for clean multi-client latency.
- The gRPC server uses gRPC's default internal threading plus our pool; neither the gRPC thread count nor the pool size was tuned (pool fixed at 8).
- The `string_view` temporary-`std::string` lookup cost (see `DESIGN.md` D-003) has not been measured against a transparent-hash variant.
- All Phase 2 numbers come from a Mac with other processes (editor, assistant) running and thermal state unrecorded; differences under about 10% in throughput and 25% in tails should not be treated as real.
- None of the numbers in this file may go on the CV without the hardware, command and commit given in this file.
