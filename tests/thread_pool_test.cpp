#include "lldstore/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
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

TEST(ThreadPool, EveryTaskRunsExactlyOnce) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 10000; ++i) pool.Submit([&] { ++count; });
  }  // destruction drains the queue
  EXPECT_EQ(count.load(), 10000);
}

TEST(ThreadPool, TasksRunOnDifferentThreadsConcurrently) {
  std::mutex mutex;
  std::set<std::thread::id> ids;
  std::atomic<int> started{0};
  std::atomic<bool> all_started{false};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 4; ++i) {
      pool.Submit([&] {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ids.insert(std::this_thread::get_id());
        }
        ++started;
        // Every task waits for all four to have started: this can only finish if they truly run
        // at the same time on four threads.
        if (WaitFor([&] { return started.load() == 4; })) all_started = true;
      });
    }
  }
  EXPECT_TRUE(all_started.load());
  EXPECT_EQ(ids.size(), 4u);
}

TEST(ThreadPool, DestructorRunsTasksStillQueued) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(1);
    pool.Submit([] { std::this_thread::sleep_for(50ms); });  // keeps the only worker busy
    for (int i = 0; i < 100; ++i) pool.Submit([&] { ++count; });
  }  // 100 tasks are still queued here
  EXPECT_EQ(count.load(), 100);
}

TEST(ThreadPool, ThrowingTaskDoesNotKillTheWorker) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(1);  // one worker: if it died, the later task would never run
    pool.Submit([] { throw std::runtime_error("boom"); });
    pool.Submit([] { throw 42; });
    pool.Submit([&] { ++count; });
  }
  EXPECT_EQ(count.load(), 1);
}

TEST(ThreadPool, TaskMaySubmitMoreTasksEvenWhileDraining) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(2);
    pool.Submit([&] {
      pool.Submit([&] {
        std::this_thread::sleep_for(20ms);  // still running when the destructor has started
        pool.Submit([&] { ++count; });
      });
    });
  }
  EXPECT_EQ(count.load(), 1);
}

TEST(ThreadPool, ManyProducersSubmitConcurrently) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(4);
    std::vector<std::thread> producers;
    for (int t = 0; t < 8; ++t) {
      producers.emplace_back([&] {
        for (int i = 0; i < 5000; ++i) pool.Submit([&] { ++count; });
      });
    }
    for (auto& p : producers) p.join();
  }
  EXPECT_EQ(count.load(), 40000);
}

TEST(ThreadPool, ABlockedTaskDoesNotStopOtherWorkers) {
  std::atomic<bool> release{false};
  std::atomic<int> count{0};
  {
    ThreadPool pool(2);
    pool.Submit([&] { WaitFor([&] { return release.load(); }); });  // occupies one worker
    for (int i = 0; i < 100; ++i) pool.Submit([&] { ++count; });
    EXPECT_TRUE(WaitFor([&] { return count.load() == 100; }));  // the other worker ran them all
    release = true;
  }
}

TEST(ThreadPool, IdlePoolsDestroyPromptly) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) ThreadPool pool(8);  // a lost wakeup would hang here
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(ThreadPool, ZeroThreadsIsClampedToOne) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(0);
    pool.Submit([&] { ++count; });
  }
  EXPECT_EQ(count.load(), 1);
}

TEST(ThreadPool, RepeatedStress) {
  for (int round = 0; round < 20; ++round) {
    std::atomic<int> count{0};
    {
      ThreadPool pool(6);
      std::vector<std::thread> producers;
      for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&] {
          for (int i = 0; i < 2000; ++i) pool.Submit([&] { ++count; });
        });
      }
      for (auto& p : producers) p.join();
    }
    ASSERT_EQ(count.load(), 8000) << "round " << round;
  }
}

}  // namespace
}  // namespace lldstore
