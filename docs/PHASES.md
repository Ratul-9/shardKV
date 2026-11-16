# Phase guide: what each phase must do

A working description of every phase of `lldstore`, a sharded, replicated in-memory key-value store in C++20. Written so it can be used on its own, for example to drive a from-scratch implementation. It is based on `PROJECT_CONTEXT.md` and on what building Phases 0 to 3 taught us.

## Rules that apply to every phase
- **Baseline first, then optimise.** Each phase builds the simple, correct version first, with tests. Optimisation happens only in the phases designed for it.
- **No optimisation without a measurement.** Record before and after in a benchmarks file: hardware, commit hash, exact command, results, interpretation. If an optimisation does not help, say so and keep the simpler code.
- **Correctness before speed.** Every concurrent component is tested under ThreadSanitizer and with a stress test before it counts as done.
- **Write down every decision** (what you chose, what you rejected, why) in a design log. Interviewers ask exactly this.
- **Say what you did not verify.** A clean compile is not a passing test, and a number measured on a busy machine is not a result.

Each phase below lists: goal, what to build, what to study first, decisions to record, tests, measurements, "done when", and mistakes that are easy to make.

---

## Phase 0: Foundations and measurement
**Goal:** a build, test and measurement setup you can trust before any store code exists.

**Build**
- Repo layout, CMake with presets for `debug`, `release`, `asan`, `ubsan`, `tsan`, GoogleTest, Google Benchmark, a formatter, a minimal CI.
- A latency histogram with fixed memory (log-linear buckets, error at most 1/64, O(1) record).
- An open-loop load generator with warm-up that measures latency from each operation's intended start time.

**Study first:** percentiles and tail amplification, coordinated omission (Gil Tene, "How NOT to Measure Latency"), what each sanitizer detects, why the optimiser can delete a benchmark (`DoNotOptimize`).

**Decisions to record:** one sanitizer per build tree (ASan and TSan cannot be combined); warnings as errors on your own code only; the histogram's bucket layout and why you report a bucket's upper bound.

**Tests:** histogram accuracy (exact for small values, bounded relative error for large), percentile lookup, merge, clamping; a test that a stall is charged to every missed slot (the coordinated-omission demonstration).

**Measure:** the harness overhead itself (cost of recording a sample, of reading the clock).

**Done when:** a hello-world test and benchmark run in CI, and you can explain why p99 matters and what coordinated omission is.

**Easy mistakes**
- Waiting code that sleeps by a fixed margin: on macOS `sleep_for` overshoots by about 25-30% of the requested time, so long waits start late and the lateness is recorded as latency. Sleep half the remaining time, repeat, and spin only at the end.
- Strict flags behave differently on GCC and Clang (`std::bit_width` returns an unsigned type in GCC 11; `-Wdangling-else` before test macros). Compile with both.
- TSan binaries can crash at start-up on newer Linux kernels; `sudo sysctl vm.mmap_rnd_bits=28` fixes it with GCC 11.

---

## Phase 1: Single-threaded baseline store
**Goal:** the plain, correct store and the numbers every later phase must beat.

**Build:** `Store` with `Put`, `Get`, `Delete`, `Size`, backed by `std::unordered_map<std::string, std::string>`; not thread-safe by design.

**Study first:** how `unordered_map` lays out memory (bucket array plus separately allocated nodes), cache misses and working-set size (CS:APP §6.2-6.6), `string_view` ownership and invalidation, small-string optimisation.

