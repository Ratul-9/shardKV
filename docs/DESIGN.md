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

## Open questions

- Apple clang is 21.0.0. Whether `std::hardware_destructive_interference_size` is available is still to verify in Phase 5.
- Which EC2 instance type for the final cluster (ARM Graviton or x86). To decide before Phase 11, after measuring costs.
