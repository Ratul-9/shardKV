#include "lldstore/locked_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lldstore {
namespace {

// Typed over both lock strategies: the same tests must pass for MutexStore and SharedMutexStore.
//
// The tests are DISABLED_ while the stubs in locked_store.h are unimplemented. When you have
// implemented the stores, remove the DISABLED_ prefixes, then run these under the debug build and
// ALSO under the tsan preset (a data race may not change a test's result, but TSan will report it).
template <typename S>
class LockedStoreTest : public ::testing::Test {};
using StoreTypes = ::testing::Types<MutexStore, SharedMutexStore>;
TYPED_TEST_SUITE(LockedStoreTest, StoreTypes);

TYPED_TEST(LockedStoreTest, DISABLED_SingleThreadedBasics) {
  TypeParam store;
  std::string out;

  EXPECT_FALSE(store.Get("a", &out));
  store.Put("a", "1");
  ASSERT_TRUE(store.Get("a", &out));
  EXPECT_EQ(out, "1");
  store.Put("a", "22");
  ASSERT_TRUE(store.Get("a", &out));
  EXPECT_EQ(out, "22");
  EXPECT_EQ(store.Size(), 1u);
  EXPECT_TRUE(store.Delete("a"));
  EXPECT_FALSE(store.Delete("a"));
  EXPECT_FALSE(store.Get("a", &out));
  EXPECT_EQ(store.Size(), 0u);
}

// Each thread owns a disjoint key range and checks every operation against its own reference
// model, so any lost or corrupted update is detected exactly.
TYPED_TEST(LockedStoreTest, DISABLED_ConcurrentDisjointKeysMatchPerThreadModel) {
  constexpr int kThreads = 8;
  constexpr int kSteps = 20000;
  constexpr uint64_t kKeysPerThread = 30;

  TypeParam store;
  std::atomic<int> failures{0};
  std::vector<size_t> final_sizes(kThreads, 0);
  std::latch go(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + static_cast<uint64_t>(t));
      std::unordered_map<std::string, std::string> model;
      std::string out;
      go.arrive_and_wait();
      for (int step = 0; step < kSteps; ++step) {
        const std::string key = "t" + std::to_string(t) + ":k" + std::to_string(rng() % kKeysPerThread);
        switch (rng() % 3) {
          case 0: {
            const std::string value = std::string(rng() % 100, static_cast<char>('a' + t)) + std::to_string(step);
            store.Put(key, value);
            model[key] = value;
            break;
          }
          case 1: {
            const bool got = store.Get(key, &out);
            const auto it = model.find(key);
            if (got != (it != model.end()) || (got && out != it->second)) ++failures;
            break;
          }
          default:
            if (store.Delete(key) != (model.erase(key) == 1)) ++failures;
            break;
        }
      }
      final_sizes[static_cast<size_t>(t)] = model.size();
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(failures.load(), 0);
  size_t expected = 0;
  for (size_t s : final_sizes) expected += s;
  EXPECT_EQ(store.Size(), expected);
}

// All threads fight over the same few keys. Every value written is `key + ":" + run of 'x'`, so a
// torn or half-written value (a read racing a write) is detectable even without a model.
TYPED_TEST(LockedStoreTest, DISABLED_SharedKeysNeverReturnTornValues) {
  constexpr int kThreads = 8;
  constexpr int kSteps = 30000;
  constexpr uint64_t kKeys = 20;

  TypeParam store;
  std::atomic<int> violations{0};
  std::latch go(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(5000 + static_cast<uint64_t>(t));
      std::string out;
      go.arrive_and_wait();
      for (int step = 0; step < kSteps; ++step) {
        const std::string key = "shared" + std::to_string(rng() % kKeys);
        switch (rng() % 4) {
          case 0:
            store.Put(key, key + ":" + std::string(rng() % 200, 'x'));
            break;
          case 1:
            store.Delete(key);
            break;
          default:
            if (store.Get(key, &out)) {
              const std::string prefix = key + ":";
              bool ok = out.compare(0, prefix.size(), prefix) == 0;
              for (size_t i = prefix.size(); ok && i < out.size(); ++i) ok = out[i] == 'x';
              if (!ok) ++violations;
            }
            break;
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(violations.load(), 0);
}

}  // namespace
}  // namespace lldstore
