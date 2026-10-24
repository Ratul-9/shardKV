# Topic Primer: lldstore

This is a map, not a textbook. For each topic: a short explanation, why it matters for this project, two interview questions, and where to study it. **CS:APP** = *Computer Systems: A Programmer's Perspective* (Bryant & O'Hallaron, 3rd ed.). Where a topic is not in CS:APP, the entry says so.

---

## Phase 0-1: Foundations

### 1.1 C++20 for systems work (RAII, value and move semantics, templates, `constexpr`, UB)
RAII ties a resource's lifetime to an object's scope, so destructors release memory, locks and file descriptors even when exceptions fly. Value semantics means objects are copied or moved rather than shared through pointers by default. Move semantics transfers ownership of internal resources cheaply through rvalue references. Templates give compile-time polymorphism with no virtual-call cost. `constexpr` moves computation to compile time. Undefined behaviour (signed overflow, data races, out-of-bounds access, use-after-free) lets the compiler assume it never happens, so bugs can appear far from their cause.
**Why it matters:** every shard, queue and allocator here manages raw memory and must not leak, double-free, or race.
**Interview questions:** (1) What does `std::move` actually do, and why can a moved-from object still be destroyed safely? (2) Why is a data race undefined behaviour and not just a wrong value?
**Study:** CS:APP Ch. 2 (data representation) and Ch. 3. C++ details: *Effective Modern C++* (Scott Meyers). **Not in CS:APP** for C++ idioms.

### 1.2 Build systems (CMake), unit testing (GoogleTest), sanitizers (ASan, UBSan, TSan)
CMake generates native build files from a portable description and manages targets, flags and dependencies. GoogleTest provides test fixtures and assertions. AddressSanitizer detects out-of-bounds access, use-after-free and leaks by instrumenting memory accesses. UndefinedBehaviorSanitizer catches signed overflow, misaligned access and similar UB at runtime. ThreadSanitizer detects data races by tracking happens-before relations at runtime. Sanitizers only find bugs on code paths that actually run, so tests must exercise concurrency.
**Why it matters:** the project's done criterion for concurrent code is "passes under TSan and stress tests."
**Interview questions:** (1) What kinds of bugs can TSan miss, and why? (2) Why can ASan and TSan not be enabled in the same build?
**Study:** CS:APP Ch. 12 (races) and Ch. 9 (memory bugs). Sanitizer docs at clang.llvm.org. Sanitizers are **not in CS:APP** as tools.

### 1.3 Benchmarking methodology (warm-up, steady state, percentiles, histograms, coordinated omission)
A benchmark must reach steady state before measuring: caches warm, JITs and allocators settled, CPU frequency stable. Averages hide tails, so report p50, p99 and p999 from a histogram. Coordinated omission happens when a load generator waits for each response before sending the next request; slow responses then reduce the number of samples taken during the stall, making the tail look better than users experienced it. The fix is to schedule requests at fixed intended times and measure latency from the intended send time. Optimiser elimination happens when the compiler proves a result unused and deletes the work; use `benchmark::DoNotOptimize`.
**Why it matters:** every CV number comes from these measurements, and coordinated omission would make them dishonest.
**Interview questions:** (1) Why is p99 more useful than mean latency for a store that serves many requests per user action? (2) Explain coordinated omission with a concrete example.
**Study:** *How NOT to Measure Latency* (Gil Tene, talk). Brendan Gregg's performance material. **Not in CS:APP.**

---

## Phase 2: Server and threading basics

### 2.1 Processes vs threads, thread lifecycle, mutexes, condition variables, `shared_mutex`, deadlock, thread pools
A process has its own address space; threads share one and have their own stacks and registers. A mutex gives mutual exclusion. A condition variable lets a thread sleep until a predicate becomes true, which avoids busy-waiting. `shared_mutex` allows many concurrent readers and one writer, which helps only when reads dominate and the critical section is long enough. Deadlock needs four conditions (mutual exclusion, hold-and-wait, no preemption, circular wait); breaking one prevents it, usually by fixed lock ordering. A thread pool reuses a fixed set of threads to run queued tasks, avoiding creation cost.
**Why it matters:** Phase 2 is the baseline every later phase must beat, and its mutex is the bottleneck we measure.
**Interview questions:** (1) Why must a condition variable wait be in a loop? (2) Give a deadlock example and how you would prevent it.
**Study:** CS:APP Ch. 8 (processes, signals) and Ch. 12 (threads, synchronisation, deadlock).

