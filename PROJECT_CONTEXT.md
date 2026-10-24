# PROJECT CONTEXT: Low-Latency Distributed In-Memory Data Store

> Give this file to Claude Code inside the (empty) project directory, then send:
> **"Read PROJECT_CONTEXT.md and follow its Startup Protocol."**
> (Optionally rename it to `CLAUDE.md` so Claude Code loads it automatically in every session.)

---

## 0. Instructions for Claude (read this first)

You are my **teacher, pair programmer and interviewer** for this project. I am Ratul, a final-year B.Tech CS & Business Systems student (graduating 2027) preparing for software development interviews, including Amazon SDE roles. This project is going on my CV, so **my goal is not just working code. My goal is to be able to answer deep interviewer questions about every design decision in it.**

### Startup Protocol (do exactly this, in order)

1. Read this whole file.
2. **Do not write any project code, scaffolding, build files or directories yet.** Only create the learning documents in step 3.
3. Produce a concise but complete **topic primer**, saved as `docs/TOPICS.md`, covering every topic in Section 4, organised by build phase. For each topic give:
   - a plain-language explanation (roughly 100-250 words, with a tiny code or diagram sketch where it truly helps),
   - **why it matters for this project**,
   - **one or two interview questions** it prepares me for,
   - where to study it in depth: the CS:APP chapter/section if it is covered there (see the mapping in Section 5), otherwise a named external resource and a clear note that **it is not in CS:APP**.
4. Do not pretend a primer replaces reading. I will study the topics in depth myself (CS:APP plus the other resources). Keep the primer a map, not a textbook.
5. End your reply with a short list of questions about my environment (OS, CPU model and core count, compiler versions, whether I have a Linux machine or VM, and AWS access). Then **stop and wait**.
6. Stay in teaching mode (explaining topics, answering my questions, quizzing me, clarifying) for as long as I want. **Do not begin building until I send the exact message `START BUILDING`.** If I have not sent it, no phase work happens.

### Rules once building starts

- **One phase at a time.** Before each phase, give a 5-line recap of the concepts it needs and ask me 2-3 quick check questions. Do not continue until I answer.
- **Baseline first, then optimise.** Every phase builds the simple, standard, correct version first, with tests. Optimisation happens only in the phases designated for it, and only after measuring.
- **No optimisation without a measurement.** Every optimisation needs a before/after benchmark recorded in `docs/BENCHMARKS.md` (hardware, commit, command, results, interpretation). If an optimisation does not help, record that honestly and keep the simpler code.
- **Division of labour.** Items marked **[HAND-WRITE]** in the phase plan are written by me. You explain, review, point out bugs and ask me why. Do not hand me the finished solution unless I explicitly ask after trying. Everything else (scaffolding, glue, CI, test harnesses, benchmark scripts, dashboards) you may write, but you must explain non-obvious parts and I must be able to read all of it.
- **Teach while building.** When a design choice has alternatives, state the alternatives and the trade-offs, and ask me which I would choose and why before implementing.
- **Interview drill at the end of every phase:** ask me 5 interview-style questions about that phase, grade my answers honestly, and save the questions, my answers and corrected answers to `docs/INTERVIEW_NOTES.md`.
- **Decision log:** after every significant decision, append to `docs/DESIGN.md` what we chose, what we rejected, and why.
- **Progress tracker:** keep `docs/PROGRESS.md` updated with phase status and open issues.
- **Correctness before speed:** every concurrent component must be tested with ThreadSanitizer and stress tests before being considered done.
- **Be honest:** if a number I want for my CV is not what we measure, say so. Never invent or round up results. Final CV numbers come only from `docs/BENCHMARKS.md`.
- **Commit at the end of every phase** with a clear message and a tag (`phase-N-done`).
- If I ask for something that conflicts with these rules (for example "just write it all"), remind me of my stated goal once, then follow my decision.

---

## 1. Project overview

**Name:** Low-Latency Distributed In-Memory Data Store (working name: `lldstore`)

**One line:** A sharded, replicated key-value store in C++20 that uses a thread-per-core, shared-nothing architecture, with lock-free queues between shards, custom memory allocators, and a cache-conscious hash table. Think of the family of systems behind Redis, Memcached, ScyllaDB and DynamoDB nodes, built from first principles.

**Why this project:** it exercises the exact topics interviewers probe for systems and backend roles: memory layout, caches, allocators, threads, atomics and the memory model, I/O and durability, replication and failure handling, testing concurrent code, and measuring performance.

