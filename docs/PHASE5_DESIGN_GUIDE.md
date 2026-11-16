# Designing a component from scratch: the Phase 5 hash table, step by step

**Authorship:** Claude wrote the Phase 5 code and this guide at Ratul's request. The code has no comments, also at his request, so this file is the commentary. Line numbers refer to `include/lldstore/flat_store.h` as first written.

**Status:** all five build presets (debug, release, asan, ubsan, tsan) compile with `-Werror` on the Mac with Apple clang. `flat_store_tests` was run once, in the debug preset only, before Ratul asked for no runs: 29 of 29 passed. Nothing else has been run: no sanitizer test runs, no `flat_sharded_store_tests`, no benchmark. GCC 11 has not compiled this code. Every performance statement below is a **prediction** until `docs/BENCHMARKS.md` B-013 says otherwise.

How to read this guide:
1. Part 1 is a method you can reuse for any systems component, such as a queue, an allocator, a log or a cache.
2. Part 2 applies the method to Phase 5, decision by decision.
3. Part 3 explains the code function by function, since the code itself has no comments.
4. Part 4 covers how the design is tested and measured.
5. Part 5 lists what was deliberately avoided.
6. Part 6 lists where the design could still be wrong.
7. Part 7 has exercises.

Try the exercises in Part 7 before you reread Part 2. Building the design again yourself is how you learn to do it.

---

## Part 1: The method

Use these steps every time. Most bad designs come from skipping step 2 or step 3.

### Step 1. Write the job and the contract in one paragraph
State the job, what callers may rely on, and what they may not. A contract is more than the function signatures. It also covers lifetimes, invalidation, thread-safety, error behaviour and complexity.

### Step 2. Describe the workload with numbers
Write down key sizes, value sizes, read/write mix, number of entries, number of threads, the access pattern (uniform or skewed) and the hardware. A design that is "fast" with no workload attached means nothing. A hash table for 8-byte integer keys and one for 1 KB string keys are different designs.

### Step 3. Build a cost model
Estimate what each operation costs on real hardware, then count those costs for every candidate design. These are typical orders of magnitude, not measurements from our machines:

| Event | Rough cost |
|---|---|
| L1 hit | ~1 ns (4-5 cycles) |
| L2 hit | ~3-5 ns |
| L3 hit | ~10-20 ns |
| DRAM access (cache miss) | ~70-100 ns |
| TLB miss with a page walk | ~10-30 ns more |
| Branch mispredict | ~15-20 cycles |
| 64-bit integer division (`%`) | ~20-40 cycles |
| `malloc` + `free` of a small block | ~20-50 ns, plus fragmentation |
| Cache line | 64 bytes (x86-64 and arm64) |

Our own anchor: at 1M keys, a `Store::Get` hit was 84 ns on the Mac against 14 ns at 1K keys (B-002). The extra ~70 ns is roughly one DRAM access. **When the data does not fit in cache, count cache misses per operation. Instructions barely matter by comparison.** (CS:APP §6.2-6.6.)

### Step 4. List the constraints you inherited
Earlier decisions bind you. Look them up in `DESIGN.md` instead of rediscovering them. For Phase 5 these were D-002 (`Get` returns a view, with its invalidation rules), D-012 (shard routing uses `HashKey % N`), Phase 3's single-owner shards (no thread-safety needed inside the table), GCC 11 compatibility and `-Wconversion -Werror`.

### Step 5. Enumerate candidates and score them against the cost model
For each candidate, count cache misses for a hit and for a miss, allocations per entry, bytes per entry, and how complex deletion and growth are. Reject each loser with a written reason. That reason is exactly what an interviewer will ask for.

### Step 6. Choose the simplest design that meets the goal, and turn guesses into knobs
If a choice depends on a number you cannot know yet (for example the best maximum load factor), make it a parameter with a sensible default and plan a measurement to set it.

### Step 7. Decide how you will prove it correct before you prove it fast
Write down the invariants. Then design tests that **can fail**. Use adversarial inputs that force the rare paths, and check that each test catches a deliberately injected bug.

### Step 8. Decide how you will measure, including controls
A before/after comparison changes one thing. When a change bundles two improvements, add a control that has only one of them, or you cannot say which one helped. Write your predictions down **before** running anything.