### 2.2 Sockets, blocking vs non-blocking I/O, `epoll`
A socket is a file descriptor for a network endpoint. Blocking calls sleep until data arrives, which costs one thread per connection. Non-blocking I/O returns immediately with `EAGAIN` when nothing is ready. `epoll` lets one thread wait on thousands of descriptors and receive only the ready ones, which is how event-driven servers scale. Edge-triggered mode reports changes; level-triggered mode reports ongoing readiness.
**Why it matters:** the I/O layer in front of the shards must accept many clients without a thread each.
**Interview questions:** (1) Why does `select` scale worse than `epoll`? (2) What bugs follow from using edge-triggered `epoll` without draining the socket?
**Study:** CS:APP Ch. 10 (system-level I/O) and Ch. 11 (network programming). `epoll(7)` man page for details.

### 2.3 gRPC and Protocol Buffers
Protobuf defines messages in a schema and generates compact binary serialisation. gRPC runs RPCs over HTTP/2, giving multiplexed streams, flow control and generated client and server stubs. Serialisation costs CPU and allocations on every request, which matters at high throughput. Idempotency means repeating a request has the same effect as doing it once; this matters when a client retries after a timeout and cannot tell whether the first attempt succeeded.
**Why it matters:** it is the external API, and retries on Put/Delete need idempotency to avoid double application.
**Interview questions:** (1) How would you make a Put safe to retry? (2) Where does gRPC spend time for a small key-value request?
**Study:** Official gRPC and protobuf docs (grpc.io, protobuf.dev). **Not in CS:APP.**

