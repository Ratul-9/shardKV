# Learning Notes (knowledge database)

Everything Ratul has studied, answered, got wrong and fixed, in one place. Updated at the end of every phase, and whenever a concept comes up that is worth keeping. `docs/INTERVIEW_NOTES.md` holds the graded drills; this file holds the knowledge behind them.

CS:APP page numbers are left as `pp. ___` for Ratul to fill in from the physical book. Chapters and sections come from the mapping in `PROJECT_CONTEXT.md`, and I have not guessed page numbers.

Legend: **[Knew]** you stated it correctly, **[Refined]** you were right but it was sharpened, **[Corrected]** a misunderstanding that was fixed, **[To do]** you still owe an answer or action.

---

## Phase 0: Foundations and measurement

### Topics Studied
- Latency as a distribution: p50 / p99 / p999 and histograms (HdrHistogram-style). Resource: Gil Tene, *How NOT to Measure Latency* (talk). Not in CS:APP.
- Coordinated omission, open-loop vs closed-loop load generation. Same resource. Not in CS:APP.
- Benchmark hygiene: warm-up, steady state, stopping the optimiser deleting the work (`benchmark::DoNotOptimize`, `ClobberMemory`). Google Benchmark docs. CS:APP Ch. 5 covers optimisation and profiling (pp. ___), not benchmark methodology.
- Sanitizers: ASan, UBSan, TSan, and why ASan and TSan can't be combined. clang.llvm.org sanitizer docs. Memory-bug background in CS:APP Ch. 9 (pp. ___); races in Ch. 12 (pp. ___).
- Build tooling: CMake presets, one build tree per sanitizer, `FetchContent` to pin dependencies. CMake docs. Not in CS:APP.
- Timer behaviour: sleep wake-up jitter vs spinning (CS:APP Ch. 8 for processes and scheduling, pp. ___).

### What you have shown you know (from your answers)
- **[Knew]** A closed-loop generator under-counts a stall: 1 slow sample recorded where 1000 requests were due.
- **[Knew]** ASan and TSan use incompatible runtime instrumentation/layout, so each needs its own build.
- **[Knew]** The compiler may delete a benchmark whose result is unused; consume the result with `DoNotOptimize`.
- **[Knew]** Log-linear buckets exist because one bucket per nanosecond is too expensive; the price is precision.
- **[Knew]** Reporting a bucket's upper bound avoids understating latency.
- **[Knew]** Sleep-then-spin gives timing precision without burning a core for the whole wait.
- **[Refined]** p99 definition: right, but the main reason it matters is tail amplification (see below).
- **[Refined]** Coordinated omission: right example, but name the cause (closed loop waits for the reply) and note the late requests have a spread of latencies.

