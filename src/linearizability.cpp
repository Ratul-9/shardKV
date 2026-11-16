#include "lldstore/linearizability.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace lldstore::lin {
namespace {

// The sequential specification of ONE key: absent, or holding a value.
using State = std::optional<std::string>;

// Applies `op` to `state`. Returns false if the op's recorded result is impossible in `state`.
bool StepModel(const Operation& op, const State& state, State* next) {
  switch (op.type) {
    case OpType::kPut:
      *next = op.value;
      return true;
    case OpType::kGet:
      if (op.found != state.has_value()) return false;
      if (op.found && op.output != *state) return false;
      *next = state;
      return true;
    case OpType::kDelete:
      if (op.known && op.found != state.has_value()) return false;
      *next = std::nullopt;
      return true;
  }
  return false;
}

struct CacheKey {
  std::vector<uint64_t> linearized;
  State state;
  bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash {
  size_t operator()(const CacheKey& k) const noexcept {
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    for (uint64_t w : k.linearized) h = (h ^ w) * 0x100000001b3ULL;
    if (k.state) h ^= std::hash<std::string>()(*k.state) + 0x51;
    return static_cast<size_t>(h);
  }
};

// The event list: every call and every (known) return, ordered by time, as a doubly linked list
// with a sentinel at index 0 so "lift" (remove an operation's call and return) and "unlift"
// (put them back) are O(1).
struct Event {
  int op = -1;         // index into the per-key ops
  bool is_call = false;
  int match = -1;      // call -> its return event (or -1 for unknown ops)
  int prev = -1;
  int next = -1;
};

Verdict CheckKey(const std::vector<const Operation*>& ops, uint64_t max_steps, uint64_t* steps) {
  const int n = static_cast<int>(ops.size());
  struct Raw {
    int64_t time;
    int kind;  // 0 = call, 1 = return: calls first at equal times (the more permissive choice)
    int op;
  };
  std::vector<Raw> raw;
  for (int i = 0; i < n; ++i) {
    raw.push_back({ops[static_cast<size_t>(i)]->call, 0, i});
    if (ops[static_cast<size_t>(i)]->known) raw.push_back({ops[static_cast<size_t>(i)]->ret, 1, i});
  }
  std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
    return a.time != b.time ? a.time < b.time : a.kind < b.kind;
  });

  std::vector<Event> ev(raw.size() + 1);  // ev[0] is the sentinel head
  std::vector<int> call_event(static_cast<size_t>(n), -1);
  for (size_t i = 0; i < raw.size(); ++i) {
    Event& e = ev[i + 1];
    e.op = raw[i].op;
    e.is_call = raw[i].kind == 0;
    e.prev = static_cast<int>(i);
    e.next = i + 1 < raw.size() ? static_cast<int>(i + 2) : -1;
    if (e.is_call) call_event[static_cast<size_t>(e.op)] = static_cast<int>(i + 1);
  }
  ev[0].next = raw.empty() ? -1 : 1;
  for (size_t i = 1; i < ev.size(); ++i) {
    if (!ev[i].is_call) ev[static_cast<size_t>(call_event[static_cast<size_t>(ev[i].op)])].match = static_cast<int>(i);
  }

  auto lift = [&](int c) {
    Event& e = ev[static_cast<size_t>(c)];
    ev[static_cast<size_t>(e.prev)].next = e.next;
    if (e.next != -1) ev[static_cast<size_t>(e.next)].prev = e.prev;
    if (e.match != -1) {
      Event& r = ev[static_cast<size_t>(e.match)];
      ev[static_cast<size_t>(r.prev)].next = r.next;
      if (r.next != -1) ev[static_cast<size_t>(r.next)].prev = r.prev;
    }
  };
  auto unlift = [&](int c) {
    Event& e = ev[static_cast<size_t>(c)];
    if (e.match != -1) {
      Event& r = ev[static_cast<size_t>(e.match)];
      ev[static_cast<size_t>(r.prev)].next = e.match;
      if (r.next != -1) ev[static_cast<size_t>(r.next)].prev = e.match;
    }
    ev[static_cast<size_t>(e.prev)].next = c;
    if (e.next != -1) ev[static_cast<size_t>(e.next)].prev = c;
  };

  int known_total = 0;
  for (const Operation* op : ops) known_total += op->known ? 1 : 0;
  int known_done = 0;
  std::vector<uint64_t> linearized((static_cast<size_t>(n) + 63) / 64, 0);
  State state;  // every key starts absent
  std::unordered_set<CacheKey, CacheKeyHash> cache;
  struct Frame {
    int call;
    State state;
  };
  std::vector<Frame> stack;

