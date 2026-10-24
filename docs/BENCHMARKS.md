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

## Not yet measured / known limits
- Single-threaded only; multi-threaded numbers start in Phase 2.
- No Linux or x86 baseline yet (needed from Phase 3 for pinning and `perf`).
- The `string_view` temporary-`std::string` lookup cost (see `DESIGN.md` D-003) has not been measured against a transparent-hash variant.
- None of the numbers above may go on the CV without the hardware, command and commit given in this file.
