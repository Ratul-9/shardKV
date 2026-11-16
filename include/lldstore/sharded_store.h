#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "lldstore/bounded_queue.h"
#include "lldstore/flat_store.h"
#include "lldstore/mpsc_queue.h"
#include "lldstore/store.h"

namespace lldstore {

struct ShardedStoreOptions {
  size_t num_shards = 4;       // clamped to at least 1
  size_t queue_capacity = 1024;  // per shard; a full queue makes callers wait (backpressure).
                                 // Ignored by the lock-free queue, which is unbounded (D-018).
  // Empty: threads are not pinned. Otherwise shard i is pinned to pin_cpus[i % pin_cpus.size()].
  // Pinning is implemented on Linux only; elsewhere (or if the kernel refuses) the shard simply
  // runs unpinned and PinnedCpus() reports -1 for it.
  std::vector<int> pin_cpus;
};

// Phase 3: shared-nothing, thread-per-shard store. Design decisions: docs/DESIGN.md D-012 to D-017.
//
//   caller thread --hash(key) % N--> shard queue --> shard worker thread --> Store (that shard only)
//
// Each shard owns a Store that ONLY its worker thread ever touches, so the hot path takes no lock
// on the data. What is shared is the queue in front of each shard (a mutex + two condition
// variables) and the reply hand-back. Callers block until their request has been executed, so
// every call is synchronous and the interface matches MutexStore: Get copies the value into the
// caller's string (D-002), because the shard may change the value as soon as it has replied.
//
// Thread safety: all methods may be called from any number of threads at once. They must not be
// called concurrently with, or after, destruction (undefined behaviour for any C++ object).
// Calls on different keys that land on different shards run in parallel; calls that land on the
// same shard are executed one at a time in arrival order.
//
// Not atomic: Size() asks each shard in turn and adds the answers, so it is not a snapshot of the
// whole store at one instant, and there are no multi-key operations.
// QueueT is the queue in front of each shard: BoundedQueue (Phase 3, mutex) or MpscQueue (Phase 4,
// lock-free). Use the aliases at the bottom of this file. Only the queue differs between the two.
template <template <typename> class QueueT, typename StoreT = Store>
class BasicShardedStore {
 public:
  explicit BasicShardedStore(ShardedStoreOptions options = {}) {
    const size_t n = std::max<size_t>(1, options.num_shards);
    shards_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      shards_.push_back(std::make_unique<Shard>(options.queue_capacity));
    }
    // Start the threads only after every Shard exists, then wait until each has tried to pin
    // itself, so PinnedCpus() is final when the constructor returns.
    std::latch ready(static_cast<std::ptrdiff_t>(n));
    for (size_t i = 0; i < n; ++i) {
      const int cpu = options.pin_cpus.empty() ? -1 : options.pin_cpus[i % options.pin_cpus.size()];
      shards_[i]->Start(cpu, ready);
    }
    ready.wait();
  }

  // Stops every queue (workers finish what is already queued, then exit) and joins them.
  ~BasicShardedStore() {
    for (auto& shard : shards_) shard->queue.Stop();
    for (auto& shard : shards_) {
      if (shard->worker.joinable()) shard->worker.join();
    }
  }

  BasicShardedStore(const BasicShardedStore&) = delete;
  BasicShardedStore& operator=(const BasicShardedStore&) = delete;

  void Put(std::string_view key, std::string_view value) {
    Request request;
    request.op = Request::Op::kPut;
    request.key = key;
    request.value = value;
    Dispatch(request, ShardIndex(key));
  }

  // Returns false if the key is absent. On success copies the value into *out.
  bool Get(std::string_view key, std::string* out) const {
    Request request;
    request.op = Request::Op::kGet;
    request.key = key;
    request.out = out;
    Dispatch(request, ShardIndex(key));
    return request.ok;
  }

  // Returns true if the key existed and was removed.
  bool Delete(std::string_view key) {
    Request request;
    request.op = Request::Op::kDelete;
    request.key = key;
    Dispatch(request, ShardIndex(key));
    return request.ok;
  }

  // Sum over all shards. Every shard is asked at once and the answers are collected, so the cost
  // is one round trip, not one per shard. Not a consistent snapshot (see the class comment).
  size_t Size() const {
    // A deque so the Requests (which hold a mutex and so cannot be moved) never relocate while
    // the shards hold pointers to them.
    std::deque<Request> requests;
    bool rejected = false;
    for (auto& shard : shards_) {
      Request& request = requests.emplace_back();
      request.op = Request::Op::kSize;
      if (!shard->queue.Push(&request)) {
        requests.pop_back();  // never enqueued
        rejected = true;
        break;
      }
    }
    size_t total = 0;
    for (Request& request : requests) {  // wait for everything that WAS enqueued before leaving
      request.Wait();
      total += request.size;
    }
    if (rejected) throw std::logic_error("ShardedStore used during shutdown");
    return total;
  }

  size_t num_shards() const noexcept { return shards_.size(); }

  size_t ShardIndex(std::string_view key) const noexcept { return ShardFor(key, shards_.size()); }

  // The routing function, usable without constructing a store (for tests and analysis).
  static size_t ShardFor(std::string_view key, size_t shard_count) noexcept {
    return static_cast<size_t>(HashKey(key) % shard_count);
  }

