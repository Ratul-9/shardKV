# Learning Notes (knowledge database)

Everything Ratul has studied, answered, got wrong and fixed, in one place. Updated at the end of every phase, and whenever a concept comes up that is worth keeping. `docs/INTERVIEW_NOTES.md` holds the graded drills; this file holds the knowledge behind them.

CS:APP references give chapters and sections (no page numbers), taken from the mapping in `PROJECT_CONTEXT.md`.

Legend: **[Knew]** you stated it correctly, **[Refined]** you were right but it was sharpened, **[Corrected]** a misunderstanding that was fixed, **[To do]** you still owe an answer or action.

---

## Phase 0: Foundations and measurement

### Topics Studied
- Latency as a distribution: p50 / p99 / p999 and histograms (HdrHistogram-style). Resource: Gil Tene, *How NOT to Measure Latency* (talk). Not in CS:APP.
- Coordinated omission, open-loop vs closed-loop load generation. Same resource. Not in CS:APP.
- Benchmark hygiene: warm-up, steady state, stopping the optimiser deleting the work (`benchmark::DoNotOptimize`, `ClobberMemory`). Google Benchmark docs. CS:APP Ch. 5 covers optimisation and profiling, not benchmark methodology.
- Sanitizers: ASan, UBSan, TSan, and why ASan and TSan can't be combined. clang.llvm.org sanitizer docs. Memory-bug background in CS:APP Ch. 9; races in Ch. 12.
- Build tooling: CMake presets, one build tree per sanitizer, `FetchContent` to pin dependencies. CMake docs. Not in CS:APP.
- Timer behaviour: sleep wake-up jitter vs spinning (CS:APP Ch. 8 for processes and scheduling).

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
- `std::unordered_map` memory layout: bucket array plus separately allocated nodes, pointer chasing, cache misses. CS:APP Ch. 6 (cache-friendly code) and Ch. 9 (dynamic allocation).
- Cache sizes on this machine: L1D 64 KB, L1I 128 KB, L2 6 MB (benchmark header). Used to choose 1K / 100K / 1M working sets; 1M entries is about 150 MB, far beyond L2.
- API ownership: owning copy vs non-owning view, view invalidation (cppreference `std::string_view`). Not in CS:APP.
- Heterogeneous lookup for unordered containers (C++20): transparent hash and equality avoid a temporary key. cppreference `unordered_map::find`. Not in CS:APP.
- `insert_or_assign` vs `operator[]` vs `emplace` vs erase-then-insert. cppreference. Not in CS:APP.
- Small-string optimisation (SSO): libc++ inlines strings up to 22 bytes, libstdc++ up to 15, which changes allocation counts between macOS and Linux. Not in CS:APP; CS:APP Ch. 9 covers the allocator side.
- Microbenchmark design: working-set size vs cache, on-the-fly key generation, random access, prefill, amortised timing. CS:APP Ch. 6 (locality), Ch. 5 (profiling).
- Memory footprint: requested bytes (counting `operator new`) vs RSS. CS:APP Ch. 9 (virtual memory, RSS).
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

## Phase 2: Naive concurrent server (the mutex baseline)

### Topics Studied
- Mutex cost under contention: blocking and wake-up cost, cache-line bouncing of the lock word. CS:APP §12.4, §12.5.3, §12.6, §8.2.5 (context switches); Drepper §3.3 for coherence (not in CS:APP).
- Amdahl's law applied to a global lock. CS:APP §1.9.1 and §12.6. (Earlier notes said Ch. 5; that was wrong.)
- Thread pools, producer-consumer queues, condition variables, predicate loops, lost wake-ups, draining shutdown. CS:APP §12.5.4-12.5.5 (prethreaded server; CS:APP uses semaphores, not condition variables); *C++ Concurrency in Action* ch. 4 and §9.1.
- `std::shared_mutex` and the readers-writers problem. CS:APP §12.5.4; *C++ Concurrency in Action* §3.3.2.
- Thread safety, data races, deadlock. CS:APP §12.7.1-12.7.5.
- Copying a value while holding the lock (view lifetime). D-002, D-008.
- gRPC and Protocol Buffers: services, `bytes` vs `string`, callback API, reactors, one connection per client. gRPC and protobuf documentation (not in CS:APP).
- Open-loop vs closed-loop load generation with many clients. Gil Tene talk (not in CS:APP).
- Signal handlers that only set a flag. CS:APP §8.5.5.
- Timer behaviour: `sleep_for` overshoot is proportional to the request (about 25-30% on macOS). Apple Energy Efficiency Guide (timer coalescing).

