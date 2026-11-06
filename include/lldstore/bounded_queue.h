#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace lldstore {

// Phase 3: bounded multi-producer queue guarded by ONE mutex and TWO condition variables. This is
// the deliberately simple baseline that Phase 4 replaces with a lock-free queue, so the interface
// (Push, Pop, Stop) is kept small. Design decisions: docs/DESIGN.md D-013.
//
//  - Push blocks while the queue is full. That is backpressure: a client that is too fast is made
//    to wait instead of the queue growing without limit.
//  - Pop blocks while the queue is empty.
//  - Stop() lets a consumer finish what is already queued: Pop keeps returning items until the
//    queue is empty and only then returns std::nullopt. After Stop, Push rejects new items by
//    returning false, and producers blocked on a full queue are woken and rejected.
//
// Two condition variables (not_full_, not_empty_), not one: a producer waits for space and a
// consumer waits for items, so one notify can wake the right kind of waiter instead of every
// waiter, and a consumer's wake-up is never wasted on another consumer.
template <typename T>
class BoundedQueue {
 public:
  // A capacity of zero would block every Push forever, so at least one slot is kept.
  explicit BoundedQueue(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Returns true if the item was enqueued, false if the queue has been stopped (the item is then
  // NOT enqueued and nobody will ever consume it, so the caller must treat it as not sent).
  bool Push(T item) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Predicate loop: wait() can wake spuriously, and another producer may take the free slot
      // between our wake-up and re-acquiring the lock.
      not_full_.wait(lock, [this] { return stopped_ || items_.size() < capacity_; });
      if (stopped_) return false;
      items_.push_back(std::move(item));
    }
    // Notify after releasing the lock so the woken consumer does not immediately block on it.
    not_empty_.notify_one();
    return true;
  }

  // Blocks until an item is available. Returns std::nullopt only when the queue is stopped AND
  // empty, which is what lets a consumer loop `while (auto item = Pop())` and exit cleanly.
  std::optional<T> Pop() {
    std::optional<T> item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock, [this] { return stopped_ || !items_.empty(); });
      if (items_.empty()) return std::nullopt;  // stopped and drained
      item = std::move(items_.front());
      items_.pop_front();
    }
    not_full_.notify_one();  // one slot freed: wake one blocked producer
    return item;
  }

  // Idempotent. Wakes everyone so that no consumer or producer sleeps through the shutdown.
  void Stop() {
    {
      // Set under the mutex the waiters' predicates are checked under, or a waiter could test the
      // predicate, miss this change, and sleep after our notify (lost wakeup).
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  size_t capacity() const noexcept { return capacity_; }

 private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<T> items_;  // guarded by mutex_
  bool stopped_ = false;  // guarded by mutex_
};

}  // namespace lldstore
