# References: everything covered through Phase 2

CS:APP = Bryant & O'Hallaron, *Computer Systems: A Programmer's Perspective*, 3rd ed. "Not in CS:APP" means use the named resource.

## Phase 0: Foundations and measurement

- Latency as a distribution (p50, p99, p999, tail amplification) — not in CS:APP; Gil Tene, "How NOT to Measure Latency" (talk); HdrHistogram documentation
- Log-linear histogram buckets and relative error — not in CS:APP; HdrHistogram documentation
- Coordinated omission, open-loop vs closed-loop load generation — not in CS:APP; Gil Tene, "How NOT to Measure Latency"
- Warm-up and steady state — CS:APP §5.14 (program profiling), §6.6 (impact of caches on performance); Google Benchmark User Guide
- Stopping the compiler deleting a benchmark (`DoNotOptimize`, `ClobberMemory`) — CS:APP §5.1 (optimizing compilers, memory aliasing); Google Benchmark User Guide
- Clock resolution, `steady_clock`, timer quantisation — not in CS:APP; cppreference `std::chrono::steady_clock`
- Sleep-then-spin waiting and scheduler wake-up jitter — CS:APP §8.2 (processes, context switches); not in CS:APP for spin/sleep trade-offs
- Undefined behaviour (signed overflow, data races, use-after-free) — CS:APP §2.3 (integer overflow), §9.11 (common memory bugs); cppreference "Undefined behavior"
- RAII, value and move semantics, `constexpr`, templates — not in CS:APP; Scott Meyers, *Effective Modern C++*
- AddressSanitizer (out-of-bounds, use-after-free) — CS:APP §3.10.3–3.10.4 (buffer overflow), §9.11.7 (use after free); Clang AddressSanitizer documentation
- UndefinedBehaviorSanitizer — Clang UndefinedBehaviorSanitizer documentation; not in CS:APP
- ThreadSanitizer (data races, happens-before) — CS:APP §12.7.4 (races); Clang ThreadSanitizer documentation
- Why ASan and TSan cannot be combined (separate shadow-memory layouts) — Clang sanitizer documentation; not in CS:APP
- CMake presets, `FetchContent`, interface targets, `add_custom_command` — CMake documentation (`cmake-presets(7)`, `FetchContent`); not in CS:APP
- Unit testing and typed tests — GoogleTest documentation (Primer, Advanced Guide); not in CS:APP
- Reading compiler warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) — Clang diagnostic flags reference; CS:APP §2.2–2.3 (integer conversions)

## Phase 1: Single-threaded baseline store

- `std::unordered_map` layout (bucket array + separately allocated nodes) — CS:APP §9.9 (dynamic memory allocation); cppreference `std::unordered_map`
- Pointer chasing and cache misses on a lookup — CS:APP §6.2 (locality), §6.4 (cache memories), §6.5 (cache-friendly code)
- Working-set size vs cache size (L1, L2, DRAM) — CS:APP §6.3 (memory hierarchy), §6.6 (memory mountain)
- Hash tables, collisions, load factor, rehashing — not in CS:APP; Cormen et al., *Introduction to Algorithms*, ch. on hash tables
- Node-based containers keep references valid across rehash — cppreference `std::unordered_map` (iterator and reference invalidation)
- `std::string_view` ownership and invalidation — cppreference `std::basic_string_view`; not in CS:APP
- Small-string optimisation (libc++ 22 bytes, libstdc++ 15 bytes) — not in CS:APP; implementation notes of libc++ / libstdc++
- Heterogeneous (transparent) lookup in unordered containers — cppreference `std::unordered_map::find`; not in CS:APP
- `insert_or_assign` vs `operator[]` vs `emplace` — cppreference `std::unordered_map`
- `std::optional` — cppreference `std::optional`
- Memory footprint: requested bytes vs resident set (RSS), allocator overhead — CS:APP §9.1–9.3 (virtual memory), §9.9.4 (fragmentation)
- Counting allocations by replacing `operator new` — cppreference "Replacement allocation functions"; not in CS:APP
- Microbenchmark design (key generation cost, random access, prefill, amortised timing) — CS:APP §5.14, §6.6; Google Benchmark User Guide
- Throughput benchmark vs paced open-loop latency (cold caches, clock changes) — CS:APP §6.6; Gil Tene, "How NOT to Measure Latency"
- Example tests, edge cases (empty, prefix, NUL bytes, large values) — not in CS:APP; GoogleTest Primer
- Model-based randomised testing against a reference implementation — not in CS:APP; no standard text, see `docs/LEARNING_NOTES.md`
- Mutation testing (injecting bugs to check tests can fail) — not in CS:APP; no standard text, see `docs/DESIGN.md` D-006
- One source of truth for derived state (no separate counter) — not in CS:APP; design principle, see `docs/DESIGN.md` D-005

