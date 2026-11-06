#include "lldstore/sharded_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace lldstore {
namespace {

ShardedStoreOptions Opts(size_t shards, size_t queue_capacity = 1024) {
  ShardedStoreOptions options;
  options.num_shards = shards;
  options.queue_capacity = queue_capacity;
  return options;
}

std::string MakeKey(uint64_t i) {
  const std::string digits = std::to_string(i);
  return "key:" + std::string(12 - digits.size(), '0') + digits;
}

// ---- Routing: these need no running store ---------------------------------------------------

// The hash must give the SAME answer on every platform and compiler, or the same key would go to
// different shards on the Mac and the Linux machine. The expected values were computed
// independently (a separate implementation of FNV-1a + splitmix64) and must never change.
TEST(ShardedStoreRouting, HashKeyIsPortableAndStable) {
  EXPECT_EQ(ShardedStore::HashKey(""), 0xf52a15e9a9b5e89bULL);
  EXPECT_EQ(ShardedStore::HashKey("a"), 0x02c0bdbf481420f8ULL);
  EXPECT_EQ(ShardedStore::HashKey("abc"), 0x0dd490490804b508ULL);
  EXPECT_EQ(ShardedStore::HashKey("hello world"), 0x05cb585112be1151ULL);
  EXPECT_EQ(ShardedStore::HashKey("key:000000000042"), 0xb093d503447d29ffULL);
}

TEST(ShardedStoreRouting, ShardForIsDeterministicAndInRange) {
  for (size_t n : {1u, 2u, 3u, 6u, 7u, 12u}) {
    for (uint64_t i = 0; i < 1000; ++i) {
      const std::string key = MakeKey(i);
      const size_t shard = ShardedStore::ShardFor(key, n);
      EXPECT_LT(shard, n);
      EXPECT_EQ(shard, ShardedStore::ShardFor(key, n));
    }
  }
  EXPECT_EQ(ShardedStore::ShardFor("anything", 1), 0u);
}

TEST(ShardedStoreRouting, KnownKeysLandOnKnownShards) {
  // hash % 6 for these keys, from the same independent computation as the test above.
  EXPECT_EQ(ShardedStore::ShardFor("", 6), 3u);
  EXPECT_EQ(ShardedStore::ShardFor("a", 6), 4u);
  EXPECT_EQ(ShardedStore::ShardFor("key:000000000042", 6), 5u);
  EXPECT_EQ(ShardedStore::ShardFor("key:000000000042", 4), 3u);
}

TEST(ShardedStoreRouting, KeysSpreadEvenlyAcrossShards) {
  for (size_t n : {4u, 6u}) {
    std::vector<size_t> counts(n, 0);
    for (uint64_t i = 0; i < 100000; ++i) ++counts[ShardedStore::ShardFor(MakeKey(i), n)];
    const auto [lo, hi] = std::minmax_element(counts.begin(), counts.end());
    ASSERT_GT(*lo, 0u);
    // Expected spread is about 2% (measured independently: 1.019 for 6 shards, 1.017 for 4).
    EXPECT_LT(static_cast<double>(*hi) / static_cast<double>(*lo), 1.1) << "shards=" << n;
  }
}

TEST(ShardedStoreRouting, ShardIndexOfAStoreMatchesShardFor) {
  ShardedStore store(Opts(6));
  EXPECT_EQ(store.num_shards(), 6u);
  for (uint64_t i = 0; i < 200; ++i) {
    const std::string key = MakeKey(i);
    EXPECT_EQ(store.ShardIndex(key), ShardedStore::ShardFor(key, 6));
  }
}

// ---- Behaviour ------------------------------------------------------------------------------

TEST(ShardedStore, ZeroShardsIsClampedToOne) {
  ShardedStore store(Opts(0));
  EXPECT_EQ(store.num_shards(), 1u);
  store.Put("k", "v");
  std::string out;
  EXPECT_TRUE(store.Get("k", &out));
  EXPECT_EQ(out, "v");
}

TEST(ShardedStore, BasicsAcrossManyShards) {
  ShardedStore store(Opts(4));
  std::string out;
  EXPECT_FALSE(store.Get("missing", &out));
  EXPECT_FALSE(store.Delete("missing"));
  EXPECT_EQ(store.Size(), 0u);

  for (uint64_t i = 0; i < 1000; ++i) store.Put(MakeKey(i), "v" + std::to_string(i));
  EXPECT_EQ(store.Size(), 1000u);
  for (uint64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(store.Get(MakeKey(i), &out)) << i;
    ASSERT_EQ(out, "v" + std::to_string(i)) << i;
  }

  store.Put(MakeKey(7), "changed");  // overwrite does not change Size
  EXPECT_EQ(store.Size(), 1000u);
  ASSERT_TRUE(store.Get(MakeKey(7), &out));
  EXPECT_EQ(out, "changed");

  for (uint64_t i = 0; i < 1000; i += 2) EXPECT_TRUE(store.Delete(MakeKey(i)));
  EXPECT_EQ(store.Size(), 500u);
  EXPECT_FALSE(store.Delete(MakeKey(0)));  // already gone
  EXPECT_FALSE(store.Get(MakeKey(0), &out));
  EXPECT_TRUE(store.Get(MakeKey(1), &out));
}

TEST(ShardedStore, EmptyKeyAndBinaryKeysWork) {
  ShardedStore store(Opts(3));
  std::string out;
  store.Put("", "empty-key");
  store.Put(std::string_view("a\0b", 3), "three");
  store.Put(std::string_view("a", 1), "one");
  store.Put(std::string_view("a\0", 2), "two");
  EXPECT_EQ(store.Size(), 4u);
  ASSERT_TRUE(store.Get("", &out));
  EXPECT_EQ(out, "empty-key");
  ASSERT_TRUE(store.Get(std::string_view("a\0b", 3), &out));
  EXPECT_EQ(out, "three");
  ASSERT_TRUE(store.Get(std::string_view("a\0", 2), &out));
  EXPECT_EQ(out, "two");
}

TEST(ShardedStore, LargeAndEmptyValues) {
  ShardedStore store(Opts(2));
  std::string out;
  const std::string big(1'000'000, 'x');
  store.Put("big", big);
  store.Put("empty", "");
  ASSERT_TRUE(store.Get("big", &out));
  EXPECT_EQ(out, big);
  ASSERT_TRUE(store.Get("empty", &out));
  EXPECT_EQ(out, "");
}

TEST(ShardedStore, ShardsReallyHoldDifferentKeys) {
  // With 6 shards and 600 keys, no shard may hold all of them: if routing were broken (everything
  // to shard 0) the totals would still add up, so check that the load is actually spread. A shard's
  // contents are private, so measure with Size() on stores that route to ONE shard each.
  ShardedStore six(Opts(6));
  for (uint64_t i = 0; i < 600; ++i) six.Put(MakeKey(i), "v");
  EXPECT_EQ(six.Size(), 600u);
  size_t keys_on_shard0 = 0;
  for (uint64_t i = 0; i < 600; ++i) {
    if (six.ShardIndex(MakeKey(i)) == 0) ++keys_on_shard0;
  }
  EXPECT_GT(keys_on_shard0, 50u);   // about 100 expected
  EXPECT_LT(keys_on_shard0, 150u);
}

TEST(ShardedStore, IdleStoresCreateAndDestroyPromptly) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) ShardedStore store(Opts(6));  // a lost wakeup would hang here
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
}