  auto backtrack = [&](int* entry) -> bool {
    if (stack.empty()) return false;
    Frame f = std::move(stack.back());
    stack.pop_back();
    const int op = ev[static_cast<size_t>(f.call)].op;
    state = std::move(f.state);
    linearized[static_cast<size_t>(op) / 64] &= ~(uint64_t{1} << (op % 64));
    if (ops[static_cast<size_t>(op)]->known) --known_done;
    unlift(f.call);
    *entry = ev[static_cast<size_t>(f.call)].next;
    return true;
  };

  int entry = ev[0].next;
  uint64_t local_steps = 0;
  for (;;) {
    if (known_done == known_total) {
      *steps += local_steps;
      return Verdict::kLinearizable;  // remaining unknown writes are taken to have never happened
    }
    if (++local_steps > max_steps) {
      *steps += local_steps;
      return Verdict::kGaveUp;
    }
    if (entry == -1) {  // ran off the end with known ops still unplaced
      if (!backtrack(&entry)) break;
      continue;
    }
    const Event& e = ev[static_cast<size_t>(entry)];
    if (e.is_call) {
      State next;
      const Operation& op = *ops[static_cast<size_t>(e.op)];
      if (StepModel(op, state, &next)) {
        std::vector<uint64_t> bits = linearized;
        bits[static_cast<size_t>(e.op) / 64] |= uint64_t{1} << (e.op % 64);
        if (cache.insert(CacheKey{bits, next}).second) {
          stack.push_back(Frame{entry, std::move(state)});
          state = std::move(next);
          linearized = std::move(bits);
          if (op.known) ++known_done;
          lift(entry);
          entry = ev[0].next;
          continue;
        }
      }
      entry = e.next;
    } else {
      // The response of an operation not yet placed: it must have taken effect before this
      // point, so the current prefix choice is wrong. Undo the last placement.
      if (!backtrack(&entry)) break;
    }
  }
  *steps += local_steps;
  return Verdict::kViolation;
}

std::string Hex(std::string_view s) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() * 2 + 1);
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out.empty() ? "-" : out;
}

bool Unhex(std::string_view s, std::string* out) {
  out->clear();
  if (s == "-") return true;
  if (s.size() % 2 != 0) return false;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); i += 2) {
    const int hi = val(s[i]), lo = val(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<char>(hi * 16 + lo));
  }
  return true;
}

}  // namespace

CheckResult Check(const std::vector<Operation>& history, uint64_t max_steps_per_key) {
  std::map<std::string, std::vector<const Operation*>> by_key;
  for (const Operation& op : history) {
    if (op.type == OpType::kGet && !op.known) continue;  // an unknown read constrains nothing
    by_key[op.key].push_back(&op);
  }
  CheckResult result;
  result.operations = history.size();
  for (const auto& [key, ops] : by_key) {
    const Verdict v = CheckKey(ops, max_steps_per_key, &result.steps);
    if (v != Verdict::kLinearizable) {
      result.verdict = v;
      result.key = key;
      if (v == Verdict::kViolation) return result;  // a violation beats a give-up
    }
  }
  return result;
}

const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::kLinearizable: return "linearizable";
    case Verdict::kViolation: return "VIOLATION";
    case Verdict::kGaveUp: return "unknown (search limit reached)";
  }
  return "?";
}

std::string FormatHistory(const std::vector<Operation>& history) {
  std::ostringstream out;
  out << "# client type call ret known found key value output\n";
  for (const Operation& op : history) {
    out << op.client << ' ' << static_cast<int>(op.type) << ' ' << op.call << ' '
        << (op.known ? op.ret : -1) << ' ' << (op.known ? 1 : 0) << ' ' << (op.found ? 1 : 0)
        << ' ' << Hex(op.key) << ' ' << Hex(op.value) << ' ' << Hex(op.output) << '\n';
  }
  return out.str();
}

bool ParseHistory(std::string_view text, std::vector<Operation>* history, std::string* error) {
  history->clear();
  std::istringstream in{std::string(text)};
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    Operation op;
    int type = 0, known = 0, found = 0;
    long long call = 0, ret = 0;
    std::string key, value, output;
    if (!(fields >> op.client >> type >> call >> ret >> known >> found >> key >> value >> output) ||
        type < 0 || type > 2 || !Unhex(key, &op.key) || !Unhex(value, &op.value) ||
        !Unhex(output, &op.output)) {
      *error = "line " + std::to_string(line_no) + ": cannot parse";
      return false;
    }
    op.type = static_cast<OpType>(type);
    op.call = call;
    op.known = known == 1;
    op.ret = op.known ? ret : kNoReturn;
    op.found = found == 1;
    if (op.known && op.ret < op.call) {
      *error = "line " + std::to_string(line_no) + ": response before invocation";
      return false;
    }
    history->push_back(std::move(op));
  }
  return true;
}

}  // namespace lldstore::lin
