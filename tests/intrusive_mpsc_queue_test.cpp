#include "lldstore/intrusive_mpsc_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

namespace lldstore {
namespace {

using namespace std::chrono_literals;

struct Item {
  uint32_t producer = 0;
  uint32_t seq = 0;
  std::atomic<Item*> queue_next{nullptr};
};

using Queue = IntrusiveMpscQueue<Item*>;

TEST(IntrusiveMpscQueue, EmptyQueueHasNothing) {
  Queue queue;
  EXPECT_FALSE(queue.TryPop().has_value());
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(IntrusiveMpscQueue, FifoWithASingleProducerIncludingTheLastItem) {
  Queue queue;
  Item items[5];
  for (uint32_t i = 0; i < 5; ++i) {
    items[i].seq = i;
    ASSERT_TRUE(queue.Push(&items[i]));
  }
  for (uint32_t i = 0; i < 5; ++i) {
    auto got = queue.TryPop();
    ASSERT_TRUE(got.has_value()) << i;
    EXPECT_EQ(*got, &items[i]);
  }
  EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(IntrusiveMpscQueue, OneItemAtATimeExercisesTheStubReinsertion) {
  Queue queue;
  Item a, b;
  for (int round = 0; round < 1000; ++round) {
    Item* item = (round % 2 == 0) ? &a : &b;
    ASSERT_TRUE(queue.Push(item));
    auto got = queue.TryPop();
    ASSERT_TRUE(got.has_value()) << round;
    ASSERT_EQ(*got, item);
    ASSERT_FALSE(queue.TryPop().has_value());
  }
}

TEST(IntrusiveMpscQueue, AnItemCanBePushedAgainAfterItWasPopped) {
  Queue queue;
  Item item;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(queue.Push(&item));
    ASSERT_EQ(queue.Pop().value(), &item);
  }
}

TEST(IntrusiveMpscQueue, StopDrainsThenReportsEmpty) {
  Queue queue;
  Item items[3];
  for (Item& item : items) ASSERT_TRUE(queue.Push(&item));
  queue.Stop();
  Item late;
  EXPECT_FALSE(queue.Push(&late));
  for (Item& item : items) EXPECT_EQ(queue.Pop().value(), &item);
  EXPECT_FALSE(queue.Pop().has_value());
}

TEST(IntrusiveMpscQueue, StopWakesASleepingConsumer) {
  Queue queue;
  std::atomic<bool> done{false};
  std::thread consumer([&] {
    EXPECT_FALSE(queue.Pop().has_value());
    done = true;
  });
  std::this_thread::sleep_for(20ms);
  queue.Stop();
  consumer.join();
  EXPECT_TRUE(done.load());
}

TEST(IntrusiveMpscQueueConcurrency, ManyProducersExactlyOnceInPerProducerOrder) {
  constexpr uint32_t kProducers = 8;
  constexpr uint32_t kPerProducer = 20000;
  Queue queue;
  std::vector<std::unique_ptr<Item[]>> items;  // Item holds an atomic, so it cannot be copied
  for (uint32_t p = 0; p < kProducers; ++p) items.push_back(std::make_unique<Item[]>(kPerProducer));
  std::latch start(kProducers + 1);
  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      start.arrive_and_wait();
      for (uint32_t i = 0; i < kPerProducer; ++i) {
        items[p][i].producer = p;
        items[p][i].seq = i;
        ASSERT_TRUE(queue.Push(&items[p][i]));
      }
    });
  }
  std::vector<uint32_t> next(kProducers, 0);
  start.arrive_and_wait();
  for (uint64_t got = 0; got < uint64_t{kProducers} * kPerProducer; ++got) {
    Item* item = queue.Pop().value();
    ASSERT_EQ(item->seq, next[item->producer]);
    ++next[item->producer];
  }
  for (auto& t : producers) t.join();
  EXPECT_FALSE(queue.TryPop().has_value());
}

// Each producer pushes one stack item and waits until the consumer has seen it before reusing it,
// which is exactly how ShardedStore uses the queue (the Request is on the caller's stack).
TEST(IntrusiveMpscQueueConcurrency, StackItemsReusedAfterHandBack) {
  constexpr int kProducers = 4;
  constexpr int kRounds = 5000;
  Queue queue;
  std::atomic<int> consumed{0};
  std::thread consumer([&] {
    while (auto item = queue.Pop()) {
      std::atomic_ref<uint32_t>((*item)->seq).store(0xffffffffu);  // hand back
      ++consumed;
    }
  });
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&] {
      Item item;
      for (int r = 0; r < kRounds; ++r) {
        std::atomic_ref<uint32_t> seq(item.seq);
        seq.store(static_cast<uint32_t>(r));
        ASSERT_TRUE(queue.Push(&item));
        while (seq.load() != 0xffffffffu) std::this_thread::yield();
      }
    });
  }
  for (auto& t : producers) t.join();
  queue.Stop();
  consumer.join();
  EXPECT_EQ(consumed.load(), kProducers * kRounds);
}

}  // namespace
}  // namespace lldstore
