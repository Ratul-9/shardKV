#include "lldstore/bounded_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace lldstore {
namespace {

using namespace std::chrono_literals;

// Waits (bounded) until `done()` is true, so a bug shows up as a failed assertion, not a hung test.
template <typename Pred>
bool WaitFor(Pred done, std::chrono::milliseconds limit = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

TEST(BoundedQueue, ItemsComeOutInTheOrderTheyWentIn) {
  BoundedQueue<int> queue(8);
  for (int i = 1; i <= 5; ++i) ASSERT_TRUE(queue.Push(i));
  EXPECT_EQ(queue.Size(), 5u);
  for (int i = 1; i <= 5; ++i) {
    const auto item = queue.Pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, i);
  }
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(BoundedQueue, ZeroCapacityIsClampedToOne) {
  BoundedQueue<int> queue(0);
  EXPECT_EQ(queue.capacity(), 1u);
  EXPECT_TRUE(queue.Push(1));
  EXPECT_EQ(*queue.Pop(), 1);
}

TEST(BoundedQueue, PushBlocksWhileFullUntilSomethingIsPopped) {
  BoundedQueue<int> queue(1);
  ASSERT_TRUE(queue.Push(1));
  std::atomic<bool> second_push_returned{false};
  std::thread producer([&] {
    queue.Push(2);  // must block: the queue is full
    second_push_returned = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(second_push_returned.load());  // still blocked after 100 ms
  EXPECT_EQ(*queue.Pop(), 1);                 // frees a slot
  EXPECT_TRUE(WaitFor([&] { return second_push_returned.load(); }));
  producer.join();
  EXPECT_EQ(*queue.Pop(), 2);
}

TEST(BoundedQueue, PopBlocksWhileEmptyUntilSomethingIsPushed) {
  BoundedQueue<int> queue(4);
  std::atomic<int> got{-1};
  std::thread consumer([&] { got = *queue.Pop(); });
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(got.load(), -1);  // still waiting
  ASSERT_TRUE(queue.Push(7));
  EXPECT_TRUE(WaitFor([&] { return got.load() == 7; }));
  consumer.join();
}

TEST(BoundedQueue, StopLetsConsumersDrainThenReturnsNullopt) {
  BoundedQueue<int> queue(8);
  ASSERT_TRUE(queue.Push(1));
  ASSERT_TRUE(queue.Push(2));
  queue.Stop();
  EXPECT_FALSE(queue.Push(3));  // rejected after Stop
  EXPECT_EQ(*queue.Pop(), 1);   // but what was already queued is still delivered
  EXPECT_EQ(*queue.Pop(), 2);
  EXPECT_FALSE(queue.Pop().has_value());  // stopped and empty
  queue.Stop();                           // idempotent
}

TEST(BoundedQueue, StopWakesEveryBlockedConsumer) {
  BoundedQueue<int> queue(4);
  std::atomic<int> woke{0};
  std::vector<std::thread> consumers;
  for (int i = 0; i < 4; ++i) {
    consumers.emplace_back([&] {
      if (!queue.Pop().has_value()) ++woke;
    });
  }
  std::this_thread::sleep_for(100ms);
  queue.Stop();
  EXPECT_TRUE(WaitFor([&] { return woke.load() == 4; }));
  for (auto& c : consumers) c.join();
}

TEST(BoundedQueue, StopWakesABlockedProducerAndRejectsIt) {
  BoundedQueue<int> queue(1);
  ASSERT_TRUE(queue.Push(1));
  std::atomic<int> result{-1};  // -1 pending, 0 rejected, 1 accepted
  std::thread producer([&] { result = queue.Push(2) ? 1 : 0; });
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(result.load(), -1);  // blocked on the full queue
  queue.Stop();
  EXPECT_TRUE(WaitFor([&] { return result.load() == 0; }));
  producer.join();
}

TEST(BoundedQueue, EveryItemIsDeliveredExactlyOnceWithManyProducersAndConsumers) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 3;
  constexpr int kPerProducer = 5000;
  BoundedQueue<int> queue(16);  // small, so producers really do block

  std::atomic<int64_t> sum{0};
  std::atomic<int> count{0};
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      while (auto item = queue.Pop()) {
        sum += *item;
        ++count;
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) queue.Push(p * kPerProducer + i + 1);
    });
  }
  for (auto& t : producers) t.join();
  queue.Stop();  // consumers finish what is queued, then exit
  for (auto& t : consumers) t.join();

  constexpr int64_t kTotal = static_cast<int64_t>(kProducers) * kPerProducer;
  EXPECT_EQ(count.load(), kProducers * kPerProducer);
  EXPECT_EQ(sum.load(), kTotal * (kTotal + 1) / 2);  // items are 1..kTotal, each once
}

}  // namespace
}  // namespace lldstore
