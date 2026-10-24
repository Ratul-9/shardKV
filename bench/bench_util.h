#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "lldstore/store.h"

namespace lldstore::bench {

constexpr size_t kKeySize = 16;    // "key:" + 12 digits
constexpr size_t kValueSize = 64;  // fixed payload for the baseline

// Cheap deterministic RNG so the benchmark loop does not pay for <random>.
struct XorShift64 {
  uint64_t s;
  explicit XorShift64(uint64_t seed) : s(seed != 0 ? seed : 1) {}
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

// Formats key number `i` into `buf` without allocating. Keys are generated on the fly instead of
// stored in a vector: a precomputed key table would add its own cache misses to every lookup.
inline std::string_view FormatKey(uint64_t i, char (&buf)[kKeySize]) {
  std::memcpy(buf, "key:", 4);
  for (size_t d = kKeySize; d-- > 4;) {
    buf[d] = static_cast<char>('0' + i % 10);
    i /= 10;
  }
  return {buf, kKeySize};
}

inline std::string_view Value() {
  static const std::string value(kValueSize, 'v');
  return value;
}

inline void Prefill(Store& store, uint64_t n) {
  char buf[kKeySize];
  for (uint64_t i = 0; i < n; ++i) store.Put(FormatKey(i, buf), Value());
}

}  // namespace lldstore::bench
