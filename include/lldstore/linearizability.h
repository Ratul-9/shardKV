#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lldstore::lin {

// Phase 10: a linearizability checker for key-value histories. Written by Claude at Ratul's
// request. Design: docs/DESIGN.md D-055 to D-057.
//
// A history is what clients saw: for each operation, when it was invoked, when its response
// arrived, and the response. The history is LINEARIZABLE if every operation can be placed at a
// single instant between its invocation and its response such that the resulting sequence is a
// legal run of a single-copy key-value map (Herlihy and Wing, 1990).
//
// Method: Wing and Gong's search with Lowe's memoisation, as in Porcupine. Because every operation
// touches one key, the history is split per key and each key is checked separately (a history is
// linearizable iff each per-key sub-history is: linearizability is "local"). That makes the
// otherwise exponential search cheap in practice.
//
// Operations whose outcome the client never learned (timeout, connection lost) are "unknown":
// they may have taken effect at any time after their invocation, or never. Unknown reads are
// dropped (they constrain nothing); unknown writes may be linearized anywhere after their call
// or left out.

enum class OpType : uint8_t { kPut = 0, kGet = 1, kDelete = 2 };

constexpr int64_t kNoReturn = std::numeric_limits<int64_t>::max();

struct Operation {
  uint64_t client = 0;
  OpType type = OpType::kGet;
  std::string key;
  std::string value;     // Put: the value written
  int64_t call = 0;      // invocation time
  int64_t ret = 0;       // response time; kNoReturn if unknown
  bool known = true;     // false: the client never learned the outcome
  bool found = false;    // Get: key present. Delete: key existed.
  std::string output;    // Get: value read (when found)
};

enum class Verdict { kLinearizable, kViolation, kGaveUp };

struct CheckResult {
  Verdict verdict = Verdict::kLinearizable;
  std::string key;     // the first key that is not linearizable (or that the search gave up on)
  uint64_t steps = 0;  // search steps over all keys
  size_t operations = 0;
};

// `max_steps_per_key` bounds the search; past it the verdict is kGaveUp (unknown), never a
// false "linearizable".
CheckResult Check(const std::vector<Operation>& history, uint64_t max_steps_per_key = 50'000'000);

// One operation per line, every string hex-encoded, so a history can be saved by a load
// generator and checked later by the `lincheck` tool.
std::string FormatHistory(const std::vector<Operation>& history);
bool ParseHistory(std::string_view text, std::vector<Operation>* history, std::string* error);

const char* VerdictName(Verdict v);

}  // namespace lldstore::lin
