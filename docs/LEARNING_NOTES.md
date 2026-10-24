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

## Phase 1: Single-threaded baseline store  (IN PROGRESS)

### Topics Studied
- `std::unordered_map` memory layout: bucket array plus separately allocated nodes, pointer chasing, cache misses. CS:APP Ch. 6 (cache-friendly code, pp. ___) and Ch. 9 (dynamic allocation, pp. ___).
- Cache sizes on this machine (Apple M5, from the benchmark header): L1D 64 KB, L1I 128 KB, L2 6 MB per core group. Used to pick the working-set sizes.
- API ownership: owning copy (`optional<string>`) vs non-owning view (`optional<string_view>`), and view invalidation. cppreference `std::string_view`. Not in CS:APP.
- Heterogeneous lookup for unordered containers (C++20): transparent hash and equality avoid a temporary `std::string` per lookup. cppreference `unordered_map::find`. Not in CS:APP.
- Microbenchmark pitfalls: working-set size vs cache, key generation cost, random access pattern, prefill. CS:APP Ch. 6 (locality) and Ch. 5 (profiling).
- Memory footprint: requested bytes (counting `operator new`) vs RSS. CS:APP Ch. 9 (virtual memory, RSS vs virtual size, pp. ___).

### What you have shown you know (from your answers so far)
- **[Knew]** A lookup hashes the key, indexes a bucket, follows a pointer to a node, then compares the key; scattered nodes mean cache misses.
- **[Refined]** You estimated 1-2 cache misses. A fuller count is 2-4: libc++ buckets point to the node *before* the target (extra hop), keys longer than the inline string buffer need another pointer chase, and the value is a further load. Long chains add more.
- **[Knew]** A view into the store dangles if the store changes while another thread reads it.
- **[Corrected]** You said views "need to be freed". A `string_view` is non-owning; nothing is freed. The risk is *invalidation*, not leaking: a later `Put` to that key or a `Delete` makes the view point at released or overwritten memory.
- **[Knew]** All-in-L1 keys would misrepresent a large, memory-bound table. Fix: benchmark 1K / 100K / 1M keys.

### Key Insights
- **Design decision D-002:** `Get` returns `std::optional<std::string_view>` (your choice). No allocation on reads, but the view is valid only until the next mutating call. Phase 2's locked wrapper must copy under the lock before replying. See `docs/DESIGN.md`.
- A string-keyed `unordered_map` looked up by `string_view` allocates a temporary key per `Get` unless the map is transparent. A baseline that pays that cost hides the real map cost; decide deliberately and record it.
- The benchmark generates keys on the fly instead of keeping a key table, because a key table adds its own cache misses to every lookup. `BM_KeyGenOnly` measures that cost so it can be subtracted.

### Mistakes & Fixes
- (Fill in as you write the store and tests.)

### Code References
- Store skeleton for you to implement: `include/lldstore/store.h` (all methods currently `abort()`)
- Tests to write: `tests/store_test.cpp` (placeholder + checklist)
- Benchmarks (not yet run): `bench/store_bench.cpp`, `bench/store_latency.cpp`, `bench/footprint.cpp`; shared helpers `bench/bench_util.h` lines 29-46

(Phase 1 section continues; "End of Phase 1" is added when the phase is tagged.)
