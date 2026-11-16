#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lldstore/codec.h"
#include "lldstore/kv_command.h"

namespace lldstore::proto {

// The client <-> node protocol (one frame per message, see net.h). Phase 8, D-053.
//
// Request:  u8 kind | u64 request_id | (kind == kCommand) bytes encoded KvCommand
// Response: u8 status | u64 request_id | bool ok | bytes value | bytes leader_hint | bytes info

enum class RequestKind : uint8_t { kCommand = 1, kStatus = 2 };

enum class Status : uint8_t {
  kOk = 0,
  kNotLeader = 1,    // try leader_hint (a client address) or another node; NOT executed
  kRetry = 2,        // outcome unknown (leadership changed while it was pending): retry, same seq
  kBadRequest = 3,
};

struct Request {
  RequestKind kind = RequestKind::kCommand;
  uint64_t id = 0;
  KvCommand command;
};

struct Response {
  Status status = Status::kOk;
  uint64_t id = 0;
  bool ok = false;
  std::string value;
  std::string leader_hint;
  std::string info;  // kStatus: a one-line description of the node
};

inline std::string Encode(const Request& r) {
  ByteWriter w;
  w.U8(static_cast<uint8_t>(r.kind));
  w.U64(r.id);
  if (r.kind == RequestKind::kCommand) w.Bytes(EncodeKvCommand(r.command));
  return w.Take();
}

inline bool Decode(std::string_view data, Request* r) {
  ByteReader rd(data);
  uint8_t kind = 0;
  if (!rd.U8(&kind) || kind < 1 || kind > 2 || !rd.U64(&r->id)) return false;
  r->kind = static_cast<RequestKind>(kind);
  if (r->kind == RequestKind::kCommand) {
    std::string_view cmd;
    if (!rd.Bytes(&cmd) || !DecodeKvCommand(cmd, &r->command)) return false;
  }
  return rd.done();
}

inline std::string Encode(const Response& r) {
  ByteWriter w;
  w.U8(static_cast<uint8_t>(r.status));
  w.U64(r.id);
  w.Bool(r.ok);
  w.Bytes(r.value);
  w.Bytes(r.leader_hint);
  w.Bytes(r.info);
  return w.Take();
}

inline bool Decode(std::string_view data, Response* r) {
  ByteReader rd(data);
  uint8_t status = 0;
  std::string_view value, hint, info;
  if (!rd.U8(&status) || status > 3 || !rd.U64(&r->id) || !rd.Bool(&r->ok) || !rd.Bytes(&value) ||
      !rd.Bytes(&hint) || !rd.Bytes(&info) || !rd.done()) {
    return false;
  }
  r->status = static_cast<Status>(status);
  r->value.assign(value.data(), value.size());
  r->leader_hint.assign(hint.data(), hint.size());
  r->info.assign(info.data(), info.size());
  return true;
}

}  // namespace lldstore::proto
