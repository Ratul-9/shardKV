#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lldstore {

// Phase 9: epoch-based memory reclamation (EBR). Written by Claude at Ratul's request.
// Design decisions: docs/DESIGN.md D-061 to D-063.
//
// THE PROBLEM. A reader thread follows a pointer into a shared structure without a lock. The owner
// removes that node and wants to free it. If it frees immediately, the reader may still be about
// to read it: use-after-free. A mutex would solve it, but then readers block the writer.
//
// THE IDEA. Time is divided into epochs (a global counter). A reader announces "I am inside, and
// I entered in epoch e" before it touches the structure, and "I left" afterwards. A node removed
// while the global epoch is r is retired, not freed. The epoch can advance from g to g + 1 only
// when every reader currently inside has announced g. So once the global epoch reaches r + 2,
// every reader that could have seen the node (entered in r or before) has left, and it is freed.
//
// CONTRACT (D-062):
//   - ONE writer thread calls Retire() and Reclaim(). (Advancing the epoch from the writer only
//     makes the happens-before argument short: see Reclaim.) Any number of reader threads.
//   - Each reader thread registers once and uses its slot only from that thread.
//   - A reader inside a guard must not block for long: a stalled reader stops ALL reclamation
//     (memory grows, nothing breaks). That is the known weakness of EBR versus hazard pointers.
class EpochManager {
 public:
  static constexpr size_t kMaxReaders = 128;

  EpochManager() = default;
  ~EpochManager() {
    for (const Retired& r : limbo_) r.deleter(r.ptr);  // nobody can be reading any more
  }
  EpochManager(const EpochManager&) = delete;
  EpochManager& operator=(const EpochManager&) = delete;

  // Claims a reader slot. Throws if all kMaxReaders slots are taken.
  size_t RegisterReader() {
    for (size_t i = 0; i < kMaxReaders; ++i) {
      bool expected = false;
      if (slots_[i].in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        slots_[i].depth = 0;
        return i;
      }
    }
    throw std::runtime_error("EpochManager: too many reader threads");
  }
  void UnregisterReader(size_t slot) { slots_[slot].in_use.store(false, std::memory_order_release); }

  class Guard {
   public:
    Guard(EpochManager* m, size_t slot) : m_(m), slot_(slot) { m_->Enter(slot_); }
    ~Guard() { m_->Leave(slot_); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    EpochManager* m_;
    size_t slot_;
  };

  Guard Pin(size_t slot) { return Guard(this, slot); }

  // WRITER ONLY. `ptr` has already been unlinked: no NEW reader can reach it. It is freed with
  // `deleter` once no OLD reader can still hold it.
  void Retire(void* ptr, void (*deleter)(void*)) {
    limbo_.push_back(Retired{ptr, deleter, global_.load(std::memory_order_relaxed)});
    if (limbo_.size() >= kReclaimThreshold) Reclaim();
  }

  // WRITER ONLY. Advances the epoch if every reader inside is in the current one, then frees
  // everything retired at least two epochs ago. Returns the number of objects freed.
  size_t Reclaim() {
    TryAdvance();
    const uint64_t g = global_.load(std::memory_order_relaxed);
    size_t freed = 0, keep = 0;
    for (size_t i = 0; i < limbo_.size(); ++i) {
      if (limbo_[i].epoch + 2 <= g) {
        limbo_[i].deleter(limbo_[i].ptr);
        ++freed;
      } else {
        limbo_[keep++] = limbo_[i];
      }
    }
    limbo_.resize(keep);
    return freed;
  }

  uint64_t epoch() const noexcept { return global_.load(std::memory_order_relaxed); }
  size_t pending() const noexcept { return limbo_.size(); }  // writer only

 private:
  static constexpr uint64_t kOutside = std::numeric_limits<uint64_t>::max();
  static constexpr size_t kReclaimThreshold = 64;

  struct alignas(64) Slot {  // one cache line each: readers do not slow each other (false sharing)
    std::atomic<uint64_t> epoch{kOutside};
    std::atomic<bool> in_use{false};
    uint32_t depth = 0;  // nesting, touched only by the owning reader thread
  };

  struct Retired {
    void* ptr;
    void (*deleter)(void*);
    uint64_t epoch;
  };

  void Enter(size_t s) {
    Slot& slot = slots_[s];
    if (slot.depth++ > 0) return;  // nested guard: already announced
    uint64_t e = global_.load(std::memory_order_seq_cst);
    for (;;) {
      slot.epoch.store(e, std::memory_order_seq_cst);
      // Pairs with the fence in TryAdvance (D-063). Either the writer sees this announcement and
      // does not advance past e, or this reader sees every unlink the writer made before that
      // fence, so it cannot pick up a pointer the writer is about to free.
      std::atomic_thread_fence(std::memory_order_seq_cst);
      const uint64_t now = global_.load(std::memory_order_seq_cst);
      if (now == e) return;  // announced the CURRENT epoch
      e = now;               // the epoch moved while we announced: announce again
    }
  }

  void Leave(size_t s) {
    Slot& slot = slots_[s];
    if (--slot.depth > 0) return;
    // Release: every read this reader made happens before the writer's acquire load that sees
    // kOutside, and so before the free.
    slot.epoch.store(kOutside, std::memory_order_release);
  }

  void TryAdvance() {
    const uint64_t g = global_.load(std::memory_order_relaxed);  // only the writer changes it
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (const Slot& slot : slots_) {
      if (!slot.in_use.load(std::memory_order_acquire)) continue;
      const uint64_t e = slot.epoch.load(std::memory_order_acquire);
      if (e != kOutside && e != g) return;  // a reader is still in an older epoch
    }
    // Release: a reader whose acquire load sees g + 1 also sees everything the writer did before,
    // in particular every unlink of the objects retired in epoch g - 1 and earlier.
    global_.store(g + 1, std::memory_order_seq_cst);
  }

  alignas(64) std::atomic<uint64_t> global_{0};
  Slot slots_[kMaxReaders];
  std::vector<Retired> limbo_;  // writer only
};

}  // namespace lldstore
