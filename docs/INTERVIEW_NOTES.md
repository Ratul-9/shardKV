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
