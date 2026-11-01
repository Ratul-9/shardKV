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

## Open questions

- Apple clang is 21.0.0. Whether `std::hardware_destructive_interference_size` is available is still to verify in Phase 5.
- Which EC2 instance type for the final cluster (ARM Graviton or x86). To decide before Phase 11, after measuring costs.
