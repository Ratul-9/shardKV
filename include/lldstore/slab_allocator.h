#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

// AddressSanitizer does not know about memory we recycle ourselves: a block on our free list is
// "live" to ASan because operator new returned it as part of a chunk. Poisoning freed blocks makes
// ASan report a use-after-free into the slab exactly as it would for malloc'd memory (D-032).
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LLDSTORE_ASAN_ENABLED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define LLDSTORE_ASAN_ENABLED 1
#endif

#if defined(LLDSTORE_ASAN_ENABLED)
#include <sanitizer/asan_interface.h>
#define LLDSTORE_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define LLDSTORE_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define LLDSTORE_POISON(p, n) ((void)(p), (void)(n))
#define LLDSTORE_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

namespace lldstore {

// Phase 6: size-class slab allocator for one shard. Written by Claude at Ratul's request.
// Design decisions: docs/DESIGN.md D-031 to D-034.
//
// - SINGLE-THREADED by design. Each shard's worker owns its allocator and is the only thread that
//   allocates or frees through it, so there is no lock and no cross-thread free (D-033).
// - Sized deallocation: the caller passes the size back to Deallocate (FlatStore knows every
//   entry's size), so blocks carry no header.
// - 36 size classes from 16 B to 16 KiB: 16-byte steps up to 128 B, then 4 classes per doubling,
//   so internal waste is at most 25% above 128 B (12.5% on average) (D-031).
// - Memory comes from 256 KiB chunks carved with a bump pointer; freed blocks go on a per-class
//   free list and are reused before the chunk is touched again (LIFO, so the most recently freed,
//   most likely cached, block is reused first).
// - Chunks are returned to the OS only when the allocator is destroyed (D-034). A shard that grew
//   and then shrank keeps its peak footprint.
// - Requests above 16 KiB go straight to operator new ("large" allocations).
class SlabAllocator {
 public:
  static constexpr size_t kMaxSmall = 16384;
  static constexpr size_t kChunkSize = 256 * 1024;
  static constexpr size_t kNumClasses = 36;
  static constexpr size_t kAlignment = 16;
  static constexpr size_t kChunkAlignment = 64;

  struct Stats {
    size_t requested_bytes = 0;   // sum of sizes the caller asked for (live blocks)
    size_t allocated_bytes = 0;   // sum of class sizes handed out (live blocks)
    size_t reserved_bytes = 0;    // chunk bytes + large blocks obtained from operator new
    size_t live_blocks = 0;
    size_t chunks = 0;
    size_t large_blocks = 0;
    size_t chunk_refills = 0;     // times a class had to start a fresh chunk region
  };

  SlabAllocator() = default;
  ~SlabAllocator() {
    for (Chunk& chunk : chunks_) {
      LLDSTORE_UNPOISON(chunk.base, chunk.size);
      ::operator delete(chunk.base, std::align_val_t{kChunkAlignment});
    }
  }

  SlabAllocator(const SlabAllocator&) = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;

  // The number of bytes a block of `n` really occupies. A caller may use all of them, and must
  // pass a size with the same UsableSize back to Deallocate.
  static constexpr size_t UsableSize(size_t n) noexcept {
    return n > kMaxSmall ? n : ClassSize(ClassIndex(n == 0 ? 1 : n));
  }

  void* Allocate(size_t n) {
    if (n == 0) n = 1;
    stats_.requested_bytes += n;
    ++stats_.live_blocks;
    if (n > kMaxSmall) {
      ++stats_.large_blocks;
      stats_.reserved_bytes += n;
      stats_.allocated_bytes += n;
      return ::operator new(n);
    }
    const size_t c = ClassIndex(n);
    const size_t size = ClassSize(c);
    stats_.allocated_bytes += size;
    Class& cls = classes_[c];
    if (cls.free_list != nullptr) {
      FreeBlock* block = cls.free_list;
      LLDSTORE_UNPOISON(block, size);
      cls.free_list = block->next;
      return block;
    }
    if (static_cast<size_t>(cls.end - cls.cursor) < size) Refill(cls, size);
    void* block = cls.cursor;
    cls.cursor += size;
    LLDSTORE_UNPOISON(block, size);
    return block;
  }

