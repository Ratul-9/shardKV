# Phase 3 task list: sharding and thread-per-core

Ratul writes the code. Claude reviews each item when you say "review task N" and lists mistakes. Tick a box only after the review passes.

Owner key: **R** = Ratul writes it, **C** = Claude writes glue or harness (only if asked).

## Decisions first (write each in docs/DESIGN.md as D-012 onward before coding)
- [ ] 1. **R** Shard count default and sweep range (plan says 1 to 6 on the 6-core Ubuntu machine). Justify it against physical cores vs hardware threads.
- [ ] 2. **R** Shard function: `hash(key) % N`. Write down why modulo is right for a fixed N, and which hash you use (`std::hash` differs between libstdc++ and libc++, so say how you handle that).
- [ ] 3. **R** Cross-shard queue: bounded or unbounded? What happens when a shard falls behind (block, reject, or grow)? Pick one and say why.
- [ ] 4. **R** Reply path: `std::promise`/`std::future`, a callback, or a per-request slot. Say what each costs on the hot path.
- [ ] 5. **R** Pinning policy: one thread per physical core, siblings left idle. Record the core-number map from `lscpu -e` on the Ubuntu machine.

## Implementation
- [ ] 6. **R** Shard object: owns its hash table (`Store` instance) and is touched only by its worker thread. Note which members are shared and which are not.
- [ ] 7. **R** Bounded request queue between clients and a shard, using one mutex and two condition variables ("not full" and "not empty"). Shutdown must drain or reject cleanly (same rule as ThreadPool, D-007).
- [ ] 8. **R** Worker loop: pop request, run store op, complete reply. Thread pinned with `pthread_setaffinity_np`.
- [ ] 9. **R** Router: hash the key, pick the shard, enqueue, wait for reply. Keep it free of locks of its own.
- [ ] 10. **R** False sharing: queue ends and per-shard counters on separate 64-byte lines (`alignas(64)`). Say which fields you padded and why.
- [ ] 11. **R** Pinning check: a test that reads back the affinity mask of each worker and fails if it is wrong (Linux only, skip on macOS).

## Verification (before any benchmark)
- [ ] 12. **R** Unit tests for routing: the same key always maps to the same shard; keys spread across shards (chi-square or max/min ratio on 1M keys).
- [ ] 13. **R** Correctness test: many client threads, random Put/Get/Delete, compared against a reference model per key (same style as `locked_store_test.cpp`).
- [ ] 14. **R** Shutdown test: destroying the sharded store with requests in flight.
- [ ] 15. Run all tests under `debug`, `asan`, `ubsan` and `tsan` on the Ubuntu machine (TSan needs `vm.mmap_rnd_bits=28`).

## Measurement (Linux machine only, release preset)
- [ ] 16. **C** Benchmark harness that sweeps shard count 1 to 6 with the same workload as Phase 2, and writes `bench/results/linux/phase3_*.txt`.
- [ ] 17. **R** Run it on the Ubuntu machine, record the commit hash and `uptime`, and paste the output back for review.
- [ ] 18. **R** Write B-011 in `docs/BENCHMARKS.md`: scaling vs Phase 2 mutex on the same machine, with the queue handoff cost named as the limit.
- [ ] 19. **R** Explain the result in `docs/LEARNING_NOTES.md`: why the curve bends, and what load imbalance looks like.

## Close-out
- [ ] 20. **R** Drill: 5 questions in `docs/INTERVIEW_NOTES.md` (Claude will write the questions when asked).
- [ ] 21. Update `PROGRESS.md`, `COMMANDS.md` and `REFERENCES.md`; tag `phase-3-done`.

## Notes
- Phase 3 code from the Linux machine is not on this Mac yet. Push it from Linux (or send the files) before task 6 so we don't duplicate work.
