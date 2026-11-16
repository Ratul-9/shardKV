#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "lldstore/slab_allocator.h"

namespace lldstore {

struct FlatHash {
  uint64_t operator()(std::string_view key) const noexcept {
    uint64_t h = kSeed ^ (static_cast<uint64_t>(key.size()) * kMulA);
    const char* p = key.data();
    size_t n = key.size();
    while (n >= 8) {
      uint64_t word;
      std::memcpy(&word, p, 8);
      h = Absorb(h, word);
      p += 8;
      n -= 8;
    }
    if (n > 0) {
      uint64_t word = 0;
      std::memcpy(&word, p, n);
      h = Absorb(h, word);
    }
    return Finalize(h);
  }

 private:
  static constexpr uint64_t kSeed = 0x243f6a8885a308d3ULL;
  static constexpr uint64_t kMulA = 0x9e3779b97f4a7c15ULL;
  static constexpr uint64_t kMulB = 0xbf58476d1ce4e5b9ULL;
  static constexpr uint64_t kMulC = 0x94d049bb133111ebULL;

  static uint64_t Absorb(uint64_t h, uint64_t word) noexcept {
    return std::rotl(h ^ (word * kMulA), 29) * kMulB;
  }

  static uint64_t Finalize(uint64_t h) noexcept {
    h ^= h >> 30;
    h *= kMulB;
    h ^= h >> 27;
    h *= kMulC;
    h ^= h >> 31;
    return h;
  }
};

template <typename Hasher, typename Alloc = HeapAllocator>
class BasicFlatStore {
 public:
  static constexpr double kDefaultMaxLoadFactor = 0.75;
  static constexpr double kMinMaxLoadFactor = 0.25;
  static constexpr double kMaxMaxLoadFactor = 0.95;
  static constexpr size_t kMinCapacity = 16;
  static constexpr size_t kCacheLine = 64;

  BasicFlatStore() = default;

  explicit BasicFlatStore(double max_load_factor)
      : max_load_factor_(std::clamp(max_load_factor, kMinMaxLoadFactor, kMaxMaxLoadFactor)) {}

