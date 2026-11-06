# Interview Notes

## Phase 0 drill (graded)

**Q1. Why does p99 matter more than the mean for a service that handles many requests per user action?**
- Your answer: p99 means 99% of requests finish under a bound; a few slow requests inflate the average, so percentiles give a bounded view of performance.
- Grade: B-. Missing tail amplification.
- Corrected answer: If one user action issues `n` requests, the chance of at least one hitting the p99 tail is `1 - 0.99^n` (63% at n=100). So the tail is what most users experience. The mean also hides the tail instead of describing it: a service with mean 5 ms can have a p99 of 500 ms.

**Q2. Explain coordinated omission in about 30 seconds.**
- Your answer: Slow requests are not accounted for; a generator at 1000 req/s sees one slow request during a 1 s freeze, a correct one reports 1000.
- Grade: B. Missing the cause; the 1000 are not all equally slow.
- Corrected answer: A closed-loop generator waits for each response before sending the next request, so a stall removes the samples that would have exposed it. An open-loop generator schedules requests at fixed times and measures from the scheduled time. In a 1 s freeze at 1000 req/s, the 1000 late requests have latencies from about 1 s down to about 0, and the p99 correctly shows several hundred ms.

**Q3. Why is the histogram log-linear, and what do you give up?**
- Your answer: One bucket per ns is too expensive; we give up precision.
- Grade: A.
- Added detail: 2^40 buckets at 8 bytes is about 8 TB, versus about 2240 buckets (about 18 KB). We give up absolute precision at large values (relative error at most 1/64, about 1.6%).

**Q4. Why return a bucket's upper bound rather than its midpoint?**
- Your answer: Avoids understating latency.
- Grade: A-.
- Added detail: The true value is guaranteed to be at most the reported value. A midpoint under-reports about half the time. The report is also clamped to the maximum recorded value so it never exceeds what was seen.

**Q5. Why does `WaitUntil` sleep then spin instead of only sleeping or only spinning?**
- Your answer: The scheduler can wake a sleeping thread late; pure spinning burns CPU; combining gives precision at lower cost.
- Grade: B+.
- Added detail: Sleeping also frees the core for other threads. The 100 us margin is an untested guess and needs tuning against measured wake-up jitter on each platform.

---

## Phase 1 drill (graded)

**Q1. What does `unordered_map::find` do in memory for a 1M-entry table, and why was a Get hit 84 ns at 1M keys but 14 ns at 1K?**
- Your answer: Hash the key, find the bucket and node; at 1M the nodes are not in cache so they come from DRAM; at 1K everything fits in cache.
- Grade: B+.
- Corrected answer: hash, then bucket array (cache miss), then in libc++ a pointer to the node before the target, then the node and key compare (another miss), so 2-4 misses. The extra ~70 ns at 1M is about one DRAM access. 1K entries (~150 KB) live in the 6 MB L2, not L1. Our `Get` returns a view and never reads the 64-byte value bytes, so the value's separate allocation is not touched here.

**Q2. What invalidates a `string_view` from `Get`, and what does that mean for a multithreaded server?**
- Your answer: Put, Delete, Put with a new key, clear, destruction; races if another thread destroys the value; fix with an owning copy, a mutex or `shared_ptr`.
- Grade: B+.
- Corrected answer: a `Put` that overwrites the same key, `Delete`, `clear`, and destruction or move of the store invalidate it. A `Put` of a *different* key does not, because `unordered_map` is node-based and elements never move (even on rehash); an open-addressing table (Phase 5) will lose this guarantee. A mutex only helps if it covers the whole *use* of the view, so copy under the lock. Alternatives: `shared_ptr` values (atomic refcount traffic on shared cache lines), epoch-based reclamation (Phase 9), or shard ownership with a copy into the response.

**Q3. Why is a miss faster than a hit at 1M keys but slower at 1K keys?**
- Your answer: At 1M a miss can end at an empty bucket quickly; at 1K a hit finds a cached node while a miss has to walk more nodes.
- Grade: A-.
- Added detail: at 1K everything is cached, so the difference is instruction and chain-walking cost: a miss must walk the whole chain, a hit stops at the first match. At 1M, memory accesses dominate and a miss often touches fewer lines. This is a plausible explanation, not verified; check with `perf` counters on Linux in Phase 5.