### Step 9. Write down what you are not doing
Each item you leave out is a decision with a reason, a cost, and a trigger that would make you revisit it.

---

## Part 2: The method applied to Phase 5

### 2.1 Job and contract
Replace `std::unordered_map` inside each shard with a cache-conscious open-addressing table. Keep the `Store` interface exactly, so it drops into `BasicShardedStore` and the benchmarks without other changes:

```
void Put(string_view key, string_view value)
optional<string_view> Get(string_view key) const
bool Delete(string_view key)
size_t Size() const
```

The contract carried over from D-002 and D-004:
- A view returned by `Get` stays valid until that key is overwritten or deleted, or the table is destroyed.
- A `Put` of a **different** key never invalidates it, even when the table grows. `unordered_map` gave us this because its nodes never move. Many open-addressing tables lose this guarantee. Keeping it became a design requirement, and it shaped the slot layout (2.4).
- Keys and values are arbitrary bytes, including `\0`, the empty string and large values.
- Single-threaded. In this architecture only the shard's worker thread touches the table.

### 2.2 Workload
- 16-byte keys (`key:` + 12 digits) and 64-byte values, uniform random access, 90% Get and 10% Put. Up to about 1M keys, roughly 150 MB with `unordered_map`, far beyond any cache.
- Secondary cases to check: 8-byte values (fit the small-string buffer), 40-byte keys (exceed it on both standard libraries) and 512-byte values.
- Hardware: the Mac (arm64, libc++) for building, and the i5-12400F (x86-64, libstdc++, `perf` available) for measuring.

### 2.3 What the baseline costs (the cost model for `unordered_map`)
A `Get` hit at 1M keys on libc++:
1. Builds a temporary `std::string` from the `string_view` (D-003). This is free on libc++ for 16 bytes, but **a heap allocation on libstdc++**, whose inline buffer holds 15 bytes.
2. Hashes the key, then reads the bucket array: **miss 1**.
3. The bucket holds a pointer to the node *before* the target (libc++): often **miss 2**.
4. Reads the node, which contains the key: **miss 3** (on libstdc++ the 16-byte key is a separate allocation, so add **miss 4**).
5. Reading the value bytes (when a shard copies them out) is a separate allocation: **one more miss**.

There are also 2 allocations per entry on libc++ and 3 on libstdc++ (B-004: 149 requested bytes and about 204 resident bytes per 80-byte entry on the Mac).

So the target is **fewer cache misses per lookup, fewer allocations per entry and fewer bytes per entry**, while keeping the view guarantee.

### 2.4 The candidates

| Design | Hit at 1M keys (misses) | Miss | Allocations per entry | Keeps views stable on growth? | Verdict |
|---|---|---|---|---|---|
| A. Chained `unordered_map` (baseline) | 2-4 (+1 for the value) | 1-2 | 2 (libc++) / 3 (libstdc++) | yes | the thing to beat |
| B. **Slot = {64-bit hash, pointer to entry}; one heap block per entry holding key and value** | ~2 (slot line + entry) | ~1 (slot line) | 1 | **yes** (entries never move) | **chosen** |
| C. Key and value `std::string`s stored inside the slots | ~1-2 | ~1-2 | 0-2 (SSO dependent) | **no** (slots move on growth) | rejected |
| D. Swiss table (1-byte control array + SIMD group probing, as in abseil and F14) | ~2 | ~1 | 1 | yes, with out-of-line entries | best follow-up, not first |
| E. Robin Hood linear probing | ~2 | ~1, shorter worst case | 1 | yes | follow-up experiment (the plan says so) |
| F. Cuckoo hashing | 2-3 | **2** (two random lines) | 1 | yes | rejected |

Why each loser lost:
- **C (inline strings):**
  - A `std::string` is 24 bytes on libc++ and 32 on libstdc++, so a slot is 48-64 bytes and holds one entry per cache line. Each step of a probe touches a new line.
  - Growth moves the strings, which breaks the view guarantee.
  - Moving a `std::string` costs more than copying 16 bytes.
- **D (Swiss table):**
  - Probing one byte per slot with SIMD is the state of the art, and it allows high load factors.
  - It needs SSE2 on x86 and NEON on arm64 (two code paths), plus tag and group logic.
  - It is the right second step once we have a measured baseline for plain open addressing. Doing it first would leave nothing to compare it against.
