// Phase 6 "done when": a test that PROVES the steady-state request path allocates nothing.
//
// This file replaces the global operator new/delete with counting versions, so it must be its own
// executable. The count is process-wide: it includes the shard worker threads, so an allocation
// anywhere on the request path (queue node, entry, reply) is caught, not only in the caller.
//
// Sanitizer builds replace operator new themselves; replacing it again would fight their
// bookkeeping, so under a sanitizer the counters are not installed and the tests skip.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/sharded_store.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
#define LLDSTORE_ANY_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LLDSTORE_ANY_SANITIZER 1
#endif

namespace {
std::atomic<uint64_t> g_allocations{0};
}

#if !defined(LLDSTORE_ANY_SANITIZER)
void* operator new(size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void* operator new(size_t n, std::align_val_t al) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  const size_t a = static_cast<size_t>(al);
  if (void* p = std::aligned_alloc(a, (n + a - 1) / a * a)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t al) { return ::operator new(n, al); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { std::free(p); }
#endif

namespace lldstore {
namespace {

constexpr int kKeys = 2000;

std::string Key(int i) { return "key:" + std::to_string(1000000 + i); }

template <typename S>
uint64_t AllocationsDuringSteadyState(S& store, int threads, int ops_per_thread) {
  // Warm-up: create every key and let every size class, queue and thread reach steady state.
  for (int i = 0; i < kKeys; ++i) store.Put(Key(i), std::string(64, 'w'));
  std::vector<std::vector<std::string>> keys(static_cast<size_t>(threads));
  for (auto& k : keys)
    for (int i = 0; i < kKeys; ++i) k.push_back(Key(i));  // allocated BEFORE measuring
  const std::string value(64, 'v');
  std::atomic<uint64_t> before{0};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::string out;
      out.reserve(128);  // the reply buffer is reused, as in contention.cpp
      const auto& my = keys[static_cast<size_t>(t)];
      ++ready;
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < ops_per_thread; ++i) {
        const std::string& k = my[static_cast<size_t>((i * 7 + t) % kKeys)];
        switch (i % 4) {
          case 0: store.Put(k, value); break;
          case 1: (void)store.Get(k, &out); break;
          case 2: (void)store.Delete(k); break;  // frees an entry into the slab ...
          default: store.Put(k, value); break;   // ... and this takes it back
        }
      }
    });
  }
  while (ready.load() != threads) std::this_thread::yield();
  before = g_allocations.load();
  go = true;
  for (auto& w : workers) w.join();
  return g_allocations.load() - before.load();
}

#if defined(LLDSTORE_ANY_SANITIZER)
#define SKIP_UNDER_SANITIZER() GTEST_SKIP() << "operator new is owned by the sanitizer"
#else
#define SKIP_UNDER_SANITIZER() (void)0
#endif

TEST(ZeroAllocation, CountingOperatorNewIsInstalled) {
  SKIP_UNDER_SANITIZER();
  const uint64_t before = g_allocations.load();
  auto* p = new int(1);
  delete p;
  EXPECT_EQ(g_allocations.load() - before, 1u);  // the counter itself works
}

TEST(ZeroAllocation, SlabShardedStoreRequestPathAllocatesNothing) {
  SKIP_UNDER_SANITIZER();
  ShardedStoreOptions options;
  options.num_shards = 4;
  SlabShardedStore store(options);
  EXPECT_EQ(AllocationsDuringSteadyState(store, 4, 20000), 0u);
}

// The control: the Phase 5 store (node-based MPSC queue) allocates a queue node on every request.
// If this ever reports zero, the counter is broken, not the store fixed.
TEST(ZeroAllocation, FlatShardedStoreAllocatesPerRequest) {
  SKIP_UNDER_SANITIZER();
  ShardedStoreOptions options;
  options.num_shards = 4;
  FlatShardedStore store(options);
  EXPECT_GE(AllocationsDuringSteadyState(store, 4, 20000), 4u * 20000u);
}

}  // namespace
}  // namespace lldstore