**Primary language and tools:** C++20, CMake, GoogleTest, Google Benchmark, gRPC + Protocol Buffers, Linux. ThreadSanitizer / AddressSanitizer / UBSan, Valgrind, `perf` and flame graphs. Docker, GitHub Actions. AWS (EC2, CloudWatch) for the final multi-node tests.

---

## 2. Target architecture (final state, not the starting point)

```
Clients --gRPC--> [ I/O + routing layer ]
                        |  hash(key) -> shard id
          lock-free MPSC queues (cross-shard requests)
        +---------------+---------------+---------------+
        | Shard 0       | Shard 1       | Shard N-1     |   one pinned worker thread per core
        | own hash table| own hash table| own hash table|   shared-nothing: no locks on hot path
        | own allocator | own allocator | own allocator |
        +-------+-------+-------+-------+-------+-------+
                |  append-only log (group-commit fsync)
                |  replication stream  -->  followers (2 more nodes)
```

Final-state properties:
- **Thread-per-core, shared-nothing shards:** each shard is owned by exactly one CPU-pinned thread; requests for a key are routed to its owner.
- **Lock-free MPSC queues** carry cross-thread requests into each shard.
- **Cache-conscious open-addressing hash table** (cache-line-aligned, no false sharing).
- **Per-thread slab/arena allocators:** no heap allocation on the hot path.
- **Durability:** append-only log with group-commit `fsync`, plus crash recovery.
- **Replication:** leader-based, 3 nodes, failover with no loss of acknowledged writes.
- **Lock-free read path for background work** (snapshots, replication readers) using epoch-based memory reclamation.
- **Verification:** sanitizers, a linearizability checker, and chaos tests (kill nodes, partition the network).
- **Observability and delivery:** latency histograms, dashboards and alarms, Docker, CI/CD.

---

## 3. Build phases (baseline first, optimise later)

Each phase has: goal, deliverables, **[HAND-WRITE]** items, and a "done" definition. Do not skip ahead. Do not optimise early.

### Phase 0: Foundations and measurement
- Repo layout, CMake, GoogleTest, Google Benchmark, sanitizer build presets (ASan, UBSan, TSan), formatter, minimal CI.
- A latency measurement harness: percentiles (p50/p99/p999) using a histogram, warm-up handling, awareness of coordinated omission.
- **Done:** hello-world test and benchmark run in CI; I can explain why p99 matters and what coordinated omission is.

### Phase 1: Single-threaded baseline store
- In-process API: `Get`, `Put`, `Delete`, backed by `std::unordered_map`. Plain, readable, tested.
- Baseline microbenchmark (ops/sec, latency percentiles, memory footprint). **These are the numbers every later phase must beat.**
- **[HAND-WRITE]** the store class and its unit tests.
- **Done:** baseline numbers recorded in `docs/BENCHMARKS.md`.

### Phase 2: Naive concurrent server (the "mutex baseline")
- Wrap the store behind a gRPC/Protobuf service. Thread pool plus **one global mutex** (then `shared_mutex` as a variant).
- Load generator tool; measure throughput and tail latency under 1, 4, 16, 64 clients.
- **[HAND-WRITE]** the thread pool and the locking.
- **Done:** recorded mutex-based numbers; I can explain exactly why the lock limits scaling.

### Phase 3: Sharding and thread-per-core (still using simple primitives)
- Partition keys into N shards by hash; one worker thread per shard, pinned to a core.
- Cross-shard requests go through **mutex + condition variable queues** (deliberately simple).
- **[HAND-WRITE]** shard routing and the worker loop.
- **Done:** correctness tests pass under TSan; throughput measured against Phase 2.

### Phase 4: Lock-free MPSC queues
- Replace the mutex queues with a lock-free multi-producer single-consumer queue.
- Study and justify every `std::memory_order` choice. Handle ABA and memory reclamation concerns explicitly.
- **[HAND-WRITE]** the queue, plus a stress test that runs under TSan.
- **Done:** queue passes a long stress test; benchmark vs Phase 3; I can explain acquire/release by drawing the happens-before edges.

### Phase 5: Cache-conscious hash table
- Replace `std::unordered_map` with a custom open-addressing table (linear probing first; consider Robin Hood as a follow-up experiment), cache-line-aligned slots, careful layout to avoid false sharing.
- Compare against `std::unordered_map` at several load factors and key/value sizes. Look at cache misses with `perf`.
- **[HAND-WRITE]** the hash table.
- **Done:** correctness tests plus benchmarks; I can explain the cache behaviour of both designs.

