#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>

namespace lldstore {

// Phase 6: the INTRUSIVE version of the Phase 4 MPSC queue. Written by Claude at Ratul's request.
// Design decision: docs/DESIGN.md D-035.
//
// MpscQueue<T> allocated one Node per Push, which put a heap allocation on every request's path.
// Here the link lives inside the item itself: NodePtr must be a pointer to a type with a member
//     std::atomic<Node*> queue_next;
// ShardedStore's Request has one, and a Request lives on the caller's stack until the worker has
// completed it, so a push allocates nothing.
//
// Algorithm: Dmitry Vyukov's intrusive MPSC queue. The queue owns one stub node of the item type,
// so head and tail are never null. Push is the same XCHG pattern as MpscQueue (wait-free). Pop is
// more involved than the node-based version because the stub is not always at the head: when the
// consumer reaches the last item, it re-inserts the stub behind it, so that it can hand out that
// last item without leaving the list empty.
//
// The same state word as MpscQueue (count of in-flight items | stopped bit) drives parking and
// draining (D-020), so Stop/Pop semantics are identical.
//
// Single consumer is a contract (not checked). An item must not be pushed again until it has been
// popped, and must stay alive until it has been popped.
template <typename NodePtr>
class IntrusiveMpscQueue {
  static_assert(std::is_pointer_v<NodePtr>, "IntrusiveMpscQueue holds pointers to items");
  using Node = std::remove_pointer_t<NodePtr>;

 public:
  IntrusiveMpscQueue() : head_(&stub_), tail_(&stub_) {
    stub_.queue_next.store(nullptr, std::memory_order_relaxed);
  }

  IntrusiveMpscQueue(const IntrusiveMpscQueue&) = delete;
  IntrusiveMpscQueue& operator=(const IntrusiveMpscQueue&) = delete;

  bool Push(NodePtr node) {
    const uint64_t before = state_.fetch_add(1, std::memory_order_acq_rel);
    if ((before & kStopped) != 0) {
      state_.fetch_sub(1, std::memory_order_release);
      return false;
    }
    Link(node);
    state_.notify_one();
    return true;
  }

  // CONSUMER ONLY. Never blocks.
  std::optional<NodePtr> TryPop() {
    Node* head = head_;
    Node* next = head->queue_next.load(std::memory_order_acquire);
    if (head == &stub_) {
      if (next == nullptr) return std::nullopt;  // empty (or the first push is mid-link)
      head_ = next;                              // skip over the stub
      head = next;
      next = next->queue_next.load(std::memory_order_acquire);
    }
    if (next != nullptr) {  // head is not the last node: hand it out
      head_ = next;
      return Taken(head);
    }
    // head is the last node we can see. If a producer has already swapped the tail past it, it is
    // mid-push: its link will appear shortly. Report nothing; Pop() yields and retries.
    if (head != tail_.load(std::memory_order_acquire)) return std::nullopt;
    // head really is the last node. Put the stub behind it so head can be handed out while the
    // list stays non-empty.
    Link(&stub_);
    next = head->queue_next.load(std::memory_order_acquire);
    if (next != nullptr) {
      head_ = next;
      return Taken(head);
    }
    return std::nullopt;  // a producer slipped in between the tail check and our stub push
  }

  // CONSUMER ONLY. Blocks; returns nullopt only when stopped and drained (as MpscQueue::Pop).
  std::optional<NodePtr> Pop() {
    for (;;) {
      if (std::optional<NodePtr> item = TryPop()) return item;
      const uint64_t state = state_.load(std::memory_order_acquire);
      if (state == kStopped) return std::nullopt;
      if (state == 0) {
        state_.wait(0, std::memory_order_acquire);
      } else {
        std::this_thread::yield();
      }
    }
  }

  void Stop() {
    state_.fetch_or(kStopped, std::memory_order_acq_rel);
    state_.notify_all();
  }

  size_t Size() const noexcept {
    return static_cast<size_t>(state_.load(std::memory_order_relaxed) & ~kStopped);
  }

 private:
  static constexpr size_t kCacheLine = 64;
  static constexpr uint64_t kStopped = uint64_t{1} << 63;

  // The two-step XCHG insert (D-019 has the memory-order reasoning; it is identical here).
  void Link(Node* node) {
    node->queue_next.store(nullptr, std::memory_order_relaxed);
    Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
    prev->queue_next.store(node, std::memory_order_release);
  }

  NodePtr Taken(Node* node) {
    state_.fetch_sub(1, std::memory_order_release);
    return node;
  }

  alignas(kCacheLine) Node* head_;               // consumer only
  alignas(kCacheLine) std::atomic<Node*> tail_;  // producers
  alignas(kCacheLine) std::atomic<uint64_t> state_{0};
  alignas(kCacheLine) Node stub_;                // never handed out
};

}  // namespace lldstore