## Phase 2: Naive concurrent server (mutex baseline)

- Processes vs threads, thread lifecycle — CS:APP §8.2 (processes), §12.3 (concurrent programming with threads)
- Shared variables and the threads memory model — CS:APP §12.4
- Mutual exclusion, mutexes — CS:APP §12.5.3 (semaphores for mutual exclusion); *C++ Concurrency in Action* (Williams), ch. 3 (§3.2 mutexes)
- RAII lock guards (`std::lock_guard`, `std::unique_lock`, `std::shared_lock`) — cppreference; *C++ Concurrency in Action*, ch. 3
- Condition variables, predicate loop, spurious wake-ups, lost wake-ups — not in CS:APP (it uses semaphores, §12.5.2); *C++ Concurrency in Action*, ch. 4 (§4.1)
- Producer–consumer queue — CS:APP §12.5.4 (scheduling shared resources); *C++ Concurrency in Action*, §4.1.2
- Thread pools (prethreaded server) — CS:APP §12.5.5 (concurrent server based on prethreading); *C++ Concurrency in Action*, ch. 9 (§9.1)
- Readers–writers and `std::shared_mutex` — CS:APP §12.5.4 (readers–writers problem); *C++ Concurrency in Action*, §3.3.2
- Why a read lock is still a write to shared memory (reader count) — Drepper, *What Every Programmer Should Know About Memory*, §3.3 (cache coherence); not in CS:APP
- Cache coherence and cache-line bouncing of a lock word — CS:APP §6.4 and §12.4 (background); Drepper, §3.3.4 (MESI)
- Thread safety, reentrancy — CS:APP §12.7.1–12.7.2
- Data races — CS:APP §12.7.4
- Deadlock and lock ordering — CS:APP §12.7.5; *C++ Concurrency in Action*, §3.2.4–3.2.5
- Amdahl's law — CS:APP §1.9.1; speedup and efficiency of parallel programs — CS:APP §12.6
- Why contention makes more threads slower (blocking cost, context switches, cache-line traffic) — CS:APP §8.2.5 (context switches), §12.6; Drepper, §3.3
- Copying a value while holding the lock (view lifetime, use-after-free, data race) — CS:APP §12.7.4, §9.11.7; see `docs/DESIGN.md` D-002, D-008
- Exceptions escaping a thread (`std::terminate`) — cppreference `std::thread`; not in CS:APP
- Graceful shutdown: drain vs discard, joining workers — *C++ Concurrency in Action*, §9.1; see `docs/DESIGN.md` D-007
- `std::latch` start barrier and `std::thread` — cppreference `std::latch`, `std::thread`
- Signal handlers (set a flag only; async-signal-safe) — CS:APP §8.5, especially §8.5.5 (writing signal handlers)
- Sockets and the client–server model — CS:APP §11.1, §11.4 (sockets interface); Ch. 10 (Unix I/O)
- Blocking vs non-blocking I/O, I/O multiplexing — CS:APP §12.2 (I/O multiplexing), Ch. 10
- Protocol Buffers: `.proto` schema, `bytes` vs `string`, generated code — Protocol Buffers language guide (proto3); not in CS:APP
- gRPC: services, unary RPC, HTTP/2 transport — gRPC documentation (core concepts); not in CS:APP
- gRPC C++ callback API (reactors, `Finish` from another thread) — gRPC C++ callback API tutorial; not in CS:APP
- Channels, connections and subchannel sharing — gRPC documentation (`GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL`)
- Idempotency of retries (Get, Put, Delete) — *Designing Data-Intensive Applications* (Kleppmann), ch. 8 (unreliable networks) and ch. 11 (exactly-once effects); not in CS:APP
- Open-loop multi-client load generation (one connection per client, intended-time latency) — Gil Tene, "How NOT to Measure Latency"
- Closed-loop maximum-throughput measurement — Gil Tene, "How NOT to Measure Latency"
- Isolating one cost by measuring it in process (no network) vs end to end — CS:APP §5.14 (profiling); Brendan Gregg, *Systems Performance*
- Oversubscription (more client threads than cores, client and server on one machine) — CS:APP §8.2.2 (concurrent flows), §12.6
- Scripted benchmarking and reproducibility (command, hardware, commit hash, load average) — see `docs/BENCHMARKS.md`; not in CS:APP
- Build and run only what the machine supports (macOS: no core pinning, no `perf`) — Linux man pages `sched_setaffinity(2)`, `perf(1)`; not in CS:APP