  // A fixed, portable 64-bit hash: FNV-1a over the bytes, then the splitmix64 finalizer.
  // std::hash is not used because its result differs between libstdc++ (Linux) and libc++ (macOS),
  // which would route the same key to different shards on the two machines and make results hard
  // to compare. FNV-1a alone mixes the low bits poorly and `% N` looks at exactly those bits, so
  // the finalizer spreads every input bit over the whole result.
  static uint64_t HashKey(std::string_view key) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (char c : key) {
      h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
      h *= 1099511628211ULL;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
  }

  // The CPU each shard's worker was running on right after pinning, or -1 if it was not pinned
  // (not requested, not Linux, or the kernel refused).
  std::vector<int> PinnedCpus() const {
    std::vector<int> cpus;
    cpus.reserve(shards_.size());
    for (const auto& shard : shards_) cpus.push_back(shard->cpu.load());
    return cpus;
  }

 private:
  // x86-64 and arm64 cache lines are 64 bytes. std::hardware_destructive_interference_size is not
  // used because GCC 11 (the Ubuntu 22.04 compiler) does not provide it.
  static constexpr size_t kCacheLine = 64;

  // One operation in flight. It lives on the CALLER's stack: the caller blocks in Wait() until the
  // worker has called Complete(), so the Request, and the key/value views and output pointer it
  // holds, outlive the worker's use of them. No allocation per request.
  struct Request {
    enum class Op { kPut, kGet, kDelete, kSize };

    Op op = Op::kGet;
    std::string_view key;
    std::string_view value;
    std::string* out = nullptr;  // Get: where the worker copies the value
    bool ok = false;             // Get: found. Delete: existed.
    size_t size = 0;             // Size: that shard's entry count

    void Wait() {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return done_; });
    }

    // Called by the worker. The worker's writes to ok/size/*out happen before the unlock here and
    // the caller's reads happen after the lock in Wait(), so the mutex orders them.
    void Complete() {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
      // Notify while STILL holding the lock. If we notified after unlocking, the waiter could
      // wake, return from Wait() and destroy this Request (it is on its stack) before we call
      // notify, and we would then be signalling a destroyed condition variable.
      cv_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
  };

  // alignas keeps two shards' hot data (queue mutex and counters) out of the same cache line, so
  // one shard's worker does not slow another by writing to a line they both hold (false sharing).
  struct alignas(kCacheLine) Shard {
    // BoundedQueue takes a capacity, MpscQueue does not (it is unbounded by design, D-018).
    explicit Shard(size_t queue_capacity)
      requires std::is_constructible_v<QueueT<Request*>, size_t>
        : queue(queue_capacity) {}
    explicit Shard(size_t /*queue_capacity*/)
      requires(!std::is_constructible_v<QueueT<Request*>, size_t>)
        : queue() {}

    void Start(int pin_cpu, std::latch& ready) {
      worker = std::thread([this, pin_cpu, &ready] { Run(pin_cpu, ready); });
    }

    void Run(int pin_cpu, std::latch& ready) {
      // Pin from inside the thread that is being pinned, so there is no window in which the
      // worker is running on the wrong core or being moved by someone else.
      if (pin_cpu >= 0 && PinCurrentThread(pin_cpu)) cpu.store(CurrentCpu());
      ready.count_down();  // last use of `ready`: the constructor may destroy it afterwards
      while (std::optional<Request*> next = queue.Pop()) Execute(**next);
    }

    void Execute(Request& request) {
      switch (request.op) {
        case Request::Op::kPut:
          store.Put(request.key, request.value);
          break;
        case Request::Op::kGet: {
          const auto value = store.Get(request.key);
          request.ok = value.has_value();
          if (value) request.out->assign(value->data(), value->size());
          break;
        }
        case Request::Op::kDelete:
          request.ok = store.Delete(request.key);
          break;
        case Request::Op::kSize:
          request.size = store.Size();
          break;
      }
      request.Complete();  // the caller may destroy `request` as soon as this returns: do not touch it
    }

    static bool PinCurrentThread(int cpu) {
#if defined(__linux__)
      if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(static_cast<size_t>(cpu), &set);
      return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
      (void)cpu;
      return false;
#endif
    }

    static int CurrentCpu() {
#if defined(__linux__)
      return sched_getcpu();
#else
      return -1;
#endif
    }

    StoreT store;                       // touched ONLY by `worker`: the shared-nothing part
    QueueT<Request*> queue;             // shared: any caller pushes, ONLY the worker pops
    std::atomic<int> cpu{-1};           // written once by the worker, read by PinnedCpus()
    std::thread worker;                 // declared last: started after everything above exists
  };

  void Dispatch(Request& request, size_t shard) const {
    if (!shards_[shard]->queue.Push(&request)) {
      throw std::logic_error("ShardedStore used during shutdown");  // nothing was enqueued
    }
    request.Wait();
  }

  // unique_ptr because a Shard holds a mutex and a thread and cannot be moved, and so each Shard
  // is a separate over-aligned heap object.
  std::vector<std::unique_ptr<Shard>> shards_;
};

// Phase 3: one mutex + two condition variables per shard queue.
using ShardedStore = BasicShardedStore<BoundedQueue>;
// Phase 4: lock-free MPSC queue per shard (D-018 to D-021). Identical in every other way.
using LockFreeShardedStore = BasicShardedStore<MpscQueue>;
using FlatShardedStore = BasicShardedStore<MpscQueue, FlatStore>;

}  // namespace lldstore
