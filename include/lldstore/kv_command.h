#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "lldstore/codec.h"
#include "lldstore/flat_store.h"

namespace lldstore {

// Phase 8: the commands replicated through Raft, and the state machine that applies them.
// Written by Claude at Ratul's request. Design: docs/DESIGN.md D-051.
//
// EXACTLY-ONCE: a client may time out and retry a request whose first attempt was in fact
// committed. Without care, "x = x + 1"-style effects would apply twice, and even a Put could be
// reordered behind a later write. Each client therefore tags requests with (client_id, seq),
// increasing seq by one per NEW request and keeping it on retries. The state machine remembers,
// per client, the last seq applied and its result, and answers a duplicate from that memory
// without applying it again. The session table is part of the replicated state, so every replica
// makes the same decision. A client has at most one request in flight.
struct KvCommand {
  enum class Op : uint8_t { kPut = 1, kGet = 2, kDelete = 3 };
  Op op = Op::kGet;
  uint64_t client_id = 0;
  uint64_t seq = 0;
  std::string key;
  std::string value;  // kPut only
};

struct KvResult {
  bool ok = false;     // Put: always true. Get: found. Delete: existed.
  std::string value;   // Get only
};

inline std::string EncodeKvCommand(const KvCommand& c) {
  ByteWriter w;
  w.U8(static_cast<uint8_t>(c.op));
  w.U64(c.client_id);
  w.U64(c.seq);
  w.Bytes(c.key);
  if (c.op == KvCommand::Op::kPut) w.Bytes(c.value);
  return w.Take();
}

inline bool DecodeKvCommand(std::string_view data, KvCommand* c) {
  ByteReader r(data);
  uint8_t op = 0;
  std::string_view key, value;
  if (!r.U8(&op) || op < 1 || op > 3 || !r.U64(&c->client_id) || !r.U64(&c->seq) || !r.Bytes(&key)) {
    return false;
  }
  c->op = static_cast<KvCommand::Op>(op);
  if (c->op == KvCommand::Op::kPut && !r.Bytes(&value)) return false;
  if (!r.done()) return false;
  c->key.assign(key.data(), key.size());
  c->value.assign(value.data(), value.size());
  return true;
}

class KvStateMachine {
 public:
  // Applies a committed command (or answers a duplicate from the session table).
  KvResult Apply(const KvCommand& c) {
    if (c.client_id != 0) {
      auto it = sessions_.find(c.client_id);
      if (it != sessions_.end() && c.seq <= it->second.seq) {
        ++duplicates_;
        return it->second.result;  // a retry of something already applied
      }
    }
    KvResult result;
    switch (c.op) {
      case KvCommand::Op::kPut:
        data_.Put(c.key, c.value);
        result.ok = true;
        break;
      case KvCommand::Op::kGet: {
        const auto v = data_.Get(c.key);
        result.ok = v.has_value();
        if (v) result.value.assign(v->data(), v->size());
        break;
      }
      case KvCommand::Op::kDelete:
        result.ok = data_.Delete(c.key);
        break;
    }
    if (c.client_id != 0) sessions_[c.client_id] = Session{c.seq, result};
    ++applied_;
    return result;
  }

  std::optional<std::string> Read(std::string_view key) const {  // for tests and debugging
    const auto v = data_.Get(key);
    if (!v) return std::nullopt;
    return std::string(*v);
  }
  size_t Size() const { return data_.Size(); }
  uint64_t applied() const noexcept { return applied_; }
  uint64_t duplicates() const noexcept { return duplicates_; }

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    data_.ForEach(fn);
  }

 private:
  struct Session {
    uint64_t seq = 0;
    KvResult result;
  };
  FlatStore data_;
  // Grows with the number of distinct clients and is never trimmed (D-051: real systems expire
  // sessions by a replicated clock; not built here).
  std::unordered_map<uint64_t, Session> sessions_;
  uint64_t applied_ = 0;
  uint64_t duplicates_ = 0;
};

}  // namespace lldstore