// ---- Concurrency (also run under the tsan preset: a data race may not change a result) ------

// Each thread owns a disjoint key range and checks every operation against its own model.
TEST(ShardedStoreConcurrency, DisjointKeysMatchPerThreadModel) {
  constexpr int kThreads = 6;
  constexpr int kSteps = 3000;
  constexpr uint64_t kKeysPerThread = 30;

  ShardedStore store(Opts(4, 64));
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

// All threads fight over the same few keys, which land on several shards. Every value written is
// `key + ":" + a run of 'x'`, so a torn or mixed-up value is detectable without a model.
TEST(ShardedStoreConcurrency, SharedKeysNeverReturnTornValues) {
  constexpr int kThreads = 6;
  constexpr int kSteps = 4000;
  constexpr uint64_t kKeys = 20;

  ShardedStore store(Opts(4, 64));
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

// Queue capacity 1 forces every producer to block constantly: the backpressure path must neither
// deadlock nor lose a request.
TEST(ShardedStoreConcurrency, TinyQueuesApplyBackpressureWithoutDeadlockOrLoss) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 1500;
  ShardedStore store(Opts(2, 1));
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        store.Put("t" + std::to_string(t) + ":" + std::to_string(i), "v");
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(store.Size(), static_cast<size_t>(kThreads) * kPerThread);
}

TEST(ShardedStoreConcurrency, SizeWhileOthersWriteStaysWithinBounds) {
  constexpr int kWrites = 4000;
  ShardedStore store(Opts(4, 64));
  std::atomic<bool> done{false};
  size_t last = 0;
  bool monotone = true;
  std::thread writer([&] {
    for (int i = 0; i < kWrites; ++i) store.Put("k" + std::to_string(i), "v");  // only inserts
    done = true;
  });
  while (!done.load()) {
    const size_t size = store.Size();
    if (size < last) monotone = false;  // inserts only: the total can never shrink
    last = size;
    EXPECT_LE(size, static_cast<size_t>(kWrites));
  }
  writer.join();
  EXPECT_TRUE(monotone);
  EXPECT_EQ(store.Size(), static_cast<size_t>(kWrites));
}

// ---- Pinning --------------------------------------------------------------------------------

TEST(ShardedStorePinning, UnpinnedByDefaultReportsMinusOne) {
  ShardedStore store(Opts(3));
  EXPECT_EQ(store.PinnedCpus(), (std::vector<int>{-1, -1, -1}));
}

TEST(ShardedStorePinning, AnImpossibleCpuIsReportedAndTheStoreStillWorks) {
  ShardedStoreOptions options = Opts(2);
  options.pin_cpus = {100000};  // no machine has this CPU
  ShardedStore store(options);
  EXPECT_EQ(store.PinnedCpus(), (std::vector<int>{-1, -1}));
  store.Put("k", "v");
  std::string out;
  EXPECT_TRUE(store.Get("k", &out));
}

TEST(ShardedStorePinning, ShardsRunOnTheRequestedCpus) {
#if defined(__linux__)
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE && cpus.size() < 3; ++c) {
    if (CPU_ISSET(c, &allowed)) cpus.push_back(c);
  }
  if (cpus.size() < 2) GTEST_SKIP() << "need at least two usable CPUs";
  ShardedStoreOptions options = Opts(cpus.size());
  options.pin_cpus = cpus;
  ShardedStore store(options);
  EXPECT_EQ(store.PinnedCpus(), cpus);  // shard i reported running on cpus[i] after pinning
#else
  GTEST_SKIP() << "thread pinning is only implemented on Linux";
#endif
}

}  // namespace
}  // namespace lldstore
