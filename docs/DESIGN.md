# Design Log: lldstore

Decision log and trade-offs. Each entry: what we chose, what we rejected, and why.

---

## D-001: Development environment (2026-10-24)

**Environment reported by the user**
- Host: macOS, Apple M5 chip (ARM64), 10-core CPU
- C++ compiler installed (versions to be verified in Phase 0)
- AWS account exists, no credits; user accepts the cost of the final 3-node cluster

**Consequences we must plan for**
- **No Linux on the host.** `perf`, `numactl`/NUMA, `epoll` and `sched_setaffinity` are Linux-only. macOS has `kqueue`, no `perf`, and no thread-to-core pinning API.
- **Phase 0-2 run natively on macOS.** TSan, ASan and UBSan work with Apple clang. Phase 2 uses `kqueue` only if we go below gRPC; gRPC hides this.
- **Phase 3 onward (pinning) needs Linux.** Options: a Linux VM (UTM or similar, ARM64 Linux), Docker Desktop (also a VM underneath, so NUMA and `perf` are limited), or EC2. Decide at the start of Phase 3.
- **Phase 5-6 (`perf` counters) need Linux with hardware counters.** Apple Silicon virtual machines may not expose PMU counters; EC2 instances do. Plan to measure there.
- **M5 is heterogeneous (performance and efficiency cores).** Results depend on which cores a thread lands on. Pinning to performance cores must be recorded in `docs/BENCHMARKS.md` with the core set used.
- **ARM64 memory model.** ARM is weakly ordered, unlike x86 (TSO). Bugs with missing acquire/release can hide on x86 and appear on ARM. The project must be tested on ARM (this machine) and on x86 (EC2) to be credible. This is a strong interview point.
- **Cost.** EC2 has no free credits. Estimate and record the hourly cost of the chosen instance type before the Phase 11 run, and shut instances down between runs.

**Rejected alternatives**
- Developing only on x86 EC2: slower iteration and ongoing cost during the whole project.
- Ignoring Linux-only features until the end: would leave Phases 3, 5 and 6 unmeasurable.

---

## D-002: `Store::Get` returns `std::optional<std::string_view>` (2026-10-25)

**Chosen (by Ratul):** `Get` returns a non-owning view into the store. Keys and values passed in are also `string_view`.
**Rejected:** `std::optional<std::string>` (copy per lookup).
**Why:** a caller usually wants to use the value, not own a copy; no allocation on the read path.
**Cost / consequences:** the view is invalidated by any later `Put` to that key or `Delete`. This is fine single-threaded. In Phase 2 a thread-safe wrapper must copy under the lock before returning to a gRPC handler. Revisit when the shard worker owns the data (Phase 3) and again for the epoch-based reader (Phase 9).
**Property of this container:** `unordered_map` is node-based, so a `Put` of a *different* key never moves existing elements and does not invalidate a view. An open-addressing table (Phase 5) moves entries on growth, so this guarantee will be lost then and views must be revisited.
**Implementation note:** looking up a `std::unordered_map<std::string, ...>` by `string_view` allocates a temporary `std::string` unless the map uses a transparent hash and equality (C++20 heterogeneous lookup).

---

## D-003: Lookups build a temporary `std::string` (no transparent hashing) in the baseline (2026-10-25)

**Chosen:** `Get` and `Delete` do `data_.find(std::string(key))`; `Put` uses `insert_or_assign(std::string(key), value)`.
**Rejected (for now):** an `unordered_map` with transparent hash and equality (`is_transparent`) so a `string_view` can be looked up without constructing a key.
**Why:** Phase 1 is the plain baseline. The 16-byte benchmark keys fit libc++'s small-string buffer, so the temporary does not heap-allocate on this platform; the cost is a copy of 16 bytes.
**Caveat:** with keys longer than the inline buffer (22 bytes on libc++, 15 on libstdc++ / Linux / EC2) every `Get` would allocate. The baseline therefore understates the cost on Linux.
**Follow-up:** measure a transparent-hash variant as its own experiment before changing the baseline, and record the before/after in `docs/BENCHMARKS.md`. Do not change the baseline code without that measurement.

---

## D-004: Overwrite semantics (2026-10-25)

**Chosen:** `insert_or_assign` assigns the new value into the existing `std::string` mapped value when the key exists.
**Rejected:** erase-and-insert, which frees and reallocates the node; `operator[]` followed by assignment (an extra lookup for new keys); `emplace` (silently does nothing for an existing key, shown by mutation M2 in `LEARNING_NOTES.md`).
**Why:** one lookup, and the existing string can reuse its buffer when the new value fits its capacity.
**Consequence:** a `string_view` previously returned by `Get` for this key is invalidated by the overwrite (the buffer may be reused or reallocated).

---

## D-005: Size is not stored separately (2026-10-25)

**Chosen:** `Size()` returns `data_.size()`.
**Rejected:** a separate `count_` member.
**Why:** a second counter must be updated in every mutating path and drifts out of sync as soon as one path is wrong. The earlier draft had exactly this bug class (a `count--` typo, and double-counting in `Put`).

---

## D-006: Test strategy for the baseline (2026-10-25)

**Chosen:** fixed example tests for each behaviour and edge case, plus a randomised model test (100,000 Put/Get/Delete on 50 keys, fixed seed 12345) compared against a reference `std::unordered_map`, run under debug, ASan, UBSan and TSan.
**Verified by mutation:** five deliberate bugs were injected into `Store` (Delete returns true for a missing key; Put never overwrites; Put truncates at NUL; Get uses only the first 4 key bytes; Size over-counts after Delete). Every mutant was caught by at least two tests. The randomised test caught M1, M2, M3 and M5 but **missed M4** (looking up only the first 4 key bytes), because its keys are `k0`..`k49`, at most 3 characters, so truncation never changes them. M4 was caught only by the fixed example tests (`PrefixKeysAreDistinct`, `LargeValue`, `ManyKeysSurviveRehashing`, `BinaryDataContainingNullCharacter`). So both kinds of test are needed, and the random test's key generator is its weak spot: widen it (variable-length keys, shared prefixes, `\0` bytes) if the key handling changes.
**Rejected:** relying on line coverage alone.

---

## D-007: ThreadPool design (2026-11-02)

**Authorship:** written by Claude at Ratul's explicit request after Ratul's own attempt had only an empty constructor. This is a [HAND-WRITE] item, so Ratul must be able to defend every choice below; challenge any of them and we will change it.

**Chosen**
- **One `std::mutex` + one `std::queue<std::function<void()>>` + one `std::condition_variable`.** Simple and obviously correct; the single queue lock is itself a contention point at high task rates (a real cost, and the motivation for per-shard queues in Phase 3 and lock-free queues in Phase 4).
- **Idle workers sleep on the condition variable** with a predicate (`stop_ || !tasks_.empty()`), so they use no CPU when idle.
- **Shutdown drains:** the destructor sets `stop_`, wakes everyone, and workers exit only when `stop_` is set AND the queue is empty. Every accepted task runs exactly once.
- **A throwing task is caught and discarded;** the worker survives.
- **`Submit` is allowed from inside a running task, including during draining.** It is not allowed concurrently with or after destruction from outside (undefined behaviour for any C++ object), so no "rejected after shutdown" path exists.
- **`num_threads == 0` is clamped to 1** (zero workers would accept tasks and never run them).
- Tasks run outside the lock; `notify_one` after releasing the lock in `Submit`; `notify_all` in the destructor.

