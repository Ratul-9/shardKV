#pragma once

#include <cstddef>
#include <cstdlib>
#include <functional>

namespace lldstore {

// Phase 2: fixed-size thread pool.
//
// TODO(Ratul): implement. The bodies below abort so nothing mistakes this skeleton for working
// code. Decisions you must make and write down in docs/DESIGN.md:
//  - What data structure holds pending tasks, and what protects it?
//  - How do idle workers wait? (busy loop vs condition variable; why a predicate loop?)
//  - Shutdown: when the destructor runs, are queued tasks drained or discarded? Why?
//  - What happens if a task throws? If Submit is called after shutdown began?
//  - How do you avoid a lost wakeup, and a deadlock between Submit and shutdown?
class ThreadPool {
 public:
  explicit ThreadPool(size_t /*num_threads*/) { std::abort(); }

  // Joins all workers.
  ~ThreadPool() = default;

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Queues a task to run on some worker thread.
  void Submit(std::function<void()> /*task*/) { std::abort(); }

 private:
  // TODO(Ratul): workers, queue, mutex, condition variable, stop flag.
};

}  // namespace lldstore
