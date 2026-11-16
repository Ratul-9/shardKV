#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lldstore/epoch.h"
#include "lldstore/flat_store.h"

namespace lldstore {

// Phase 9: a hash map with ONE writer and lock-free readers, made safe by epoch-based reclamation.
// Written by Claude at Ratul's request. Design: docs/DESIGN.md D-064.
//
// This is the "background reader" the phase asks for: a snapshot, replication or metrics thread
// can read a shard while the shard's owner keeps changing it, without either one taking a lock.
//
// Layout: an array of buckets, each the head of a singly linked chain of IMMUTABLE nodes. The
// writer never changes a node a reader might be reading; it publishes a replacement:
//   Put (new key)      new node -> old head; bucket = new node              (release store)
//   Put (existing key) copy with the new value, same `next`; predecessor -> copy; retire old node
//   Delete             predecessor -> node->next; retire node (its own `next` stays intact, so a
//                      reader standing on it can still walk on)
//   Resize             build a complete new table with new nodes, publish it, retire the old one
// Readers: Pin a guard, load the table (acquire), walk the chain (acquire loads).
//
// kDeferFree = false gives UnsafeRcuMap, which deletes retired memory immediately. It exists only
// so a test can show that the naive version IS a use-after-free (caught by ASan).
template <bool kDeferFree>
class BasicRcuMap {
 public:
  BasicRcuMap() { table_.store(NewTable(kInitialBuckets), std::memory_order_relaxed); }
  ~BasicRcuMap() {
    // No reader may be inside at destruction (contract). Free the live table; the epoch manager
    // frees what is still retired.
    FreeTable(table_.load(std::memory_order_relaxed), /*with_nodes=*/true);
  }
  BasicRcuMap(const BasicRcuMap&) = delete;
  BasicRcuMap& operator=(const BasicRcuMap&) = delete;

  // ---- Readers (any registered thread) ------------------------------------------------------

  size_t RegisterReader() { return epochs_.RegisterReader(); }
  void UnregisterReader(size_t slot) { epochs_.UnregisterReader(slot); }
  EpochManager::Guard Pin(size_t slot) { return epochs_.Pin(slot); }

  // The value's bytes, valid until the guard is released. Copy them if they must outlive it.
  std::optional<std::string_view> Get(const EpochManager::Guard& /*proof of a pinned guard*/,
                                      std::string_view key) const {
    const Table* t = table_.load(std::memory_order_acquire);
    const uint64_t h = hasher_(key);
    for (const Node* n = t->buckets[h & t->mask].load(std::memory_order_acquire); n != nullptr;
         n = n->next.load(std::memory_order_acquire)) {
      if (n->hash == h && n->key == key) return std::string_view(n->value);
    }
    return std::nullopt;
  }

  // ---- The writer (one thread) --------------------------------------------------------------

  void Put(std::string_view key, std::string_view value) {
    Table* t = table_.load(std::memory_order_relaxed);
    const uint64_t h = hasher_(key);
    std::atomic<Node*>* link = &t->buckets[h & t->mask];
    for (Node* n = link->load(std::memory_order_relaxed); n != nullptr;
         link = &n->next, n = n->next.load(std::memory_order_relaxed)) {
      if (n->hash == h && n->key == key) {
        Node* copy = new Node{h, std::string(key), std::string(value), {}};
        copy->next.store(n->next.load(std::memory_order_relaxed), std::memory_order_relaxed);
        link->store(copy, std::memory_order_release);  // publishes the fully built copy
        Retire(n);
        return;
      }
    }
    Node* fresh = new Node{h, std::string(key), std::string(value), {}};
    std::atomic<Node*>& head = t->buckets[h & t->mask];
    fresh->next.store(head.load(std::memory_order_relaxed), std::memory_order_relaxed);
    head.store(fresh, std::memory_order_release);
    if (++t->size > t->mask + 1) Grow(t);
  }

  bool Delete(std::string_view key) {
    Table* t = table_.load(std::memory_order_relaxed);
    const uint64_t h = hasher_(key);
    std::atomic<Node*>* link = &t->buckets[h & t->mask];
    for (Node* n = link->load(std::memory_order_relaxed); n != nullptr;
         link = &n->next, n = n->next.load(std::memory_order_relaxed)) {
      if (n->hash == h && n->key == key) {
        link->store(n->next.load(std::memory_order_relaxed), std::memory_order_release);
        --t->size;
        Retire(n);
        return true;
      }
    }
    return false;
  }

  size_t Size() const { return table_.load(std::memory_order_relaxed)->size; }  // writer only
  size_t Reclaim() { return kDeferFree ? epochs_.Reclaim() : 0; }                 // writer only
  size_t PendingReclamation() const { return epochs_.pending(); }                 // writer only

 private:
  static constexpr size_t kInitialBuckets = 16;

  struct Node {
    uint64_t hash;
    std::string key;
    std::string value;
    std::atomic<Node*> next;
  };

  struct Table {
    size_t mask;
    size_t size;
    std::unique_ptr<std::atomic<Node*>[]> buckets;
  };

  static Table* NewTable(size_t buckets) {
    auto* t = new Table{buckets - 1, 0, std::make_unique<std::atomic<Node*>[]>(buckets)};
    for (size_t i = 0; i < buckets; ++i) t->buckets[i].store(nullptr, std::memory_order_relaxed);
    return t;
  }

  static void FreeTable(Table* t, bool with_nodes) {
    if (with_nodes) {
      for (size_t i = 0; i <= t->mask; ++i) {
        Node* n = t->buckets[i].load(std::memory_order_relaxed);
        while (n != nullptr) {
          Node* next = n->next.load(std::memory_order_relaxed);
          delete n;
          n = next;
        }
      }
    }
    delete t;
  }

  static void DeleteNode(void* p) { delete static_cast<Node*>(p); }
  static void DeleteTableAndNodes(void* p) { FreeTable(static_cast<Table*>(p), true); }

  void Retire(Node* n) {
    if constexpr (kDeferFree) {
      epochs_.Retire(n, &DeleteNode);
    } else {
      DeleteNode(n);  // UNSAFE: a reader may be standing on it
    }
  }

  // Copy-on-resize: the old table and its nodes stay intact for readers already inside them.
  void Grow(Table* old) {
    Table* t = NewTable((old->mask + 1) * 2);
    for (size_t i = 0; i <= old->mask; ++i) {
      for (Node* n = old->buckets[i].load(std::memory_order_relaxed); n != nullptr;
           n = n->next.load(std::memory_order_relaxed)) {
        Node* copy = new Node{n->hash, n->key, n->value, {}};
        std::atomic<Node*>& head = t->buckets[n->hash & t->mask];
        copy->next.store(head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        head.store(copy, std::memory_order_relaxed);  // not visible yet: no ordering needed
        ++t->size;
      }
    }
    table_.store(t, std::memory_order_release);  // publishes the whole new table at once
    if constexpr (kDeferFree) {
      epochs_.Retire(old, &DeleteTableAndNodes);
    } else {
      FreeTable(old, true);
    }
  }

  // Readers and the writer both touch table_, so it is on its own cache line.
  alignas(64) std::atomic<Table*> table_{nullptr};
  FlatHash hasher_;
  EpochManager epochs_;
};

using RcuMap = BasicRcuMap<true>;
using UnsafeRcuMap = BasicRcuMap<false>;

}  // namespace lldstore