### Key Insights
- **Tail amplification:** if a user action makes `n` requests, the chance of hitting at least one p99-slow request is `1 - 0.99^n` (63% at n=100). The mean hides this; the tail is what users feel.
- **Coordinated omission is a measurement bug, not a server bug.** A stall that removes the samples which would expose it makes the server look better than it is. Open-loop fixes it by charging latency from the *scheduled* time, so a 1 s freeze at 1000 req/s yields ~1000 late samples with latencies spread from ~1 s down to ~0.
- **Design decision: log-linear histogram** (values < 128 exact; each power of two split into 64 sub-buckets; error <= 1/64 = 1.6%). A bucket per ns up to 2^40 would need ~10^12 buckets (~8 TB); this uses ~2240 buckets (~18 KB) and records in O(1) with no allocation.
- **Design decision: report the bucket upper bound** (clamped to the observed max), so the true value is never above what we report.
- **Design decision: one sanitizer per build tree**, flags applied globally so fetched dependencies are instrumented too (TSan needs every translation unit instrumented).
- **Design decision: warnings only on our code** (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`), third-party included as SYSTEM so their warnings don't fail our build.
- **Platform fact:** Apple clang on macOS has no leak checking in ASan; Linux CI does.

### Mistakes & Fixes
- Said all 1000 late requests are "slow" at 1 s -> they are 1000 requests with a spread of delays (roughly 1 s down to 0). Remember the spread; it is why the open-loop p99 is large but not equal to the stall length.
- Gave p99 as "bounded performance" only -> add the fan-out argument and compute the number.
- Left out the cause of coordinated omission -> say "closed-loop: waits for each response before sending the next".
- Treated sleep-then-spin as purely a precision trick -> also: sleeping frees the core for other threads, and the 100 us spin margin is an untested guess that must be tuned from measured wake-up jitter.
- Tooling note: IDE showed "no member named clamp / bit_width" errors. Cause: the IDE wasn't using C++20 or the project's include paths. The real build (`-std=c++20`) was fine. Not a code bug.

### Code References
- Histogram bucket index: `include/lldstore/latency_histogram.h` lines 67-73 (`IndexOf`), inverse at lines 75-82 (`HighestEquivalentValue`)
- Percentile lookup (upper bound, clamped to max): `include/lldstore/latency_histogram.h` lines 38-49
- Open-loop runner (intended-time latency): `include/lldstore/open_loop.h` lines 45-70
- Sleep-then-spin wait: `include/lldstore/open_loop.h` lines 28-37
- Coordinated-omission test: `tests/open_loop_test.cpp` lines 36-49 (`StallIsChargedToEveryMissedSlot`)
- Sanitizer selection: `CMakeLists.txt` lines 9-27; presets in `CMakePresets.json`
- Smoke benchmark (record 1.3 ns, percentile 310 ns, `steady_clock::now` 13 ns, taken at load average ~9 so NOT valid for the CV): `bench/histogram_bench.cpp`; not recorded in `docs/BENCHMARKS.md`
- Interview drill: `docs/INTERVIEW_NOTES.md` (Phase 0)

**End of Phase 0** (tag `phase-0-done`)

---

## Phase 1: Single-threaded baseline store

### Topics Studied
- `std::unordered_map` memory layout: bucket array plus separately allocated nodes, pointer chasing, cache misses. CS:APP Ch. 6 (cache-friendly code, pp. ___) and Ch. 9 (dynamic allocation, pp. ___).
- Cache sizes on this machine: L1D 64 KB, L1I 128 KB, L2 6 MB (benchmark header). Used to choose 1K / 100K / 1M working sets; 1M entries is about 150 MB, far beyond L2.
- API ownership: owning copy vs non-owning view, view invalidation (cppreference `std::string_view`). Not in CS:APP.
- Heterogeneous lookup for unordered containers (C++20): transparent hash and equality avoid a temporary key. cppreference `unordered_map::find`. Not in CS:APP.
- `insert_or_assign` vs `operator[]` vs `emplace` vs erase-then-insert. cppreference. Not in CS:APP.
- Small-string optimisation (SSO): libc++ inlines strings up to 22 bytes, libstdc++ up to 15, which changes allocation counts between macOS and Linux. Not in CS:APP; CS:APP Ch. 9 covers the allocator side.
- Microbenchmark design: working-set size vs cache, on-the-fly key generation, random access, prefill, amortised timing. CS:APP Ch. 6 (locality), Ch. 5 (profiling, pp. ___).
- Memory footprint: requested bytes (counting `operator new`) vs RSS. CS:APP Ch. 9 (virtual memory, RSS, pp. ___).
- Testing: example tests, edge cases (empty, prefix, NUL bytes, large), model-based random testing, and **mutation testing** (inject bugs to check the tests can fail). Not in CS:APP.

### What you have shown you know
- **[Knew]** A lookup hashes the key, indexes a bucket, follows a pointer to a node, then compares the key; scattered nodes mean cache misses.
- **[Refined]** You estimated 1-2 cache misses. A fuller count is 2-4 (libc++ buckets point to the node *before* the target; long keys need another hop; the value is a further load).
- **[Knew]** A view into the store dangles if the store changes while another thread reads it.
- **[Corrected]** You said views "need to be freed". A `string_view` is non-owning; nothing is freed. The risk is invalidation, not leaking. Phase 1 drill: you listed Put/Delete/clear/destruction correctly; a `Put` of a *different* key does not invalidate (node-based map). Re-answers from earlier drills are self-reported, not seen by me.
- **[Knew]** All-in-L1 keys would misrepresent a large memory-bound table. This is why we measure 1K, 100K and 1M.
- **[Knew]** Edge cases worth testing: empty key/value, prefix keys, NUL bytes, large values (your first test file covered these well).

- **Drill results:** Q1 B+, Q2 B+, Q3 A-; Q4 and Q5 not answered (model answers in `docs/INTERVIEW_NOTES.md`, retry owed).
- **[Corrected]** A mutex only fixes view lifetime if it covers the whole use of the view, not just the `Get` call. Copy under the lock.
- **[Knew]** Remedies for cross-thread views: owning copy, mutex, `shared_ptr` (plus epoch reclamation in Phase 9 and shard ownership in Phase 3).

### Key Insights
- **Container property that will not survive Phase 5:** because `unordered_map` is node-based, elements never move, so views stay valid across inserts of other keys. Open addressing moves entries on growth, which breaks this.
- **Latency grows with working set.** Get hit: 14 ns (1K keys) -> 25 ns (100K) -> 84 ns (1M). The 1M figure is the memory-bound number later phases must beat.
- **A miss can be cheaper or dearer than a hit.** At 1M keys a miss (71 ns) beats a hit (84 ns) because many lookups end at an empty bucket without touching a node. At 1K keys a miss (19 ns) is slower than a hit (14 ns) because it must walk past colliding nodes.
- **Throughput benchmark vs open-loop latency can disagree, and both are right.** The tight loop gives 84 ns for a Get at 1M keys; the paced open-loop run gave a p50 of 375-459 ns for the same operation, because the core idles for ~5 us between operations (lower clock, colder caches). Quote them for what they are, not interchangeably.
- **The latency tail on this machine is the OS, not the store.** p999 of 36-50 us and max of 1-1.6 ms on a ~100 ns operation come from scheduling noise (no pinning on macOS).
- **The clock has a resolution.** `steady_clock` ticks at 24 MHz on this Mac, so values under ~42 ns are quantised (our p50 of 42 ns at 1K keys is one tick).
- **Subtracting key-generation time is only approximate**, because the CPU overlaps independent work.
- **Two heap allocations per entry** (node + 64-byte value); about 1.8x payload requested, about 2.5x payload resident. The 16-byte key lives inside the node thanks to SSO on libc++, so the Linux footprint will be worse.
- **Mutation testing earns its time.** Five injected bugs, all caught, but the randomised test missed one (key truncated to 4 bytes) because its keys were at most 3 characters long. A random test is only as good as its generator.
- **One source of truth.** Keeping a separate `count_` next to the map created both a typo (`count--`) and a double-count in `Put`. `data_.size()` removes the bug class.
- **Design decisions:** D-002 string_view, D-003 plain temporary-key lookups in the baseline, D-004 overwrite via `insert_or_assign`, D-005 no separate counter, D-006 test strategy (all in `docs/DESIGN.md`).

### Mistakes & Fixes
First draft of `store.h` -> these were found in review:
- Put and Get ended with the skeleton's `std::abort()` -> remove placeholders when implementing; an unreachable `abort()` in `Get` hid that the function had other bugs.
- `return std::optional<std::nullopt>;` is not C++ (`nullopt` is a value, not a type) -> `return std::nullopt;`.
- `data_[std::string(key)]` inside a `const` method can't compile (`operator[]` can insert) -> use the iterator you already have: `it->second`.
- Looked the key up twice in `Get` (find, then operator[]) and three times in `Put` -> one lookup: `find` + iterator, or `insert_or_assign`.
- `Put` wrote the value twice (once when inserting, then again unconditionally) -> a single `insert_or_assign`.
- `count--` typo (the member is `count_`), and `Delete` had no `return true` at the end (undefined behaviour) -> fixed; then removed `count_` entirely.
- Missing `#include <string>` / `<unordered_map>` in the first draft -> added.
- `Delete` hashed the key twice (`find`, then `erase(key)`) -> `erase(it)`.
- Tests compared `size_t` with signed ints (`Size(), 0`) -> use `0u` / `size_t` literals.
- Test gaps found in review: no randomised model test, no NUL bytes in keys, delete only checked the deleted key, no overwrite with different sizes, no rehash test -> all added (5 new tests).

Tooling lessons:
- IDE red errors ("no member named optional", "file not found") were the editor parsing without C++20 or project include paths, not real errors. Trust the actual build.
- While mutation-testing with a script, `make` rebuilt only on one-second timestamps, so rewriting a header within the same second as the previous compile reused a stale object file and gave wrong results (I caught it because two different mutations printed identical output). Fix: compile directly for mutation runs, and sanity-check that results differ when the mutation differs. Never trust a result you didn't see change.
- BSD `sed` on macOS does not support GNU-only address syntax like `0,/re/`; use Python for scripted edits.

### Code References
- Put (single lookup, overwrite): `include/lldstore/store.h` lines 20-24
- Get (view into the map's string): `include/lldstore/store.h` lines 27-36
- Delete (erase by iterator): `include/lldstore/store.h` lines 39-46
- Size and single source of truth: `include/lldstore/store.h` lines 48 and 51-53
- Edge-case tests: `tests/store_test.cpp` lines 76-135
- Rehash test: `tests/store_test.cpp` lines 163-183
- Randomised model test: `tests/store_test.cpp` lines 186-end
- Throughput benchmarks: `bench/store_bench.cpp`; open-loop latency: `bench/store_latency.cpp`; footprint: `bench/footprint.cpp`; key helpers: `bench/bench_util.h` lines 29-46
- Results and interpretation: `docs/BENCHMARKS.md` B-002 to B-004; raw output `bench/results/`
- Mutation results: `docs/DESIGN.md` D-006
- Commit: `0eef24b` (code, tests, benchmark tools); drill questions: `docs/INTERVIEW_NOTES.md` (Phase 1)

**End of Phase 1** (tag `phase-1-done`; Q4 and Q5 of the drill and the earlier re-answers carried into Phase 2)

---

## Phase 2: Naive concurrent server (the mutex baseline)  (IN PROGRESS)

### Topics Studied
- Mutex cost under contention: blocking/wake-up cost and cache-line bouncing of the lock word. CS:APP Ch. 12 (synchronisation, pp. ___), Ch. 6 (cache coherence background, pp. ___). Coherence protocol detail is not in CS:APP (Drepper's memory paper).
- Amdahl's law applied to a global lock. CS:APP §1.9.1 (and §12.6 for speedup and efficiency), pp. ___. (Earlier notes said Ch. 5; that was wrong.)
- `shared_mutex`: readers still write the shared reader count. cppreference `std::shared_mutex`; *C++ Concurrency in Action*. Not in CS:APP.
- Why a view must be copied while the lock is held (use-after-free and data race). Not specific to CS:APP.
- Open-loop vs closed-loop load generation with many clients: open loop for latency, closed loop only for maximum throughput. Gil Tene's talk. Not in CS:APP.

### What you have shown you know
- **[Knew]** Waiting threads cannot do useful work while blocked (serialisation).
- **[Refined]** Why contention makes things *slower* (wake-up cost, cache-line bouncing).
- **[To do]** Why copy-after-unlock is unsafe (use-after-free, data race) and what the copy costs.
- **[Corrected]** `shared_mutex` is not automatically better for read-heavy loads; the reader count is itself a contended write. Hypothesis to be tested by measurement.

### Key Insights
- **Thread pool, how it works:** one mutex guards one queue; idle workers sleep on a condition variable; tasks run outside the lock. (Written by Claude at Ratul's request; design in `docs/DESIGN.md` D-007.)
- **Lost wakeup:** if the stop flag or the queue is changed without the mutex the workers wait on, a worker can check the predicate, find it false, and sleep just before the notify; the notification is lost. Always change the predicate's state under the same mutex.
- **Predicate loop:** `wait` can wake spuriously, and another worker may take the task first, so the condition is re-checked on every wake-up.
- **Drain on shutdown:** workers exit only when stopping AND the queue is empty, so accepted tasks are never dropped.
- **An exception escaping a `std::thread` function calls `std::terminate`;** the pool catches it so one bad task cannot kill the process (at the price of hiding the error).
- **A single queue lock is a contention point** at high task rates; this is what Phases 3 and 4 remove.
- (Fill in after measuring: mutex vs shared_mutex at 1, 4, 16, 64 clients.)

### Mistakes & Fixes
- (Fill in as you write the thread pool and the locks.)
- Tooling: macOS has no `timeout` command (use `perl -e 'alarm N; exec @ARGV'`); zsh does not word-split an unquoted `$VAR`, so scripts that build argument lists need bash arrays or `bash -c`.
- Tooling: `-Wconversion -Werror` rejected `std::string(n, 'a' + t)` (int to char); cast explicitly.

### Code References
- Thread pool (written by Claude at Ratul's request, read and defend it): `include/lldstore/thread_pool.h` (worker loop `WorkerLoop`, destructor shutdown, `Submit`)
- Locked stores stubs (yours to write): `include/lldstore/locked_store.h`
- Multi-client load generator (open and closed loop): `include/lldstore/multi_client.h`
- In-process contention tool: `bench/contention.cpp`
- Stress tests (disabled until the stores exist): `tests/locked_store_test.cpp`; harness tests: `tests/multi_client_test.cpp`
- Pre-check grades: `docs/INTERVIEW_NOTES.md` (Phase 2 pre-check)