- **E (Robin Hood):** it lowers the *variance* of probe lengths, which matters at high load. The plan schedules it as a follow-up experiment, so it gets its own before/after measurement.
- **F (cuckoo):** every miss checks two unrelated locations (two cache misses), and an insert can loop and force a rehash. It is good for read-mostly tables with tiny entries, but not here.

### 2.5 The decisions, one by one

Each decision has an ID in `docs/DESIGN.md` (D-022 to D-030). Here is the reasoning in teaching form.

#### Decision 1: Slots hold `{hash, Entry*}`; key and value live in one out-of-line block (D-022)
```
slot array (64-byte aligned, 16 B per slot, 4 slots per cache line)
+----------------+----------------+----------------+----------------+
| hash | Entry*  | hash | null    | hash | Entry*  | hash | Entry*  |   <- one cache line
+----------------+----------------+----------------+----------------+
          |
          v
   Entry block (one allocation):  [key_size u32][value_size u32][value_capacity u32][key bytes][value bytes]
```
- A probe reads only slots: 4 per cache line, adjacent in memory.
- A slot whose hash does not match is rejected **without following the pointer**. With 64-bit hashes a false match is about 1 in 2^64, so a hit usually costs one slot line plus one entry, and a miss usually costs one slot line.
- Entries never move. Growth and deletion move only the 16-byte slots, so **views stay valid when other keys are inserted or the table grows**, which keeps the D-002 contract.
- One allocation per entry instead of 2-3.

#### Decision 2: Store the full 64-bit hash in the slot (D-022)
- It costs 8 bytes per slot. In return:
  1. It is a near-perfect filter before any key comparison.
  2. Growth re-places entries using the stored hash, without recomputing it or touching a single entry. Rehashing the keys would mean a DRAM miss for every entry.
  3. Backward-shift deletion can compute a slot's home position from the slot alone.
- **Alternatives rejected:**
  - A 1-byte tag (the Swiss-table approach) is denser, but growth would then need to re-hash every key.
  - Packing the tag into the unused top bits of the pointer saves 8 bytes, but it is not portable: arm64 top-byte-ignore, pointer authentication and memory tagging all use those bits.

#### Decision 3: Entry header of three `uint32_t`, overwrite in place when the new value fits (D-023)
- The header is 12 bytes instead of 24 with `size_t`. Keys and values are limited to 4 GiB - 1 bytes, and `Put` throws `std::length_error` beyond that (`CheckLength`, line 232). A 4 GiB value in an in-memory shard is not a real use case. A silent truncation would be a real bug, so the limit is checked.
- **Overwrite** (`Assign`, line 194): if the new value fits the existing capacity, copy it in place (no allocation), like `std::string::assign` in D-004. Otherwise allocate a new entry, copy, swap the pointer, then free the old entry.
- **Aliasing:** a caller can legally write `Put(k, *Get(k))` or `Put(k, Get(k)->substr(6))`. The source then overlaps the destination, so the copy uses `memmove`, not `memcpy` (`CopyBytes`, line 228). The reallocation path reads the old value before freeing it. A test pins this (`ValueMayAliasAStoredValue`).
- **Consequence:** like `std::string`, an entry keeps its largest capacity. Overwriting a 1 MB value with 1 byte keeps the 1 MB. That is the same behaviour as the baseline. Shrinking is an optimisation we have not measured a need for.

#### Decision 4: Linear probing (D-024)
- Successive probes look at the next slot, which is usually in the same cache line or the next one, and the hardware prefetcher follows a sequential walk.
- Quadratic probing and double hashing jump around, so each step can be a new cache miss. They reduce clustering at the price of locality.
- Linear probing also has the simplest correct deletion (Decision 7).
- **Its weakness is primary clustering:** runs of occupied slots merge and grow. Knuth's estimates for linear probing at load factor α are about ½(1 + 1/(1-α)) slots for a hit and ½(1 + 1/(1-α)²) for a miss:

| α | hit (slots) | miss (slots) |
|---|---|---|
| 0.25 | 1.2 | 1.4 |
| 0.50 | 1.5 | 2.5 |
| 0.75 | 2.5 | 8.5 |
| 0.90 | 5.5 | 50.5 |
| 0.95 | 10.5 | 200.5 |

Misses blow up past about 0.8. That drives Decision 6 and the load-factor sweep in B-013.

