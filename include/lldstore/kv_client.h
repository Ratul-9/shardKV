#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lldstore/kv_command.h"
#include "lldstore/net.h"

namespace lldstore {

struct KvClientOptions {
  std::vector<std::string> servers;  // client addresses of every node
  std::chrono::milliseconds attempt_timeout{1000};
  std::chrono::milliseconds total_timeout{10000};
  uint64_t client_id = 0;  // 0: pick a random one
};

// Phase 8: a client for the replicated store. Written by Claude at Ratul's request (D-054).
// Not thread-safe: one KvClient per thread (it has one request in flight, which is what the
// exactly-once sessions assume).
//
// It remembers which node led last time, follows "not leader" hints, and on a timeout or broken
// connection tries the next node. Retries reuse the request's (client_id, seq), so a request is
// executed at most once even if an attempt that "failed" had in fact committed.
//
// If no attempt succeeds before total_timeout, the outcome is UNKNOWN: the write may or may not
// have happened (it may even happen later). A caller that records history must say so
// (lin::Operation::known = false) instead of guessing.
class KvClient {
 public:
  enum class Outcome { kOk, kUnknown };
  struct Reply {
    Outcome outcome = Outcome::kUnknown;
    bool ok = false;  // Put: true. Get: found. Delete: existed.
    std::string value;
  };

  explicit KvClient(KvClientOptions options);
  ~KvClient();
  KvClient(const KvClient&) = delete;
  KvClient& operator=(const KvClient&) = delete;

  Reply Put(std::string_view key, std::string_view value);
  Reply Get(std::string_view key);
  Reply Delete(std::string_view key);
  // One node's "id=.. role=.. term=.. leader=.." line, or an empty string if it does not answer.
  std::string Status(size_t server);

  uint64_t client_id() const noexcept { return client_id_; }
  uint64_t attempts() const noexcept { return attempts_; }

 private:
  Reply Execute(KvCommand command);
  bool EnsureConnected(size_t server);
  void Disconnect(size_t server);

  KvClientOptions options_;
  std::vector<net::HostPort> addresses_;
  std::vector<int> fds_;
  size_t leader_guess_ = 0;
  uint64_t client_id_;
  uint64_t seq_ = 0;
  uint64_t request_id_ = 0;
  uint64_t attempts_ = 0;
};

}  // namespace lldstore
