#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "lldstore/store.h"

namespace lldstore {

// Phase 2: the "mutex baseline". Thread-safe wrappers around the single-threaded Store.
// Design decisions are in docs/DESIGN.md D-008.
//
// Unlike Store::Get, Get here COPIES the value into the caller's string, because a string_view
// returned after unlocking could dangle (docs/DESIGN.md D-002): another thread may overwrite or
// delete the key the moment the lock is released. The copy therefore happens while the lock is
// held. Get takes a destination pointer so a caller can reuse one buffer and avoid allocating a
// new std::string per call (std::string::assign reuses its capacity).
//
// Both classes are non-copyable and non-movable (a mutex cannot be copied or moved).

// One global std::mutex around everything: every operation, including reads, is exclusive.
class MutexStore {
 public:
  void Put(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_.Put(key, value);
  }

  // Returns false if the key is absent. On success copies the value into *out.
  bool Get(std::string_view key, std::string* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto value = store_.Get(key);
    if (!value) return false;
    out->assign(value->data(), value->size());  // copy while the lock is still held
    return true;
  }

  bool Delete(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.Delete(key);
  }

  // Locked too: unordered_map::size() reads state that a concurrent Put/Delete writes, which
  // would be a data race even though it is "just a read".
  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.Size();
  }

 private:
  Store store_;
  // mutable: Get and Size are const for the caller, but locking changes the mutex's state.
  mutable std::mutex mutex_;
};

// Same interface with std::shared_mutex. Get and Size take a shared (reader) lock, so any number
// of them may run at the same time; Put and Delete take an exclusive (writer) lock and run alone.
// A reader may only touch the Store through its const interface (Store::Get and Store::Size are
// const), which is what makes concurrent readers safe.
class SharedMutexStore {
 public:
  void Put(std::string_view key, std::string_view value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    store_.Put(key, value);
  }

  bool Get(std::string_view key, std::string* out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto value = store_.Get(key);
    if (!value) return false;
    out->assign(value->data(), value->size());  // *out is the caller's own buffer: not shared
    return true;
  }

  bool Delete(std::string_view key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return store_.Delete(key);
  }

  size_t Size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return store_.Size();
  }

 private:
  Store store_;
  mutable std::shared_mutex mutex_;
};

}  // namespace lldstore