### 2.4 Amdahl's law and lock contention
Amdahl's law says that if a fraction `p` of work is parallel across `n` workers, speedup is at most `1 / ((1 - p) + p / n)`. A serial section, such as one global mutex, caps speedup no matter how many threads you add. Contention also adds cost: waiting threads spin or sleep, and the lock's cache line bounces between cores.
**Why it matters:** explains why the Phase 2 mutex baseline stops scaling, which motivates sharding.
**Interview questions:** (1) If 10% of a request is serial, what is the maximum speedup on 16 cores? (2) Why can more threads make a contended lock slower?
**Study:** CS:APP Ch. 5 (performance, Amdahl's law) and Ch. 12.

---

## Phase 3: Thread-per-core and sharding

### 3.1 CPU cores, hardware threads, context switches and their cost
A core executes instructions; SMT (hyper-threading) runs two hardware threads on one core sharing its execution units. A context switch saves one thread's registers and loads another's, and costs direct cycles plus lost cache and TLB warmth. Too many runnable threads per core causes switching overhead and cache thrash.
**Why it matters:** the design runs one worker per core so the scheduler rarely preempts a shard's thread.
**Interview questions:** (1) What does a context switch cost beyond the register save? (2) Why might you pin one thread per physical core and leave SMT siblings idle?
**Study:** CS:APP Ch. 1 (hardware overview) and Ch. 8. Linux `sched(7)`.

### 3.2 CPU affinity and pinning; shared-nothing vs shared-state designs
Affinity restricts a thread to a set of cores (`pthread_setaffinity_np`, Linux). Pinning keeps a thread's working set in one core's caches. Shared-nothing means each shard's data is touched only by its owner thread, so the hot path needs no locks and no coherence traffic. Shared-state designs protect data with locks or atomics, which is simpler to write but makes lines bounce between cores.
**Why it matters:** the central architectural bet. Cross-shard work goes through queues instead of locks.
**Interview questions:** (1) What breaks if two threads touch the same shard? (2) When is shared state the better choice?
**Study:** CS:APP Ch. 12 for shared-state issues. The Seastar framework design notes (shared-nothing, thread-per-core). **Affinity APIs are not in CS:APP.**

### 3.3 Partitioning, consistent hashing, hot keys and skew
Partitioning maps keys to shards, here by `hash(key) % N`. Modulo hashing reshuffles most keys when N changes; consistent hashing moves only about `1/N` of keys. A hot key sends most traffic to one shard, so one core becomes the bottleneck regardless of total shard count. Mitigations include key-level caching, splitting the hot key, or replication for reads.
**Why it matters:** determines how load spreads across the N pinned workers and what the benchmarks can and cannot claim.
**Interview questions:** (1) Why does `hash % N` fail when you add a shard? (2) How would you detect and handle a hot key?
**Study:** *Designing Data-Intensive Applications* (Kleppmann), Ch. 6. Karger et al. 1997 paper on consistent hashing. **Not in CS:APP.**

### 3.4 NUMA basics
On multi-socket machines, memory attached to one socket is slower for cores on another. Linux places a page on the node of the core that first touches it ("first-touch" policy). `numactl` and `libnuma` control placement.
**Why it matters:** a shard's memory should live on its pinned core's NUMA node.
**Interview questions:** (1) What is first-touch placement and why does it matter for a shard that is initialised by the main thread? (2) How would you measure remote memory access?
**Study:** Linux `numa(7)` man page; Drepper, *What Every Programmer Should Know About Memory*. CS:APP covers the memory hierarchy (Ch. 6) but not NUMA specifically.

---

## Phase 4: Lock-free programming

### 4.1 Cache coherence (MESI) and why shared writes are expensive
In MESI, each cache line is Modified, Exclusive, Shared or Invalid. A write to a line held by another core forces an invalidation and a transfer, often 50-100+ ns across sockets. Frequent shared writes therefore serialise on cache-line ownership even without locks.
**Why it matters:** explains why queue head and tail pointers, and per-shard counters, must be kept on separate cache lines.
**Interview questions:** (1) Why can two unrelated atomics in the same cache line slow each other down? (2) What state transition happens when core A writes a line that core B has in Shared state?
**Study:** CS:APP Ch. 6 (memory hierarchy) and Ch. 12 (shared variables). Ulrich Drepper's paper for coherence details.

### 4.2 The C++ memory model: atomics and `memory_order`
`std::atomic` gives indivisible operations on a variable. `memory_order` controls which other memory operations may be reordered around them. `relaxed` gives atomicity only. `acquire` on a load and `release` on a store create a happens-before edge: everything written before the release is visible after the matching acquire. `acq_rel` does both for read-modify-write. `seq_cst` adds a single global order for all such operations, at the highest cost. Happens-before is the partial order the model guarantees, and data races are defined against it.
**Why it matters:** the Phase 4 queue's correctness rests on choosing these orders correctly.
**Interview questions:** (1) Draw the happens-before edges in a producer that publishes a pointer with release and a consumer that reads it with acquire. (2) When is `relaxed` safe?
**Study:** cppreference `std::memory_order`; *C++ Concurrency in Action* (Anthony Williams). **Not in CS:APP.** CS:APP Ch. 12 touches on synchronisation only briefly.

### 4.3 Compare-and-swap, ABA, and progress guarantees
Compare-and-swap (CAS) atomically writes a new value only if the current value equals an expected value. ABA: a value changes A to B and back to A between a thread's read and its CAS; the CAS succeeds although the structure changed underneath it. Lock-free means some thread always makes progress; wait-free means every thread finishes in bounded steps; obstruction-free means progress only when a thread runs alone.
**Why it matters:** the MPSC queue and any pointer-swinging structure must handle ABA explicitly.
**Interview questions:** (1) Show how ABA corrupts a lock-free stack. (2) Is a CAS retry loop lock-free or wait-free, and why?
**Study:** *The Art of Multiprocessor Programming* (Herlihy & Shavit), Ch. 5 and 9. **Not in CS:APP.**

### 4.4 MPSC queue designs and the memory reclamation problem
Vyukov's MPSC queue uses an intrusive singly linked list with a per-node sequence or next pointer; producers swap the tail atomically, and the single consumer walks the head. Memory reclamation asks when a node may be freed: a concurrent reader might still hold a pointer to it. Freeing too early causes use-after-free; freeing never causes leaks. Solutions include epochs, hazard pointers and reference counting (Phase 9).
**Why it matters:** the cross-shard request path is this queue; a bug here corrupts routing for every request.
**Interview questions:** (1) What is the difference between a single-consumer and multi-consumer queue for memory reclamation? (2) Why can't you just `delete` a node right after dequeuing it?
**Study:** Dmitry Vyukov's intrusive MPSC queue write-ups; *The Art of Multiprocessor Programming*. **Not in CS:APP.**

---

## Phase 5: Cache-conscious data structures

### 5.1 Memory hierarchy, cache lines, locality, associativity, TLB
Registers, L1, L2, L3 and DRAM form a hierarchy with rising latency. Caches move data in 64-byte lines. Temporal locality reuses recently accessed data; spatial locality uses neighbours in the same line. Set-associative caches map each address to a limited set of ways, so bad strides can cause conflict misses. The TLB caches virtual-to-physical translations; TLB misses are expensive for large, scattered tables.
**Why it matters:** determines whether the hash table probes stay within a line or two.
**Interview questions:** (1) Why is an array of structs versus a struct of arrays a cache question? (2) What is a TLB miss and when does it dominate?
**Study:** CS:APP Ch. 6 (memory hierarchy, cache-friendly code).

### 5.2 False sharing, alignment, padding, `alignas`
False sharing occurs when threads write different variables that share a cache line, so the line bounces between cores even though no data is shared. Padding inserts unused bytes; `alignas(64)` (or `std::hardware_destructive_interference_size`) aligns objects to line boundaries. The cost is memory.
**Why it matters:** per-shard counters and slots must not share lines.
**Interview questions:** (1) How would you detect false sharing with `perf`? (2) Why is `alignas(64)` sometimes not enough?
**Study:** CS:APP Ch. 6 and Ch. 12. Drepper's memory paper.

### 5.3 Hash functions, collisions, open addressing, load factor, resizing
A hash function maps keys to indices; good ones spread bits evenly and resist adversarial input. Open addressing stores all entries in one array and resolves collisions by probing: linear probing checks the next slot; Robin Hood probing swaps entries to keep probe lengths similar. Load factor is entries divided by slots; high load makes probes long. Resizing rehashes into a larger array, a costly pause unless done incrementally.
**Why it matters:** this is the store's core data structure, hand-written in Phase 5.
**Interview questions:** (1) Why does linear probing suffer from clustering, and how does Robin Hood help? (2) How would you delete from an open-addressing table?
**Study:** CS:APP Ch. 9 for dynamic structures. Herlihy & Shavit for hash-table design. Robin Hood hashing paper (Celis, 1986).

### 5.4 Branch prediction, instruction-level parallelism, prefetching
Modern CPUs guess branch outcomes; mispredictions cost roughly 15-20 cycles. Out-of-order execution overlaps independent instructions. Prefetching loads data before it is needed, either by hardware (stride detection) or with explicit `__builtin_prefetch`. Branchless code can avoid mispredictions in tight loops.
**Why it matters:** probe loops in the hash table are branch-heavy; their cost is partly a prediction question.
**Interview questions:** (1) Why can sorting an array speed up a branchy loop? (2) When would an explicit prefetch hurt?
**Study:** CS:APP Ch. 5 (ILP, loop unrolling) and Ch. 6.

### 5.5 Reading compiler output and `perf` counters
`objdump -d` or `godbolt.org` shows the assembly the compiler emits. `perf stat` reports hardware counters: cycles, instructions, cache misses, branch misses. `perf record` with flame graphs shows where time goes. IPC (instructions per cycle) shows whether the CPU is busy or stalled.
**Why it matters:** the Phase 5 claim is "fewer cache misses," and `perf` is how we prove it.
**Interview questions:** (1) What does a low IPC tell you? (2) How do you tell cache misses from branch misses in `perf stat`?
**Study:** CS:APP Ch. 3 (reading assembly), Ch. 5 (profiling). Brendan Gregg's `perf` and flame graph material. Tooling is **not in CS:APP**.

---

## Phase 6: Memory management

### 6.1 Virtual memory, pages, `mmap`, page faults, RSS vs virtual size
Each process sees a virtual address space mapped to physical pages (typically 4 KiB). `mmap` maps files or anonymous memory into that space. The first access to a page causes a page fault, which the OS resolves by allocating the page. RSS is the resident physical memory; virtual size is the mapped address range, which can be much larger.
**Why it matters:** footprint claims need RSS, not virtual size, and first-touch affects when page faults occur.
**Interview questions:** (1) Why can a process have large virtual size but small RSS? (2) What happens on a page fault for a page that was never written?
**Study:** CS:APP Ch. 9 (virtual memory, `mmap`).

### 6.2 Dynamic memory allocation: how `malloc` works, fragmentation, free lists
`malloc` tracks free blocks in free lists, splits and coalesces them, and returns metadata alongside each block. External fragmentation is free memory split into pieces too small to use; internal fragmentation is space wasted inside a block. Allocators trade speed, fragmentation and per-block overhead.
**Why it matters:** explains why the store avoids `malloc` on the hot path.
**Interview questions:** (1) What is the difference between internal and external fragmentation? (2) Why does a general-purpose allocator struggle under multithreaded load?
**Study:** CS:APP Ch. 9 (dynamic memory allocation). This is a direct CS:APP topic.

### 6.3 Slab and arena (bump) allocators, thread-local allocation, `std::pmr`
An arena allocates by advancing a pointer and frees everything at once; it is very fast but cannot free single objects. A slab keeps fixed-size object pools so frees are O(1) and fragmentation is low. Thread-local allocation avoids locks because each thread owns its arena. `std::pmr::memory_resource` is the standard hook for swapping allocators into containers.
**Why it matters:** the Phase 6 allocator is hand-written and must prove zero hot-path allocations.
**Interview questions:** (1) When is an arena the wrong choice? (2) How does a slab allocator avoid fragmentation for fixed-size objects?
**Study:** *Mallocing* writeups; Berger's Hoard paper for multithreaded allocators. `std::pmr` on cppreference. **Slab and arena designs are not covered in CS:APP.**

### 6.4 Huge pages and footprint measurement
Huge pages (2 MiB or 1 GiB) reduce TLB misses for large tables. Linux supports them via transparent huge pages or `hugetlbfs`. Footprint should be measured via RSS, allocator counters, or `/proc/self/status`.
**Why it matters:** an optional optimisation, to be measured before adopting.
**Interview questions:** (1) What trade-off do huge pages introduce? (2) How would you prove a footprint improvement is real?
**Study:** CS:APP Ch. 9. Linux `transhuge` kernel docs. **Partially not in CS:APP.**

---

## Phase 7: Durability

### 7.1 System-level I/O, the page cache, `write` vs `fsync` vs `fdatasync`, `O_DIRECT`
`write` copies data into the kernel page cache and returns; the data may not be on disk. `fsync` forces the file's data and metadata to stable storage; `fdatasync` skips metadata that is not needed to read the data back. `O_DIRECT` bypasses the page cache. Disks and controllers may have their own volatile caches, so "fsync returned" depends on hardware honouring flush requests.
**Why it matters:** the durability guarantee of an acknowledged write rests on exactly what `fsync` promises.
**Interview questions:** (1) What does `fsync` guarantee and what does it not? (2) Why must you fsync the directory after creating a new log file?
**Study:** CS:APP Ch. 10 (system-level I/O). `fsync(2)` and the PostgreSQL WAL documentation for practical issues.

### 7.2 Write-ahead logging, group commit, checksums, torn writes
A write-ahead log records an operation durably before applying it in memory. Group commit batches many pending records into one `fsync`, amortising its cost across concurrent writers at a small latency cost. Checksums (CRC32, for example) detect corrupted or partially written records. A torn write happens when a sector or record is only partly persisted at a crash.
**Why it matters:** the log is the spine of recovery and replication.
**Interview questions:** (1) How does group commit improve throughput without raising p99 much? (2) How does a log detect that its last record was torn?
**Study:** *Designing Data-Intensive Applications*, Ch. 3 (storage and logs). **Not in CS:APP.**

### 7.3 Crash consistency and recovery design
After a crash, recovery replays the valid prefix of the log, stops at the first record that fails its checksum, and rebuilds memory state. Recovery must be idempotent, so a crash during recovery is also safe. Log compaction or snapshotting bounds recovery time.
**Why it matters:** the Phase 7 crash test (`kill -9` at random points) checks exactly this.
**Interview questions:** (1) Why must recovery stop at the first bad checksum rather than skip it? (2) What makes recovery idempotent?
**Study:** *Designing Data-Intensive Applications*, Ch. 7 and 8. CS:APP Ch. 10 for the I/O side.

---

## Phase 8: Replication and failover

### 8.1 Replication models and consistency guarantees
Primary-backup sends all writes through one leader to followers. Quorum replication requires acknowledgements from a majority, so any two quorums overlap. Consistency guarantees range from eventual, to read-your-writes, to linearizable. Synchronous replication waits for followers before acknowledging; asynchronous replication does not, risking loss of recent acknowledged writes on failover.
**Why it matters:** the project requires no loss of acknowledged writes, which rules out plain async replication.
**Interview questions:** (1) Why does majority acknowledgement prevent lost writes during a leader change? (2) What is the difference between replication and consistency?
**Study:** *Designing Data-Intensive Applications*, Ch. 5. **Not in CS:APP.**

### 8.2 Consensus basics and Raft
Raft divides consensus into leader election, log replication and safety. Time is split into terms. A leader is elected by majority vote; a log entry is committed once a majority stores it; the Log Matching property means entries with the same index and term are identical. Safety comes from voting only for candidates whose logs are at least as up to date.
**Why it matters:** Phase 8's leader election follows Raft-style ideas, scoped to what we can test.
**Interview questions:** (1) Why can a Raft leader only commit entries from its current term directly? (2) Why does Raft require a majority to elect a leader?
**Study:** Ongaro & Ousterhout, "In Search of an Understandable Consensus Algorithm" (the Raft paper) and the Raft visualisation at raft.github.io. **Not in CS:APP.**

### 8.3 Failure detection, timeouts, split brain, partitions, CAP/PACELC
Failure detectors use heartbeats and timeouts, which cannot distinguish a dead node from a slow one. Split brain is when two nodes both believe they are leader. Partitions force a choice in CAP: consistency or availability during a partition. PACELC adds the trade-off without a partition: latency or consistency. Fencing tokens or terms prevent a stale leader from acting.
**Why it matters:** determines the failover design and what the chaos tests should expect.
**Interview questions:** (1) Why can't a timeout reliably detect a crashed node? (2) What does a fencing token prevent?
**Study:** *Designing Data-Intensive Applications*, Ch. 8 and 9. Kleppmann's writing on fencing tokens. **Not in CS:APP.**

---

## Phase 9: Safe memory reclamation

### 9.1 Epoch-based reclamation, hazard pointers, RCU
All three answer "when is it safe to free a node that lock-free readers may still see?" Epoch-based reclamation (EBR) has threads announce the global epoch on entry; memory retired in epoch e is freed once all threads have advanced past e. It is fast but one stalled thread blocks reclamation. Hazard pointers publish each pointer a reader is using; reclamation checks these, bounding garbage at the cost of a per-access store. RCU (Read-Copy-Update, from the Linux kernel) makes readers nearly free but writers must wait for a grace period.
**Why it matters:** Phase 9 lets snapshots and replication readers read shard data without locks.
**Interview questions:** (1) Why is naive `delete` unsafe even when the writer holds no lock? (2) What failure mode does EBR have that hazard pointers avoid?
**Study:** McKenney, "Is Parallel Programming Hard?" (RCU); Michael, "Hazard Pointers" (2004). Herlihy & Shavit, Ch. 10. **Not in CS:APP.**

---

## Phase 10: Verifying concurrent and distributed systems

### 10.1 Linearizability, sequential consistency, history checking
Linearizability means each operation appears to take effect atomically at some point between its invocation and response, and that order respects real time. Sequential consistency keeps a single total order that matches each client's program order but ignores real time. A history checker takes a recorded trace of concurrent operations and searches for a valid linearization against a sequential model. The search is NP-hard in general, so practical checkers (Porcupine, Knossos) prune aggressively.
**Why it matters:** the Phase 10 claim of zero violations is only meaningful if the checker is correct.
**Interview questions:** (1) Give a history that is sequentially consistent but not linearizable. (2) Why is checking linearizability expensive?
**Study:** Herbert & Wing, "Linearizability: A Correctness Condition" (1990). Porcupine and Knossos repositories. **Not in CS:APP.**

### 10.2 Fault injection and chaos testing; what Jepsen-style testing finds
Chaos testing kills processes, partitions networks, and delays or drops messages while clients run operations, then checks the recorded history. Tools include `iptables`, `tc netem` and process-kill scripts. Jepsen analyses found real lost-write bugs in many databases by exactly this method. Results are only useful with many runs, varied timing and recorded seeds.
**Why it matters:** this is how we earn the "no loss of acknowledged writes" claim.
**Interview questions:** (1) Why does a single passing chaos run prove little? (2) How would you make a failing run reproducible?
**Study:** Kyle Kingsbury's Jepsen analyses (jepsen.io). `tc-netem(8)` man page. **Not in CS:APP.**

---

## Phase 11: Operations

### 11.1 Latency histograms and percentiles in production; metrics vs tracing
Histograms bucket latencies so percentiles can be computed and aggregated across instances (HdrHistogram is the standard for accurate tails). Metrics are aggregated numbers over time; tracing follows a single request across components. Percentiles cannot be averaged across hosts, so store the histogram or compute from raw buckets.
**Why it matters:** the dashboards and alarms are only as honest as their percentile method.
**Interview questions:** (1) Why can't you average p99 values from two servers? (2) When do you need tracing rather than metrics?
**Study:** HdrHistogram docs (hdrhistogram.org). Google SRE book, monitoring chapter. **Not in CS:APP.**

### 11.2 `perf`, flame graphs, `strace`
`perf record` samples the call stack at intervals; a flame graph visualises these stacks so wide frames mark hot code. `strace` traces system calls, useful for finding unexpected syscalls on the hot path, such as an accidental `malloc` or `futex`.
**Why it matters:** locates bottlenecks before any optimisation is attempted.
**Interview questions:** (1) How do you read a flame graph to find the hot function? (2) How would you use `strace` to check for syscalls in a hot loop?
**Study:** Brendan Gregg, *Systems Performance* and his flame graph site. CS:APP Ch. 8 covers the syscall interface. **Tooling is not in CS:APP.**

### 11.3 Docker, CI/CD, AWS EC2 and CloudWatch basics
Docker packages the binary and its dependencies in an image so benchmarks run the same way anywhere. CI/CD (GitHub Actions) builds, tests and runs sanitizers on each push. EC2 provides VMs; placement groups and instance types affect network and NUMA behaviour, so benchmark results must record the instance type. CloudWatch collects metrics and drives alarms.
**Why it matters:** the final 3-node cluster and the reproducible benchmark run on this stack.
**Interview questions:** (1) What can make a benchmark in a container disagree with the bare-metal result? (2) Why must you record the instance type with every benchmark?
**Study:** Official Docker, GitHub Actions and AWS documentation. **Not in CS:APP.**

---

## Next steps

Before the first phase, I need a few details about your environment (answers go into `docs/DESIGN.md`):

1. Operating system and version (macOS is the current platform; is a Linux machine or VM available?)
2. CPU model and core count, and whether it has multiple sockets
3. Compiler versions (`clang++ --version`, `g++ --version`) and CMake version
4. Linux access for `perf`, NUMA and `epoll` (VM or remote machine)
5. AWS access: do you have an account and credits, and are you comfortable with costs for the final cluster?

Send `START BUILDING` when you are ready for Phase 0. Until then, we stay in teaching mode.