**Decisions to record:** what `Get` returns (a view into the store, or a copy) and what invalidates a view; how overwrite works (`insert_or_assign` reuses the existing string's buffer); whether the lookup builds a temporary key (transparent hashing avoids it); keep one source of truth for the size (no separate counter).

**Tests:** empty store, missing key, overwrite, delete present and missing, empty key and value, prefix keys, NUL bytes in keys and values, large values, many keys across rehashes, and a randomised test against a reference model. Check the tests can fail: inject bugs (mutation testing) and confirm they are caught; a random-key generator that only makes short keys misses truncation bugs.

**Measure:** time per operation at 1K, 100K and 1M keys (cached versus memory-bound); open-loop latency percentiles; bytes per entry (requested and resident).

**Done when:** tests pass under all sanitizers and the baseline numbers are recorded with their context.

**Easy mistakes:** a benchmark whose keys all fit in L1; counting key generation as store cost; a `Get` returning a view the caller keeps after a mutation; forgetting that one benchmark run on a noisy machine is not a result.

---

## Phase 2: Naive concurrent server (the mutex baseline)
**Goal:** make the store safe for many threads in the simplest way, and measure exactly why the lock limits scaling.

**Build**
- A thread pool: one mutex, one queue, one condition variable, workers that sleep when idle, shutdown that drains.
- `MutexStore` (one global `std::mutex`) and `SharedMutexStore` (`std::shared_mutex`).
- A multi-client load generator (open loop for latency, closed loop for maximum throughput).
- Optional: a gRPC service in front of the store.

**Study first:** mutexes and condition variables (CS:APP §12.4-12.7, *C++ Concurrency in Action* ch. 3-4 and §9.1), Amdahl's law (CS:APP §1.9.1), why a contended lock costs more than waiting (cache-line bouncing, blocking and waking).

**Decisions to record:** which lock each method takes; `Get` copies the value into the caller's buffer while the lock is held (never returns a view); `Size` is locked too; thread-pool shutdown (drain, not discard), what happens when a task throws, the contract for `Submit` during destruction.

**Tests:** per-thread reference-model tests on disjoint keys, a torn-value test on shared keys, a thread-pool suite (each task runs once, real concurrency, drain on destruction, submit from inside a task, many producers, a blocked task does not stall others, idle create/destroy loops for lost wakeups). Run all under TSan.

**Measure:** throughput with 1, 4, 16 and 64 clients, for no lock, mutex and shared mutex, in process first (to isolate the lock) and then end to end if you add gRPC.

**Done when:** you can explain with numbers why adding clients made the mutex store slower, not just flat, and what shared_mutex did.

**What we measured (macOS, libc++):** the mutex store fell from 4.78M to 1.90M ops/s going from 1 to 4 clients; `shared_mutex` was slower than the mutex at every client count (its libc++ implementation uses an internal mutex and condition variables). At the gRPC level the lock was invisible (the RPC stack dominates) and routing work through the thread pool cost about 22% throughput.

**Easy mistakes:** setting the stop flag without the mutex (lost wakeup, `join` hangs); waiting without a predicate loop; running a task while holding the queue lock; copying a value after unlocking; load-generator waits that bias latency; judging a result without a control experiment.

---

## Phase 3: Sharding and thread-per-core (still simple primitives)
**Goal:** split the data so threads stop fighting over one lock, and learn what each sharding step buys.

**Suggested steps (build and measure each; the project plan's step is 3b)**
- **3a, lock striping (optional but recommended):** N shards, each with its own mutex; any thread may touch any shard; keep each shard's mutex on its own cache line. Simplest, and likely faster than 3b at modest thread counts.
- **3b, shared-nothing thread-per-shard:** one worker thread per shard, owned data needs no lock; callers send requests through a bounded mutex-and-two-condition-variable queue and block for the reply; workers pinned to cores.

**Build for 3b:** a generic bounded queue (`Push` blocks when full, `Pop` blocks when empty, `Stop` drains); a request object on the caller's stack with its own mutex and condition variable; a router; per-shard worker loop; optional pinning (`pthread_setaffinity_np`, Linux only).

**Study first:** partitioning and skew (*Designing Data-Intensive Applications* ch. 6), why consistent or rendezvous hashing only matters when the shard count changes, shared-nothing design (Seastar), false sharing (CS:APP §6.4-6.5), CPU topology and SMT siblings (`lscpu -e`).

**Decisions to record:** shard count and sweep; the routing hash (use fixed code, not `std::hash`, which differs between standard libraries) and `% N`; queue bounded or not, and what a full queue does (block, reject, grow); the reply path (stack object with mutex and condition variable, versus `promise`/`future`); pin policy (one shard per physical core); what `Size()` means across shards (not a snapshot).

**Tests:** routing is deterministic, in range, and even (about 2% spread over 100K keys); behaviour across shards; per-thread-model and shared-key concurrency tests; tiny queues for backpressure without deadlock or loss; shutdown; a pinning test that reads back the CPU each worker is on (Linux only).

**Measure (on Linux, the Mac cannot pin):** throughput versus shard count 1 to 6 and client count, against the Phase 2 mutex store on the same machine; latency at a fixed offered load; pinned versus unpinned; load imbalance. State the client count: client threads share the cores.

**Done when:** all tests pass under TSan on Linux, the scaling curve is recorded against Phase 2, and you can explain what limits it (the per-request hand-off).

**Easy mistakes:** a `Get` that returns a view into data another thread owns; `const` on mutating methods; a macro for the shard count; notifying after the caller may have destroyed the stack request (notify while holding the lock); pinning from outside the thread; two threads on SMT siblings counted as two cores; a pinning failure ignored silently.

---

## Phase 4: Lock-free MPSC queues
**Goal:** replace the mutex queue in front of each shard with a lock-free multi-producer, single-consumer queue and measure the difference.

**Build:** a Vyukov-style intrusive MPSC queue (producers swap the tail atomically, the single consumer walks the head), with the same small interface as the Phase 3 queue so the two can be compared directly.

**Study first:** the C++ memory model (`memory_order` relaxed, acquire, release, acq_rel, seq_cst; happens-before), compare-and-swap and the ABA problem, lock-free versus wait-free, the memory reclamation problem (*C++ Concurrency in Action* ch. 5 and 7, *The Art of Multiprocessor Programming* ch. 5 and 9, cppreference `std::memory_order`).

**Decisions to record:** every `memory_order` choice with the happens-before edge it creates (draw it); how a node is freed and by whom; how ABA is avoided; what the consumer does when the queue looks empty during a producer's two-step push.

**Tests:** a long stress test under TSan with many producers; every item delivered exactly once; ordering per producer preserved; test on both x86 (strong memory model) and arm64 (weak): missing acquire/release can hide on x86.

**Measure:** hand-off latency and throughput versus the Phase 3 queue, same machine, same workload.

**Done when:** the stress test is clean under TSan for a long run and you can draw the acquire/release edges from memory.

**Easy mistakes:** using `relaxed` where an acquire/release pair is needed; freeing a node a producer still points at; a consumer that spins at 100% CPU instead of waiting; testing only on x86.

---

## Phase 5: Cache-conscious hash table
**Goal:** replace `unordered_map` inside each shard with a custom open-addressing table and prove the cache behaviour.

**Build:** open addressing with linear probing (Robin Hood probing as a follow-up), cache-line-aligned slots, a chosen load factor, and a resize strategy.

**Study first:** the memory hierarchy, locality and the TLB (CS:APP §6.2-6.6), false sharing and alignment, hash functions and collisions, branch prediction and prefetching (CS:APP §5.7, §5.11), reading `perf stat` counters, reading assembly (CS:APP §3).

**Decisions to record:** slot layout (key and value inline versus pointers), maximum load factor, deletion method (tombstones versus backward shift), when to resize and whether to do it incrementally, and the change in view-validity rules (an open-addressing table moves entries on growth, so a view that survived inserts in the node-based map no longer does).

**Tests:** the same suites as Phase 1 plus a randomised comparison against `std::unordered_map`, tests at high load factors, deletion patterns, and resize boundaries.

**Measure:** against `unordered_map` at several load factors and key/value sizes; `perf stat` cache-misses and branch-misses on Linux; memory per entry.

**Done when:** correctness tests and benchmarks are in, and you can explain the cache behaviour of both designs.

**Easy mistakes:** tombstone build-up that silently slows lookups; resize that invalidates iterators or views you promised were stable; benchmarking on a working set that fits in cache.

---

## Phase 6: Memory management
**Goal:** remove heap allocation from the hot path and cut memory use.

**Build:** per-thread slab and arena allocators (optionally behind `std::pmr::memory_resource`); a counting allocator or test that proves zero hot-path allocations; optionally huge pages and NUMA first-touch.

**Study first:** virtual memory, `mmap` and page faults, RSS versus virtual size (CS:APP ch. 9), how `malloc` works and fragmentation (CS:APP §9.9), thread-local allocation, huge pages and NUMA (Linux man pages, Drepper).

**Decisions to record:** size classes, what is freed individually versus in bulk, what happens when a slab is exhausted, who may free memory allocated by another thread.

**Tests:** a test that replaces `operator new` with a counter and asserts zero allocations during a steady-state run; allocator tests for alignment, reuse and exhaustion; sanitizer runs (ASan with a custom allocator may need annotations).

**Measure:** footprint (requested bytes and resident set) versus the Phase 5 table with `unordered_map`/`malloc`; latency change.

**Done when:** zero hot-path allocations are proven by a test and footprint is measured against the baseline.

**Easy mistakes:** an allocator that is correct single-threaded but frees across threads; leaking inside an arena; measuring only requested bytes when resident memory is what matters.

---

## Phase 7: Durability
**Goal:** an acknowledged write survives a crash.

**Build:** an append-only log with a record format (length, payload, checksum), group commit (batch many records into one `fdatasync`), recovery on restart (replay the valid prefix, stop at the first bad checksum), and basic truncation or compaction.

**Study first:** the page cache and what `write`, `fsync` and `fdatasync` do and do not guarantee, torn writes, write-ahead logging and group commit (CS:APP ch. 10; *Designing Data-Intensive Applications* ch. 3 and 7; PostgreSQL WAL documentation).

**Decisions to record:** the record format and checksum, when a write is acknowledged (after the fsync that covers it), the group-commit window, whether to fsync the directory after creating a file, how recovery treats a torn last record.

**Tests:** round-trip, corrupted-record detection, recovery after truncation at every byte offset, and crash tests: `kill -9` the process at random points under load and verify no acknowledged write is lost and no unacknowledged one appears wrongly.

**Measure:** throughput and latency with and without group commit; fsync cost on the actual disk.

**Done when:** crash-recovery tests pass repeatedly and you can say what `fsync` guarantees and what it does not (volatile disk caches, filesystem behaviour).

**Easy mistakes:** acknowledging before the sync; skipping a bad record instead of stopping; recovery that is not idempotent; testing only clean shutdowns.

---

## Phase 8: Replication and failover
**Goal:** a 3-node cluster that survives losing the leader without losing acknowledged writes.

**Build:** primary-backup log shipping first, then leader election and failover with Raft-style ideas (terms, majority acknowledgement, log matching). Scope it honestly: implement only what you can test and explain.

**Study first:** replication models, the Raft paper and its visualisation, failure detection and timeouts, split brain, CAP and PACELC (*Designing Data-Intensive Applications* ch. 5, 8, 9).

**Decisions to record:** what "committed" means (replicated to a majority), election timeouts and heartbeats, what a stale leader may do (fence with terms), what you will not implement (membership change, log compaction).

**Tests:** deterministic simulation of message delay, drop and reorder; kill the leader under load; verify no committed write is lost and clients see one leader per term.

**Measure:** failover time; write latency with majority acknowledgement.

**Done when:** a 3-node cluster survives killing the leader with no loss of acknowledged writes, and failover time is recorded.

**Easy mistakes:** acknowledging before a majority has the entry; electing a leader with an incomplete log; timeouts too short (needless elections) or too long (slow failover).

---

## Phase 9: Lock-free reads for background work
**Goal:** a snapshot or replication reader thread can read a shard while its owner changes it.

**Build:** an epoch-based reclamation manager: readers announce the epoch they entered, retired memory is freed only after every reader has moved past it.

**Study first:** epoch-based reclamation, hazard pointers and RCU and their trade-offs (McKenney, *Is Parallel Programming Hard?*; Michael, hazard pointers; *The Art of Multiprocessor Programming* ch. 10).

**Decisions to record:** how readers enter and leave an epoch, what happens when a reader stalls (reclamation is blocked), when memory is retired and freed.

**Tests:** a long TSan stress test with a reader and a mutating owner; a deliberately naive `delete` version that the test must catch.

**Done when:** the stress test is clean under TSan and you can explain why naive reclamation is unsafe.

**Easy mistakes:** a reader that holds an epoch forever; freeing in the same epoch as the retire.

---

## Phase 10: Correctness and chaos testing
**Goal:** evidence, not hope, that the whole system is correct under failure.

**Build:** a linearizability checker over recorded histories of operations (invocation and response times, results), and fault injection: kill processes, partition the network (`iptables`, `tc netem`), delay and drop messages.

**Study first:** linearizability versus sequential consistency (Herbert and Wing), checker ideas (Porcupine, Knossos), what Jepsen-style testing finds.

**Decisions to record:** what the sequential model is, how histories are recorded without disturbing timing, how a failing run is reproduced (seeds, logged schedule).

**Tests and measurement:** hundreds of automated runs with injected faults; zero violations; every failure found along the way written up with its cause.

**Done when:** zero linearizability violations in recorded runs and a log of bugs found and fixed.

**Easy mistakes:** a checker that is itself wrong (test it on histories you know are bad); a single passing run treated as proof.

---

## Phase 11: Observability, cloud and delivery
**Goal:** run it on a real cluster, watch it, and make the results reproducible.

**Build:** latency histograms and metrics, dashboards and alarms (CloudWatch), a Docker image, CI/CD (build, tests, sanitizers, a benchmark smoke run), the 3-node cluster on EC2, the final README with an architecture diagram, real results and honest limitations.

**Study first:** histograms and percentiles in production (percentiles cannot be averaged across hosts), metrics versus tracing, `perf` and flame graphs, Docker, CI, EC2 and CloudWatch basics.

**Decisions to record:** instance types and cost per run, what is in the container, which results are quoted and from which machine.

**Measure:** repeat the final benchmarks on the cluster; every number with command, hardware and commit.

**Done when:** one command reproduces the benchmarks, and every CV number comes from the benchmarks file only.

**Easy mistakes:** quoting a number you cannot reproduce; benchmarking inside a container and calling it bare metal; leaving cloud instances running.

---

## Status in the `shardKV` repo (the reference build)

| Phase | Status |
|---|---|
| 0 | Done, tag `phase-0-done` |
| 1 | Done, tag `phase-1-done` |
| 2 | Done, tag `phase-2-done` |
| 3 to 5 | Written by Claude, built; tests and benchmarks not run (one debug run of the Phase 5 table tests) |
| 6 to 11 | Written by Claude in one go (2026-11-16), built in all five presets on the Mac; nothing run. Runbook: `docs/LINUX_RUNBOOK.md` |
