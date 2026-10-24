#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lldstore {

// Phase 1 baseline: single-threaded in-process key-value store, backed by std::unordered_map.
// NOT thread-safe by design; Phase 2 adds locking around it.
//
// Get returns a non-owning string_view (docs/DESIGN.md D-002), so a returned view is only valid
// until the next call that mutates the store.
class Store {
 public:
  // Inserts the key, or overwrites its value if it already exists.
  void Put(std::string_view key, std::string_view value) {
    // One lookup, whether the key is new or not. For an existing key, the value is assigned into
    // the existing std::string, which can reuse its buffer instead of freeing and reallocating.
    // The key is copied into a std::string only because the map is keyed by std::string.
    data_.insert_or_assign(std::string(key), value);
  }

  // Returns the value, or std::nullopt if the key is absent. The view is valid only until the next
  // call that mutates the store (Put or Delete).
  std::optional<std::string_view> Get(std::string_view key) const {
    // BASELINE COST: building a std::string for the lookup allocates when the key is longer than
    // the small-string buffer. A transparent hash/equality would avoid it (see DESIGN.md).
    const auto it = data_.find(std::string(key));
    if (it == data_.end()) {
      return std::nullopt;
    }
    return std::string_view(it->second);  // view into the map's own string, not a copy
  }

  // Returns true if the key existed and was removed.
  bool Delete(std::string_view key) {
    const auto it = data_.find(std::string(key));
    if (it == data_.end()) {
      return false;
    }
    data_.erase(it);  // erase by iterator: no second hash and lookup
    return true;
  }

  size_t Size() const { return data_.size(); }

 private:
  // The map is the single source of truth for the contents and for the count, so there is no
  // separate counter that could drift out of sync with it.
  std::unordered_map<std::string, std::string> data_;
};

}  // namespace lldstore
