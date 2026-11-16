#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace lldstore {

// Phase 4: unbounded multi-producer, SINGLE-consumer queue, after Dmitry Vyukov's intrusive MPSC
// node queue. Design decisions: docs/DESIGN.md D-018 to D-021. Written by Claude at Ratul's request.
//
// Shape: a singly linked list. `tail_` is where producers add; `head_` is where the one consumer
// reads. The head always points at a "stub" node whose value has already been taken (or never
// existed), so the list is never empty and head/tail are never null.
//
//     head_ -> [stub] -> [A] -> [B] -> [C]        <- tail_
//
// Push (any thread): link a new node after the current tail with ONE atomic exchange, then publish
// the link. The exchange has no retry loop, so a push never waits for another producer.
// TryPop (the consumer only): look at head_->next. If it is set, take that node's value, make that
// node the new stub and free the old stub.
//
// THE CONTRACT THE CODE CANNOT CHECK: exactly one thread ever calls TryPop()/Pop(). head_ is a
// plain pointer because of it. Two consumers would be a data race.
//
// Interface matches BoundedQueue (Push, Pop, Stop, Size) so ShardedStore can use either, with two
// differences: there is no capacity (a slow consumer lets the queue grow without limit, so there
// is NO backpressure), and TryPop() is added.
template <typename T>
class MpscQueue {
 public:
  MpscQueue() : head_(new Node), tail_(head_) {}

  // Frees any nodes still queued. Single-threaded by contract: no other thread may be using the
  // queue while it is destroyed.
  ~MpscQueue() {
    Node* node = head_;
    while (node != nullptr) {
      Node* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
  }

  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  // Any thread. Returns true if the item was enqueued, false if the queue has been stopped (then
  // nothing was enqueued). Never blocks. Allocates one node (see D-018).
  bool Push(T item) {
    std::unique_ptr<Node> node(new Node);
    node->value.emplace(std::move(item));

    // Claim a slot in the shared state BEFORE touching the list. The state word holds the number of
    // pushed-but-not-yet-popped items (low bits) and the stopped flag (top bit), so "am I stopped?"
    // and "I am now in flight" are decided by ONE atomic operation. That is what lets Pop() tell
    // "stopped and truly empty" from "stopped, but a producer is still linking its node" (D-020).
    const uint64_t before = state_.fetch_add(1, std::memory_order_acq_rel);
    if ((before & kStopped) != 0) {
      state_.fetch_sub(1, std::memory_order_release);  // undo; nothing was linked
      return false;
    }

    Node* const fresh = node.release();
    // Step 1: make `fresh` the new tail. acq_rel:
    //  - release: the previous tail's producer, and anyone who later exchanges with us, must see
    //    `fresh` fully constructed (value, next == nullptr) before we are visible as the tail.
    //  - acquire: we get the previous tail pointer `prev` and must see that node fully constructed
    //    before we write prev->next below (otherwise our store could be overwritten by its
    //    constructor's nullptr, in the abstract machine).
    Node* const prev = tail_.exchange(fresh, std::memory_order_acq_rel);
    // Step 2: link prev -> fresh. RELEASE publishes `fresh` and its value to the consumer, whose
    // matching acquire load of prev->next is in TryPop().
    // Between step 1 and step 2 the list is "inconsistent": tail_ already points at `fresh` but
    // nothing points to `fresh` yet, so the consumer cannot reach it. D-020.
    prev->next.store(fresh, std::memory_order_release);

    // Wake the consumer if it is parked in Pop(). It parks on state_, which this push changed, so
    // the wake-up cannot be lost (the futex-style wait compares the word atomically).
    state_.notify_one();
    return true;
  }

  // CONSUMER ONLY. Never blocks. Returns std::nullopt if no item is reachable right now. That can
  // mean "empty", or "a producer has claimed a slot but not linked its node yet": the caller
  // cannot tell which, only Pop() (which looks at the state word) can.
  std::optional<T> TryPop() {
    Node* const stub = head_;
    // ACQUIRE pairs with the producer's RELEASE store in Push(): once we see `next`, we also see
    // its value.
    Node* const next = stub->next.load(std::memory_order_acquire);
    if (next == nullptr) return std::nullopt;

    std::optional<T> out(std::move(next->value));  // move the value out of the new stub
    next->value.reset();
    head_ = next;
    // Safe to free the old stub: the only thread that ever writes stub->next is the one producer
    // that got `stub` back from its exchange, and we just saw that it has already done so. No
    // other thread holds a pointer to it. No compare-and-swap is used, so there is no ABA.
    delete stub;
    state_.fetch_sub(1, std::memory_order_release);
    return out;
  }

  // CONSUMER ONLY. Blocks until an item is available. Returns std::nullopt only when the queue is
  // stopped AND drained, so `while (auto item = Pop())` ends cleanly (same as BoundedQueue).
  std::optional<T> Pop() {
    for (;;) {
      if (std::optional<T> item = TryPop()) return item;
      const uint64_t state = state_.load(std::memory_order_acquire);
      if (state == kStopped) return std::nullopt;  // stopped, nothing queued, nothing in flight
      if (state == 0) {
        // Empty and running: sleep until state_ changes (a push or Stop). wait() returns at once
        // if state_ is no longer 0, so a push that slips in here is not missed.
        state_.wait(0, std::memory_order_acquire);
      } else {
        // state > 0 but TryPop found nothing: a producer has claimed a slot but not finished
        // linking its node (a few instructions' window), or has just rejected itself after Stop.
        // Yield instead of sleeping: there is nothing to be woken by.
        std::this_thread::yield();
      }
    }
  }

  // Idempotent, any thread. After it returns, Push rejects. Items whose Push already returned true
  // are still delivered by Pop before it reports "stopped and drained".
  void Stop() {
    state_.fetch_or(kStopped, std::memory_order_acq_rel);
    state_.notify_all();
  }

  // Approximate when called while others push or pop: it also counts pushes still in flight.
  size_t Size() const noexcept {
    return static_cast<size_t>(state_.load(std::memory_order_relaxed) & ~kStopped);
  }

 private:
  static constexpr size_t kCacheLine = 64;
  static constexpr uint64_t kStopped = uint64_t{1} << 63;

  struct Node {
    std::atomic<Node*> next{nullptr};
    std::optional<T> value;  // empty for the stub
  };

  // Three different writers, so three different cache lines: producers hammer tail_ and state_,
  // the consumer owns head_. Without alignas, head_ would share a line with something producers
  // write and every push would slow the consumer (false sharing).
  alignas(kCacheLine) Node* head_;                    // consumer only
  alignas(kCacheLine) std::atomic<Node*> tail_;       // producers (exchange)
  alignas(kCacheLine) std::atomic<uint64_t> state_{0};  // in-flight count | kStopped
};

}  // namespace lldstore
