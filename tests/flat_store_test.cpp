#include "lldstore/flat_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lldstore {
namespace {

struct ConstantHash {
  uint64_t operator()(std::string_view) const noexcept { return ~uint64_t{0}; }
};

struct FirstByteHash {
  uint64_t operator()(std::string_view key) const noexcept {
    return key.empty() ? 0 : uint64_t{static_cast<unsigned char>(key[0])} << 56;
  }
};

struct HomeHash {
  uint64_t operator()(std::string_view key) const noexcept {
    return (uint64_t{static_cast<unsigned char>(key[0])} << 60) |
           uint64_t{static_cast<unsigned char>(key[1])};
  }
};

std::string BenchKey(uint64_t i) {
  const std::string digits = std::to_string(i);
  return "key:" + std::string(12 - digits.size(), '0') + digits;
}

TEST(FlatStoreTable, EmptyTableAllocatesNothingAndAnswersLookups) {
  FlatStore store;
  EXPECT_EQ(store.Capacity(), 0u);
  EXPECT_EQ(store.LoadFactor(), 0.0);
  EXPECT_FALSE(store.Get("missing").has_value());
  EXPECT_FALSE(store.Delete("missing"));
  EXPECT_EQ(store.Size(), 0u);
}

TEST(FlatStoreTable, GrowsInPowersOfTwoAndNeverExceedsMaxLoadFactor) {
  FlatStore store;
  size_t growths = 0;
  size_t last_capacity = 0;
  for (uint64_t i = 0; i < 20'000; ++i) {
    store.Put(BenchKey(i), "v");
    ASSERT_TRUE(std::has_single_bit(store.Capacity())) << "i=" << i;
    ASSERT_GE(store.Capacity(), FlatStore::kMinCapacity);
    ASSERT_LE(store.LoadFactor(), store.MaxLoadFactor()) << "i=" << i;
    if (store.Capacity() != last_capacity) {
      ++growths;
      last_capacity = store.Capacity();
    }
  }
  EXPECT_EQ(store.Size(), 20'000u);
  EXPECT_GT(growths, 5u);
}

TEST(FlatStoreTable, MaxLoadFactorIsClampedAndHonoured) {
  EXPECT_EQ(FlatStore(0.0).MaxLoadFactor(), FlatStore::kMinMaxLoadFactor);
  EXPECT_EQ(FlatStore(2.0).MaxLoadFactor(), FlatStore::kMaxMaxLoadFactor);

  FlatStore half(0.5);
  for (uint64_t i = 0; i < 5'000; ++i) {
    half.Put(BenchKey(i), "v");
    ASSERT_LE(half.LoadFactor(), 0.5) << "i=" << i;
  }
}

TEST(FlatStoreTable, ReserveAvoidsGrowthDuringInsertion) {
  FlatStore store;
  store.Reserve(10'000);
  const size_t capacity = store.Capacity();
  ASSERT_GE(static_cast<double>(capacity) * store.MaxLoadFactor(), 10'000.0);
  for (uint64_t i = 0; i < 10'000; ++i) store.Put(BenchKey(i), "v");
  EXPECT_EQ(store.Capacity(), capacity);

  store.Reserve(10);
  EXPECT_EQ(store.Capacity(), capacity);
}

TEST(FlatStoreTable, ViewsSurviveInsertsGrowthAndDeletesOfOtherKeys) {
  FlatStore store;
  const std::string expected(100, 'a');
  store.Put("anchor", expected);
  const std::string_view view = *store.Get("anchor");
  const size_t capacity_before = store.Capacity();

  for (uint64_t i = 0; i < 100'000; ++i) store.Put(BenchKey(i), "value");
  ASSERT_GT(store.Capacity(), capacity_before);
  EXPECT_EQ(view, expected);

  for (uint64_t i = 0; i < 100'000; i += 2) ASSERT_TRUE(store.Delete(BenchKey(i)));
  EXPECT_EQ(view, expected);
  EXPECT_EQ(store.Get("anchor")->data(), view.data());
}

TEST(FlatStoreTable, OverwriteThatFitsReusesTheEntry) {
  FlatStore store;
  store.Put("k", std::string(64, 'x'));
  const char* before = store.Get("k")->data();

  store.Put("k", "short");
  EXPECT_EQ(store.Get("k")->data(), before);
  EXPECT_EQ(*store.Get("k"), "short");

  store.Put("k", std::string(64, 'y'));
  EXPECT_EQ(store.Get("k")->data(), before);
  EXPECT_EQ(*store.Get("k"), std::string(64, 'y'));

  store.Put("k", std::string(65, 'z'));
  EXPECT_EQ(*store.Get("k"), std::string(65, 'z'));
  EXPECT_EQ(store.Size(), 1u);
}

TEST(FlatStoreTable, ValueMayAliasAStoredValue) {
  FlatStore store;
  store.Put("k", "hello world");

  store.Put("k", *store.Get("k"));
  EXPECT_EQ(*store.Get("k"), "hello world");

  store.Put("k", store.Get("k")->substr(6));
  EXPECT_EQ(*store.Get("k"), "world");

  store.Put("other", *store.Get("k"));
  EXPECT_EQ(*store.Get("other"), "world");

  store.Put("k", std::string(500, 'b'));
  store.Put("k", store.Get("k")->substr(1, 3));
  EXPECT_EQ(*store.Get("k"), "bbb");
}

TEST(FlatStoreTable, EveryKeyCollidingAndWrappingAroundStillWorks) {
  BasicFlatStore<ConstantHash> store;
  constexpr int kKeys = 300;
  for (int i = 0; i < kKeys; ++i) store.Put("k" + std::to_string(i), std::to_string(i));
  ASSERT_EQ(store.Size(), static_cast<size_t>(kKeys));
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_EQ(*store.Get("k" + std::to_string(i)), std::to_string(i)) << "i=" << i;
  }

  for (int i = 1; i < kKeys; i += 2) ASSERT_TRUE(store.Delete("k" + std::to_string(i)));
  for (int i = 0; i < kKeys; ++i) {
    EXPECT_EQ(store.Get("k" + std::to_string(i)).has_value(), i % 2 == 0) << "i=" << i;
  }

  for (int i = 1; i < kKeys; i += 2) store.Put("k" + std::to_string(i), "again");
  EXPECT_EQ(store.Size(), static_cast<size_t>(kKeys));
  EXPECT_EQ(*store.Get("k299"), "again");
  EXPECT_EQ(*store.Get("k298"), "298");
}

TEST(FlatStoreTable, BackwardShiftKeepsEveryRemainingKeyReachable) {
  std::mt19937_64 rng(2026);
  for (int round = 0; round < 3'000; ++round) {
    const size_t count = 1 + rng() % 12;
    std::vector<std::string> keys;
    for (size_t id = 0; id < count; ++id) {
      keys.push_back({static_cast<char>(rng() % 16), static_cast<char>(id)});
    }
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);

    BasicFlatStore<HomeHash> store;
    for (const auto& key : keys) store.Put(key, key);
    ASSERT_EQ(store.Capacity(), 16u);

    std::vector<bool> present(count, true);
    for (size_t victim : order) {
      ASSERT_TRUE(store.Delete(keys[victim])) << "round=" << round;
      present[victim] = false;
      for (size_t i = 0; i < count; ++i) {
        const auto got = store.Get(keys[i]);
        ASSERT_EQ(got.has_value(), present[i]) << "round=" << round << " key=" << i;
        if (got.has_value()) {
          ASSERT_EQ(*got, keys[i]) << "round=" << round;
        }
      }
    }
    ASSERT_EQ(store.Size(), 0u);
  }
}

TEST(FlatStoreTable, ChurnDoesNotGrowTheTableAndMissesTerminate) {
  FlatStore store;
  constexpr uint64_t kLive = 1'000;
  store.Reserve(kLive);
  const size_t capacity = store.Capacity();
  for (uint64_t i = 0; i < 200'000; ++i) {
    store.Put(BenchKey(i), "v");
    if (i >= kLive) {
      ASSERT_TRUE(store.Delete(BenchKey(i - kLive))) << "i=" << i;
    }
  }
  EXPECT_EQ(store.Size(), kLive);
  EXPECT_EQ(store.Capacity(), capacity);
  EXPECT_FALSE(store.Get("never inserted").has_value());
  EXPECT_FALSE(store.Delete("never inserted"));
}

TEST(FlatHashFunction, SeparatesKeysThatDifferOnlyInLengthOrTrailingZeros) {
  const FlatHash hash;
  using namespace std::string_view_literals;
  EXPECT_NE(hash(""sv), hash("\0"sv));
  EXPECT_NE(hash("a"sv), hash("a\0"sv));
  EXPECT_NE(hash("aaaaaaaa"sv), hash("aaaaaaaa\0"sv));
  EXPECT_NE(hash("aaaaaaaa\0"sv), hash("aaaaaaaa\0\0"sv));
  EXPECT_NE(hash("key:000000000001"sv), hash("key:000000000002"sv));
}

TEST(FlatHashFunction, SpreadsBenchmarkKeysEvenlyOverTheHighBits) {
  const FlatHash hash;
  constexpr size_t kBuckets = 64;
  std::vector<size_t> counts(kBuckets, 0);
  for (uint64_t i = 0; i < 100'000; ++i) ++counts[hash(BenchKey(i)) >> 58];
  const auto [lo, hi] = std::minmax_element(counts.begin(), counts.end());
  EXPECT_LT(static_cast<double>(*hi) / static_cast<double>(*lo), 1.3);
}

std::vector<std::string> MakeVariedKeys(std::mt19937_64& rng, size_t count) {
  const std::string prefixes[] = {"", "user:", "user:000000000000", std::string("\0\0", 2)};
  std::vector<std::string> keys;
  std::unordered_map<std::string, bool> seen;
  while (keys.size() < count) {
    std::string key = prefixes[rng() % 4];
    const size_t extra = rng() % 33;
    for (size_t i = 0; i < extra; ++i) key.push_back(static_cast<char>(rng() % 4 == 0 ? 0 : rng() % 256));
    if (seen.emplace(key, true).second) keys.push_back(std::move(key));
  }
  return keys;
}

template <typename Hasher>
void RunModelTest(uint64_t seed, int steps, size_t key_count) {
  SCOPED_TRACE("seed=" + std::to_string(seed));
  std::mt19937_64 rng(seed);
  const std::vector<std::string> keys = MakeVariedKeys(rng, key_count);
  BasicFlatStore<Hasher> store;
  std::unordered_map<std::string, std::string> model;

  for (int step = 0; step < steps; ++step) {
    const std::string& key = keys[rng() % keys.size()];
    const uint64_t action = rng() % 10;
    if (action < 4) {
      std::string value(rng() % 300, '\0');
      for (char& c : value) c = static_cast<char>(rng() % 256);
      store.Put(key, value);
      model[key] = value;
    } else if (action < 8) {
      const auto got = store.Get(key);
      const auto expected = model.find(key);
      ASSERT_EQ(got.has_value(), expected != model.end()) << "step=" << step;
      if (got.has_value()) {
        ASSERT_EQ(*got, expected->second) << "step=" << step;
      }
    } else {
      ASSERT_EQ(store.Delete(key), model.erase(key) == 1) << "step=" << step;
    }
    ASSERT_EQ(store.Size(), model.size()) << "step=" << step;
    ASSERT_LE(store.LoadFactor(), store.MaxLoadFactor()) << "step=" << step;
  }

  for (const auto& key : keys) {
    const auto got = store.Get(key);
    const auto expected = model.find(key);
    ASSERT_EQ(got.has_value(), expected != model.end());
    if (got.has_value()) {
      ASSERT_EQ(*got, expected->second);
    }
  }
}

TEST(FlatStoreModel, VariedKeysMatchReferenceModel) {
  RunModelTest<FlatHash>(12345, 300'000, 3'000);
}

TEST(FlatStoreModel, ClusteringHashMatchesReferenceModel) {
  RunModelTest<FirstByteHash>(777, 100'000, 1'000);
}

TEST(FlatStoreModel, ConstantHashMatchesReferenceModel) {
  RunModelTest<ConstantHash>(4242, 20'000, 200);
}

}
}