### What you have shown you know
- **[Knew]** Waiting threads cannot do useful work while blocked (serialisation).
- **[Refined]** Why contention makes things *slower*: wake-up cost and cache-line bouncing, not only waiting.
- **[To do]** Why copy-after-unlock is unsafe (use-after-free, data race) and what the copy costs (you answered "do not know").
- **[Corrected]** `shared_mutex` is not automatically better for read-heavy loads. Your prediction was wrong here; measured in B-005 (the reasoning, that readers run in parallel, is right in principle but the critical section is too short).
- Phase 2 drill: model answers written by Claude (not Ratul's own, not graded) in `docs/INTERVIEW_NOTES.md`.

### Key Insights
- **A global mutex makes more clients slower, not just flat.** In process, 1M keys: 4.78M ops/s with 1 client, 1.90M with 4, plateau near 2.2M at 16 and 64 (B-005). Nearly the whole operation is inside the lock, and a contended lock costs more than the 100 ns of work it protects.
- **`shared_mutex` lost to `mutex` at every client count** (0.72x with 1 client, 0.15x with 64 at 1M keys). On this machine libc++ builds it from a mutex plus two condition variables, so each reader briefly takes a mutex. Specific to macOS/libc++; re-measure on Linux.
- **At the service level the lock disappears:** the gRPC server tops out near 52K req/s while the store handles ~2.2M ops/s, so the store lock is about 2% of a request's cost. The Phase 2 conclusion "the lock limits scaling" is visible in process but hidden behind gRPC cost end to end. (B-005 vs B-007.)
- **Handing work to a thread pool is not free:** the pool cost ~22% of server throughput against running inline, because the store work is tiny compared with the queue, wake-up and context-switch cost. A pool pays off only when tasks are long or blocking.
- **A measurement tool can be the bug.** The open-loop latency grew with client count; the identical numbers across three server configurations pointed away from the server; a 20-line experiment (`sleep_for` overshoot is ~25-30% of the request on macOS) found the cause (B-009). Fixing it removed part of the effect; a control experiment showed the rest is not the generator, and we recorded it as undiagnosed instead of explaining it away (B-010).
- **Thread pool, how it works:** one mutex guards one queue; idle workers sleep on a condition variable; tasks run outside the lock; shutdown drains. Design in D-007.
- **Lost wakeup:** if the stop flag or the queue changes without the mutex the workers wait on, a worker can check the predicate, sleep just before the notify, and never wake. Always change the predicate's state under the same mutex.
- **Predicate loop:** `wait` can wake spuriously and another worker may take the task first, so re-check every time.
- **An exception escaping a `std::thread` function calls `std::terminate`;** the pool catches it (and hides the error).
- **A single queue lock is a contention point at high task rates;** this is what Phases 3 and 4 remove.
- **Design decisions:** D-007 thread pool, D-008 locking (copy under the lock, `mutable` mutex, locked `Size`), D-009 callback-API server with pool or inline, D-010 open-loop wait strategy.
- **TSan limits:** the system gRPC libraries are not instrumented, so gRPC targets are skipped in sanitizer builds; the locks and pool are TSan-tested in process instead.

### Mistakes & Fixes
- (Mine) Cited Amdahl's law as CS:APP Ch. 5; it is §1.9.1 (and §12.6) -> corrected in `TOPICS.md` and these notes.
- (Mine, from Phase 0) A 100 us spin margin in the open-loop wait was an untested guess and was wrong on macOS -> measured the overshoot, switched to "sleep half the remaining time", added a regression test (D-010).
- (Mine) First regression test asserted on p99 and failed once under ASan on a loaded machine -> asserted on the median instead, because the bug was systematic and the median ignores a single scheduler hiccup.
- (Mine) A scripted edit ran in the wrong working directory and silently did nothing; I only noticed because the later checks printed nothing where output was expected -> always check that a step produced output, and use absolute paths.
- (Mine) Summariser was not marked executable (`chmod` covered only `.sh` files) -> `chmod +x`.
- (Tooling) macOS has no `timeout` command (use `perl -e 'alarm N; exec @ARGV'`); zsh does not word-split an unquoted `$VAR`, so scripts that build argument lists need bash arrays or `bash -c`.
- (Tooling) `-Wconversion -Werror` rejected `std::string(n, 'a' + t)` (int to char); cast explicitly.
- (Ratul) The ThreadPool and locked stores were left to be written by Claude; their design choices must now be read and defended (drill Q5, D-007, D-008).

### Code References
- Thread pool: `include/lldstore/thread_pool.h` (worker loop `WorkerLoop`, destructor shutdown, `Submit`); tests `tests/thread_pool_test.cpp`
- Locked stores: `include/lldstore/locked_store.h` (`MutexStore`, `SharedMutexStore`); stress tests `tests/locked_store_test.cpp`
- Open-loop wait fix: `include/lldstore/open_loop.h` (`WaitUntil`); regression test `tests/open_loop_test.cpp` (`LongIntervalsDoNotAddSleepOvershootToLatency`)
- Multi-client load generator: `include/lldstore/multi_client.h`
- In-process contention tool: `bench/contention.cpp`
- gRPC: `proto/kv.proto`, `bench/kv_server.cpp`, `bench/kv_client.cpp`
- Benchmark scripts and summariser: `scripts/`
- Results and interpretation: `docs/BENCHMARKS.md` B-005 to B-010; raw files `bench/results/phase2_*`
- Decisions: `docs/DESIGN.md` D-007 to D-010
- Drill questions and grades: `docs/INTERVIEW_NOTES.md` (Phase 2 pre-check, Phase 2 drill)

**End of Phase 2** (tag `phase-2-done`; the Phase 2 drill and earlier retries carried into Phase 3)

---

## Phase 3: Sharding and thread-per-core  (IN PROGRESS: implemented and compiled, not yet run)

### Topics Studied
- Partitioning by hash (`hash(key) % N`), skew and hot keys, and why consistent or rendezvous (highest-random-weight) hashing only pays off when N changes. *Designing Data-Intensive Applications* ch. 6; not in CS:APP.
- Shared-nothing and thread-per-core designs: each shard owned by one thread, message passing between threads. Seastar design notes (shared-nothing); not in CS:APP. Shared-variable background: CS:APP §12.4.
- Why `std::hash` must not be used for routing across platforms (differs between libstdc++ and libc++); FNV-1a and the splitmix64 finalizer (Fowler-Noll-Vo; Vigna). Not in CS:APP.
- Bounded producer-consumer queues, backpressure, two condition variables, draining shutdown. CS:APP §12.5.4 (producer-consumer, with semaphores); *C++ Concurrency in Action* §4.1.2.
- Request/reply between threads without allocation: a stack object plus a mutex and condition variable; why `notify` is done while holding the lock (object lifetime). *C++ Concurrency in Action* ch. 4.
- CPU affinity on Linux: `pthread_setaffinity_np(3)`, `sched_getcpu(3)`, `sched_getaffinity(2)`, `taskset(1)`, `lscpu -e` (SMT siblings share a CORE number). Linux man pages; not in CS:APP. Context-switch cost: CS:APP §8.2.5.
- False sharing and cache-line alignment (`alignas(64)`); GCC 11 lacks `std::hardware_destructive_interference_size`. CS:APP §6.4-6.5; Drepper §6.4.
- Verifying code you cannot run: compile under several sanitizer presets, and syntax-check a platform branch against a stand-in header.

### What you have shown you know
- **[Knew]** Asked the right design questions before coding: which sharding scheme (hash vs consistent vs range), whether highest-random-weight hashing helps (it does not for a fixed N), and the difference between shards (threads in one process) and nodes (processes in a cluster, Phase 8).
- **[Knew]** Phase 3 is shared-nothing for the data only; the queues and replies are still shared (you confirmed the right phrase: "shared-nothing shards with message passing").
- **[To do]** Read and defend the Phase 3 code (written by Claude): see the model answers in `docs/INTERVIEW_NOTES.md`.

### Key Insights
- **Shared-nothing moves the cost, it does not remove it.** The data needs no lock, but every request now crosses a thread boundary through a queue: a lock, a wake-up and a context switch each way. Whether that is cheaper than lock contention is exactly what the Phase 3 benchmark must answer; it is a prediction until measured (B-011 is planned, not run).
- **Route with a function you control.** `std::hash` differs between standard libraries, so the same key would land on different shards on the Mac and the Linux machine. FNV-1a plus a finalizer is fixed code, and unit tests pin known hash values and shard numbers so it can never change silently. The expected load imbalance is about 2% (computed independently).
- **Backpressure is a design decision.** A bounded queue that blocks producers turns overload into waiting instead of unbounded memory and latency. The alternatives (drop, reject, grow) each push a decision onto callers.
- **Object lifetime across threads.** The request lives on the caller's stack, which is safe only because the caller blocks until the worker is done; and the worker must notify while holding the lock, or the caller could destroy the condition variable before the notify runs.
- **The queue is the replaceable part.** Keeping it a small generic class (`Push`, `Pop`, `Stop`) means Phase 4 can swap in a lock-free queue behind the same interface and the before/after benchmark is a clean comparison.
- **Pin from inside the thread** being pinned, and report the result (`PinnedCpus()`); a pin that silently failed would make a benchmark meaningless.
- **Honest verification limits.** The code compiled under five presets with Apple clang, but never ran, never saw GCC 11, and the Linux branch was checked only against a stand-in header. A clean compile is evidence of syntax and types, not of correctness.
- **Design decisions:** D-012 shard count and routing hash, D-013 queue, D-014 reply path, D-015 pinning, D-016 semantics, D-017 test strategy and status.

### Mistakes & Fixes
First draft of `shardedStore.h` (Ratul's), found in review:
- `Size()` called itself (infinite recursion) -> sum the per-shard sizes.
- `Get`, `Delete` and `Size` had empty bodies but return values (undefined behaviour) -> they must return a value on every path.
- `Put` and `Delete` were `const` although they modify data -> not `const`.
- `shardIndex(int key)` took an `int` although keys are strings, so `key % R` cannot work -> hash the `string_view` first.
- The constructor created a local `Store s;` and discarded it, so there were no shards -> shards must be members created in the constructor.
- `#define R 6` leaked into every includer and hard-coded the shard count -> a runtime option.
- `Get` returned a `string_view` into data another thread can change -> copy into the caller's string (D-002).
- No deleted copy operations and no destructor, although the class owns threads -> delete copying, stop and join in the destructor.
- File name `shardedStore.h` broke the snake_case convention -> `sharded_store.h`.

By Claude while writing Phase 3:
- A comment on `BoundedQueue::Push` was garbled -> rewritten.
- A parameter named like a member function (`num_shards`) could trigger GCC's stricter `-Wshadow` -> renamed `shard_count`.
- Cannot run tests or see GCC here -> compile in five presets, syntax-check the Linux branch with a stand-in header, and say plainly that nothing has run.

Tooling: Docker was installed but its daemon was not running, so a GCC 11 container could not be used to pre-check the code.

### Code References
- Hash and routing: `include/lldstore/sharded_store.h` `ShardFor` line 140, `HashKey` line 149
- Request and reply: `Request` line 180, `Complete` line 197, `Dispatch` line 276
- Shard, worker loop, pinning: `Shard` line 213, `Run` line 220, `Execute` line 228, `PinCurrentThread` line 249
- Public interface: constructor line 56, destructor line 73, `Put`/`Get`/`Delete`/`Size` lines 83-112, `PinnedCpus` line 165
- Bounded queue: `include/lldstore/bounded_queue.h` `Push` line 38, `Pop` line 54, `Stop` line 68
- Tests: `tests/bounded_queue_test.cpp`, `tests/sharded_store_test.cpp`
- Benchmark harness: `bench/contention.cpp` (`--store=sharded`), `scripts/bench_sharded.sh`
- Decisions: `docs/DESIGN.md` D-012 to D-017; task list: `docs/PHASE3_TASKS.md`; planned measurements: `docs/BENCHMARKS.md` B-011
- Drill: `docs/INTERVIEW_NOTES.md` (Phase 3 drill, model answers)

(Phase 3 section is closed with "End of Phase 3" only after the tests and benchmarks have run and the phase is tagged.)

---

## Phase 4: Lock-free MPSC queue  (IN PROGRESS: implemented and build-checked, not yet run)

### Topics Studied
- Lock-free vs wait-free, and what they do and do not promise. *The Art of Multiprocessor Programming* ch. 3 and 5; *C++ Concurrency in Action* ch. 7. Not in CS:APP.
- The C++ memory model: `memory_order` relaxed, acquire, release, acq_rel, seq_cst; happens-before; why x86 hides ordering bugs. *C++ Concurrency in Action* ch. 5; cppreference `std::memory_order`. Hardware background: CS:APP §12.4 (shared variables), §6.4 (caches).
- Vyukov's intrusive MPSC node queue (exchange the tail, then link): 1024cores.net. Not in CS:APP.
- ABA and memory reclamation (why a CAS-free design with one consumer avoids both). *The Art of Multiprocessor Programming* ch. 10; *C++ Concurrency in Action* §7.2.
- `std::atomic::wait`/`notify_one` (C++20) as a futex-style sleep keyed on a value. cppreference `std::atomic::wait`.
- False sharing of head/tail/counter (`alignas(64)`). CS:APP §6.4-6.5; Drepper §6.4.

### What you have shown you know
- **[Knew]** Asked whether the design is "wait-free" and recognised Vyukov's as the XCHG pattern.
- **[Corrected]** "A bounded queue cannot be lock-free, so it must be unbounded": bounded lock-free queues exist (Vyukov's bounded ring); unbounded is a simplicity choice with a real cost (no backpressure), not a necessity.
- **[Corrected]** "One MPSC queue all threads write to, one reader": that would give the whole store ONE consumer and destroy the sharding. The design is one MPSC queue per shard: many producers, that shard's single worker as the consumer.
- **[Corrected]** The skeleton kept a `deque`, a `std::thread` and a mutex-era include list. The queue does not own the consumer thread, and a deque cannot be shared between threads.
- **[To do]** Draw the acquire/release edges of D-019 from memory and defend D-018 to D-021 (code written by Claude).

### Key Insights
- **"Lock-free" is a claim you have to read carefully.** Producers never wait, but the consumer can be held up by a preempted producer. Say "wait-free push, non-blocking pop with a blocking window".
- **Release/acquire is about the data, not the pointer.** The pointer store/load is the carrier; the point is that everything written before the release is visible after the acquire. On x86 you can get this wrong and never see it.
- **No CAS means no ABA.** With one consumer freeing a node only after its successor is linked, the reclamation problem of general lock-free structures disappears; it comes back in Phase 9 for structures that need it.
- **Removing the mutex does not remove the cost.** Still there: a heap allocation per push, an atomic counter every producer shares, and the consumer's wake-up. Whether the net is a win is B-012, unmeasured.

### Code References
- `include/lldstore/mpsc_queue.h`: `Push`, `TryPop`, `Pop`, `Stop`; `state_` word
- `include/lldstore/sharded_store.h`: `BasicShardedStore<QueueT>`, aliases at the bottom
- Tests: `tests/mpsc_queue_test.cpp`, `tests/lockfree_sharded_store_test.cpp`
- Decisions: `docs/DESIGN.md` D-018 to D-021; planned measurements: `docs/BENCHMARKS.md` B-012
- Drill: `docs/INTERVIEW_NOTES.md` (Phase 4 drill, model answers)

---

## Phase 5: Cache-conscious hash table  (IN PROGRESS: implemented and built; one debug test run; no benchmark)

### Topics Studied
- Memory hierarchy, cache lines, locality, counting cache misses per operation. CS:APP §6.2-6.6.
- Open addressing, linear probing, primary clustering, expected probe lengths; Knuth's deletion algorithm (backward shift). Knuth, *The Art of Computer Programming* vol. 3 §6.4; not in CS:APP.
- Load factor and resizing strategies (doubling, incremental). Cormen et al., hash tables chapter; Redis `dict.c` (incremental rehash); not in CS:APP.
- Swiss tables and Robin Hood hashing (the follow-ups). Kulukundis, "Designing a Fast, Efficient, Cache-friendly Hash Table, Step by Step" (CppCon 2017); Celis, *Robin Hood Hashing* (1986); not in CS:APP.
- Hash function construction: word-at-a-time mixing, finalizers, why length must be mixed in, HashDoS. splitmix64 (Vigna); SipHash paper (Aumasson, Bernstein); not in CS:APP.
- Aligned allocation (`operator new(size, std::align_val_t)`), alignment and padding. CS:APP §3.9.3 (data alignment); cppreference `operator new`.
- Reading the memory cost of a design: requested bytes vs RSS, size classes. CS:APP §9.9.

### What you have shown you know
- **[To do]** Phase 5 pre-check (`docs/INTERVIEW_NOTES.md`): not answered yet.
- **[To do]** Phase 5 was written by Claude at Ratul's request, with no code comments. Work through `docs/PHASE5_DESIGN_GUIDE.md`, its exercises (Part 7) and the defend-it questions.

### Key Insights
- **Count cache misses, not instructions,** once the data is bigger than the caches. The design follows from that count: fewer lines per probe, a hash filter before following a pointer, one allocation per entry.
- **Out-of-line entries keep views stable.** Open addressing usually breaks D-002 because entries move on growth. Here only the 16-byte slots move, so a view survives inserts and growth, and a test pins it.
- **Store the full hash:** it filters non-matches, makes growth avoid touching entries, and gives deletion each slot's home for free.
- **Two hashes over one key must be independent.** Shard routing and slot choice must not use the same bits, or a shard's keys share low bits and fill only part of the table.
- **Deletion in linear probing:** backward shift instead of tombstones.
- **A knob is not a result.** The 0.75 load factor stays a guess until the load sweep runs.
- **Controls make a benchmark honest.** `TransparentStore` separates "no temporary key string" from "open addressing"; without it a libstdc++-specific win would be credited to the cache design.
- **Adversarial test inputs** (constant hash, first-byte hash, chosen home slots) force the rare paths that random tests with a good hash almost never reach.
- **Design decisions:** D-022 to D-030 (`docs/DESIGN.md`); teaching narrative in `docs/PHASE5_DESIGN_GUIDE.md`.

### Mistakes & Fixes
- (Claude, while writing) The first `footprint` change kept the store alive by deliberately leaking it -> restructured so the measurement happens inside a function that owns the store.
- (Claude, while writing) The first `footprint` would not have counted the slot array at all, because aligned `operator new` was not replaced -> added counting aligned `new`/`delete`.
- (Process) Phase 5 was started before Phases 3 and 4 were run or tagged, against the plan's "one phase at a time" rule (Ratul's decision, after a reminder).

### Code References
- `include/lldstore/flat_store.h`: `FlatHash` lines 17-55; `Put` 81-97; `Get` 99-104; `Delete` and backward shift 106-122; `Reserve` 124-128; `Entry` 139-150; `Slot` 152-158; `Home` 160; `Probe` 162-167; `Rehash` 179-192; `Assign` 194-203; `AllocateSlots` 205-214; `MakeEntry` 216-224
- `include/lldstore/sharded_store.h`: `BasicShardedStore<QueueT, StoreT>`, alias `FlatShardedStore`
- Tests: `tests/flat_store_test.cpp`, `tests/flat_store_suite_test.cpp`, `tests/flat_sharded_store_test.cpp`
- Benchmarks: `bench/table_bench.cpp`, `bench/footprint.cpp` (`map|flat`), `bench/contention.cpp` (`--table=flat`), `scripts/bench_table.sh`; plan in `docs/BENCHMARKS.md` B-013
- Decisions: `docs/DESIGN.md` D-022 to D-030; guide: `docs/PHASE5_DESIGN_GUIDE.md`; drill: `docs/INTERVIEW_NOTES.md` (Phase 5)

(The Phase 5 section is closed with "End of Phase 5" only after the tests and benchmarks have run and the phase is tagged.)