#### Decision 5: Power-of-two capacity; home slot = the **top** bits of the hash (D-025)
`Home(hash) = hash >> shift_` where `shift_ = 64 - log2(capacity)` (line 160).
- **No `%`:** a 64-bit division costs 20-40 cycles. A shift costs 1.
- **Top bits rather than `hash & mask`:** multiply-based mixing pushes the best-mixed bits upward, so the top bits are the best ones.
- **Independence from shard routing.** This is the subtle one, and a great interview point. A key reaches shard `i` because `HashKey(key) % N == i`. If the table reused that same hash and took the **low** bits with `N = 4`, every key in shard `i` would share the same 2 low bits. Only a quarter of the home slots would ever be used, and clusters would explode. We avoid this twice: the table uses a **different** hash function (Decision 8), and it takes the **high** bits.

#### Decision 6: Maximum load factor 0.75 by default, configurable from 0.25 to 0.95 (D-026)
- After a doubling the load is 0.375, and it climbs to 0.75 before the next doubling. Using the table above, a hit costs 1.3 to 2.5 slots and a miss costs 1.8 to 8.5, mostly within 1-3 adjacent cache lines.
- Memory for the slot array is 16/α bytes per entry, about 21-43 bytes.
- The value **0.75 is a starting point, not a result.** It is a constructor parameter so B-013 can sweep 25/50/75/90% at a fixed capacity and replace the default with a measured one. The clamp keeps at least one empty slot (`growth_limit_ < capacity_` for every capacity ≥ 16), which **every probe loop relies on to terminate**.

