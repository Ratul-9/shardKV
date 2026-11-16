#include "lldstore/slab_allocator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "lldstore/flat_store.h"

namespace lldstore {
namespace {

TEST(SlabAllocatorClasses, EveryRequestFitsItsClassWithBoundedWaste) {
  for (size_t n = 1; n <= SlabAllocator::kMaxSmall; ++n) {
    const size_t c = SlabAllocator::ClassIndex(n);
    ASSERT_LT(c, SlabAllocator::kNumClasses) << n;
    const size_t size = SlabAllocator::ClassSize(c);
    ASSERT_GE(size, n) << n;
    ASSERT_EQ(size % SlabAllocator::kAlignment, 0u) << n;
    if (c > 0) ASSERT_LT(SlabAllocator::ClassSize(c - 1), n) << "not the smallest class for " << n;
    if (n > 128) ASSERT_LE(size * 4, n * 5 + 4 * 32) << "more than 25% waste for " << n;
  }
}

TEST(SlabAllocatorClasses, ClassSizesStrictlyIncrease) {
  for (size_t c = 1; c < SlabAllocator::kNumClasses; ++c) {
    EXPECT_LT(SlabAllocator::ClassSize(c - 1), SlabAllocator::ClassSize(c));
    EXPECT_EQ(SlabAllocator::ClassIndex(SlabAllocator::ClassSize(c)), c);
  }
}

TEST(SlabAllocator, BlocksAreAlignedDistinctAndWritable) {
  SlabAllocator slab;
  std::set<uintptr_t> seen;
  std::vector<std::pair<void*, size_t>> blocks;
  for (size_t n : {1u, 15u, 16u, 17u, 100u, 129u, 1000u, 4096u, 16384u}) {
    for (int i = 0; i < 50; ++i) {
      void* p = slab.Allocate(n);
      ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % SlabAllocator::kAlignment, 0u);
      ASSERT_TRUE(seen.insert(reinterpret_cast<uintptr_t>(p)).second);
      std::memset(p, 0xab, SlabAllocator::UsableSize(n));  // ASan checks the whole usable size
      blocks.emplace_back(p, n);
    }
  }
  for (auto [p, n] : blocks) slab.Deallocate(p, n);
  EXPECT_EQ(slab.stats().live_blocks, 0u);
  EXPECT_EQ(slab.stats().requested_bytes, 0u);
  EXPECT_EQ(slab.stats().allocated_bytes, 0u);
}

TEST(SlabAllocator, FreedBlocksAreReusedLastInFirstOut) {
  SlabAllocator slab;
  void* a = slab.Allocate(48);
  void* b = slab.Allocate(48);
  slab.Deallocate(a, 48);
  slab.Deallocate(b, 48);
  EXPECT_EQ(slab.Allocate(48), b);
  EXPECT_EQ(slab.Allocate(40), a);  // same class (48)
  EXPECT_EQ(slab.stats().chunks, 1u);
}

TEST(SlabAllocator, SteadyStateChurnTakesNoNewChunks) {
  SlabAllocator slab;
  std::vector<void*> live;
  for (int i = 0; i < 1000; ++i) live.push_back(slab.Allocate(64));
  const size_t chunks = slab.stats().chunks;
  for (int round = 0; round < 100; ++round) {
    for (void*& p : live) {
      slab.Deallocate(p, 64);
      p = slab.Allocate(64);
    }
  }
  EXPECT_EQ(slab.stats().chunks, chunks);
  for (void* p : live) slab.Deallocate(p, 64);
}

TEST(SlabAllocator, ExhaustingAChunkStartsAnother) {
  SlabAllocator slab;
  const size_t per_chunk = SlabAllocator::kChunkSize / 1024;
  std::vector<void*> blocks;
  for (size_t i = 0; i < per_chunk; ++i) blocks.push_back(slab.Allocate(1024));
  EXPECT_EQ(slab.stats().chunks, 1u);
  blocks.push_back(slab.Allocate(1024));
  EXPECT_EQ(slab.stats().chunks, 2u);
  for (void* p : blocks) slab.Deallocate(p, 1024);
}

TEST(SlabAllocator, LargeRequestsBypassTheSlab) {
  SlabAllocator slab;
  void* p = slab.Allocate(SlabAllocator::kMaxSmall + 1);
  EXPECT_EQ(slab.stats().large_blocks, 1u);
  EXPECT_EQ(slab.stats().chunks, 0u);
  EXPECT_EQ(SlabAllocator::UsableSize(SlabAllocator::kMaxSmall + 1), SlabAllocator::kMaxSmall + 1);
  slab.Deallocate(p, SlabAllocator::kMaxSmall + 1);
  EXPECT_EQ(slab.stats().large_blocks, 0u);
}

TEST(SlabAllocator, RandomisedAgainstAShadowOfContents) {
  SlabAllocator slab;
  std::mt19937_64 rng(7);
  struct Block {
    unsigned char* p;
    size_t n;
    unsigned char fill;
  };
  std::vector<Block> live;
  for (int step = 0; step < 50000; ++step) {
    if (live.empty() || rng() % 3 != 0) {
      const size_t n = 1 + rng() % (rng() % 8 == 0 ? 40000 : 600);
      auto* p = static_cast<unsigned char*>(slab.Allocate(n));
      const auto fill = static_cast<unsigned char>(rng());
      std::memset(p, fill, n);
      live.push_back({p, n, fill});
    } else {
      const size_t i = rng() % live.size();
      const Block b = live[i];
      for (size_t k = 0; k < b.n; ++k) ASSERT_EQ(b.p[k], b.fill) << "block overwritten";
      slab.Deallocate(b.p, b.n);
      live[i] = live.back();
      live.pop_back();
    }
  }
  for (const Block& b : live) slab.Deallocate(b.p, b.n);
  EXPECT_EQ(slab.stats().live_blocks, 0u);
}

TEST(SlabFlatStore, BehavesLikeFlatStoreAndReturnsAllMemory) {
  SlabFlatStore store;
  std::map<std::string, std::string> model;
  std::mt19937_64 rng(11);
  for (int step = 0; step < 20000; ++step) {
    const std::string key = "k" + std::to_string(rng() % 500);
    switch (rng() % 3) {
      case 0: {
        const std::string value(rng() % 300, static_cast<char>('a' + rng() % 26));
        store.Put(key, value);
        model[key] = value;
        break;
      }
      case 1: {
        const auto got = store.Get(key);
        const auto it = model.find(key);
        ASSERT_EQ(got.has_value(), it != model.end());
        if (got) ASSERT_EQ(*got, it->second);
        break;
      }
      default:
        ASSERT_EQ(store.Delete(key), model.erase(key) == 1);
    }
  }
  ASSERT_EQ(store.Size(), model.size());
  EXPECT_EQ(store.allocator().stats().live_blocks, model.size());
  for (const auto& [k, v] : model) ASSERT_TRUE(store.Delete(k));
  EXPECT_EQ(store.allocator().stats().live_blocks, 0u);
}

TEST(SlabFlatStore, GrowingAValueWithinItsClassReusesTheBlock) {
  SlabFlatStore store;
  store.Put("key", std::string(20, 'a'));
  const size_t requested = store.allocator().stats().requested_bytes;
  store.Put("key", std::string(30, 'b'));
  EXPECT_EQ(store.allocator().stats().requested_bytes, requested);
  EXPECT_EQ(*store.Get("key"), std::string(30, 'b'));
}

TEST(FlatStoreForEach, VisitsEveryEntryOnce) {
  FlatStore store;
  for (int i = 0; i < 1000; ++i) store.Put("k" + std::to_string(i), "v" + std::to_string(i));
  std::map<std::string, std::string> seen;
  store.ForEach([&](std::string_view k, std::string_view v) {
    EXPECT_TRUE(seen.emplace(std::string(k), std::string(v)).second);
  });
  ASSERT_EQ(seen.size(), 1000u);
  EXPECT_EQ(seen["k7"], "v7");
}

}  // namespace
}  // namespace lldstore
