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

## Phase 4 drill: model answers (written by Claude, NOT Ratul's own, NOT graded)

Ratul asked Claude to write the Phase 4 code. Answer these in your own words before treating the phase as yours. Draw the edges on paper first.

**Q1. Is Vyukov's MPSC queue wait-free? Lock-free? Say exactly what is true of `Push` and of `Pop`.**

`Push` is wait-free: one atomic `exchange` and one store, no loop, so it finishes in a bounded number of steps whatever other producers do. `Pop` never blocks by itself but is not wait-free or lock-free: between a producer's `exchange` and its `next` store the list is inconsistent, and the consumer sees nothing even though a push has begun. If that producer is preempted there, the consumer (and every item behind it) waits for it. So: wait-free producers, a consumer with a known blocking window. A fully lock-free MPSC needs more machinery.

**Q2. Draw the happens-before edge that makes the consumer see a fully written value. What if `next` were stored `relaxed`?**

Producer writes the node's value, then `prev->next.store(fresh, release)`. The consumer's `stub->next.load(acquire)` reads that value; the release/acquire pair means the value write happens-before the consumer's read. With `relaxed` there is no ordering: the consumer could see the pointer but stale or half-written contents. On x86 this usually still works (stores are not reordered with other stores), so the bug would hide there; on arm64 it can fail. That is why the phase says to test on both.

**Q3. Why is `tail_.exchange` `acq_rel` and not just release or just acquire?**

Release: the next producer that exchanges and gets our node as its `prev` must see it fully constructed (value, `next == nullptr`). Acquire: we receive `prev` and are about to write `prev->next`; we must see that node fully constructed, or the constructor's initialisation could be ordered after our store and overwrite it. Each exchange is both consumer of the previous producer's node and publisher of ours.

**Q4. How are nodes freed, by whom, and why is there no ABA problem?**

The consumer frees the old stub after advancing `head_` past it. That is safe because the only thread that writes `stub->next` is the single producer that got `stub` back from its own exchange, and the consumer has just seen that write; no other thread holds a pointer to the stub. ABA is a problem for compare-and-swap loops, where a pointer is reused between a read and the CAS. There is no CAS here (only `exchange`), and a node is never reused while anyone can reach it, so the question never arises.

**Q5. What does the consumer do when the queue looks empty during a producer's two-step push? What stops it exiting early on `Stop`?**

A push first increments `state_` (count of items pushed and not yet popped, plus a stop bit), then links. `Pop` sees `TryPop` empty: if `state_ == 0` it sleeps (`wait(0)` returns immediately if a push already changed the word, so no lost wake-up); if `state_ > 0` a producer is mid-push, so it yields and retries. On stop, `Pop` returns "stopped and drained" only when `state_` is exactly the stop bit: no queued item and none in flight. A push racing with `Stop` is decided by one `fetch_add`: it either sees the stop bit and is rejected, or is counted and will be delivered.

**Defend-it questions for Ratul (he did not write this code):**
1. Why one `MpscQueue` per shard and not one for the whole store?
2. What did unbounded cost compared with Phase 3, and what would you measure to see it?
3. Allocating a node per push may cost more than the lock it replaced. How would you find out, and what is the intrusive alternative?
4. Why are `head_`, `tail_` and `state_` on separate cache lines?
5. Which test would catch a lost wake-up? Which would catch a Push/Stop race? What bug would they NOT catch on x86?