**Rejected**
- **Discard queued tasks on shutdown:** faster to stop, but silently drops work the caller was told was accepted.
- **Let exceptions propagate:** `std::terminate` kills the whole server for one bad request.
- **`Submit` returns `std::future`:** useful but adds a `packaged_task` allocation per task; not needed for the baseline.
- **Busy-wait workers:** burns cores, which would distort the benchmarks.
- **Work-stealing / per-worker queues:** the right answer at scale, but this phase is the deliberately naive baseline.

**Why the subtle parts are the way they are**
- `stop_` is set under the mutex: otherwise a worker could check the predicate, then sleep just before the notify (lost wakeup) and `join()` would hang.
- The wait is a predicate loop: spurious wake-ups, and another worker can take the task first.
- `workers_` is declared last so all other members are constructed before any thread starts.

**Verified by** a throwaway 9-test check (not the repo's tests) under no sanitizer, TSan, ASan and UBSan with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`: each task runs once, concurrent execution, throwing tasks, submit-from-task during drain, 8 concurrent producers, a blocked task not stalling others, 200 idle create/destroy cycles, zero-thread clamp, repeated stress. The repo's `tests/thread_pool_test.cpp` is still Ratul's to write.

---

## D-008: Locking strategy for the mutex baseline (2026-11-02)

**Authorship:** written by Claude at Ratul's explicit request (the stubs were still unwritten). Ratul must be able to defend these choices; challenge any of them.

**Chosen**
- `MutexStore`: one `std::mutex`; every method takes `std::lock_guard`. Reads are exclusive too.
- `SharedMutexStore`: one `std::shared_mutex`; `Get` and `Size` take `std::shared_lock`, `Put` and `Delete` take `std::unique_lock`.
- `Get` copies the value into the caller's `std::string*` **while the lock is held**; it never returns a view. A reusable output buffer avoids an allocation per call.
- `Size()` is locked. An unsynchronised `unordered_map::size()` racing a `Put` is a data race even though it only reads.
- The mutex members are `mutable` so that `Get` and `Size` can stay `const` for callers.
- Reader safety in `SharedMutexStore` rests on `Store::Get` and `Store::Size` being `const` and not mutating anything (no lazy caching, no hidden counters). Anyone who later adds a mutation to those paths silently breaks it; TSan stress tests guard it.

**Rejected**
- Returning a `string_view` under a lock held by the caller (a "guard" object): correct but a leaky abstraction for a network server that must copy into a response anyway.
- A template over the mutex type: removes duplication but hides which lock each method takes, which is the thing to study in this phase.
- Lock-free or sharded designs: that is Phases 3 and 4. This phase is deliberately the naive baseline.

**Expected, to be tested:** `SharedMutexStore` is not automatically faster for a 90% read mix with ~100 ns operations, because every reader still performs an atomic update to the shared reader count (see `docs/BENCHMARKS.md`).

---

## D-009: Server design for the gRPC baseline (2026-11-02)

**Authorship:** chosen by Claude. Ratul was asked (sync API vs callback API plus our `ThreadPool`) and had not answered when the work was requested; this is the recommended option, flagged here so it can be changed.

**Chosen:** gRPC **callback API**. A handler grabs the call's reactor, hands the store work to our `ThreadPool` (or runs it inline when `--workers=0`), and the worker completes the call with `reactor->Finish()`.
- `--workers=N > 0`: our hand-built `ThreadPool` of N threads runs the store call. This is the literal Phase 2 design (thread pool + global mutex) and exercises the pool.
- `--workers=0`: the store call runs on gRPC's own thread. This is the comparison point that shows the cost of the extra queue hop.
- Request and response objects stay valid until `Finish()`, so tasks capture raw pointers safely.
- `Get` copies the value out of the store under the lock into a local string, then moves it into the response.

**Rejected**
- **Sync API:** simplest and avoids the hop, but leaves our `ThreadPool` out of the request path and gives no built-in way to compare "inline vs pool".
- **Custom socket server with epoll/kqueue:** out of scope for the baseline; gRPC is in the plan.
- **gRPC servers built with sanitizers:** the system gRPC libraries are not instrumented, so TSan would report false races inside them. gRPC targets build only in non-sanitizer presets; the locks and pool are TSan-tested separately in process.

**Idempotency:** Get is read-only. Put is idempotent. Delete's final state is idempotent but its reply (`existed`) changes on a retry. Documented in `proto/kv.proto`. Clients do not retry in the benchmark.

**Load generator:** each client thread has its own channel and TCP connection (`GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL`) and one outstanding request at a time. Open loop: intended-time latency, no coordinated omission. Closed loop: throughput only. Any RPC error invalidates a run and is printed.

**Limits:** client and server share one machine and its 10 cores (more client threads than cores at 64 clients); loopback TCP, no real network; macOS cannot pin threads.

---

## D-010: Open-loop wait strategy: sleep half the remaining time, spin only at the end (2026-11-03)

**Problem found by measurement:** the Phase 0 runner slept until a fixed 100 us before each deadline and then spun. On macOS `sleep_for` overshoots by about 25-30% of the requested time (12.7 ms returns ~3.2 ms late), so long waits made every operation start late, and open-loop latency (measured from the intended time) absorbed that lateness. For intervals under 200 us it never slept at all, so many clients meant many spinning threads fighting over the cores. See `docs/BENCHMARKS.md` B-009. The Phase 0 note that the 100 us margin was "an untested guess" was right.

**Chosen:** `WaitUntil` sleeps for half of the remaining time and re-checks, repeating until less than 50 us remain, then spins. The overshoot of each sleep is at most about 31% of that sleep, so it can never carry the wait past the deadline, and the remaining time shrinks geometrically (about five sleeps for a 12 ms wait).

**Rejected**
- A larger fixed spin margin (for example 5 ms): wastes most of a core per waiting client, and with 64 clients that is more cores than the machine has.
- A single shared scheduler thread that dispatches operations to clients: removes per-client jitter but changes what is being measured and is much more code.
- `sleep_until` with an absolute time: still subject to the same proportional leeway.

**Verified by** `OpenLoop.LongIntervalsDoNotAddSleepOvershootToLatency` (failed on the old code, passes now, asserts on the median so a single scheduler hiccup does not fail it), the full suite under all four presets, and the before/after open-loop runs kept in `bench/results/*_before_sleepfix.txt`.

**Remaining limit:** with many clients and long intervals, wake-up lateness of sleeping threads still adds up to about 0.1 ms on this machine (B-010).

---

## D-011: Linux development machine (2026-11-04)

**Machine:** Intel Core i5-12400F (6 cores, 12 hardware threads, no efficiency cores), 1 socket, 1 NUMA node, L1d 48 KiB and L2 1.25 MiB per core, L3 18 MiB shared, 15 GiB RAM, bare metal (`systemd-detect-virt` = none). Ubuntu 22.04.5, kernel 6.8.0-138, GCC 11.4 with libstdc++.

**Why it matters:** this resolves D-001's open item. Thread pinning, `perf` and NUMA tools work here, and a homogeneous 6-core CPU gives far more repeatable pinned results than the heterogeneous Mac. It is a different ISA (x86-64, TSO memory model) from the Mac (arm64, weak ordering), so lock-free code from Phase 4 gets tested on both.

**Setup problems met and their fixes** (all recorded in `CLAUDE.md`):
- Ubuntu 22.04's CMake 3.22 cannot read our presets (needs 3.25): installed a newer CMake.
- GCC 11 rejected `std::bit_width` narrowing and a `-Wdangling-else` in a gtest `if`: fixed in the source (commits `288ba08`, `773d9dc`).
- TSan binaries crashed at start-up: kernel 6.8 randomises mappings over a range GCC 11's libtsan does not support; `sudo sysctl vm.mmap_rnd_bits=28` fixes it. This slightly lowers address-space randomisation (acceptable on a development machine).
- gRPC 1.30 from apt is too old for the callback API, so the gRPC server and client are skipped here.
- ASan on Linux includes leak checking: all 42 tests pass, no leaks.

**Not yet verified on this machine:** whether `perf` hardware counters work, the SMT sibling layout (`lscpu -e`), and any benchmark results.

---

## D-012: Shard count and routing hash (2026-11-06)

**Authorship:** written by Claude at Ratul's explicit request (his own draft, `shardedStore.h`, had been reviewed and was replaced; see LEARNING_NOTES Phase 3). Ratul must be able to defend these; challenge any of them.

**Chosen**
- Shard count is a constructor option (`ShardedStoreOptions::num_shards`, default 4, clamped to at least 1). The Linux sweep is 1 to 6 shards: 6 is the number of physical cores on the Ubuntu machine (D-011).
- Routing: `shard = HashKey(key) % N`, where `HashKey` is FNV-1a (64-bit) over the key's bytes followed by the splitmix64 finalizer. It is fixed code, so the same key goes to the same shard on every platform and compiler.
- Spread check: computed independently in Python, 100,000 keys `key:000000000000...` give max/min shard load 1.019 over 6 shards and 1.017 over 4. The unit test asserts under 1.1, and pins five known hash values and four known shard numbers so the function can never change silently.

**Rejected**
- `std::hash<std::string_view>`: its result differs between libstdc++ (Linux) and libc++ (macOS), so the same key would land on different shards on the two machines and results would not be comparable.
- FNV-1a without a finalizer: FNV mixes the low bits poorly and `% N` uses exactly those bits.
- Power-of-two shard count with a bit mask (`hash & (N - 1)`): cheaper than `%`, but forbids N = 3, 5, 6, which the sweep needs. The cost of one 64-bit modulo is small against a cross-thread hand-off.
- Consistent hashing or rendezvous (highest random weight) hashing: their advantage is that changing N moves only about 1/N of the keys. N is fixed for the lifetime of a store, so that advantage cannot be measured here, and rendezvous hashing costs N hashes per lookup instead of one. Revisit in Phase 8 when cluster membership can change.
- Range sharding: needs an ordered key space and creates hot ranges; hashing spreads load.

---

## D-013: Per-shard request queue (2026-11-06)

**Chosen:** `BoundedQueue<T>` (`include/lldstore/bounded_queue.h`): one `std::mutex`, two condition variables (`not_full_`, `not_empty_`), a `std::deque`, a capacity (default 1024 per shard).
- `Push` blocks while full (backpressure: a client that is too fast waits instead of memory growing).
- `Pop` blocks while empty.
- `Stop()` drains: `Pop` keeps returning queued items and returns `nullopt` only when the queue is stopped and empty. After `Stop`, `Push` returns false and wakes blocked producers.
- The queue holds `Request*` (a pointer to the caller's stack object), so enqueueing copies eight bytes.
- It is a separate generic header with a deliberately small interface so Phase 4 can replace it with a lock-free queue behind the same calls.

**Rejected**
- Unbounded queue: hides overload and lets latency and memory grow without limit.
- Reject or drop on a full queue: needs retry semantics in every caller; blocking is the simplest correct baseline.
- One shared queue that all workers pull from: that is a thread pool plus a lock again (Phase 2), not shared-nothing.
- One condition variable for both directions: every notify would have to wake everyone or risk waking the wrong kind of waiter.
- Lock-free queue now: that is Phase 4, and this one is the baseline it must beat.

**Known cost:** every request pays a mutex acquire/release, a condition-variable wake-up of an idle worker, and a context switch each way. This is the cost Phase 4 attacks.

---

## D-014: Reply path (2026-11-06)

**Chosen:** a `Request` object on the CALLER's stack carries the operation, views of the key and value, a pointer to the caller's output string, the result, and its own mutex + condition variable. The caller pushes a pointer to it and blocks in `Wait()`; the worker executes, writes the result and calls `Complete()`. No heap allocation per request, and the key/value views are safe because the caller cannot return before the worker is done.
- `Complete()` notifies while still holding the lock. If it notified after unlocking, the woken caller could return and destroy the stack `Request` (with its condition variable) before the worker called `notify_one`, which would signal a destroyed object.
- `Get` copies the value into the caller's string inside the worker, before replying (D-002: a view into a shard's memory is unsafe once another request may change it).

**Rejected**
- `std::promise` / `std::future`: allocates shared state per request and carries exception machinery we do not need.
- A callback or fully asynchronous API: not needed for the baseline; the callers (benchmarks, the Phase 2 server) are synchronous, and callbacks complicate lifetimes.
- Spinning on an atomic flag: burns a core per waiting caller, which on a 6-core machine with pinned shard threads would distort the measurement.

**Cost:** one mutex + condition variable per request, and two context switches per round trip. Phase 4 and later phases look at removing them.

---

## D-015: Thread pinning (2026-11-06)

**Chosen:** `ShardedStoreOptions::pin_cpus`. If non-empty, shard i is pinned to `pin_cpus[i % size]`. A worker pins ITSELF at the start of its thread function with `pthread_setaffinity_np(pthread_self(), ...)` (Linux only), then records `sched_getcpu()`. The constructor waits for all workers to finish this step, so `PinnedCpus()` is final when it returns; -1 means "not pinned" (not requested, not Linux, or the kernel refused). Failure to pin is not fatal. Default is unpinned so the tests run on any machine.
- Policy for the benchmarks: one shard per physical core, SMT siblings left to the client threads. Find siblings with `lscpu -e=CPU,CORE`: CPUs with the same CORE number share a core.
- Client threads are NOT pinned and share the machine with the shard threads. With 6 shards on 6 cores and many clients, the clients compete with the shards; record the client count with every result.

**Rejected:** pinning from the creating thread after `std::thread` starts (a window where the worker runs on the wrong core); real-time scheduling classes (need privileges and can starve the system).

**Not verified on a real Linux machine yet:** the Linux branch was compiled here only against a stand-in for glibc's `<sched.h>`, and the pinning test is Linux-only. First real check is on the Ubuntu machine.

---

## D-016: Interface and semantics of `ShardedStore` (2026-11-06)

**Chosen:** the same synchronous interface as `MutexStore` (`Put`, `Get(key, std::string*)`, `Delete`, `Size`), callable from any number of threads; not copyable; the destructor stops every queue (workers finish queued requests) and joins.
- `Size()` sends one request to every shard at once and sums the answers (one round trip, not N). It is NOT a snapshot of the whole store at one instant, and there are no multi-key operations.
- Using the store during or after destruction is undefined behaviour (as for any C++ object); a call that finds its queue already stopped throws `std::logic_error` and enqueues nothing.

**Rejected:** a snapshot `Size()` (would need to pause all shards, which defeats shared-nothing); copyable stores (they own threads).

---

## D-017: Phase 3 test strategy and status (2026-11-06)

**Written:** 8 `BoundedQueue` tests and 18 `ShardedStore` tests (routing without threads; behaviour across shards; concurrency with disjoint keys against a per-thread model and with shared keys checking for torn values; capacity-1 queues for backpressure; `Size()` under concurrent inserts; pinning).
**Status: compiled but NOT RUN.** Ratul asked for the work to be compile-checked only. Debug, release, ASan, UBSan and TSan presets compile cleanly on the Mac with Apple clang (`-Werror`), and the Linux-only branch was syntax-checked against a stand-in header. GCC 11 has not seen this code. The tests have never executed, so any failure they reveal is a normal finding, not a regression.
**Before this phase counts as done:** run all four non-release presets on the Ubuntu machine (TSan needs `vm.mmap_rnd_bits=28`), then the sweep in `scripts/bench_sharded.sh`.

---

## D-018: Unbounded lock-free MPSC queue with one allocated node per push (2026-11-09)

**Written by Claude at Ratul's request** (a [HAND-WRITE] item; Ratul chose unbounded and the Vyukov XCHG design, and had started `mpsc_queue.h`). Compiled and build-checked only; **no test or benchmark has been run**.

**Chosen:** `MpscQueue<T>` (`include/lldstore/mpsc_queue.h`), Vyukov's node-based MPSC queue: producers `exchange` the tail pointer then link the old tail's `next`; the single consumer reads `head->next`. One queue PER SHARD (many client threads push, that shard's worker pops), exactly where `BoundedQueue` sat in Phase 3. Interface: `Push`, `Pop`, `Stop`, `Size` as in `BoundedQueue`, plus `TryPop`.
- **Unbounded, no backpressure.** A slow shard lets its queue grow without limit. Chosen by Ratul because the XCHG design is unbounded; bounded lock-free queues exist (Vyukov's bounded MPMC ring with per-slot sequence numbers) but are a different, harder design. Cost to measure: memory and latency growth under overload, which Phase 3's queue prevented.
- **One heap node per push** (`new Node`, freed by the consumer). Simple and generic (`T = Request*`), but it costs an allocation per request, shared with all other threads' allocator traffic. **Rejected for now:** an intrusive node embedded in `Request` (the request is already on the caller's stack and alive until `Wait()` returns, so no allocation would be needed). It is the obvious follow-up if the measurement shows allocation cost; Phase 6 revisits allocation.
- **Progress guarantee, stated precisely:** `Push` is wait-free (one `exchange`, one store, no loop). `TryPop` never blocks but can report "nothing" while a producer is mid-push, so the queue as a whole is NOT wait-free or lock-free in the strict sense: a producer stalled between its two steps hides every item behind it from the consumer until it resumes (D-020).
- `Size()` is approximate and counts pushes in flight.

**Rejected:** a mutex around a deque (that is `BoundedQueue`); a CAS-loop queue (Michael-Scott) which allows multiple consumers but pays a retry loop and needs ABA/reclamation machinery we do not need with one consumer.

**Single-consumer is a contract, not enforced.** `head_` is a plain pointer because exactly one thread (the shard worker) pops. Nothing in the code checks it.

---

## D-019: Memory orders in `MpscQueue` and the happens-before edges (2026-11-09)

Written by Claude at Ratul's request. **Ratul: draw these edges yourself until you can do it from memory (the phase's "done when").**

| Operation | Order | Edge it creates |
|---|---|---|
| `Push`: `tail_.exchange(fresh)` | `acq_rel` | release: whoever next exchanges and gets `fresh` as its `prev` sees `fresh` fully constructed. acquire: we see `prev` fully constructed before we write `prev->next`. Also the single total order on `tail_` is what serialises producers: each gets a distinct `prev`. |
| `Push`: `prev->next.store(fresh)` | `release` | publishes `fresh` and its value to the consumer. |
| `TryPop`: `stub->next.load` | `acquire` | pairs with the store above: seeing `next` means seeing its value. Without it the consumer could read a half-written `T` (a data race that x86 would often hide). |
| `state_.fetch_add` in `Push`, `fetch_or` in `Stop` | `acq_rel` | read-modify-writes on one word, so "stopped?" and "I am now in flight" are decided atomically (D-020). |
| `state_.fetch_sub` in `TryPop` | `release` | the consumer's reads of the node happen before the count drops. |
| `state_.load` in `Pop` | `acquire` | sees the count/stopped bit together with what the producer did before bumping it. |

**Why no `seq_cst` anywhere:** nothing needs one global order across two different variables; every edge above is a release/acquire pair on a single location. **Risk:** x86 is strongly ordered, so a too-weak order here would not fail on the Ubuntu machine; the arm64 Mac is the better place to find such a bug. Run the TSan and stress tests on both.

---

## D-020: The inconsistent window, stop, and the parking protocol (2026-11-09)

**The window:** between a producer's `tail_.exchange` and its `prev->next.store`, `tail_` points at a node nothing links to. The consumer sees `head->next == nullptr` although a push has begun.

**Chosen:** one atomic `state_` word = (count of pushed-but-not-popped items) | (stopped flag in the top bit). `Push` increments it BEFORE linking, so `count > 0` means "an item exists or is being linked". `Pop`:
- `TryPop` succeeds: return it.
- `TryPop` empty and `state_ == 0`: truly idle, sleep with `state_.wait(0)`. It wakes on any change (push or Stop), and `wait` compares atomically, so a push cannot slip between the check and the sleep (no lost wake-up).
- `TryPop` empty and `state_ > 0`: a producer is mid-push. `yield()` and retry (the window is a few instructions long; there is nothing to be woken by).
- `state_ == kStopped` exactly: stopped, nothing queued, nothing in flight: return `nullopt`. This keeps the Phase 3 contract (drain, then report stopped) without a mutex.

`Push` after `Stop` is detected from the SAME `fetch_add` that claims the slot, so a push either is rejected (and undoes its increment) or is counted before `Stop` and will be delivered. Pushes racing with `Stop` are therefore delivered if and only if `Push` returned true (tested).

**Costs, unmeasured:** every push does a `fetch_add` on a shared word and a `notify_one` call (an implementation may make that a system call or a cheap check when nobody waits; not verified); every pop does a `fetch_sub`. The shared counter is a second contended cache line next to `tail_`. The 64-bit word is used so the count cannot overflow into the flag; `std::atomic<uint64_t>::wait` may be implemented with a lock pool rather than a native futex (libstdc++ and libc++ differ). **Rejected:** a separate `stopped_` flag plus an eventcount (three atomics, harder to prove); spinning when idle (100% CPU per shard).

**Where it can still go wrong:** a producer preempted between its two steps stalls the consumer for the length of that preemption (the known blocking window of this design). With pinned shards and many more client threads than cores this is plausible; watch the tail latency.

---

## D-021: Making `ShardedStore` generic over its queue, and test status (2026-11-09)

**Chosen:** `BasicShardedStore<template<class> class QueueT>`; `ShardedStore = BasicShardedStore<BoundedQueue>` (Phase 3, unchanged behaviour and name) and `LockFreeShardedStore = BasicShardedStore<MpscQueue>`. Only the queue differs, so a benchmark comparison changes one variable. `Shard`'s constructor is selected by `requires std::is_constructible_v<Queue, size_t>`: a capacity for `BoundedQueue`, none for `MpscQueue` (`queue_capacity` is ignored there).
- `bench/contention.cpp`: `--store=lockfree` (same `--shards`, `--pin`).
- **Tests:** `tests/mpsc_queue_test.cpp` (15 tests: order, empty, move-only, destructor frees, Stop semantics, blocking Pop and wake-ups, ping-pong lost-wakeup check, 8 producers x 50,000 items exactly once and in per-producer order, pushes racing Stop). `tests/lockfree_sharded_store_test.cpp` includes `sharded_store_test.cpp` with `ShardedStore` renamed by a macro, so all 18 ShardedStore tests run against the lock-free queue too (ctest prefix `lockfree.`). One of them (`TinyQueuesApplyBackpressure...`) passes without testing backpressure, since the queue is unbounded.
- **Rejected:** typed tests (would have meant rewriting 18 tests); copying the test file (two copies drift).

**Status: all five presets (debug, release, asan, ubsan, tsan) BUILD with `-Werror` on the Mac (Apple clang, arm64). NO test and NO benchmark has been run. GCC 11 has not seen this code** (risks: `requires` on constructors of a class template, `std::atomic::wait/notify`). Before the phase counts as done: run the four non-release presets on both machines (the Mac is arm64 and weakly ordered, the better place to expose a missing acquire/release), then a TSan soak, then the sweep in `docs/BENCHMARKS.md` B-012.

---

## Phase 5: cache-conscious hash table (D-022 to D-030)

**Authorship and status (2026-11-12):** Claude wrote all of Phase 5 (`include/lldstore/flat_store.h`, its tests, `bench/table_bench.cpp`, and the footprint and contention changes) at Ratul's request. The hash table is a [HAND-WRITE] item. **At Ratul's request the code has no comments.** The line-by-line commentary and the full reasoning are in `docs/PHASE5_DESIGN_GUIDE.md`, which is the companion to these entries. All five presets build with `-Werror` on the Mac (Apple clang, arm64). `flat_store_tests` ran once in the debug preset before Ratul asked for no runs (29/29 passed). No other test has run, and no benchmark. GCC 11 has not compiled it.

## D-022: Slot = {64-bit hash, pointer to entry}; key and value in one out-of-line block (2026-11-12)

**Chosen:** a 64-byte-aligned array of 16-byte slots (4 per cache line). Each occupied slot holds the key's full 64-bit hash and a pointer to one heap block: a 12-byte header, then the key bytes, then the value bytes.
**Why:**
- Probing reads only slots, and the stored hash rejects non-matches without following the pointer. Expected cost at 1M keys: about one slot line plus one entry for a hit, and about one slot line for a miss, against 2-4 lines plus the value for `unordered_map` (prediction, B-013).
- Entries never move, so a `Get` view survives inserts of other keys and growth: **the D-002 guarantee is kept**.
- Growth and backward-shift deletion use the stored hash and never touch entries.
- One allocation per entry instead of 2 (libc++) or 3 (libstdc++).

**Rejected:**
- `std::string` key and value inside slots: 48-64-byte slots, one per line, and views break on growth.
- A Swiss table with 1-byte tags and SIMD groups: the best follow-up, but two SIMD code paths and no plain baseline yet.
- Cuckoo hashing: two unrelated cache lines per miss, and inserts can loop.
- Pointer tagging to fit the hash into 8 bytes: not portable (arm64 top-byte-ignore, pointer authentication, memory tagging).

## D-023: Entry header of three `uint32_t`; in-place overwrite; aliasing-safe copies (2026-11-12)

**Chosen:**
- Header `{key_size, value_size, value_capacity}` (12 bytes). Keys and values are limited to 4 GiB - 1 bytes; `Put` throws `std::length_error` beyond that.
- Overwrite in place when the new value fits `value_capacity`; otherwise allocate a new entry, copy, swap the pointer, then free the old one.
- Copies use `memmove`, because `Put(k, *Get(k))` and `Put(k, Get(k)->substr(n))` are legal and overlap.

**Rejected:** `size_t` sizes (24-byte header for an unrealistic case); always reallocating on overwrite (an allocation per Put, unlike the D-004 baseline).
**Consequence:** like `std::string`, an entry keeps its largest capacity. That matches the baseline's behaviour.

## D-024: Linear probing (2026-11-12)

**Chosen:** probe `home, home+1, ...` modulo the capacity. Sequential slots share cache lines and the hardware prefetcher follows them, and deletion by backward shift is simple.
**Rejected:** quadratic probing and double hashing (each step can be a new cache line); Robin Hood for now (the plan's follow-up experiment, which needs this as its baseline).
**Known cost:** primary clustering. Knuth's estimates for a miss are 2.5 slots at load 0.5, 8.5 at 0.75 and 50.5 at 0.9. This is the reason for D-026's limit and the B-013 load sweep.

## D-025: Power-of-two capacity; home slot = top bits of the hash (2026-11-12)

**Chosen:** `home = hash >> (64 - log2(capacity))`.
**Why:**
- A shift instead of a 20-40-cycle `%`.
- The top bits are the best mixed after multiplication.
- Independence from routing. Shard = `HashKey % N`, so if the table took the low bits of that same hash, with N = 4 every key in a shard would share 2 low bits and only a quarter of the home slots would be used. The table uses a different hash (D-028) and the high bits.

**Rejected:** `%` with prime capacities (slower); `hash & mask` (low bits).

## D-026: Maximum load factor 0.75 by default, configurable within [0.25, 0.95] (2026-11-12)

**Chosen:** the load is between 0.375 and 0.75 in steady state. The clamp guarantees `growth_limit_ < capacity_`, so at least one slot is empty and every probe loop terminates.
**Status:** 0.75 is a starting value, not a measured optimum. B-013 sweeps 25/50/75/90% at a fixed capacity of 2^20, and the default changes if the data says so.

## D-027: Deletion by backward shift, no tombstones (2026-11-12)

**Chosen:** Knuth's Algorithm R. From the hole, scan forward until an empty slot. Move an entry into the hole when `((next - home) & mask) >= ((next - hole) & mask)`, that is, when moving it keeps it reachable from its home, and continue from its old position.
**Rejected:** tombstones. They accumulate and lengthen misses, they need clean-up rehashes, and if inserts never reuse them a churning table can fill with them and a miss never terminates.
**Tested by:** 3,000 random layouts with chosen home slots, deleting in every order; an all-colliding cluster that wraps past the end; a churn test.

## D-028: `FlatHash`, a word-at-a-time hash separate from the routing hash (2026-11-12)

**Chosen:**
- Seed xor (length × constant).
- For each 8-byte word (loaded with `memcpy`, so alignment does not matter): `h = rotl(h ^ word × A, 29) × B`.
- The tail is zero-padded.
- Finished with the splitmix64 finalizer.

**Why:**
- FNV-1a (routing, D-012) costs one dependent multiply per byte.
- A separate function keeps slot choice independent of shard choice.
- The length in the seed separates `"a"` from `"a\0"`, which would otherwise always collide.
- Fixed code keeps probe behaviour identical on both machines. Unlike routing, this is a convenience, not a requirement: the table hash never leaves the process.

**Known weakness, open:** the seed is fixed, so keys chosen by an attacker can all collide (HashDoS). Before serving untrusted clients, use a per-process random seed and a keyed hash such as SipHash.
**Rejected:** `std::hash` (differs by library, unmeasured quality); reusing `HashKey` (bit correlation, and FNV is byte-at-a-time).

## D-029: Grow by doubling, all at once, never shrink (2026-11-12)

**Chosen:**
- `Rehash` allocates the new array first (strong exception guarantee), re-places slots by their stored hash without reading any entry, then frees the old array.
- `Reserve(n)` pre-sizes the table.
- The empty table allocates nothing; the first `Put` allocates 16 slots.

**Cost:** a stop-the-world pause proportional to the capacity at each doubling, which is a p999 spike for the request that triggers it (size unmeasured).
**Rejected:** incremental (two-table) resizing as in Redis (both tables checked on every operation); shrinking (no workload needs it).

## D-030: Smaller decisions (alignment, testability, integration) (2026-11-12)

- **Slot array aligned to 64 bytes** (`operator new(size, align_val_t{64})`). With 16-byte slots, no slot straddles a cache line. False sharing inside a table is impossible because only the shard's worker writes it. Alignment keeps two shards' slot arrays on separate lines. Entries come from `malloc` and could in theory share a line across shards; Phase 6 closes this.
- **The hash is a template parameter** (`BasicFlatStore<Hasher>`, `FlatStore = BasicFlatStore<FlatHash>`), only so tests can force collisions, wrap-around and chosen home slots. `[[no_unique_address]]` makes it free.
- **Not copyable, not movable.** Nothing needs it, and owning raw memory makes copy and move easy to get wrong.
- **Integration:**
  - `BasicShardedStore<QueueT, StoreT = Store>`.
  - `FlatShardedStore = BasicShardedStore<MpscQueue, FlatStore>` differs from `LockFreeShardedStore` only in the table.
  - `Store` (Phase 1) is unchanged, so old baselines stay reproducible.
  - `contention --table=flat` works with `--store=none|sharded|lockfree`.
- **Tests:** Ratul's 14 Phase 1 `Store` tests are reused unchanged through the D-021 macro trick (`tests/flat_store_suite_test.cpp`). There are 15 new table tests (`tests/flat_store_test.cpp`), including model tests with varied keys of length 0-48 (closing the Phase 1 random-test blind spot), and the 18 sharded-store tests run against `FlatShardedStore` (ctest prefix `flat.`).
- **Benchmarks:** `bench/table_bench.cpp` adds `TransparentStore` (`unordered_map` with heterogeneous lookup) as a **control**, so the gain from removing D-003's temporary key string is not credited to open addressing. This also closes the D-003 follow-up once B-013 runs. `footprint` takes `map|flat` and now counts aligned `operator new` (the slot array would otherwise be invisible).

---

## Phases 6 to 11: written by Claude at Ratul's request (2026-11-16)

Ratul asked for "the finished product, not phase by phase". Every decision below was made by Claude; the [HAND-WRITE] items of these phases (slab allocator, log writer and recovery, replication and election, epoch manager) are not Ratul's work. **Status for all of D-031 to D-066: built with `-Werror` in all five presets on the Mac (Apple clang 21, arm64). No test and no benchmark from these phases has been run, and GCC 11 has not compiled them.** The first runs are listed in `docs/LINUX_RUNBOOK.md`.

---

## D-031: Slab allocator size classes (2026-11-16)

**Chosen:** `SlabAllocator` (`include/lldstore/slab_allocator.h`), 36 classes from 16 B to 16 KiB: 16-byte steps up to 128 B, then 4 classes per power of two (160, 192, 224, 256, 320, ...). Worst-case internal waste is 25% above 128 B and below 16 B per block below it. Larger requests go to `operator new` directly.
**Why:** a FlatStore entry is 12 B of header + key + value. 16 + 64 B (the benchmark's key and value) = 92 B, which goes in the 96 B class. Power-of-two classes alone would put it in 128 B (+39%).
**Rejected:** exact-size free lists (unbounded number of lists); power-of-two only (waste up to 50%); jemalloc/tcmalloc (would hide exactly what this phase is meant to teach).
**FlatStore change:** `BasicFlatStore<Hasher, Alloc>` asks the allocator for `UsableSize` and records the spare bytes as `value_capacity`, so an overwrite with a slightly longer value reuses the block instead of reallocating.

## D-032: ASan poisoning of free slab blocks (2026-11-16)

**Chosen:** freed blocks and untouched chunk space are poisoned with `ASAN_POISON_MEMORY_REGION` (when built with ASan), unpoisoned on allocation.
**Why:** to ASan, a slab block is a piece of a live 256 KiB chunk. Without poisoning, a use-after-free inside the slab is invisible to the sanitizer the project relies on. This is the "custom allocators need annotations" item in `docs/PHASES.md`.

## D-033: One allocator per shard, single-threaded, no cross-thread free (2026-11-16)

**Chosen:** the allocator lives inside the shard's `FlatStore`, so only that shard's worker thread allocates or frees. No lock, no atomic.
**Why it is safe here:** a Get copies the value into the caller's string before the reply (D-002), so no other thread ever holds a pointer into a shard's memory.
**The trap avoided:** if another thread could free a block (e.g. the caller freeing a returned buffer), the free list would need a lock or a remote-free queue (as mimalloc and tcmalloc have). Using this allocator from two threads is undefined behaviour, and nothing in the code checks for it.

## D-034: Chunks are kept until the store is destroyed (2026-11-16)

**Chosen:** freed blocks go back on their class's free list; chunks are never returned to the OS.
**Cost:** a shard that grows to 10 GiB and shrinks keeps 10 GiB resident. Fragmentation across classes is also permanent: free 96 B blocks cannot serve a 160 B request.
**Rejected for now:** per-chunk live counts and `madvise(MADV_DONTNEED)` on empty chunks (more bookkeeping on every free; unmeasured need).

## D-035: Intrusive MPSC queue for zero-allocation requests (2026-11-16)

**Chosen:** `IntrusiveMpscQueue<T*>`, Vyukov's intrusive version: the link is `Request::queue_next`, a field of the request itself, so a push allocates nothing. `SlabShardedStore = BasicShardedStore<IntrusiveMpscQueue, SlabFlatStore>`.
**Why:** D-018 (Phase 4) allocated one node per push. With the slab, that node was the last heap allocation on the request path.
**Algorithm detail to defend:** the queue owns a stub node. When the consumer reaches the last real item, it pushes the stub back behind that item, so it can hand the item out without the list becoming empty. That is safe because the only thread that ever writes `stub->queue_next` is the producer that got the stub from its exchange, and the consumer only re-links the stub after it has skipped past it.
**Proof of "zero allocations":** `tests/zero_alloc_test.cpp` replaces global `operator new` with a counter and asserts 0 allocations across 4 threads x 20,000 mixed requests on a warmed-up `SlabShardedStore`. As a control, it asserts at least one allocation per request on `FlatShardedStore`, so a broken counter cannot pass silently. The test skips under sanitizers, which own `operator new`.

## D-036: One little-endian codec for disk and network (2026-11-16)

**Chosen:** `ByteWriter`/`ByteReader` (`include/lldstore/codec.h`): fixed-width little-endian integers written byte by byte, and length-prefixed strings. Every read is bounds-checked and the reader stays failed after the first error.
**Rejected:** Protobuf for the cluster protocol (gRPC 1.30 on Ubuntu 22.04 is too old for the callback API anyway, D-011); `memcpy` of structs (padding and byte order are machine-specific).

## D-037: Record framing and CRC-32C (2026-11-16)

**Chosen:** `[u32 length][u32 crc32c(length bytes + payload)][payload]`.
**Why the CRC covers the length:** a flipped bit in the length would otherwise make the reader frame garbage as a record, and a CRC over the payload alone could then match by accident.
**Why CRC-32C:** it is the checksum ext4, iSCSI and RocksDB use, and it has a hardware instruction (SSE4.2, ARMv8). It is table-driven here, and the speed difference is unmeasured.
**Limit:** lengths above 64 MiB are treated as corruption, so a damaged length cannot trigger a 4 GiB allocation.

## D-038: Recovery stops at the first invalid record (2026-11-16)

**Chosen:** replay from the start and stop at the first record that is incomplete, fails its CRC or does not decode. Truncate the file there (durably) before appending again.
**Why not skip it and continue:** records are appended in order and acknowledged only after the sync that covers them. A torn record can only be the unacknowledged tail. A valid-looking record after a bad one means the disk reordered writes, or the bytes are garbage, and it was never acknowledged in either case. Skipping could resurrect a write that a client was told failed, or apply records out of order.
**Idempotence:** the truncation is synced before anything new is written, so recovering twice gives the same state (tested).

## D-039: Atomic file replacement (2026-11-16)

**Chosen:** write `path.tmp`, fsync it, `rename` over `path`, fsync the directory. Used for snapshots and for Raft's term/vote file.
**Why each step:** if you rename before syncing the data, a crash can leave the new name pointing to empty or partial data (ext4's delayed allocation is the classic case). If you skip the directory fsync, the rename itself may not survive a crash.

## D-040: Group commit with an optional batching window (2026-11-16)

**Chosen:** `GroupCommitLog`. Writers frame their record into a shared buffer under a mutex and get a ticket. One flusher thread swaps the buffer out, does `write` + `fdatasync`, and publishes the highest durable ticket. `WaitDurable(ticket)` blocks until it is covered.
**Why it helps:** fsync costs the same whether it covers 1 record or 100, so N concurrent writers share one sync. Batches form on their own even with no delay, because whatever arrives during a sync goes into the next batch. `max_batch_delay` trades latency at low load for larger batches; it defaults to 0.
**macOS:** `fsync` does not flush the drive's cache there; `F_FULLFSYNC` is used (with a fallback).
**Measured?** No. `bench/wal_bench` and B-016 are the plan.

## D-041: An fsync error is fatal for the log (2026-11-16)

**Chosen:** after any failed write or sync, the log is permanently failed. All waiters and all later appends throw, and the process must restart and recover from disk.
**Why:** after a failed fsync, Linux may have dropped the dirty pages and cleared the error, so a retried fsync can "succeed" without the data on disk (the 2018 PostgreSQL "fsyncgate"). Retrying is not safe.

## D-042: DurableStore acknowledgement and read rules (2026-11-16)

**Chosen:** a write is (1) appended to the log, (2) applied to memory, both under one mutex, then (3) the caller waits for durability outside the mutex and only then returns.
**Reads also wait:** a `Get` (and a `Delete` that finds nothing) waits until the last write applied before it is durable. Otherwise a reader could see a value that a crash then erases, so it would observe something that "never happened".
**Rejected:** applying after the sync (memory would lag the log, and a reader would not see its own acknowledged write until the flusher thread applied it).

## D-043: Checkpoints: stop-the-world, in a crash-safe order (2026-11-16)

**Chosen:** while holding the store's mutex, wait for the log to be durable, write the snapshot atomically (D-039, holding `next_generation`), open the new log `wal-<next>.log`, then delete the older logs and sync the directory. Recovery loads the snapshot, replays logs from `next_generation` on, and deletes older ones.
**Crash at any step:** before the rename, the old snapshot and old logs are complete. After the rename but before the new log exists, the snapshot covers everything and a new log is created. Old logs left behind are deleted at the next recovery.
**Cost:** every operation pauses for the snapshot write (unmeasured). A fuzzy checkpoint (snapshot while writes continue, then replay from the snapshot's start position) is possible because Put/Delete are blind writes whose replay is idempotent. Not built; RcuMap (D-064) would provide the concurrent read it needs.

## D-044: RaftNode is a deterministic state machine (2026-11-16)

**Chosen:** `RaftNode` has no threads, clock or sockets. Inputs are `Tick()`, `Step(message)` and `Propose(commands)`. Outputs are `TakeMessages()` and `TakeCommitted()`. The same object runs in the simulator (`tests/raft_sim.h`) and in the server (`src/kv_node.cpp`).
**Why:** distributed bugs depend on timing. A deterministic core plus a seeded simulator turns "flaky once a week" into "seed 1234 fails every time".
**Persistence rule:** every Storage call is durable when it returns. Messages that depend on it (a vote, an append acknowledgement) are produced afterwards, so they cannot be sent before the state they claim is on disk.

## D-045: What "committed" means (2026-11-16)

An entry is committed when the leader of its term has it stored on a majority (itself included), counted only for entries of the leader's current term (Raft paper, figure 8). Earlier-term entries become committed when a current-term entry above them commits. Clients are answered only after the entry commits **and** is applied on the node that proposed it, with a term check: the node replies OK only if the committed entry at that index is the one it proposed.

## D-046: Timeouts (2026-11-16)

**Server defaults:** tick 10 ms, election timeout drawn uniformly from 15 to 30 ticks (150 to 300 ms), heartbeat every 3 ticks (30 ms). Randomisation makes split votes rare. The heartbeat is several times shorter than the minimum timeout, so a single lost heartbeat does not trigger an election.
**The trade-off:** shorter timeouts mean faster failover but more needless elections when the network or a GC-like pause stalls. Failover time is expected to be roughly timeout plus one election round trip. That is a prediction (B-018).

## D-047: The no-op entry at the start of a term (2026-11-16)

A new leader appends an empty entry of its own term. Without it, entries left uncommitted from earlier terms stay uncommitted until a client happens to write (figure 8 again), and reads routed through the log would wait for that write.

## D-048: Check-quorum (2026-11-16)

Once per maximum election timeout, a leader that has not heard an append reply from a majority steps down to follower, keeping its term. That stops a leader cut off by a partition from accepting proposals it can never commit. Its clients get "not leader" and go and find the real leader. It also limits how long two nodes can both believe they are leader (in different terms), which matters for any future lease-based read. Without leases, safety does not depend on it, because every read goes through the log.

## D-049: FileStorage for Raft (2026-11-16)

`raft-meta` (term and vote) is replaced atomically (D-039). Vote and term are written in one file, so "voted in term T" can never be persisted without T itself. `raft-log` is a framed log (D-037) of APPEND(index, term, data) and TRUNCATE(from) records, fsynced per call (one sync per proposal batch or per append message). The whole log is held in memory. **Not built:** log compaction and snapshots, so the log grows forever and a restart replays everything.

## D-050: Out of scope for the Raft implementation (2026-11-16)

Not implemented, on purpose:
- Membership change: the cluster is fixed at start.
- Log compaction and InstallSnapshot.
- PreVote: a node that was partitioned away rejoins with a higher term and forces an election.
- Leader leases and ReadIndex: every read is a log entry, which is linearizable but costs a replication round trip and an fsync.
- Pipelining limits beyond "optimistic next index".
- Flow control.

Each item is a known interview follow-up: say what it fixes and what it costs.

## D-051: Exactly-once client sessions (2026-11-16)

Each client sends `(client_id, seq)` and keeps the same seq on every retry of a request. The state machine stores the last seq and its result per client, and answers a duplicate from that table. The table is replicated state, so every node decides the same way. A client has one request in flight.
**Why:** a client whose request timed out cannot know if it committed. Without sessions, a retried Put could be applied twice, and the second application could land after another client's later write. **Not built:** session expiry. The table grows with the number of distinct clients.

## D-052: The deterministic cluster simulator (2026-11-16)

`tests/raft_sim.h`: N `RaftNode`s with `MemoryStorage`. Messages are delivered after random delays, so they reorder. Messages can be dropped, partitions block links, and a crash destroys the node object but keeps its storage. Simulated clients have per-attempt timeouts, leader hints and give-up as "unknown outcome". Every message goes through the wire codec. The simulator checks election safety, state machine safety and convergence, and runs the linearizability checker on the client history. Random numbers come from `mt19937_64` with our own float conversion, so a seed reproduces identically under libc++ and libstdc++ (`std::uniform_real_distribution` does not).

## D-053: Cluster networking (2026-11-16)

Plain POSIX TCP, length-prefixed frames (u32, capped at 64 MiB), TCP_NODELAY, no SIGPIPE, send timeouts. Raft and clients use separate ports. Threads: one loop thread owns RaftNode and the state machine, with one sender thread per peer (drops messages when the peer is down; Raft retransmits) and one reader thread per connection.
**Rejected:** an event loop (epoll/kqueue). Two backends, and thread-per-connection is fine for 3 nodes and tens of clients. gRPC: too old on Ubuntu 22.04.
**Known limit:** reader threads of closed connections are joined only at shutdown.

## D-054: The node's request path (2026-11-16)

The loop drains its inbox, steps peer messages, and turns all client commands from that drain into one `Propose` (one durable append and one fsync for the batch: group commit at the Raft level). It then ticks, sends, and applies what has committed. When leadership is lost, pending requests are answered "retry", because they may still commit under the next leader, and the session table makes the client's retry safe.

## D-055: Linearizability checker algorithm (2026-11-16)

Wing and Gong's backtracking search with Lowe's memoisation of (set of linearized operations, model state), as in Porcupine. The model is one key: absent, or a value. The history is split per key first, which is valid because linearizability is local and every operation touches one key. Each key's search has a step budget; past it the verdict is "gave up", never "linearizable".
**Tested against itself:** known-good and known-bad histories (stale read, lost write, value never written, non-monotonic reads, impossible delete results), 200 random sequential histories (must pass) each corrupted once (must fail), real stores recorded under threads, and a deliberately broken store that must be caught.

## D-056: Operations with unknown outcome (2026-11-16)

A timed-out write may have taken effect at any moment after its invocation, or never. The checker gives it no return event, and the search succeeds once every *known* operation is placed, so unknown writes are either placed somewhere after their call or left out. Unknown reads are dropped, since they constrain nothing. Recording an unknown write as "failed" would be wrong: the checker would then reject correct histories.

## D-057: Recording histories (2026-11-16)

Times are `steady_clock` on the recording machine (microseconds in `kvctl history`, simulator ticks in tests). The clients run on one machine, so their clocks are the same clock. Comparing timestamps from different machines would need clock bounds; that is not attempted. Recording happens around each client call, outside the measured system, and each client buffers its own operations and merges them at the end, so recording does not add contention between clients.

## D-058: Chaos runs and reproduction (2026-11-16)

`tests/chaos_test.cpp`: 200 seeded runs on 3 nodes and 100 on 5 nodes, with 5% message loss, delays of 1 to 6 ticks, crashes and partitions, each followed by a check of safety, convergence and linearizability. The test also asserts the faults happened (more than 100 crashes and 100 partitions over the 200 runs), so "no violations" cannot come from a run where nothing went wrong. A failing seed replays with `LLDSTORE_CHAOS_SEED`. Real-process versions: `scripts/chaos_local.sh` (kill -9) and `scripts/chaos_partition_linux.sh` (iptables and netem; root). **No run yet, so no bug log yet.** The phase's "log of bugs found and fixed" starts with the first run.

## D-059: How the [HAND-WRITE] gap is recorded (2026-11-16)

The plan says Ratul writes the allocator, the log writer and recovery, replication and election, and the epoch manager. All of them were written by Claude. To keep the project defensible:
- `docs/INTERVIEW_NOTES.md` has a drill for each phase, with model answers labelled as Claude's and defend-it questions.
- `docs/LEARNING_NOTES.md` records each section as "not yet learned".

Nothing in these phases should go on a CV until Ratul can explain it unaided.

## D-060: Metrics (2026-11-16)

`MetricsRegistry`: atomic counters and gauges, plus histograms (the Phase 0 log-linear histogram behind a mutex), rendered in the Prometheus text format and served over HTTP by `kvnode --metrics`. Histograms are exported as summaries (p50/p90/p99/p99.9 per node).
**The rule to defend:** quantiles cannot be averaged across nodes or time windows. A cluster p99 needs merged distributions, as `kvctl bench` does from its own histograms.
**Rejected:** Prometheus histogram buckets (fixed bucket bounds lose the 1/64 relative precision); a client library (one more dependency).

## D-061: Epoch-based reclamation rather than hazard pointers or RCU (2026-11-16)

| Option | Read cost | Writer frees after | Stalled reader |
|---|---|---|---|
| EBR (chosen) | two stores and a fence per *guard* | two epoch advances | blocks all reclamation (memory grows) |
| Hazard pointers | a store and a fence per *pointer* followed | a scan of all hazards | blocks only what it protects |
| Userspace RCU (liburcu) | near zero (QSBR) | a grace period | blocks grace periods |

EBR is the simplest to get right and cheapest for readers who look at many nodes per guard. Its weakness, a reader stuck inside a guard, is tested and documented rather than solved.

## D-062: The EBR contract (2026-11-16)

- One writer thread retires, and only it advances the epoch.
- Readers register a slot (128 maximum), and only that thread uses the slot.
- Guards can be nested, and the outermost one announces the epoch.
- A node retired in epoch r is freed once the epoch reaches r + 2.

Single-writer advancing is what makes the happens-before argument short: the advancing store and the unlinks are in the same thread.

## D-063: The memory-order argument for EBR (2026-11-16)

Reader: store its epoch e, then a seq_cst fence, then re-read the global epoch, and retry until the two are equal. Only then does it load pointers (acquire). Writer: unlink (release), then a seq_cst fence before reading every slot (acquire), then advance only if every active reader is at the current epoch.
- The two fences give a Dekker-style guarantee: either the writer sees the reader's announcement and does not advance, or the reader sees the unlink and cannot pick up the removed node.
- The reader's leave is a release store, and the writer's acquire load of it orders every read the reader did before the free.

**Defend this on paper; x86 will not show you a mistake here, and arm64 might.**

## D-064: RcuMap, a single-writer map with lock-free reads (2026-11-16)

Chains of immutable nodes. The writer replaces rather than mutates. A Put to an existing key publishes a copy with the same `next`. A Delete unlinks the node but leaves the node's own `next` intact, so a reader standing on it can walk on. Resize builds a whole new table and publishes it with one release store; the old table and its nodes are retired together.
- `Get` returns a view valid for the life of the guard.
- `UnsafeRcuMap` frees immediately. It exists only so that a test (ASan preset, death test) proves the naive version is a use-after-free.
- The concurrency test has 4 readers and one writer making 50,000 updates. Values carry their own CRC and a version that must never go backwards for a reader.
- **Not integrated** into ShardedStore or DurableStore: it is the building block for the fuzzy checkpoint (D-043) and for background readers.

## D-065: Delivery (2026-11-16)

- A multi-stage `Dockerfile` (Ubuntu 24.04 builder, slim runtime).
- `docker-compose.yml` with 3 nodes and Prometheus.
- CI with all presets on Linux and macOS, plus benchmark smoke runs, a cluster smoke test with a killed leader and two chaos runs, and a Docker build.
- `deploy/aws/` has `cluster.sh up|status|bench|down`, user-data, a CloudWatch agent config and alarms. **None of it has run.** No GitHub remote has run CI either, and no AWS resource was created.
- Container and cloud numbers are always labelled as such, and never compared with bare-metal numbers as if they were the same setup.

## D-066: What "finished" means here (2026-11-16)

The code for every phase exists and builds. "Done" in `docs/PHASES.md` also needs: the tests passing (none of Phases 3 to 11 have fully run), sanitizer runs, the measurements (B-011 to B-019 are all planned, not measured), and Ratul being able to defend the design. The remaining work is a runbook (`docs/LINUX_RUNBOOK.md`), not more code. Expect the first runs to find bugs. That is the point of running them, and each one goes in this file with its cause.

---

## Open questions

- Apple clang is 21.0.0. Whether `std::hardware_destructive_interference_size` is available is still to verify in Phase 5.
- Which EC2 instance type for the final cluster (ARM Graviton or x86). To decide before Phase 11, after measuring costs.
