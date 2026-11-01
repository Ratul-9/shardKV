#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace lldstore {

// Phase 2: fixed-size thread pool. One mutex protects one FIFO queue; idle workers sleep on a
// condition variable. See docs/DESIGN.md D-007 for the decisions below and what was rejected.
//
// Contract:
//  - Every task accepted by Submit runs exactly once, even if the pool is destroyed first
//    (the destructor drains the queue, then joins).
//  - A task that throws is caught and discarded; the worker keeps running.
//  - Submit may be called from any thread, including from inside a running task (also while the
//    pool is draining). It must NOT be called concurrently with, or after, destruction from
//    outside the pool: calling a member function on an object that is being destroyed is
//    undefined behaviour no matter what the pool does.
class ThreadPool {
 public:
  // A pool with zero workers would accept tasks and never run them, so at least one is started.
  explicit ThreadPool(size_t num_threads) {
    const size_t n = std::max<size_t>(1, num_threads);
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  // Drains the queue and joins all workers.
  ~ThreadPool() {
    {
      // stop_ is written under the same mutex the workers check it under. If we set it without
      // the lock, a worker could test the predicate, find it false, and go to sleep just before
      // our notify: a lost wakeup, and join() below would hang forever.
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();  // every idle worker must see stop_, so notify_all, not notify_one
    for (auto& worker : workers_) worker.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Queues a task to run on some worker thread.
  void Submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push(std::move(task));
    }
    // Notify after releasing the lock so the woken worker does not immediately block on a mutex
    // we still hold. One task, so waking one worker is enough.
    cv_.notify_one();
  }

 private:
  void WorkerLoop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        // The predicate form is a loop: wait() can return spuriously, and another worker may
        // have taken the task between our wake-up and re-acquiring the lock. Re-check every time.
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        // Only exit once stopping AND the queue is empty: this is what makes shutdown drain
        // instead of dropping accepted tasks.
        if (tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }  // lock released here: the task must run WITHOUT the lock, or the pool would be serial
      try {
        task();
      } catch (...) {
        // A std::thread whose function throws calls std::terminate and takes the whole process
        // down. We keep the worker alive instead. This hides the failure, which is acceptable
        // for fire-and-forget tasks but means callers who need errors must report them
        // themselves (for example through a promise captured in the task).
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;  // guarded by mutex_
  bool stop_ = false;                        // guarded by mutex_
  // Declared last: members above are fully constructed before any worker thread can touch them.
  std::vector<std::thread> workers_;
};

}  // namespace lldstore