## Still owed (carry forward)
- Phase 3 drill: model answers above (Claude's, not graded); answer in your own words when ready.
- Phase 1 drill Q4 and Q5, and the Phase 2 pre-check Q1 and Q2: retry in your own words.
- Earlier re-answers are self-reported and ungraded.

---

## Phase 5 pre-check (for Ratul to answer in his own words; not yet answered)
1. A `Get` hit on a 1M-entry `std::unordered_map` touches how many cache lines, and which ones? Why does that matter more than the instruction count?
2. In open addressing, why can you not simply empty a slot when a key is deleted?
3. Our shards route with `HashKey(key) % N`. What goes wrong if each shard's table uses the low bits of the same hash to choose a slot?

## Phase 5 drill: model answers (written by Claude, NOT Ratul's own, NOT graded)

Ratul asked Claude to write the Phase 5 code without comments. The reasoning is in `docs/PHASE5_DESIGN_GUIDE.md`. No Phase 5 benchmark has run, so every performance claim below is a prediction.

**Q1. Why is the slot `{64-bit hash, pointer}` rather than the key and value stored inline?**

A probe only reads slots: 16 bytes each, 4 per cache line, adjacent. The stored hash rejects almost every non-matching slot without following the pointer, so a hit costs about one slot line plus one entry line, and a miss about one slot line. Inline `std::string` keys and values would make slots 48-64 bytes (one per line), and growth would move them, invalidating every view `Get` had returned. Out-of-line entries never move, so the D-002 promise ("a Put of a different key never invalidates your view") survives, even across a resize. The stored hash also lets growth and deletion work without touching any entry.

**Q2. Explain backward-shift deletion, and why not tombstones.**

Deleting leaves a hole, and a lookup stops at the first empty slot, so any key whose probe passed through the hole would become unreachable. Backward shift walks forward from the hole until an empty slot. Each entry that can move into the hole and still be reachable from its home (distance from home to its slot ≥ distance from hole to its slot, modulo the capacity) is moved, and the hole follows it. Tombstones instead mark "deleted, keep looking". They pile up, make misses longer, need periodic clean-up rehashes, and if inserts never reuse them, a churning table fills with them and a miss never terminates.

**Q3. Why a power-of-two capacity, and why the TOP bits of the hash for the home slot?**

A power of two turns `hash % capacity` (a 20-40-cycle division) into a shift. The top bits are best mixed by multiplication-based hashing. The important reason is shard routing: keys reach shard `i` because `HashKey(key) % N == i`. If a shard's table used the low bits of the same hash with N = 4, every key in that shard would share its low 2 bits, only a quarter of the home slots would be used, and clusters would explode. We use a different hash function **and** the top bits.

**Q4. What is the maximum load factor, and how did you choose it?**

0.75, but only as a starting point. With linear probing, Knuth's estimate for a miss is ½(1 + 1/(1-α)²): 2.5 slots at 0.5, 8.5 at 0.75 and 50.5 at 0.9. After a doubling the load drops to 0.375, so in steady state it sits between 0.375 and 0.75. The slot array costs 16/α bytes per entry. The value is a constructor parameter, and B-013 sweeps 25-90% at a fixed capacity to replace the guess with a measurement. The clamp at 0.95 guarantees an empty slot, which every probe loop needs in order to terminate.

**Q5. You claim "no false sharing". Defend it precisely.**

The claim is narrow:
- Inside a table there is nothing to falsely share, because only the shard's worker thread ever touches it (shared-nothing).
- The slot array is allocated 64-byte aligned, so two shards' arrays never share a cache line, and no 16-byte slot straddles two lines.
- What is **not** guaranteed: entries come from `malloc`, so in principle two shards' entries could share a line. glibc and macOS use per-thread arenas or caches, which makes this unlikely, but Phase 6's per-thread allocators are what make it impossible.
- The `Shard` object itself is `alignas(64)`.

**Defend-it questions for Ratul (he did not write this code):**
1. `Put` probes once, and probes again after a rehash. Why is the first index useless after `Rehash`?
2. Why must `Assign` allocate the new entry before freeing the old one? Give the call that breaks if the order is reversed.
3. Why `memmove` and not `memcpy` in `CopyBytes`?
4. What does `TransparentStore` in `table_bench.cpp` control for, and what wrong conclusion would you draw without it on Linux?
5. Why does `Get` check `size_ == 0` before probing? (Two reasons: speed, and a shift by 64.)
6. What does the `ConstantHash` test exercise that a random test with a good hash almost never would?
7. Where does the resize pause show up in a benchmark, and in which kind of benchmark does it not show up?
8. The hash seed is fixed. Who could exploit that, and how would you fix it?

## Phases 6 to 11 drills: model answers (written by Claude, NOT Ratul's own, NOT graded)

Ratul asked Claude to finish the project in one go (2026-11-16). None of this code has run. Answer each question in your own words before treating these phases as yours. The design records are `docs/DESIGN.md` D-031 to D-066.

### Phase 6: memory management

**Q1. Why does a per-shard slab allocator need no lock, and what would break it?**
Only the shard's worker thread allocates and frees, because a Get copies the value out before replying, so no other thread holds shard memory. One owner means no lock and no atomics. It breaks the moment another thread frees a block, for example if the shard handed out a pointer instead of a copy. Two threads would then push onto the same free list and corrupt it. Real allocators handle this with a remote-free queue per owner (mimalloc, tcmalloc).

**Q2. How did you prove zero hot-path allocations?**
A test replaces global `operator new` with a counting version. That covers every thread in the process, including the shard workers. The test warms the store, then runs 80,000 mixed requests and asserts the count did not change. A control store that does allocate (one queue node per push) must show at least one allocation per request, so a counter that never counts cannot pass. Two allocations had to go: the queue node (fixed by an intrusive queue, with the link inside the stack-allocated request) and the entry (fixed by slab free lists, since Delete then Put reuses the block).

**Q3. Requested bytes vs RSS: which do you report, and why can the slab look worse on one and better on the other?**
RSS is what the machine pays. Requested bytes are what the program asked `operator new` for. The slab asks for 256 KiB chunks up front, so its requested bytes include unused chunk space. But it puts no 16 B malloc header on each block and rounds 92 B to 96 B, so its RSS per entry can be lower. Report both and say which one matters.

**Q4. What is the cost of never returning chunks?**
Peak memory is kept forever, and memory is stuck in the wrong size class after the workload changes. The fix would be per-chunk live counts and `madvise(MADV_DONTNEED)` on empty chunks, at the price of bookkeeping on every free.

**Q5. Why poison freed slab blocks for ASan?**
To ASan the whole chunk is one live allocation, so a use-after-free into a recycled block would be invisible. Poisoning restores the detection the project relies on.

Defend-it questions: (1) Walk through `ClassIndex(200)` by hand. (2) Why does `FlatStore` ask for `UsableSize` and store the spare bytes? (3) In the intrusive queue, why may the consumer re-link the stub without a race?

### Phase 7: durability

**Q1. When exactly is a write acknowledged, and what does fsync not guarantee?**
After the fdatasync that covers its record has returned, and not before. fsync does not guarantee anything if the drive lies about its volatile cache (consumer SSDs without power-loss protection, and macOS unless you use `F_FULLFSYNC`). It does not make a newly created file's directory entry durable; that needs an fsync of the directory. And after an fsync *error* it does not mean "retry will work": the kernel may have dropped the pages (fsyncgate), so we fail the log and recover from disk.

**Q2. Explain group commit and why it raises throughput without lowering durability.**
Concurrent writers append to a shared buffer. One flusher writes the batch and does one fdatasync, then wakes everyone whose records it covered. Each writer still waits for a sync that covers its own record, so durability is unchanged, but N writers share one sync. Throughput rises roughly with the batch size until bandwidth or the append mutex limits it.

**Q3. Recovery finds a bad record in the middle. Why stop there rather than skip it?**
Records are written in order and acknowledged only after the covering sync, so a damaged record is part of the unacknowledged tail. A valid-looking record after it is either garbage or a write the disk reordered, and in both cases it was never acknowledged. Skipping could apply records out of order or resurrect a write whose client saw a failure.

**Q4. Why must a read wait for durability too?**
Memory is updated before the sync completes. A reader that returns an applied but undurable value could show a client data that a crash then erases: the client observed something that, after recovery, never happened.

**Q5. Walk through a checkpoint and a crash at each step.**
Wait for the log to be durable, write the snapshot to a tmp file, fsync, rename, fsync the directory, create the next log generation, delete older logs, fsync the directory.
- Crash before the rename: the old snapshot and the old logs are both complete.
- Crash after the rename: the new snapshot names the next generation, so recovery creates or continues that log and deletes the stale ones.

Defend-it questions: (1) Why does the CRC cover the length field? (2) What does the crash test check, and why can the store hold a *later* value than the last acknowledged one? (3) What happens to a waiter if the checkpoint swaps the log object while it waits?

### Phase 8: replication (Raft)

**Q1. Why does Raft vote only for a candidate whose log is at least as up to date as yours?**
A committed entry is on a majority. A winning candidate needs votes from a majority. Any two majorities overlap, so at least one voter has the entry, and that voter refuses a candidate that lacks it. So every leader holds every committed entry (Leader Completeness).

**Q2. Why may a leader count replicas only for entries of its own term (figure 8)?**
An entry from an old term can be on a majority and still be overwritten. A later leader that never received it can win with votes from nodes whose logs have a higher last term. If the current leader commits an entry of its own term on a majority, any future leader must have that entry, and so, by Log Matching, everything before it.

**Q3. What are the no-op entry and check-quorum for?**
- **The no-op:** it lets a new leader commit the earlier-term entries it inherited straight away, without waiting for a client write.
- **Check-quorum:** a leader that has not heard from a majority within an election timeout steps down, so clients stop sending to a leader that cannot commit.

Neither is needed for safety; both are needed for good liveness.

**Q4. A client times out. Did its write happen? How do you make the retry safe?**
The client cannot know. The entry may still commit. It retries with the same (client_id, seq), and the replicated session table answers a duplicate from its memory instead of applying it twice. In a history, a request that never got an answer is "unknown", not "failed".

**Q5. Why is the core a deterministic state machine?**
So the same code runs under a simulator where a seed fixes every delay, drop, partition and crash. A failing run then reproduces exactly. That is how consensus bugs, which hide in rare interleavings, get found and fixed.

Defend-it questions: (1) Why are term and vote persisted in one atomic write? (2) Why does a follower skip entries it already has, rather than truncate and rewrite? (3) What does `next_index` optimistic pipelining risk, and how does the conflict hint recover? (4) What did you NOT implement, and what goes wrong without each piece (PreVote, compaction, membership change)?

### Phase 9: epoch-based reclamation

**Q1. Why is `delete` immediately after unlinking unsafe without a lock?**
A reader may have loaded the pointer just before the unlink and be about to read the node. The unlink stops new readers from finding it, not old ones. The test shows it deterministically: a reader holds a view, the writer deletes, and under ASan the read is a heap-use-after-free.

**Q2. Explain EBR in four sentences.**
- A global epoch counter, and every reader announces the epoch it entered (or "outside").
- Removed nodes are retired with the epoch of their removal.
- The epoch advances from g only when every reader inside has announced g.
- A node retired in r is freed at r + 2, by which time every reader that could have seen it has left.

**Q3. What happens if a reader stalls inside a guard?**
The epoch cannot advance, so nothing is freed and memory grows. Correctness is unaffected. This is EBR's weakness; hazard pointers bound the damage by protecting individual pointers, at the cost of a fence per pointer followed.

**Q4. Why the seq_cst fences?**
Two threads each store to one location and then load the other (the reader stores its slot and loads pointers; the writer stores the unlink and loads the slots). Without a full fence on both sides, both loads can see old values: the reader misses the unlink and the writer misses the announcement. Acquire/release cannot forbid that "store then load" reordering; seq_cst fences can.

**Q5. How do readers survive a resize?**
The writer builds a complete new table with new nodes, publishes it with one release store, and retires the old table and its nodes together. A reader inside the old table finishes there safely.

Defend-it questions: (1) Why must the reader re-check the global epoch after announcing? (2) Why is only the writer allowed to advance the epoch? (3) What does the per-reader version check in the stress test catch?

### Phase 10: correctness

**Q1. Linearizability vs sequential consistency?**
Both need one total order that is consistent with each client's program order. Linearizability also requires that if operation A finishes before B starts in real time, A comes before B. That real-time condition is what lets a client trust "my write returned, so everyone now sees it".

**Q2. How does the checker work, and why per key?**
It backtracks over which pending operation takes effect next, applying each to a model of one key, and memoises (set of operations placed, model state) so no state is explored twice. Linearizability is local: a history is linearizable if and only if each object's sub-history is. Splitting by key turns one huge search into many small ones.

**Q3. How do you know the checker is not just saying "yes"?**
- It must reject known-bad histories: a stale read, a lost write, a value never written, non-monotonic reads.
- It must reject 200 randomly generated correct histories after one read in each is corrupted.
- It must catch a deliberately broken store.

**Q4. How are timeouts represented, and why does it matter?**
As unknown outcomes: a write may be placed anywhere after its call, or left out. Treating a timed-out write as failed would make the checker reject correct histories. Treating it as succeeded would hide lost writes.

**Q5. Why "hundreds of runs", and why assert that faults happened?**
Bugs live in rare interleavings, and one clean run proves little. Asserting the crash and partition counts proves the runs exercised failures; otherwise "zero violations" might only mean nothing went wrong.

Defend-it questions: (1) How is a failing seed replayed? (2) What does `scripts/chaos_partition_linux.sh` change on the machine, and how does it clean up?

### Phase 11: observability and delivery

**Q1. Why can't you average p99 across nodes?**
A percentile is not linear. Two nodes with p99 = 10 ms and 100 ms do not give a cluster p99 of 55 ms; it depends on the request counts and the shape of each distribution. You merge the histograms and take the percentile of the merged one.

**Q2. What would you alarm on, and why?**
- A node that stops reporting (crashed or partitioned).
- Election churn (unstable timeouts or a flaky network).
- p99 latency per node (users feel tails).

**Q3. Why is a benchmark inside Docker not a bare-metal number?**
Shared kernel, cgroup CPU limits, an overlay or bridge network, and noisy neighbours. It is a different setup and must be labelled as one.

**Q4. What does CI prove and what does it not?**
It proves the code builds and the tests pass under every sanitizer on two operating systems, and that the cluster survives a leader kill on a CI runner. It does not prove performance: CI machines are shared and noisy, so the benchmark jobs are smoke runs only.

**Q5. How do you avoid a surprise cloud bill?**
Tag every resource, delete by tag (`cluster.sh down`), set a billing alarm first, and estimate before starting: about US$0.30/hour for 4 small instances, about US$220/month if forgotten.

---

## Still owed (carry forward)
- Phase 5 pre-check above, in your own words.
- Phase 3 and Phase 4 drills: model answers are above (Claude's, not graded); answer in your own words when ready.
- Phase 1 drill Q4 and Q5, and the Phase 2 pre-check Q1 and Q2: retry in your own words.
- Phases 6 to 11 drills: model answers above are Claude's, not graded. Answer each, and the defend-it questions, in your own words.
