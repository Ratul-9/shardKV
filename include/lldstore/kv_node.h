#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/kv_command.h"
#include "lldstore/metrics.h"
#include "lldstore/net.h"
#include "lldstore/raft.h"

namespace lldstore {

struct KvNodeOptions {
  raft::NodeId id = 1;
  std::map<raft::NodeId, std::string> raft_addresses;    // every node, including this one
  std::map<raft::NodeId, std::string> client_addresses;  // every node (used for leader hints)
  std::string listen_host;     // bind here instead of the advertised host (e.g. 0.0.0.0 in Docker)
  std::string data_dir;
  std::string metrics_address;  // empty: no metrics endpoint
  SyncMode sync = SyncMode::kData;
  std::chrono::milliseconds tick{10};
  raft::Config raft;            // timeouts in ticks; id and peers are filled in from the above
  size_t max_batch = 1024;      // client commands proposed together (one fsync)
};

// Phase 8 + 11: one member of the replicated key-value cluster. Written by Claude at Ratul's
// request. Design: docs/DESIGN.md D-053 and D-054.
//
// Threads:
//   loop      owns RaftNode, the state machine and all pending requests. Nothing else touches
//             them, so they need no lock (the Phase 3 idea again: one owner thread).
//   accept x3 peer listener, client listener, metrics listener
//   readers   one per open connection: decode frames, push events to the loop's inbox
//   senders   one per peer: connect (with back-off), send queued Raft messages, drop on failure
//             (Raft retransmits anything that matters)
//
// Loop iteration: drain the inbox (peer messages -> Step; client commands -> one batch), propose
// the batch (one durable append), tick if a tick is due, hand outgoing messages to the senders,
// apply newly committed entries and answer the clients whose entries they were.
class KvNode {
 public:
  explicit KvNode(KvNodeOptions options);
  ~KvNode();
  KvNode(const KvNode&) = delete;
  KvNode& operator=(const KvNode&) = delete;

  void Start();
  void Stop();

  MetricsRegistry& metrics() { return metrics_; }
  raft::Role role() const { return static_cast<raft::Role>(role_.load()); }
  uint64_t term() const { return term_.load(); }
  raft::NodeId leader() const { return leader_.load(); }

 private:
  struct ClientConn {
    int fd = -1;
    std::mutex write_mu;
    bool Send(const std::string& frame) {
      std::lock_guard<std::mutex> lock(write_mu);
      return net::SendFrame(fd, frame);
    }
  };

  struct Event {
    enum class Kind { kPeer, kClient } kind = Kind::kPeer;
    raft::Message message;
    std::shared_ptr<ClientConn> conn;
    uint64_t request_id = 0;
    bool status_request = false;
    KvCommand command;
    std::chrono::steady_clock::time_point received;
  };

  struct PendingRequest {
    uint64_t term;
    std::shared_ptr<ClientConn> conn;
    uint64_t request_id;
    KvCommand::Op op;
    std::chrono::steady_clock::time_point received;
  };

  struct Peer {
    raft::NodeId id = 0;
    net::HostPort address;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::thread thread;
  };

  void LoopMain();
  void SenderMain(Peer* peer);
  void AcceptMain(int listen_fd, bool peers);
  void PeerReaderMain(int fd);
  void ClientReaderMain(std::shared_ptr<ClientConn> conn);
  void MetricsMain(int listen_fd);
  void Push(Event e);
  void TrackConnection(int fd, std::thread t);
  std::string StatusLine() const;
  std::string LeaderHint() const;

  KvNodeOptions options_;
  MetricsRegistry metrics_;
  std::unique_ptr<raft::FileStorage> storage_;
  std::unique_ptr<raft::RaftNode> node_;  // loop thread only
  KvStateMachine machine_;                // loop thread only
  std::map<uint64_t, PendingRequest> pending_;  // loop thread only: by log index

  std::atomic<bool> stopping_{false};
  std::atomic<int> role_{0};
  std::atomic<uint64_t> term_{0};
  std::atomic<raft::NodeId> leader_{0};

  std::mutex inbox_mu_;
  std::condition_variable inbox_cv_;
  std::deque<Event> inbox_;

  std::vector<std::unique_ptr<Peer>> peers_;
  int peer_listen_fd_ = -1;
  int client_listen_fd_ = -1;
  int metrics_listen_fd_ = -1;
  std::thread loop_thread_;
  std::vector<std::thread> accept_threads_;

  std::mutex conns_mu_;
  std::vector<int> conn_fds_;
  std::vector<std::thread> conn_threads_;

  // Metrics (references into metrics_).
  Counter* requests_ = nullptr;
  Counter* not_leader_ = nullptr;
  Counter* retries_ = nullptr;
  Counter* batches_ = nullptr;
  Counter* batched_commands_ = nullptr;
  Counter* peer_messages_in_ = nullptr;
  Counter* peer_messages_dropped_ = nullptr;
  Gauge* term_gauge_ = nullptr;
  Gauge* role_gauge_ = nullptr;
  Gauge* commit_gauge_ = nullptr;
  Gauge* applied_gauge_ = nullptr;
  Gauge* log_gauge_ = nullptr;
  Gauge* elections_gauge_ = nullptr;
  Gauge* keys_gauge_ = nullptr;
  Histogram* latency_ = nullptr;
  Histogram* append_latency_ = nullptr;
};

}  // namespace lldstore