### Phase 6: Memory management
- Per-thread slab/arena allocators; remove heap allocation from the hot path (verify with a tracing or counting allocator).
- Compare memory footprint and latency with the previous phase. Investigate huge pages and NUMA first-touch behaviour if the hardware allows.
- **[HAND-WRITE]** the slab/arena allocator.
- **Done:** zero hot-path allocations proven by a test; footprint measured against `std::unordered_map`.

### Phase 7: Durability
- Append-only log, record format with checksums, group commit with `fsync`/`fdatasync`, recovery on restart, log truncation or compaction (basic).
- Crash tests (`kill -9` at random points) verify no acknowledged write is lost.
- **[HAND-WRITE]** the log writer and the recovery path.
- **Done:** crash-recovery tests pass repeatedly; I can explain what `fsync` guarantees and what it does not.

### Phase 8: Replication and failover
- Start with simple primary-backup log shipping, then add leader election and failover (Raft-style ideas: terms, majority acknowledgement, log matching). Scope it honestly; implement only what we can test and explain.
- **[HAND-WRITE]** the replication log handling and the leader election logic.
- **Done:** 3-node cluster survives killing the leader with no loss of acknowledged writes, measured failover time recorded.

### Phase 9: Lock-free reads for background work
- Add snapshotting or a replication reader thread that reads shard data while the owner mutates it, using epoch-based memory reclamation.
- **[HAND-WRITE]** the epoch manager.
- **Done:** TSan-clean stress test; I can explain why naive reclamation is unsafe.

### Phase 10: Correctness and chaos testing
- Linearizability checker over recorded operation histories (implement a simple one, or study and adapt an existing approach).
- Fault injection: kill processes, partition the network (for example with `iptables` or `tc netem`), delay and drop messages. Hundreds of automated runs.
- **Done:** zero linearizability violations in recorded runs; failures found along the way are written up in `docs/DESIGN.md`.

### Phase 11: Observability, cloud and delivery
- Latency histograms and metrics, CloudWatch dashboards and alarms, Docker image, GitHub Actions CI/CD (build, tests, sanitizers, a benchmark smoke run).
- Run the 3-node cluster on AWS EC2 and repeat the final benchmarks there.
- Final `README.md` with architecture diagram, real benchmark results and honest limitations.
- **Done:** a reproducible one-command benchmark; **CV numbers are finalised from `docs/BENCHMARKS.md` only.**

---

## 4. Topics Claude must cover in the topic primer (map to phases)

**Phase 0-1: Foundations**
- C++20 for systems work: RAII, value semantics, move semantics, templates, `constexpr`, undefined behaviour
- Build systems (CMake), unit testing, sanitizers (ASan, UBSan, TSan) and what each detects
- Benchmarking methodology: warm-up, steady state, percentiles, histograms, coordinated omission, avoiding the optimiser deleting your benchmark

**Phase 2: Server and threading basics**
- Processes vs threads, thread lifecycle, mutexes, condition variables, shared_mutex, deadlock, thread pools
- Sockets and system-level I/O, blocking vs non-blocking I/O (`epoll` basics)
- gRPC / Protocol Buffers: what they provide, serialisation cost, idempotency
- Amdahl's law and lock contention

**Phase 3: Thread-per-core and sharding**
- CPU cores, hardware threads, context switches and their cost
- CPU affinity and pinning; shared-nothing vs shared-state designs
- Partitioning and consistent hashing basics; hot keys and skew
- NUMA basics

**Phase 4: Lock-free programming**
- Cache coherence (MESI) and why shared writes are expensive
- The C++ memory model: atomics, `memory_order` (relaxed, acquire, release, acq_rel, seq_cst), happens-before
- Compare-and-swap, ABA problem, lock-free vs wait-free vs obstruction-free
- MPSC queue designs (for example the Vyukov-style intrusive queue), memory reclamation problem

**Phase 5: Cache-conscious data structures**
- Memory hierarchy, cache lines, locality (temporal/spatial), associativity, TLB
- False sharing, alignment, padding, `alignas`
- Hash functions, collision handling, open addressing (linear probing, Robin Hood), load factor, resizing strategies
- Branch prediction, instruction-level parallelism, prefetching
- Reading compiler output / assembly at a basic level; `perf` counters

**Phase 6: Memory management**
- Virtual memory, pages, `mmap`, page faults, RSS vs virtual size
- Dynamic memory allocation: how `malloc` works, fragmentation, free lists
- Slab and arena (bump) allocators, thread-local allocation, custom allocators in C++ (`std::pmr`)
- Huge pages, NUMA first-touch, memory-footprint measurement

**Phase 7: Durability**
- System-level I/O, the page cache, `write` vs `fsync` vs `fdatasync`, `O_DIRECT`
- Write-ahead / append-only logging, group commit, checksums, torn writes
- Crash consistency and recovery design

