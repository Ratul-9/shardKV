#include <benchmark/benchmark.h>

#include "bench_util.h"
#include "lldstore/store.h"

namespace {

using lldstore::Store;
using lldstore::bench::FormatKey;
using lldstore::bench::kKeySize;
using lldstore::bench::Prefill;
using lldstore::bench::Value;
using lldstore::bench::XorShift64;

// Working-set sizes: 1K fits in L1/L2, 100K roughly fits the M5's L2/SLC, 1M is memory-bound.
// Arg = number of keys in the store.
#define STORE_SIZES ->Arg(1'000)->Arg(100'000)->Arg(1'000'000)

// Cost of generating a key and drawing a random index, with no store involved. Subtract this from
// the numbers below to see the store's own cost.
void BM_KeyGenOnly(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  XorShift64 rng(42);
  char buf[kKeySize];
  for (auto _ : state) benchmark::DoNotOptimize(FormatKey(rng.Next() % n, buf));
}
BENCHMARK(BM_KeyGenOnly) STORE_SIZES;

void BM_GetHit(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  Store store;
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[kKeySize];
  for (auto _ : state) {
    auto r = store.Get(FormatKey(rng.Next() % n, buf));
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_GetHit) STORE_SIZES;

// Keys n..2n-1 are never inserted, so every lookup misses.
void BM_GetMiss(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  Store store;
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[kKeySize];
  for (auto _ : state) {
    auto r = store.Get(FormatKey(n + rng.Next() % n, buf));
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_GetMiss) STORE_SIZES;

void BM_PutOverwrite(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  Store store;
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[kKeySize];
  for (auto _ : state) {
    store.Put(FormatKey(rng.Next() % n, buf), Value());
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_PutOverwrite) STORE_SIZES;

// Inserts brand-new keys into an empty store, so growth and rehashing are included.
// Fixed iteration count keeps memory use bounded.
void BM_PutInsert(benchmark::State& state) {
  Store store;
  uint64_t i = 0;
  char buf[kKeySize];
  for (auto _ : state) {
    store.Put(FormatKey(i++, buf), Value());
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_PutInsert)->Iterations(1'000'000);

}  // namespace
