#include "lldstore/epoch.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/crc32c.h"
#include "lldstore/rcu_map.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LLDSTORE_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define LLDSTORE_TEST_ASAN 1
#endif

namespace lldstore {
namespace {

std::atomic<int> g_freed{0};
void CountingDelete(void* p) {
  delete static_cast<int*>(p);
  ++g_freed;
}

TEST(EpochManager, NothingIsFreedWhileAReaderThatCouldSeeItIsInside) {
  g_freed = 0;
  EpochManager m;
  const size_t reader = m.RegisterReader();
  {
    auto guard = m.Pin(reader);
    m.Retire(new int(1), &CountingDelete);
    for (int i = 0; i < 10; ++i) m.Reclaim();
    EXPECT_EQ(g_freed.load(), 0);  // the reader entered before the retire: must not free
    EXPECT_EQ(m.pending(), 1u);
  }
  for (int i = 0; i < 3; ++i) m.Reclaim();
  EXPECT_EQ(g_freed.load(), 1);
  EXPECT_EQ(m.pending(), 0u);
}

TEST(EpochManager, FreesAfterTwoEpochsWithNoReaders) {
  g_freed = 0;
  EpochManager m;
  m.Retire(new int(1), &CountingDelete);
  m.Reclaim();  // epoch 0 -> 1: not yet (retired in 0, needs 2)
  EXPECT_EQ(g_freed.load(), 0);
  m.Reclaim();  // 1 -> 2: free
  EXPECT_EQ(g_freed.load(), 1);
}

TEST(EpochManager, AStalledReaderBlocksReclamationButNothingBreaks) {
  g_freed = 0;
  EpochManager m;
  const size_t reader = m.RegisterReader();
  auto guard = std::make_unique<EpochManager::Guard>(&m, reader);
  for (int i = 0; i < 1000; ++i) m.Retire(new int(i), &CountingDelete);
  EXPECT_EQ(g_freed.load(), 0);
  EXPECT_EQ(m.pending(), 1000u);  // memory grows: the known weakness of EBR
  guard.reset();
  m.Reclaim();
  m.Reclaim();
  EXPECT_EQ(g_freed.load(), 1000);
}

TEST(EpochManager, NestedGuardsAnnounceOnce) {
  g_freed = 0;
  EpochManager m;
  const size_t reader = m.RegisterReader();
  {
    auto outer = m.Pin(reader);
    {
      auto inner = m.Pin(reader);
    }
    m.Retire(new int(1), &CountingDelete);
    m.Reclaim();
    m.Reclaim();
    EXPECT_EQ(g_freed.load(), 0);  // still inside the outer guard
  }
  m.Reclaim();
  m.Reclaim();
  EXPECT_EQ(g_freed.load(), 1);
}

TEST(EpochManager, ReaderSlotsAreReusedAndLimited) {
  EpochManager m;
  std::vector<size_t> slots;
  for (size_t i = 0; i < EpochManager::kMaxReaders; ++i) slots.push_back(m.RegisterReader());
  EXPECT_THROW(m.RegisterReader(), std::runtime_error);
  m.UnregisterReader(slots[5]);
  EXPECT_EQ(m.RegisterReader(), slots[5]);
}

// ---- RcuMap -------------------------------------------------------------------------------

TEST(RcuMap, BehavesLikeAMapIncludingGrowth) {
  RcuMap map;
  const size_t reader = map.RegisterReader();
  std::map<std::string, std::string> model;
  std::mt19937_64 rng(5);
  for (int i = 0; i < 20000; ++i) {
    const std::string key = "k" + std::to_string(rng() % 3000);
    if (rng() % 4 == 0) {
      EXPECT_EQ(map.Delete(key), model.erase(key) == 1);
    } else {
      const std::string value = std::to_string(i);
      map.Put(key, value);
      model[key] = value;
    }
  }
  ASSERT_EQ(map.Size(), model.size());
  auto guard = map.Pin(reader);
  for (const auto& [k, v] : model) {
    const auto got = map.Get(guard, k);
    ASSERT_TRUE(got.has_value()) << k;
    ASSERT_EQ(*got, v);
  }
  EXPECT_FALSE(map.Get(guard, "absent").has_value());
}

// The deterministic version of the race the phase is about: a reader holds a pointer into a node,
// the writer deletes and overwrites the key (and grows the table), and the reader reads the bytes.
template <typename Map>
std::string ReadAcrossConcurrentDelete() {
  Map map;
  const size_t reader = map.RegisterReader();
  map.Put("victim", std::string(100, 'v'));
  auto guard = map.Pin(reader);
  const std::string_view view = *map.Get(guard, "victim");
  map.Delete("victim");                                                // retires the node
  for (int i = 0; i < 100; ++i) map.Put("other" + std::to_string(i), "x");  // grows: retires tables
  map.Reclaim();
  map.Reclaim();
  return std::string(view);  // with EBR the bytes are still there; naively they were freed
}

TEST(RcuMap, AViewStaysValidWhileTheGuardIsHeld) {
  EXPECT_EQ(ReadAcrossConcurrentDelete<RcuMap>(), std::string(100, 'v'));
}

TEST(RcuMap, NaiveImmediateFreeIsAUseAfterFree) {
#if defined(LLDSTORE_TEST_ASAN)
  EXPECT_DEATH(ReadAcrossConcurrentDelete<UnsafeRcuMap>(), "heap-use-after-free");
#else
  GTEST_SKIP() << "needs the asan preset: without ASan a use-after-free is undefined behaviour, "
                  "not a reliable failure";
#endif
}

// One writer mutating, four readers reading, under TSan. Values carry their own checksum, so a
// reader that read freed or half-written memory would see a bad checksum; per-key versions must
// never go backwards for a reader.
TEST(RcuMapConcurrency, ReadersNeverSeeTornOrStaleValues) {
  RcuMap map;
  constexpr int kKeys = 64;
  auto make_value = [](int key, uint64_t version) {
    std::string body = std::to_string(key) + ":" + std::to_string(version) + ":" + std::string(static_cast<size_t>(key % 50), 'p');
    return body + "#" + std::to_string(Crc32c(body));
  };
  for (int k = 0; k < kKeys; ++k) map.Put("k" + std::to_string(k), make_value(k, 0));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      const size_t slot = map.RegisterReader();
      std::vector<uint64_t> last(kKeys, 0);
      std::mt19937_64 rng(static_cast<uint64_t>(r));
      while (!stop.load(std::memory_order_relaxed)) {
        const int k = static_cast<int>(rng() % kKeys);
        auto guard = map.Pin(slot);
        const auto v = map.Get(guard, "k" + std::to_string(k));
        if (!v) continue;  // deleted right now
        const size_t hash = v->rfind('#');
        ASSERT_NE(hash, std::string_view::npos);
        const std::string_view body = v->substr(0, hash);
        ASSERT_EQ(std::to_string(Crc32c(body)), std::string(v->substr(hash + 1))) << "torn value";
        const size_t c1 = body.find(':');
        ASSERT_EQ(std::stoi(std::string(body.substr(0, c1))), k);
        const uint64_t version = std::stoull(std::string(body.substr(c1 + 1, body.find(':', c1 + 1) - c1 - 1)));
        ASSERT_GE(version, last[static_cast<size_t>(k)]) << "a reader went back in time";
        last[static_cast<size_t>(k)] = version;
        reads.fetch_add(1, std::memory_order_relaxed);
      }
      map.UnregisterReader(slot);
    });
  }
  std::mt19937_64 rng(99);
  for (uint64_t version = 1; version <= 50000; ++version) {
    const int k = static_cast<int>(rng() % kKeys);
    if (rng() % 10 == 0) {
      map.Delete("k" + std::to_string(k));
      map.Put("k" + std::to_string(k), make_value(k, version));
    } else {
      map.Put("k" + std::to_string(k), make_value(k, version));
    }
    if (version % 5000 == 0) {  // occasional growth: more keys, new tables
      for (int extra = 0; extra < 50; ++extra) map.Put("extra" + std::to_string(version + static_cast<uint64_t>(extra)), "e");
    }
  }
  stop = true;
  for (auto& t : readers) t.join();
  EXPECT_GT(reads.load(), 1000u);
  map.Reclaim();
  map.Reclaim();
  map.Reclaim();
  EXPECT_EQ(map.PendingReclamation(), 0u);  // with no readers left, everything is freed
}

}  // namespace
}  // namespace lldstore