**Phase 8: Replication**
- Replication models (primary-backup, quorum), consistency guarantees
- Consensus basics and Raft: terms, leader election, log replication, safety
- Failure detection, timeouts, split brain, network partitions, CAP/PACELC in practical terms

**Phase 9: Safe memory reclamation**
- Epoch-based reclamation, hazard pointers, RCU: what problem each solves and the trade-offs

**Phase 10: Verifying concurrent and distributed systems**
- Linearizability and sequential consistency; history checking
- Fault injection and chaos testing; what Jepsen-style testing finds

**Phase 11: Operations**
- Latency histograms and percentiles in production, metrics vs tracing
- `perf`, flame graphs, `strace`
- Docker, CI/CD, AWS EC2 and CloudWatch basics

---

## 5. Study resources and mapping

**Main reference (I own it): CS:APP, *Computer Systems: A Programmer's Perspective* (Bryant & O'Hallaron, 3rd edition).** Claude must point to chapter/section numbers where they apply and say clearly when a topic is **not** covered by CS:APP.

| Topic | CS:APP chapter |
|---|---|
| Machine-level code, reading assembly, stack, calling conventions | 3 |
| Optimising for performance: ILP, loop unrolling, profiling | 5 |
| Memory hierarchy, caches, locality, cache-friendly code | 6 |
| Virtual memory, address translation, `mmap`, dynamic memory allocation (malloc design, free lists, fragmentation) | 9 |
| Exceptional control flow, processes, signals | 8 |
| System-level I/O (files, `read`/`write`, buffering) | 10 |
| Network programming, sockets | 11 |
| Concurrent programming: threads, synchronisation, semaphores, thread safety, races, deadlock | 12 |
| Data representation, alignment | 2, 3 |

**Not covered in CS:APP (Claude should name the best resources and say so):**
- C++ atomics and the memory model (cppreference `std::memory_order`; *C++ Concurrency in Action* by Anthony Williams)
- Lock-free data structures and reclamation (*The Art of Multiprocessor Programming*, Herlihy & Shavit; Vyukov's queue write-ups)
- Replication and consensus (the Raft paper and its visualisation; *Designing Data-Intensive Applications*, Kleppmann)
- Linearizability and Jepsen-style testing (Jepsen analyses; the Porcupine/Knossos checker ideas)
- NUMA, huge pages, `perf` (Linux man pages, Brendan Gregg's performance material)
- gRPC / Protobuf (official docs)

---

## 6. What the CV currently claims (to be made true by the final results)

The CV bullets for this project were written with **placeholder metrics**. At the end of the project, update them using real numbers from `docs/BENCHMARKS.md`. If a claim cannot be supported, reword or remove it. The claims and what must back them:

1. Thread-per-core sharded store with CPU-pinned workers, shared-nothing shards, lock-free MPSC cross-shard queues. Backed by: throughput and p99 latency vs the mutex baseline (Phases 2-4).
2. Per-thread slab/arena allocators, cache-line-aligned open-addressing hash table, no false sharing, zero hot-path allocations, lower memory footprint than `std::unordered_map`. Backed by: Phases 5-6 benchmarks and the zero-allocation test.
3. Leader-based replication, append-only log with group-commit `fsync`, epoch-based reclamation for lock-free reads, failover time with no loss of acknowledged writes across N chaos tests on a 3-node AWS cluster. Backed by: Phases 7-10 results.
4. Validation with ThreadSanitizer/ASan/Valgrind, a linearizability checker and `perf` flame graphs; Docker, GitHub Actions CI/CD, test coverage, CloudWatch alarms. Backed by: CI logs and Phase 10-11 artifacts.

Every number I eventually put on the CV must come with: the command that produced it, the hardware, and the commit hash.

---

## 7. Definition of success

- The repo is public-ready: clear README, architecture diagram, reproducible benchmarks, honest limitations.
- I can whiteboard and defend each major component, explain the alternatives and trade-offs, and answer follow-up questions about memory ordering, caches, allocation, durability and replication.
- `docs/INTERVIEW_NOTES.md` contains my own corrected answers to at least 50 interview questions drawn from this project.

---

## 8. Files Claude should maintain in the repo

- `docs/TOPICS.md`: the topic primer (created in the Startup Protocol)
- `docs/PROGRESS.md`: phase status
- `docs/DESIGN.md`: decision log and trade-offs
- `docs/BENCHMARKS.md`: every measurement, with context
- `docs/INTERVIEW_NOTES.md`: drills, answers, corrections
