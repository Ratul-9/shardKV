# Phase 3 task list: sharding and thread-per-core

**Status (2026-11-06):** at Ratul's request, Claude wrote the implementation, tests and benchmark harness. Everything below is compiled but **not run**. Ratul's own draft (`shardedStore.h`) was reviewed and replaced by `include/lldstore/sharded_store.h`. Ratul's remaining job is to read, challenge and defend the code, run it on the Ubuntu machine, and write up the results.

Legend: [x] done (and by whom), [ ] still to do.

## Decisions (all in `docs/DESIGN.md`; Ratul should read each and be ready to defend or change it)
- [x] 1. Shard count default and sweep: D-012 (written by Claude)
- [x] 2. Shard function and hash: D-012 (written by Claude)
- [x] 3. Queue bounds and overflow policy: D-013 (written by Claude)
- [x] 4. Reply path: D-014 (written by Claude)
- [x] 5. Pinning policy: D-015 (written by Claude; SMT sibling map still to be read from `lscpu -e` on Ubuntu)

## Implementation (written by Claude, compiled, not run)
- [x] 6. Shard object owning its `Store`, touched only by its worker: `include/lldstore/sharded_store.h` (`Shard`)
- [x] 7. Bounded queue, one mutex + two condition variables, draining shutdown: `include/lldstore/bounded_queue.h`
- [x] 8. Worker loop with self-pinning: `Shard::Run`, `Shard::Execute`, `Shard::PinCurrentThread`
- [x] 9. Router: `ShardedStore::ShardIndex`, `ShardFor`, `HashKey`, `Dispatch`
- [x] 10. False sharing: `alignas(64)` on `Shard` (no `std::hardware_destructive_interference_size`: GCC 11 lacks it)
- [x] 11. Pinning check: `ShardedStorePinning.ShardsRunOnTheRequestedCpus` (Linux only)

## Verification
- [x] 12. Routing tests: `tests/sharded_store_test.cpp` (`ShardedStoreRouting.*`)
- [x] 13. Concurrent correctness tests: `ShardedStoreConcurrency.*`
- [x] 14. Shutdown and backpressure tests: `ShardedStore.IdleStoresCreateAndDestroyPromptly`, `ShardedStoreConcurrency.TinyQueues...`, `tests/bounded_queue_test.cpp`
- [ ] 15. **Run** all tests under `debug`, `asan`, `ubsan` and `tsan` on the Ubuntu machine (GCC 11 has not compiled this code yet). Expect to fix things.

## Measurement (Linux machine only, release preset)
- [x] 16. Harness: `bench/contention.cpp` (`--store=sharded --shards=N --pin=...`) and `scripts/bench_sharded.sh`
- [ ] 17. Run `scripts/bench_sharded.sh 3` on the Ubuntu machine (see COMMANDS.md section 3h) and paste the output back
- [ ] 18. Write B-011 in `docs/BENCHMARKS.md`: scaling versus the Phase 2 mutex baseline on the same machine, with the hypotheses there confirmed or refuted
- [ ] 19. Explain the result in `docs/LEARNING_NOTES.md`

## Close-out
- [x] 20a. Drill questions and model answers written (labelled as Claude's, not graded): `docs/INTERVIEW_NOTES.md`
- [ ] 20b. Ratul answers the drill in his own words and asks for grading
- [ ] 21. After items 15, 17, 18 and 19: tag `phase-3-done`

## Things to try reading first (suggested order)
1. `bounded_queue.h` (about 100 lines): the simplest piece, and the Phase 2 thread-pool answers apply to it.
2. `sharded_store.h`: read `Request`, then `Shard::Run` and `Execute`, then the public methods.
3. `tests/sharded_store_test.cpp`: the routing tests, then the concurrency tests.
