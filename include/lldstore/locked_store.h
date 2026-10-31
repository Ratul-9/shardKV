#pragma once

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "lldstore/store.h"

namespace lldstore {

// Phase 2: the "mutex baseline". Thread-safe wrappers around the single-threaded Store.
//
// Unlike Store::Get, Get here COPIES the value into the caller's string, because a string_view
// returned after unlocking could dangle (docs/DESIGN.md D-002). The copy must happen while the
// lock is held. Get takes a destination string pointer so the caller can reuse one buffer instead
// of allocating a fresh std::string per call.
//
// TODO(Ratul): implement both classes. The bodies below abort so nothing mistakes this skeleton
// for working code. Questions to answer in docs/DESIGN.md:
//  - Which lock does each method take, and why? (std::lock_guard / unique_lock / shared_lock)
//  - Get is `const` but must lock: what does that force on the mutex member?
//  - Which operations may run concurrently in SharedMutexStore, which may not?
//  - Is Size() safe to call without a lock? Why or why not?

// One global std::mutex around everything.
class MutexStore {
 public:
  void Put(std::string_view /*key*/, std::string_view /*value*/) { std::abort(); }

  // Returns false if the key is absent. On success copies the value into *out.
  bool Get(std::string_view /*key*/, std::string* /*out*/) const { std::abort(); }

  bool Delete(std::string_view /*key*/) { std::abort(); }

  size_t Size() const { std::abort(); }

 private:
  // TODO(Ratul): Store, mutex.
};

// Same interface, std::shared_mutex: concurrent readers, exclusive writers.
class SharedMutexStore {
 public:
  void Put(std::string_view /*key*/, std::string_view /*value*/) { std::abort(); }

  bool Get(std::string_view /*key*/, std::string* /*out*/) const { std::abort(); }

  bool Delete(std::string_view /*key*/) { std::abort(); }

  size_t Size() const { std::abort(); }

 private:
  // TODO(Ratul): Store, shared_mutex.
};

}  // namespace lldstore
