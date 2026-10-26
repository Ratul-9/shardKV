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