  void Deallocate(void* p, size_t n) noexcept {
    if (p == nullptr) return;
    if (n == 0) n = 1;
    stats_.requested_bytes -= n;
    --stats_.live_blocks;
    if (n > kMaxSmall) {
      --stats_.large_blocks;
      stats_.reserved_bytes -= n;
      stats_.allocated_bytes -= n;
      ::operator delete(p);
      return;
    }
    const size_t c = ClassIndex(n);
    const size_t size = ClassSize(c);
    stats_.allocated_bytes -= size;
    Class& cls = classes_[c];
    FreeBlock* block = static_cast<FreeBlock*>(p);
    block->next = cls.free_list;
    cls.free_list = block;
    LLDSTORE_POISON(block, size);
  }

  const Stats& stats() const noexcept { return stats_; }

  // Size-class arithmetic, public so tests can check it directly.
  static constexpr size_t ClassIndex(size_t n) noexcept {
    if (n <= 128) return (n + 15) / 16 - 1;
    const unsigned lg = static_cast<unsigned>(std::bit_width(n - 1)) - 1;  // floor(log2(n - 1))
    const size_t step = size_t{1} << (lg - 2);                             // 4 classes per doubling
    const size_t sub = (n - 1 - (size_t{1} << lg)) / step;
    return 8 + (lg - 7) * 4 + sub;
  }

  static constexpr size_t ClassSize(size_t c) noexcept {
    if (c < 8) return (c + 1) * 16;
    const size_t k = c - 8;
    const unsigned lg = 7 + static_cast<unsigned>(k / 4);
    return (size_t{1} << lg) + (k % 4 + 1) * (size_t{1} << (lg - 2));
  }

 private:
  struct FreeBlock {
    FreeBlock* next;
  };

  struct Class {
    FreeBlock* free_list = nullptr;
    char* cursor = nullptr;  // bump region inside the newest chunk this class owns
    char* end = nullptr;
  };

  struct Chunk {
    char* base;
    size_t size;
  };

  // Gives the class a fresh chunk. The tail of the old region (smaller than one block) is
  // abandoned: at most one block's worth per chunk, at most 6% for the 16 KiB class.
  void Refill(Class& cls, size_t block_size) {
    (void)block_size;
    char* base = static_cast<char*>(::operator new(kChunkSize, std::align_val_t{kChunkAlignment}));
    chunks_.push_back(Chunk{base, kChunkSize});
    LLDSTORE_POISON(base, kChunkSize);
    cls.cursor = base;
    cls.end = base + kChunkSize;
    stats_.reserved_bytes += kChunkSize;
    ++stats_.chunks;
    ++stats_.chunk_refills;
  }

  std::array<Class, kNumClasses> classes_{};
  std::vector<Chunk> chunks_;
  Stats stats_;
};

static_assert(SlabAllocator::ClassSize(SlabAllocator::kNumClasses - 1) == SlabAllocator::kMaxSmall);
static_assert(SlabAllocator::ClassIndex(SlabAllocator::kMaxSmall) == SlabAllocator::kNumClasses - 1);
static_assert(SlabAllocator::ClassIndex(129) == 8 && SlabAllocator::ClassSize(8) == 160);
static_assert(SlabAllocator::kChunkSize % SlabAllocator::kMaxSmall == 0);

// The default policy for FlatStore: plain operator new/delete, no size rounding.
struct HeapAllocator {
  static constexpr size_t UsableSize(size_t n) noexcept { return n; }
  void* Allocate(size_t n) { return ::operator new(n); }
  void Deallocate(void* p, size_t /*n*/) noexcept { ::operator delete(p); }
};

}  // namespace lldstore
