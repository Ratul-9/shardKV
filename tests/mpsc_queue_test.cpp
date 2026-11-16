#include "lldstore/mpsc_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lldstore {
namespace {

using namespace std::chrono_literals;

// Waits (bounded) until `done()` is true, so a bug shows up as a failed assertion, not a hang.
template <typename Pred>
bool WaitFor(Pred done, std::chrono::milliseconds limit = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

// ---- Single-threaded behaviour --------------------------------------------------------------

TEST(MpscQueue, TryPopOnEmptyReturnsNothing) {
  MpscQueue<int> queue;
  EXPECT_FALSE(queue.TryPop().has_value());
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(MpscQueue, ItemsComeOutInTheOrderTheyWentIn) {
  MpscQueue<int> queue;
  for (int i = 1; i <= 5; ++i) ASSERT_TRUE(queue.Push(i));
  EXPECT_EQ(queue.Size(), 5u);
  for (int i = 1; i <= 5; ++i) {
    const auto item = queue.TryPop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, i);
  }
  EXPECT_FALSE(queue.TryPop().has_value());
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(MpscQueue, ReusableAfterBeingEmptied) {
  MpscQueue<int> queue;
  for (int round = 0; round < 100; ++round) {
    ASSERT_TRUE(queue.Push(round));
    ASSERT_EQ(queue.TryPop().value(), round);
    ASSERT_FALSE(queue.TryPop().has_value());
  }
}

TEST(MpscQueue, HoldsMoveOnlyAndLargeItems) {
  MpscQueue<std::unique_ptr<std::string>> queue;
  ASSERT_TRUE(queue.Push(std::make_unique<std::string>(100000, 'x')));
  auto item = queue.TryPop();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ((*item)->size(), 100000u);
}

TEST(MpscQueue, DestructorFreesItemsStillQueued) {
  // The leak checker (ASan on Linux) is what fails this test if the destructor forgets the list.
  MpscQueue<std::string> queue;
  for (int i = 0; i < 1000; ++i) ASSERT_TRUE(queue.Push(std::string(64, 'a')));
}

TEST(MpscQueue, DestroyingAnUnusedQueueIsFine) { MpscQueue<int> queue; }

// ---- Stop -----------------------------------------------------------------------------------

TEST(MpscQueue, PushAfterStopIsRejectedAndNothingIsEnqueued) {
  MpscQueue<int> queue;
  queue.Stop();
  EXPECT_FALSE(queue.Push(1));
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_FALSE(queue.Pop().has_value());  // stopped and empty: returns at once
}

TEST(MpscQueue, StopIsIdempotent) {
  MpscQueue<int> queue;
  queue.Stop();
  queue.Stop();
  EXPECT_FALSE(queue.Pop().has_value());
}

TEST(MpscQueue, PopDrainsEverythingQueuedBeforeStop) {
  MpscQueue<int> queue;
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(queue.Push(i));
  queue.Stop();
  for (int i = 0; i < 10; ++i) {
    const auto item = queue.Pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, i);
  }
  EXPECT_FALSE(queue.Pop().has_value());
}

// ---- Blocking Pop (the consumer sleeps and must not miss a wake-up) -------------------------

TEST(MpscQueue, PopBlocksUntilAnItemArrives) {
  MpscQueue<int> queue;
  std::atomic<bool> got{false};
  std::thread consumer([&] {
    const auto item = queue.Pop();
    got = item.has_value() && *item == 42;
  });
  std::this_thread::sleep_for(50ms);  // let the consumer reach its wait
  EXPECT_FALSE(got.load());
  ASSERT_TRUE(queue.Push(42));
  EXPECT_TRUE(WaitFor([&] { return got.load(); }));
  consumer.join();
}

TEST(MpscQueue, StopWakesASleepingConsumer) {
  MpscQueue<int> queue;
  std::atomic<bool> returned_empty{false};
  std::thread consumer([&] { returned_empty = !queue.Pop().has_value(); });
  std::this_thread::sleep_for(50ms);
  queue.Stop();
  EXPECT_TRUE(WaitFor([&] { return returned_empty.load(); }));
  consumer.join();
}

TEST(MpscQueue, PingPongNeverLosesAWakeup) {
  // One item at a time, so the consumer is asleep nearly every round. A lost wake-up hangs here.
  MpscQueue<int> queue;
  constexpr int kRounds = 20000;
  std::atomic<int> consumed{0};
  std::thread consumer([&] {
    while (auto item = queue.Pop()) {
      EXPECT_EQ(*item, consumed.load());
      ++consumed;
    }
  });
  for (int i = 0; i < kRounds; ++i) {
    ASSERT_TRUE(queue.Push(i));
    ASSERT_TRUE(WaitFor([&] { return consumed.load() == i + 1; })) << "stuck at round " << i;
  }
  queue.Stop();
  consumer.join();
}

TEST(MpscQueue, IdleQueuesCreateAndDestroyPromptly) {
  for (int i = 0; i < 200; ++i) {
    MpscQueue<int> queue;
    std::thread consumer([&] { while (queue.Pop()) {} });
    queue.Stop();
    consumer.join();
  }
}

// ---- Many producers -------------------------------------------------------------------------

struct Tagged {
  uint32_t producer;
  uint32_t seq;
};

// Every item arrives exactly once, and each producer's items arrive in the order that producer
// pushed them (the queue is FIFO per producer; across producers the order is arbitrary).
TEST(MpscQueueConcurrency, ManyProducersDeliverEverythingOnceAndInPerProducerOrder) {
  constexpr uint32_t kProducers = 8;
  constexpr uint32_t kPerProducer = 50000;
  MpscQueue<Tagged> queue;
  std::latch start(kProducers + 1);

  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      start.arrive_and_wait();
      for (uint32_t i = 0; i < kPerProducer; ++i) ASSERT_TRUE(queue.Push(Tagged{p, i}));
    });
  }

  std::vector<uint32_t> next_expected(kProducers, 0);
  uint64_t total = 0;
  start.arrive_and_wait();
  while (total < uint64_t{kProducers} * kPerProducer) {
    const auto item = queue.Pop();
    ASSERT_TRUE(item.has_value());
    ASSERT_LT(item->producer, kProducers);
    ASSERT_EQ(item->seq, next_expected[item->producer]) << "producer " << item->producer;
    ++next_expected[item->producer];
    ++total;
  }
  for (auto& t : producers) t.join();
  for (uint32_t p = 0; p < kProducers; ++p) EXPECT_EQ(next_expected[p], kPerProducer);
  EXPECT_FALSE(queue.TryPop().has_value());
  EXPECT_EQ(queue.Size(), 0u);
}

// Producers race with Stop(). The rule: every Push that returned true is delivered, every Push
// that returned false is not, and nothing is delivered twice or invented. The consumer must also
// terminate (it may only return "stopped and drained" once no push is still in flight).
TEST(MpscQueueConcurrency, PushesRacingWithStopAreDeliveredIffAccepted) {
  for (int round = 0; round < 200; ++round) {
    constexpr int kProducers = 4;
    MpscQueue<int> queue;
    std::atomic<uint64_t> accepted{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
      producers.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
          if (queue.Push(i)) {
            accepted.fetch_add(1);
          } else {
            break;  // stopped
          }
        }
      });
    }

    uint64_t delivered = 0;
    std::thread consumer([&] { while (queue.Pop()) ++delivered; });
    go = true;
    std::this_thread::sleep_for(std::chrono::microseconds(50 * (round % 7)));
    queue.Stop();
    for (auto& t : producers) t.join();
    consumer.join();  // hangs (and the test times out) if Pop never reports stopped-and-drained
    ASSERT_EQ(delivered, accepted.load()) << "round " << round;
  }
}

}  // namespace
}  // namespace lldstore