  ~BasicFlatStore() {
    for (size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].entry != nullptr) FreeEntry(slots_[i].entry);
    }
    FreeSlots(slots_);
  }

  BasicFlatStore(const BasicFlatStore&) = delete;
  BasicFlatStore& operator=(const BasicFlatStore&) = delete;

  void Put(std::string_view key, std::string_view value) {
    CheckLength(key.size());
    CheckLength(value.size());
    const uint64_t hash = hasher_(key);
    if (capacity_ == 0) Rehash(kMinCapacity);
    size_t index = Probe(key, hash);
    if (slots_[index].entry != nullptr) {
      Assign(slots_[index], value);
      return;
    }
    if (size_ + 1 > growth_limit_) {
      Rehash(capacity_ * 2);
      index = ProbeEmpty(hash);
    }
    slots_[index] = Slot{hash, MakeEntry(key, value)};
    ++size_;
  }

  std::optional<std::string_view> Get(std::string_view key) const {
    if (size_ == 0) return std::nullopt;
    const Entry* entry = slots_[Probe(key, hasher_(key))].entry;
    if (entry == nullptr) return std::nullopt;
    return entry->Value();
  }

  bool Delete(std::string_view key) {
    if (size_ == 0) return false;
    size_t hole = Probe(key, hasher_(key));
    if (slots_[hole].entry == nullptr) return false;
    FreeEntry(slots_[hole].entry);
    for (size_t next = (hole + 1) & mask_; slots_[next].entry != nullptr;
         next = (next + 1) & mask_) {
      const size_t home = Home(slots_[next].hash);
      if (((next - home) & mask_) >= ((next - hole) & mask_)) {
        slots_[hole] = slots_[next];
        hole = next;
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
  }

  void Reserve(size_t entries) {
    size_t capacity = std::max(capacity_, kMinCapacity);
    while (LimitFor(capacity) < entries) capacity *= 2;
    if (capacity != capacity_) Rehash(capacity);
  }

  size_t Size() const noexcept { return size_; }
  size_t Capacity() const noexcept { return capacity_; }
  double MaxLoadFactor() const noexcept { return max_load_factor_; }

  double LoadFactor() const noexcept {
    return capacity_ == 0 ? 0.0 : static_cast<double>(size_) / static_cast<double>(capacity_);
  }

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].entry != nullptr) fn(slots_[i].entry->Key(), slots_[i].entry->Value());
    }
  }

  const Alloc& allocator() const noexcept { return alloc_; }

 private:
  struct Entry {
    uint32_t key_size;
    uint32_t value_size;
    uint32_t value_capacity;

    char* KeyData() noexcept { return reinterpret_cast<char*>(this + 1); }
    const char* KeyData() const noexcept { return reinterpret_cast<const char*>(this + 1); }
    char* ValueData() noexcept { return KeyData() + key_size; }
    const char* ValueData() const noexcept { return KeyData() + key_size; }
    std::string_view Key() const noexcept { return {KeyData(), key_size}; }
    std::string_view Value() const noexcept { return {ValueData(), value_size}; }
  };

  struct Slot {
    uint64_t hash = 0;
    Entry* entry = nullptr;
  };

  static_assert(sizeof(Slot) == 16);
  static_assert(kCacheLine % sizeof(Slot) == 0);

  size_t Home(uint64_t hash) const noexcept { return static_cast<size_t>(hash >> shift_); }

  size_t Probe(std::string_view key, uint64_t hash) const noexcept {
    for (size_t i = Home(hash);; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.entry == nullptr || (slot.hash == hash && slot.entry->Key() == key)) return i;
    }
  }

  size_t ProbeEmpty(uint64_t hash) const noexcept {
    size_t i = Home(hash);
    while (slots_[i].entry != nullptr) i = (i + 1) & mask_;
    return i;
  }

  size_t LimitFor(size_t capacity) const noexcept {
    return static_cast<size_t>(static_cast<double>(capacity) * max_load_factor_);
  }

  void Rehash(size_t new_capacity) {
    Slot* fresh = AllocateSlots(new_capacity);
    Slot* old = slots_;
    const size_t old_capacity = capacity_;
    slots_ = fresh;
    capacity_ = new_capacity;
    mask_ = new_capacity - 1;
    shift_ = 64 - static_cast<unsigned>(std::countr_zero(new_capacity));
    growth_limit_ = LimitFor(new_capacity);
    for (size_t i = 0; i < old_capacity; ++i) {
      if (old[i].entry != nullptr) slots_[ProbeEmpty(old[i].hash)] = old[i];
    }
    FreeSlots(old);
  }

  void Assign(Slot& slot, std::string_view value) {
    Entry* entry = slot.entry;
    if (value.size() <= entry->value_capacity) {
      CopyBytes(entry->ValueData(), value);
      entry->value_size = static_cast<uint32_t>(value.size());
      return;
    }
    slot.entry = MakeEntry(entry->Key(), value);
    FreeEntry(entry);
  }

  static Slot* AllocateSlots(size_t count) {
    void* raw = ::operator new(count * sizeof(Slot), std::align_val_t{kCacheLine});
    Slot* slots = static_cast<Slot*>(raw);
    std::uninitialized_value_construct_n(slots, count);
    return slots;
  }

  static void FreeSlots(Slot* slots) noexcept {
    if (slots != nullptr) ::operator delete(slots, std::align_val_t{kCacheLine});
  }

  Entry* MakeEntry(std::string_view key, std::string_view value) {
    const size_t usable = Alloc::UsableSize(sizeof(Entry) + key.size() + value.size());
    const size_t spare = std::min<size_t>(usable - sizeof(Entry) - key.size(),
                                          std::numeric_limits<uint32_t>::max());
    void* raw = alloc_.Allocate(sizeof(Entry) + key.size() + spare);
    Entry* entry = ::new (raw) Entry{static_cast<uint32_t>(key.size()),
                                     static_cast<uint32_t>(value.size()),
                                     static_cast<uint32_t>(spare)};
    CopyBytes(entry->KeyData(), key);
    CopyBytes(entry->ValueData(), value);
    return entry;
  }

  void FreeEntry(Entry* entry) noexcept {
    alloc_.Deallocate(entry, sizeof(Entry) + entry->key_size + entry->value_capacity);
  }

  static void CopyBytes(char* destination, std::string_view source) noexcept {
    if (!source.empty()) std::memmove(destination, source.data(), source.size());
  }

  static void CheckLength(size_t length) {
    if (length > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("FlatStore: keys and values are limited to 4 GiB - 1 bytes");
    }
  }

  Slot* slots_ = nullptr;
  size_t capacity_ = 0;
  size_t mask_ = 0;
  size_t size_ = 0;
  size_t growth_limit_ = 0;
  unsigned shift_ = 64;
  double max_load_factor_ = kDefaultMaxLoadFactor;
  [[no_unique_address]] Hasher hasher_;
  [[no_unique_address]] Alloc alloc_;
};

using FlatStore = BasicFlatStore<FlatHash>;
using SlabFlatStore = BasicFlatStore<FlatHash, SlabAllocator>;

}
