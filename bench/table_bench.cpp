#include <benchmark/benchmark.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bench_util.h"
#include "lldstore/flat_store.h"
#include "lldstore/store.h"

namespace {

using lldstore::FlatStore;
using lldstore::Store;
using lldstore::bench::FormatKey;
using lldstore::bench::Prefill;
using lldstore::bench::Value;
using lldstore::bench::XorShift64;

class TransparentStore {
 public:
  void Put(std::string_view key, std::string_view value) {
    const auto it = data_.find(key);
    if (it != data_.end()) {
      it->second.assign(value.data(), value.size());
    } else {
      data_.emplace(key, value);
    }
  }

  std::optional<std::string_view> Get(std::string_view key) const {
    const auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return std::string_view(it->second);
  }

  bool Delete(std::string_view key) {
    const auto it = data_.find(key);
    if (it == data_.end()) return false;
    data_.erase(it);
    return true;
  }

  size_t Size() const { return data_.size(); }

 private:
  struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const noexcept {
      return std::hash<std::string_view>{}(key);
    }
  };

  std::unordered_map<std::string, std::string, Hash, std::equal_to<>> data_;
};

template <typename S, size_t K, size_t V>
void BM_GetHit(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  S store;
  Prefill<K, V>(store, n);
  XorShift64 rng(42);
  char buf[K];
  for (auto _ : state) {
    auto r = store.Get(FormatKey(rng.Next() % n, buf));
    benchmark::DoNotOptimize(r);
  }
}

template <typename S, size_t K, size_t V>
void BM_GetMiss(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  S store;
  Prefill<K, V>(store, n);
  XorShift64 rng(42);
  char buf[K];
  for (auto _ : state) {
    auto r = store.Get(FormatKey(n + rng.Next() % n, buf));
    benchmark::DoNotOptimize(r);
  }
}

template <typename S, size_t K, size_t V>
void BM_GetCopy(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  S store;
  Prefill<K, V>(store, n);
  XorShift64 rng(42);
  char buf[K];
  std::string out;
  for (auto _ : state) {
    const auto r = store.Get(FormatKey(rng.Next() % n, buf));
    if (r) out.assign(r->data(), r->size());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
}

template <typename S, size_t K, size_t V>
void BM_PutOverwrite(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  S store;
  Prefill<K, V>(store, n);
  XorShift64 rng(42);
  char buf[K];
  for (auto _ : state) {
    store.Put(FormatKey(rng.Next() % n, buf), Value<V>());
    benchmark::ClobberMemory();
  }
}

template <typename S>
void BM_PutInsert(benchmark::State& state) {
  S store;
  uint64_t i = 0;
  char buf[lldstore::bench::kKeySize];
  for (auto _ : state) {
    store.Put(FormatKey(i++, buf), Value());
    benchmark::ClobberMemory();
  }
}

template <typename S>
void BM_DeleteReinsert(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  S store;
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[lldstore::bench::kKeySize];
  for (auto _ : state) {
    const auto key = FormatKey(rng.Next() % n, buf);
    benchmark::DoNotOptimize(store.Delete(key));
    store.Put(key, Value());
    benchmark::ClobberMemory();
  }
}

constexpr uint64_t kFixedCapacity = uint64_t{1} << 20;

template <bool kHit>
void BM_FlatAtLoad(benchmark::State& state) {
  const auto n = kFixedCapacity * static_cast<uint64_t>(state.range(0)) / 100;
  FlatStore store(FlatStore::kMaxMaxLoadFactor);
  store.Reserve(static_cast<size_t>(kFixedCapacity * 9 / 10));
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[lldstore::bench::kKeySize];
  for (auto _ : state) {
    const uint64_t i = kHit ? rng.Next() % n : n + rng.Next() % n;
    auto r = store.Get(FormatKey(i, buf));
    benchmark::DoNotOptimize(r);
  }
  state.counters["capacity"] = static_cast<double>(store.Capacity());
  state.counters["load"] = store.LoadFactor();
}

template <bool kHit>
void BM_MapAtSameKeys(benchmark::State& state) {
  const auto n = kFixedCapacity * static_cast<uint64_t>(state.range(0)) / 100;
  Store store;
  Prefill(store, n);
  XorShift64 rng(42);
  char buf[lldstore::bench::kKeySize];
  for (auto _ : state) {
    const uint64_t i = kHit ? rng.Next() % n : n + rng.Next() % n;
    auto r = store.Get(FormatKey(i, buf));
    benchmark::DoNotOptimize(r);
  }
}

#define SIZES ->Arg(1'000)->Arg(100'000)->Arg(1'000'000)
#define LOADS ->Arg(25)->Arg(50)->Arg(75)->Arg(90)

BENCHMARK_TEMPLATE(BM_GetHit, Store, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_GetHit, TransparentStore, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_GetHit, FlatStore, 16, 64) SIZES;

BENCHMARK_TEMPLATE(BM_GetMiss, Store, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_GetMiss, TransparentStore, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_GetMiss, FlatStore, 16, 64) SIZES;

BENCHMARK_TEMPLATE(BM_PutOverwrite, Store, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_PutOverwrite, TransparentStore, 16, 64) SIZES;
BENCHMARK_TEMPLATE(BM_PutOverwrite, FlatStore, 16, 64) SIZES;

BENCHMARK_TEMPLATE(BM_PutInsert, Store)->Iterations(1'000'000);
BENCHMARK_TEMPLATE(BM_PutInsert, TransparentStore)->Iterations(1'000'000);
BENCHMARK_TEMPLATE(BM_PutInsert, FlatStore)->Iterations(1'000'000);

BENCHMARK_TEMPLATE(BM_DeleteReinsert, Store)->Arg(100'000)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_DeleteReinsert, TransparentStore)->Arg(100'000)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_DeleteReinsert, FlatStore)->Arg(100'000)->Arg(1'000'000);

BENCHMARK_TEMPLATE(BM_GetCopy, Store, 16, 8)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, TransparentStore, 16, 8)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, FlatStore, 16, 8)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, Store, 16, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, TransparentStore, 16, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, FlatStore, 16, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, Store, 40, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, TransparentStore, 40, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, FlatStore, 40, 64)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, Store, 16, 512)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, TransparentStore, 16, 512)->Arg(1'000'000);
BENCHMARK_TEMPLATE(BM_GetCopy, FlatStore, 16, 512)->Arg(1'000'000);

BENCHMARK_TEMPLATE(BM_FlatAtLoad, true) LOADS;
BENCHMARK_TEMPLATE(BM_FlatAtLoad, false) LOADS;
BENCHMARK_TEMPLATE(BM_MapAtSameKeys, true) LOADS;
BENCHMARK_TEMPLATE(BM_MapAtSameKeys, false) LOADS;

}