**Q4. Why did the open-loop p50 (375-459 ns) differ from the throughput benchmark (84 ns), and which would you quote on a CV?**
- Your answer: not answered.
- Grade: not answered. **Retry.**
- Model answer: the tight loop runs back to back with a warm, full-clock core and overlapped work; the paced run idles ~5 us between operations (clock changes, colder caches), and each sample also includes a clock read and key formatting. Quote the open-loop latency with its offered load stated, and call the tight-loop figure best-case throughput. Never present 84 ns alone as "latency".

**Q5. Which bug did the random test miss, why, and how do you fix the random test?**
- Your answer: not answered.
- Grade: not answered. **Retry.**
- Model answer: `Get` looked up only the first 4 bytes of the key (mutation M4). The random test used keys `k0`-`k49`, at most 3 characters, so truncation changed nothing. Fix the generator: variable-length keys, shared long prefixes, and NUL bytes.

---

## Still owed (carry into the Phase 2 recap)
- Retry Phase 1 Q4 and Q5 in your own words.
- Earlier re-answers (p99 tail amplification, coordinated omission cause and latency spread, sleep-then-spin, cache-miss count, string_view invalidation): you said you did these; I have not seen them, so they are recorded as self-reported and ungraded.

---

## Phase 2 pre-check (graded)

**Q1. Why does a contended `std::mutex` make more threads slower, not just no faster?**
- Your answer: Waiting threads cannot do their work, so all those routines are halted, making them slower overall.
- Grade: B-. Explains "no faster" (serialisation), not "slower".
- Corrected answer: (1) blocking and waking a thread costs microseconds (sleep, context switch, wake-up) and adds latency to every handoff; (2) the lock word is written on every acquire and release, so its cache line bounces between cores (cache coherence), and the more threads, the more bouncing. Both costs grow with thread count while the critical section stays serial (Amdahl).

**Q2. What goes wrong if the wrapper copies the value after unlocking, and what does the copy cost?**
- Your answer: Do not know.
- Grade: not answered.
- Model answer: another thread can `Put` or `Delete` the key between unlock and copy, so the view points at freed or overwritten memory: a use-after-free and a data race (TSan would report it). The copy costs an allocation plus a `memcpy` while holding the lock: tens of ns for 64 bytes, milliseconds for 1 MB, which lengthens the serial section. A caller-supplied reusable buffer avoids the allocation.

**Q3. Would `shared_mutex` beat a plain mutex for 90% Get / 10% Put with ~100 ns operations?**
- Your answer: Yes, because many readers can proceed at once and only 10% are writes.
- Grade: C+. Right principle, likely wrong conclusion for this case.
- Corrected answer: a read lock still performs an atomic increment on a shared reader count, so concurrent readers still contend on one cache line. With a ~100 ns critical section that overhead can cancel or exceed the saving, so `shared_mutex` is often no faster, or slower, than a plain mutex. `shared_mutex` pays off when the critical section is long compared with the locking overhead. **Hypothesis recorded; we will measure it** (`contention --store=mutex` vs `--store=shared`).

---

## Phase 2 drill: model answers

**Authorship note:** these answers were written by Claude at Ratul's request so they can be studied from a printout. They are NOT Ratul's own answers and have not been graded. The numbers come from `docs/BENCHMARKS.md` (B-005 to B-010), measured on an Apple M5 Mac with libc++.

**Q1. In the in-process results, `MutexStore` throughput fell from 4.78M ops/s with 1 client to 1.90M with 4. Explain why, using Amdahl's law and what happens to the lock itself.**

*Amdahl's law.* If a fraction `p` of the work can run in parallel on `n` workers, the best speedup is `1 / ((1 - p) + p / n)`. In our loop almost everything (hashing, lookup, copying the value) happens inside the lock, so it is serial. Only generating the key and a random number, about 11 ns of roughly 190 ns per operation at 1M keys, is outside it. So `p` is about 0.06 and the best possible speedup with 4 threads is about `1 / (0.94 + 0.015)`, which is 1.05x. Even a perfect lock could not make 4 clients meaningfully faster than 1.