#### Decision 7: Delete by backward shift, no tombstones (D-027)
Removing an entry leaves a hole. A later key whose probe passed through that slot would then become unreachable, because lookups stop at the first empty slot. There are two standard fixes.
- **Tombstones:** mark the slot "deleted, keep probing". Tombstones accumulate, make misses longer, and need periodic clean-up rehashes. If inserts never reuse them, a churning table can fill completely with tombstones, and a miss then loops forever.
- **Backward shift** (chosen; Knuth's Algorithm R): walk forward from the hole. Each entry that would still be reachable from its home if moved into the hole is moved there, and the hole moves to where it was. Stop at the first empty slot.

Example, capacity 8, entries labelled with their home slot:
```
index:   0   1   2   3   4   5   6   7
before:  .   A1  B1  C2  D4  .   .   .      delete A (at 1)
step 1:  hole=1; B at 2 has home 1 -> may move into 1: [. B1 . C2 D4 ...], hole=2
step 2:  C at 3 has home 2 -> may move into 2:          [. B1 C2 . D4 ...], hole=3
step 3:  D at 4 has home 4 -> moving it to 3 would put it BEFORE its home: leave it
step 4:  slot 5 empty -> stop; slot 3 becomes empty
```
The test "may the entry at `next` move into `hole`?" is `distance(home, next) >= distance(hole, next)`, computed modulo the capacity: `((next - home) & mask_) >= ((next - hole) & mask_)` (line 111). The modulo arithmetic handles runs that wrap past the end of the array, and a test forces exactly that. Because the full hash is stored, `home` comes from the slot without touching the entry.

#### Decision 8: Our own word-at-a-time hash, `FlatHash` (D-028)
- **Not FNV-1a:** routing uses FNV-1a (D-012), which processes **one byte per multiply**. That is 16 dependent multiplies for a 16-byte key, roughly 50-70 cycles. `FlatHash` reads 8 bytes at a time with `memcpy` (no alignment or aliasing problems), mixes each word with a multiply and a rotate, and finishes with the splitmix64 finalizer, so every input bit affects every output bit.
- **Different from the routing hash**, so table positions are independent of shard choice (see Decision 5).
- **The length is mixed into the seed.** The last partial word is zero-padded, so without the length `"a"` and `"a\0"` would hash the same, and so would every key differing only in trailing zero bytes. They would still compare unequal (correctness is fine), but they would all collide. A test pins this.
- **Why not `std::hash`:**
  - Routing must be identical on every machine. The table hash only has to be stable within one process, so `std::hash` would be legal here.
  - We still use fixed code, so the probe behaviour is the same on the Mac and on Linux. That keeps benchmark comparisons honest, and lets the tests reason about it.
- **Known weakness (not fixed):** the hash has a fixed seed. Someone who can choose keys can choose colliding ones (HashDoS) and turn every operation into a long probe. A server exposed to untrusted clients should use a per-process random seed with a keyed hash such as SipHash. This is recorded as an open issue. It does not matter for benchmarks, where the keys are ours.

#### Decision 9: Grow by doubling, all at once, never shrink (D-029)
- `Rehash` (line 179) allocates the new array **first**. If that throws, nothing has changed (strong exception guarantee). It then re-places every slot by its stored hash, and frees the old array. It never reads an entry.
- **Cost:** a stop-the-world pause proportional to the capacity. At about 1M entries that means copying and re-placing about 1M 16-byte slots into a 32 MB array. Expect milliseconds (a prediction, not measured). That pause is a p999 spike for whichever request triggers it.
- **Rejected for now:**
  - Incremental resizing, where two tables coexist and a few buckets move per operation, as Redis does. It flattens the spike but makes every operation check two tables.
  - Shrinking. No workload here needs it.
- **Mitigation available:** `Reserve(n)` (line 124) pre-sizes the table.

#### Decision 10: The slot array is 64-byte aligned (D-030)
- `AllocateSlots` (line 205) uses `operator new(size, std::align_val_t{64})`. Because 64 / 16 = 4 exactly, no slot ever straddles two cache lines, and a run of up to 4 slots starting on a line boundary touches one line.
- **False sharing, honestly stated:**
  - Only one thread ever writes a table (shared-nothing shards), so false sharing *inside* a table cannot happen.
  - Alignment guarantees that two shards' slot arrays never share a cache line.
  - The **entries** come from `malloc`, so in theory two shards' entries could sit in the same line. glibc and macOS give each thread its own arenas or caches, so this is unlikely but not guaranteed. Phase 6 (per-thread allocators) closes it.
- The `Shard` object that holds the table's header fields is already `alignas(64)` (D-015).

#### Decision 11: The hash function is a template parameter (D-030)
`BasicFlatStore<Hasher>`, with `using FlatStore = BasicFlatStore<FlatHash>`. Production code never changes the hasher. The parameter exists **for testing**: a constant hash puts every key in one cluster that wraps around the end of the array, a first-byte hash creates heavy clustering, and a "home hash" lets a test choose each key's home slot exactly. `[[no_unique_address]]` makes the empty hasher take no space.

#### Decision 12: Small things that are still decisions (D-030)
- **An empty table allocates nothing** (`capacity_ == 0`). `Get` and `Delete` return early when `size_ == 0`, which also avoids `hash >> 64`, undefined behaviour while `shift_` is still 64.
- **Not copyable, not movable.** It owns raw memory, and nobody needs to copy or move it (the `Shard` holds it in place). Writing correct copy and move operations is code to get wrong for no benefit.
- **Exception safety in `Put`:** grow first, then allocate the entry, then write the slot. If anything throws, the table holds only what it held before, perhaps with more capacity.
- **The baseline `Store` is untouched.** Phase 1's numbers must stay reproducible, so the new table is a new class and the sharded store takes the table type as a template parameter (`BasicShardedStore<QueueT, StoreT = Store>`). `FlatShardedStore = BasicShardedStore<MpscQueue, FlatStore>` differs from `LockFreeShardedStore` in exactly one type.

---

## Part 3: The code, function by function (`include/lldstore/flat_store.h`)

| Lines | What | Why it is written this way |
|---|---|---|
| 17-55 | `FlatHash` | Seed mixed with the length. 8-byte words via `memcpy` (safe for any alignment). Tail zero-padded. `Absorb` = xor, multiply, rotate, multiply; `Finalize` = splitmix64. |
| 60-64 | constants | Default load factor 0.75, clamp range 0.25-0.95, minimum capacity 16, cache line 64. |
| 71-76 | destructor | Frees every entry, then the aligned slot array. |
| 81-97 | `Put` | Hash once. Lazily allocate 16 slots. **One probe** finds either the key or the empty slot where it belongs. An existing key is overwritten without growing. A new key grows first if it would exceed the limit, then probes again for an empty slot (the old index is meaningless after a rehash). |
| 99-104 | `Get` | Early exit when empty. One probe. Returns a view into the entry, which stays valid until this key is overwritten or deleted. |
| 106-122 | `Delete` | Probe. Free the entry. Backward-shift loop (Decision 7). Clear the final hole. |
| 124-128 | `Reserve` | Smallest power of two (at least 16) whose growth limit covers `entries`. Never shrinks. |
| 139-150 | `Entry` | 12-byte header. Key bytes start right after it (`this + 1`) and value bytes right after the key. |
| 152-158 | `Slot` | 16 bytes, checked by `static_assert`, so 4 fit a 64-byte line exactly. |
| 160 | `Home` | Top bits of the hash. |
| 162-167 | `Probe` | Stops at an empty slot or at a slot with an equal hash **and** equal key. The hash comparison comes first, so the entry is followed only when it is almost certainly the key. |
| 169-173 | `ProbeEmpty` | Used only when the key is known to be absent (after growth, and during rehash). Skips the key comparison. |
| 179-192 | `Rehash` | Allocate first (exception safety), update the geometry, re-place slots by their stored hash, free the old array. |
| 194-203 | `Assign` | Overwrite in place if the value fits. Otherwise allocate the new entry first, then free the old one (this order is what keeps aliasing safe). |
| 205-214 | `AllocateSlots` / `FreeSlots` | Aligned `operator new` and the matching aligned `operator delete`. Value-initialised slots (`entry == nullptr` means empty). |
| 216-226 | `MakeEntry` / `FreeEntry` | One allocation sized exactly key + value + header. Placement-new of the header, then two copies. `FreeEntry` returns the whole block in one call. |
| 228-230 | `CopyBytes` | `memmove`, because a value may overlap its own destination. Skipped for empty input (a null `data()` must not reach `memmove`). |
| 232-236 | `CheckLength` | The 4 GiB limit of the 32-bit header fields. |

Read the code next to this table. For each line, ask: "what would break if this were different?" If you cannot answer, that line is your next study topic.

---

## Part 4: Proving it, then measuring it

### 4.1 Tests (`tests/flat_store_test.cpp`, `tests/flat_store_suite_test.cpp`, `tests/flat_sharded_store_test.cpp`)
| Test | What it proves | A bug it would catch |
|---|---|---|
| Your 14 Phase 1 `Store` tests, run unchanged against `FlatStore` (macro trick, as in D-021) | same observable behaviour as the baseline | NUL handling, prefix keys, overwrite sizes, rehash survival |
| `EmptyTableAllocatesNothingAndAnswersLookups` | the empty-table path | a shift by 64 (UB) or a dereference of null slots |
| `GrowsInPowersOfTwoAndNeverExceedsMaxLoadFactor` | growth invariants after every insert | growing too late, so probe loops lose their empty slot |
| `MaxLoadFactorIsClampedAndHonoured` | the knob and its clamp | a load factor of 1.0, which would make probe loops infinite |
| `ReserveAvoidsGrowthDuringInsertion` | `Reserve` | an off-by-one in `LimitFor` |
| `ViewsSurviveInsertsGrowthAndDeletesOfOtherKeys` | **the D-002 contract** | any design that moves entries (ASan would also report a use-after-free) |
| `OverwriteThatFitsReusesTheEntry` | the in-place overwrite (D-023) | needless reallocation |
| `ValueMayAliasAStoredValue` | the `memmove` and allocate-before-free ordering | `memcpy` on overlapping memory, freeing before copying |
| `EveryKeyCollidingAndWrappingAroundStillWorks` (constant hash; home = the last slot) | every key in one cluster that wraps past the end | a wrap-around bug in probing or in backward shift |
| `BackwardShiftKeepsEveryRemainingKeyReachable` (3,000 random layouts with chosen homes, every deletion order) | Algorithm R is correct | a wrong move condition, which leaves a key unreachable |
| `ChurnDoesNotGrowTheTableAndMissesTerminate` | deletes really free slots | tombstone-style leaks, or a miss that never terminates |
| `FlatHashFunction.*` | length and trailing zeros separate keys; benchmark keys spread evenly over the top bits | a hash that ignores the length or uses badly mixed bits |
| `FlatStoreModel.*` with three hashes | 300K random operations against `std::unordered_map`, with **varied keys** (length 0-48, shared prefixes, NUL bytes) | fixes the Phase 1 blind spot (random keys of at most 3 characters, mutation M4) |
| The 18 `ShardedStore` tests against `FlatShardedStore` (ctest prefix `flat.`) | the table works behind the shard worker | integration mistakes |

The adversarial hashers are the important idea. With a good hash, collisions and wrap-around are rare, so a random test may never take those paths. A deliberately terrible hash forces them every time.

**Still to do (yours):** run all of these under debug, ASan, UBSan and TSan on both machines. Then do a mutation check like Phase 1. Suggested mutants:
1. Change `>=` to `>` in the backward-shift condition.
2. Remove the length from the hash seed.
3. Use `memcpy` instead of `memmove`.
4. Allow a load factor of 1.0.
5. Use `hash & mask_` instead of `hash >> shift_` in `Home`.

Each one should fail at least one test. If one survives, the tests have a hole.

### 4.2 Measurements (planned in `docs/BENCHMARKS.md` B-013; nothing run yet)
`bench/table_bench.cpp` compares three contenders:

1. `Store`, the baseline: `unordered_map` with a temporary `std::string` per lookup.
2. **`TransparentStore`, the control:** `unordered_map` with heterogeneous lookup, so no temporary string.
3. `FlatStore`.

**Why the control exists:** `FlatStore` fixes two things at once, the temporary key string (D-003) and the chained layout. Without the control, a speed-up on Linux could come entirely from no longer allocating a `std::string` per `Get` (libstdc++ cannot fit a 16-byte key inline) and would wrongly be credited to "cache-conscious design". The control isolates that effect. It also closes the open D-003 follow-up.

The matrix:
- Get hit, Get miss, Put overwrite at 1K / 100K / 1M keys.
- Insert of 1M new keys (includes growth).
- Delete and reinsert at 100K / 1M keys (exercises backward shift).
- Get **with a copy of the value** for (key, value) = (16, 8), (16, 64), (40, 64) and (16, 512) bytes.
- `FlatStore` at load 25/50/75/90% with a fixed capacity of 2^20, next to `Store` holding the same keys.
- `footprint <keys> flat` against `footprint <keys> map`.
- On Linux, `perf stat -e cache-misses,...` around the Get benchmarks.

**Predictions (to confirm or refute; these are not results):**
1. At 1M keys `FlatStore` beats `Store` on Get hit (about 2 misses instead of 3-4) and wins more on Get miss (about 1 miss instead of 1-2, and no node walk).
2. On Linux, a large part of `Store`'s gap comes from the temporary string, so `TransparentStore` lands between the two. On the Mac (libc++, where a 16-byte key fits inline), `Store` and `TransparentStore` are close.
3. At 1K keys everything fits in cache, so the differences shrink to instruction counts. `FlatHash` against libc++'s `std::hash` may decide it.
4. Load sweep: hits barely change from 25% to 75%. Misses grow clearly at 90% (Knuth: 50 slots, about 13 cache lines).
5. Footprint at 1M keys (estimate):
   - Each entry is a 92-byte allocation (12 + 16 + 64). The slot array at load ~0.48 adds about 34 bytes per entry, so about 126 requested bytes per entry against 149.
   - Resident memory depends on `malloc` size classes (92 rounds to 96 on macOS and to 112 on glibc).
   - 1 allocation per entry against 2.
6. Insert of 1M keys: `FlatStore` is faster on average, but has a few very slow inserts at each doubling. The mean hides them; open-loop latency (Phase 0 tools) would show them.

---

## Part 5: What was deliberately avoided, and when to revisit it

| Avoided | Why not now | Revisit when |
|---|---|---|
| SIMD Swiss-table probing | two instruction sets of code, and no plain baseline to compare against yet | after B-013, as its own experiment |
| Robin Hood probing | the plan schedules it as a follow-up; it needs a baseline to beat | the load sweep shows long miss probes |
| Keys stored inline in slots | breaks density (fewer slots per line) and makes slots variable-size | keys are fixed-size integers |
| Incremental resize | two-table complexity on every operation | open-loop p999 shows resize spikes that matter |
| Shrinking | no workload deletes most of its keys | a churn-heavy workload appears |
| Random seed / keyed hash | benchmarks use our own keys | the store faces untrusted clients (Phase 11) |
| Reusing the routing hash to save hashing twice | correlated bits between shard choice and slot choice (Decision 5), plus coupling | profiling shows hashing dominates |
| Software prefetch of the entry | unmeasured guess | `perf` shows the entry miss dominates and batched lookups exist |
| A custom allocator for entries | that is Phase 6 | Phase 6 |
| Huge pages for the 32 MB slot array | TLB effects not yet measured | Phase 6 (with `perf` dTLB counters) |
| Tombstones | see Decision 7 | never for linear probing; maybe for other schemes |
| `%` for indexing | slower, and it forces nothing useful here | never with power-of-two capacity |
| Changing `Store` itself | Phase 1 numbers must remain reproducible | never; old baselines stay |
| Iterators, `operator[]`, a generic map API | nothing needs them; YAGNI | a snapshot or replication reader (Phase 9) needs iteration |
| Copy and move operations | nobody needs them, and they are easy to get wrong | a caller needs them |
| Tuning the 0.75 default by intuition | that is what the load sweep is for | B-013 numbers exist |

The habit to copy: **every "no" gets a reason and a trigger.** "Not needed" alone is weak. "Not needed until X, and we will know X has happened because of measurement Y" is a design.

---

## Part 6: Where this design could still be wrong (honest risks)

1. **Untested on GCC 11 and libstdc++.** It compiled only with Apple clang, so compiler surprises are possible on Ubuntu (for example `[[no_unique_address]]`, `std::align_val_t` list-initialisation, the `requires` constructors in `BasicShardedStore`).
2. **Hash quality is checked only lightly:** one spread test over 100K similar keys. Clustering with real-world keys is unmeasured. Watch the load-sweep miss numbers.
3. **HashDoS:** the seed is fixed (Decision 8).
4. **Resize pause:** unmeasured. Size it with an open-loop run that crosses a doubling.
5. **The view contract** is now enforced by a test, but `Get` returns a view of memory that the next `Put` to the same key may overwrite in place. The shard worker copies before replying (D-014), so this is safe in the server. A future caller that keeps a view across operations on the same key would be wrong, exactly as with `unordered_map`.
6. **`this + 1` to reach the bytes after the header** is the usual "header followed by data" idiom. It is well defined in practice on every compiler we use, but it is the kind of line a pedantic reviewer will ask about. The fully pedantic alternative is to keep a `char*` to the block and `memcpy` the header fields in and out.
7. **The footprint tool now counts aligned `operator new`.** Before this change it would have missed the slot array entirely. Re-check that the requested-byte totals add up (entries plus slot array).

---

## Part 7: Exercises (do these yourself)

1. **Redo the cost model.** Without looking, count cache misses for a Get hit and a Get miss at 1M keys for `unordered_map` (libc++ and libstdc++) and for `FlatStore`. Then check your answer against the `perf` numbers once B-013 has run.
2. **Change the workload, change the design.** Sketch the table you would build for:
   - (a) 8-byte integer keys and 8-byte values: inline everything, no entries, maybe a sentinel key instead of a null pointer.
   - (b) 1 KB values that are read 99.9% of the time.
   - (c) a table read by a second thread (Phase 9).

   For each one, say which of Decisions 1-12 change and why.
3. **Break it on purpose.** Apply the five mutants from 4.1, one at a time. Record which tests fail. If a mutant survives, write the test that kills it.
4. **Robin Hood.** Implement it as `BasicRobinHoodStore` behind the same interface, reuse all the tests (the macro trick), and measure it against `FlatStore` at 75% and 90% load. Write your prediction first.
5. **Explain Decision 5 to someone else** in under a minute, with a drawing: why the table must not use the low bits of the routing hash.
6. **Write the B-013 interpretation** in your own words once the numbers exist. Say which predictions held, which failed, and why.

---

## A one-page checklist for your next component

- [ ] One-paragraph job and contract, including lifetimes, invalidation, thread-safety and errors
- [ ] Workload in numbers (sizes, mix, counts, threads, hardware)
- [ ] Cost model: misses, allocations and bytes per operation for each candidate
- [ ] Inherited constraints listed (from `DESIGN.md`)
- [ ] At least three candidates, each loser rejected in one sentence
- [ ] Guesses turned into parameters, each with a planned measurement
- [ ] Invariants written down; tests that force rare paths (adversarial inputs); a mutation check
- [ ] Measurement plan with a control for every bundled improvement; predictions written before running
- [ ] "Avoided" list with a reason and a revisit trigger for each item
- [ ] Honest risks section: what is unverified, on which compiler or machine