*Why it got slower instead of staying flat (0.4x).* The lock is not free once threads contend for it.
- The lock word is written on every acquire and release, so its cache line must move between cores each time (cache coherence, MESI: a write invalidates every other core's copy). The hash-table data that the previous holder touched is in that holder's cache too, so the next holder has to fetch it.
- A thread that finds the mutex taken goes to sleep and must be woken by the kernel. A sleep-and-wake costs microseconds (a context switch), while the work it protects takes about 100 ns, so the handoff costs far more than the work.
- More threads than cores (16 and 64 clients on 10 cores) adds nothing: throughput plateaus at about 2.2M ops/s because the machine is saturated.

*Also visible:* with one client the lock already costs 10% (4.78M vs 5.31M for no lock), which is the price of the uncontended acquire and release.

**Q2. `SharedMutexStore` was slower than `MutexStore` at every client count, even though 90% of operations are reads. Why? What would you change in the workload for `shared_mutex` to win, and why may the result differ on Linux?**

*Why it lost.*
- A read lock is still a write to shared memory. Taking a shared lock updates a reader count, so concurrent readers still fight over one cache line. The "readers run in parallel" benefit only appears if the locking overhead is small compared with the work done while holding the lock. Here the work is about 100 ns, so the overhead dominates.
- On this machine libc++'s `std::shared_mutex` is built from an internal `std::mutex` plus two condition variables (I checked the SDK header). Every `lock_shared()` briefly takes that internal mutex, so it costs more than a plain mutex even with no contention (0.72x with 1 client at 1M keys).
- The 10% writers must wait for readers to drain and block new readers while they wait, which causes queuing and sleeping. The gap widens with threads: 0.75x at 4 clients, 0.25x at 16, 0.15x at 64.

*What would make it win.* A critical section that is long compared with the locking overhead: for example reads that scan a range, copy megabytes, or touch disk; or a very read-heavy mix (well above 99% reads) with many reader threads. Then readers genuinely overlap for a long time while the cost of the reader-count update is small by comparison.

*Why Linux may differ.* libstdc++ implements `shared_mutex` with `pthread_rwlock`, whose reader fast path is, as I understand it, an atomic update of a single state word. That is lighter than libc++'s mutex and condition variables, so the gap should be smaller, but readers still share that cache line. This is an expectation, not a measurement: re-measure on the Linux machine before quoting any ratio.

**Q3. The gRPC server tops out near 52K requests/s, but the in-process mutex store handles about 2.2M ops/s. What does that tell you about the bottleneck? Why did running the work on our `ThreadPool` cost about 22% throughput compared with running inline?**

*Where the bottleneck is.* At 52K requests/s each request costs about 19 us of machine time, while the locked store call costs under 0.5 us. So the store and its lock are only about 2% of a request. The rest is the RPC stack: serialising and parsing protobuf messages, HTTP/2 framing, system calls to read and write loopback TCP, waking threads, allocating memory, plus the load-generator clients running on the same 10 cores. Making the lock faster would change almost nothing here. This is why the Phase 2 lesson "the global lock limits scaling" is visible in process (B-005) but hidden behind gRPC end to end (B-007).

*Why the pool costs 22%.* With the pool, every request takes an extra trip: the gRPC thread locks the pool's single mutex to push the task, notifies a condition variable (a system call to wake a worker), and a worker thread wakes (a context switch), runs a store call that takes about 0.3 us, and completes the reply from a different thread. The extra work is a lock, a queue operation, a wake-up and a context switch per request, and the store work is far too small to repay it. The pool's one queue mutex is also a contention point. A pool is worth it when tasks are long or can block (disk, network, slow computation), not when they take a fraction of a microsecond.

**Q4. The first open-loop gRPC results showed latency growing with the number of clients at a constant offered load. How did we find that this came from the load generator, what was the root cause, and why does the fix sleep for half the remaining time instead of using a bigger fixed margin?**

*How we found it.* Offered load was a constant 5,000 requests/s, yet p50 latency was 65 us, 0.4 ms, 1.3 ms and 4.4 ms for 1, 4, 16 and 64 clients, and the numbers were the same for all three server configurations (inline, pool with mutex, pool with shared lock). Something common to every configuration, so not the lock or the pool, was responsible. The latency was roughly 0.35 times the interval between a client's requests (200 us, 800 us, 3.2 ms, 12.8 ms). That pointed at the waiting code in the generator. A 20-line experiment measuring `sleep_for` showed it returns late by about 25-30% of the requested time (a 12.7 ms sleep returned about 3.2 ms late).

*Root cause.* The runner slept until 100 us before the deadline and then spun. The overshoot is proportional to the sleep, so for long waits it was far bigger than the 100 us margin, every operation started late, and because latency is measured from the intended start time, that lateness was recorded as latency. For intervals under 200 us it never slept at all, so 16 clients meant 16 spinning threads competing for 10 cores.

*Why halve instead of a bigger margin.* Overshoot grows with the sleep length, so no single fixed margin suits all intervals. A margin big enough for a 12.8 ms interval (above 4 ms) would have each of 64 clients spin for milliseconds per request, burning more cores than the machine has and distorting what we measure. Sleeping for half of the remaining time can never overshoot the deadline, because the overshoot is under about 31% of the sleep, which is less than the half we left. The remaining time shrinks geometrically (about five sleeps for 12 ms), and we spin only for the last 50 us.

*What remained.* After the fix the gRPC latency still grew with the client count (p50 2.7 ms at 64 clients). An in-process control showed the generator's own wake-up lateness explains only about 0.1 ms of that. The rest is somewhere in the gRPC and operating-system path for many sparsely used connections, and it is recorded as undiagnosed. Only the 1-client round trip (about 65-69 us) is a clean number.

**Q5. Thread pool: (a) why is `stop_` set under the mutex? (b) why is the wait a predicate loop? (c) why must tasks run outside the lock? (d) why does shutdown exit only when the queue is also empty?**

*(a) Setting `stop_` under the mutex.* `wait` checks the predicate while holding the mutex and then releases the mutex and sleeps as one atomic step. If the destructor changed `stop_` without the mutex, a worker could check the predicate (false), and then before it actually sleeps the destructor sets the flag and calls `notify_all`. Nobody is asleep yet, so the notification is lost; the worker then sleeps forever and `join()` hangs. This is the lost wakeup. Changing the state under the same mutex makes "check, then sleep" atomic with respect to the change.

*(b) The predicate loop.* `wait(lock, pred)` is `while (!pred()) wait(lock);`. A condition variable may wake spuriously, with no one having notified. Also a wake-up can be "stolen": `notify_one` wakes worker A, but worker B, which just finished a task, takes the new task first, so A wakes to an empty queue and must go back to sleep. At shutdown, `notify_all` wakes everyone and each must re-check whether to exit or run a leftover task. Re-checking the condition each time is the only safe approach.

*(c) Running tasks outside the lock.* If a worker held the mutex while running its task, only one task could run at a time, so the pool would be serial. It would also deadlock when a task calls `Submit`, which needs the same mutex (`std::mutex` is not recursive).

*(d) Exiting only when the queue is empty too.* A worker leaves only when `stop_` is set and the queue is empty. That guarantees every task the pool accepted runs exactly once. If workers left as soon as `stop_` was set, tasks still queued would be silently dropped. It also lets tasks that submit more tasks during shutdown finish, because the submitting worker is still alive and will pick up the new task.

*Bonus: why `workers_` is declared last.* Members are constructed in declaration order. The threads start in the constructor body, but putting `workers_` last means every other member (mutex, condition variable, queue, flag) is fully constructed before any thread can touch it, and it is destroyed first, after the destructor has already joined the threads.

---

## Still owed (carry into Phase 3)
- Phase 1 drill Q4 and Q5, and the Phase 2 pre-check Q1 and Q2: model answers are above; retry in your own words when ready.
- Phase 2 drill: model answers are above (written by Claude, not graded); optionally answer in your own words and ask for grading.
- Earlier re-answers (p99 tail amplification, coordinated omission cause, sleep-then-spin, cache-miss count, `string_view` invalidation) are self-reported and ungraded.
- Update to the Phase 0 Q5 model answer: the 100 us spin margin was measured and found wrong on macOS (sleep overshoot is proportional to the sleep length); see D-010.

---

## Phase 3 drill: model answers

**Authorship note:** the Phase 3 code and these answers were written by Claude at Ratul's request. They are NOT Ratul's own answers and are not graded. No Phase 3 measurement exists yet, so every statement about performance below is a prediction, marked as such.

**Q1. What does "shared-nothing" mean in `ShardedStore`, and what is still shared?**

Each shard owns a `Store` that only its own worker thread ever touches, so reading and writing the data needs no lock and no atomic. Ownership replaces locking: if only one thread can touch the data, there is nothing to protect.

What is still shared: (1) the queue in front of each shard, since any caller thread pushes into it and the worker pops, so it has a mutex and condition variables; (2) the reply hand-back, a mutex and condition variable per request; (3) the router (hash and shard table), which is read-only after construction and so needs no lock. The accurate phrase is "shared-nothing shards with message passing between threads". The consequence: a request for a key owned by another shard must be handed over by message, which costs a lock, a wake-up and a context switch; and nothing spanning two shards can be atomic without extra machinery.

**Q2. Why a fixed hash plus modulo, not `std::hash`, consistent hashing, or rendezvous hashing?**

`std::hash<std::string_view>` gives different results in libstdc++ and libc++, so the same key would reach different shards on the Mac and the Linux machine. A fixed function (FNV-1a over the bytes, then the splitmix64 finalizer) is identical everywhere, and unit tests pin known hash values. The finalizer matters because `% N` uses the low bits and FNV mixes those poorly.

Consistent hashing and rendezvous (highest random weight) hashing exist to limit how many keys move when the shard count changes: about 1/N instead of nearly all of them. Our N never changes while a store is running, so the benefit cannot appear, and rendezvous hashing would compute N hashes per request instead of one. They become relevant in Phase 8, when cluster members can join or leave. Range sharding needs an ordered key space and creates hot ranges. A bit mask instead of `%` would restrict N to powers of two, which the 1-to-6 sweep forbids. Expected load imbalance, computed independently on 100,000 keys, is about 2%.

**Q3. Why does the request live on the caller's stack, and why does `Complete()` notify while holding the lock?**

The caller blocks until the worker has finished, so a stack `Request` is guaranteed to outlive the worker's use of it, along with the key and value views and the output-string pointer inside it. That avoids a heap allocation per request (which `std::promise`/`std::future` would need) and avoids copying the key and value into the queue.

The same property creates a trap: the moment the worker sets `done_` and releases the mutex, the caller may wake, return from `Wait()`, and destroy the `Request`, including its condition variable. If the worker then called `notify_one`, it would signal a destroyed object. Notifying while still holding the lock prevents it, because the caller cannot finish `Wait()` until the worker releases the mutex, which happens after the notify. The result writes (`ok`, `size`, the copied value) are ordered before the caller's reads by the mutex, so no extra atomics are needed.

**Q4. Why a bounded queue with blocking `Push` and two condition variables? What happens with a hot key?**

A bounded queue gives backpressure: if a shard falls behind, producers wait instead of the queue growing without limit and latency and memory exploding. Rejecting or dropping needs retry logic in every caller; an unbounded queue hides overload. Two condition variables (`not_full_`, `not_empty_`) let each notify wake the right kind of waiter: a producer freed by a pop, a consumer woken by a push. With one, every notify would have to wake everyone, or might wake a waiter that cannot proceed. `Stop()` makes consumers drain what is queued, then exit, so no accepted request is lost.

A hot key sends most traffic to one shard, so that shard's queue fills and its worker is the bottleneck however many shards exist; the other cores idle. Because `Push` blocks, callers using that key slow down, and callers using other keys can also be held up if they are stuck behind it on the same threads. Mitigations (not built): detect and cache the hot key, split it, or replicate for reads.

**Q5. What do you expect the scaling curve to look like, why one pinned thread per physical core, and what would make you distrust the benchmark? (predictions only)**

Prediction, to be tested: one shard is slower than the plain mutex store at one client, because every operation pays the hand-off. Throughput should then rise with shards until the client threads and shard threads together exhaust the cores, and flatten or fall near 6 shards because clients and shards share the same 6 cores / 12 hardware threads. Whether the sharded store beats the mutex store at 64 clients depends on whether the hand-off costs less than the contention it removes; that is the open question.

One shard per physical core avoids two shards sharing a core's execution units through SMT, and pinning stops the scheduler moving a shard and discarding its warm caches. `lscpu -e` shows which CPU numbers are siblings (same CORE value). I would distrust the benchmark if: a shard reported `-1` (not pinned), the machine was not quiet (`uptime`), turbo or the frequency governor changed between runs, client threads and shards were oversubscribed without recording the client count, the spread between repetitions was large, or the tests had not passed under TSan first.

---

## Still owed (carry forward)
- Phase 3 drill: model answers above (Claude's, not graded); answer in your own words when ready.
- Phase 1 drill Q4 and Q5, and the Phase 2 pre-check Q1 and Q2: retry in your own words.
- Earlier re-answers are self-reported and ungraded.
