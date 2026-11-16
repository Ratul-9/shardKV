#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lldstore/log_file.h"

namespace lldstore::raft {

// Phase 8: Raft consensus (leader election + log replication). Written by Claude at Ratul's
// request. Design decisions: docs/DESIGN.md D-044 to D-050.
//
// RaftNode is a DETERMINISTIC STATE MACHINE: no threads, no clock, no sockets. The caller feeds it
// ticks (logical time), incoming messages and client proposals, and collects outgoing messages
// and newly committed entries. The same code therefore runs inside the deterministic simulator
// (tests/raft_sim.h, which can delay, drop, reorder and partition messages from a seed) and inside
// the real server (src/kv_node.cpp, which adds sockets and a timer).
//
// Implemented: elections with randomised timeouts, log replication with the conflict-index
// back-off, commit only of current-term entries by majority (Raft paper fig. 8), a no-op entry
// at the start of each term, check-quorum (a leader that cannot reach a majority steps down).
// NOT implemented (D-050): membership change, log compaction / InstallSnapshot, PreVote,
// leader lease reads (every read goes through the log instead).

using NodeId = uint32_t;  // 0 means "nobody"

struct Entry {
  uint64_t term = 0;
  std::string data;  // empty = the leader's no-op
  bool operator==(const Entry&) const = default;
};

enum class MessageType : uint8_t {
  kVoteRequest = 1,
  kVoteReply = 2,
  kAppend = 3,
  kAppendReply = 4,
};

struct Message {
  MessageType type = MessageType::kAppend;
  NodeId from = 0;
  NodeId to = 0;
  uint64_t term = 0;
  // kVoteRequest
  uint64_t last_log_index = 0;
  uint64_t last_log_term = 0;
  // kVoteReply
  bool granted = false;
  // kAppend
  uint64_t prev_index = 0;
  uint64_t prev_term = 0;
  uint64_t commit = 0;
  std::vector<Entry> entries;
  // kAppendReply: on success, the highest index known to match the leader; on failure, the index
  // the leader should try next (the follower's "conflict hint").
  bool success = false;
  uint64_t match_index = 0;
};

std::string EncodeMessage(const Message& m);
bool DecodeMessage(std::string_view data, Message* m);

// The durable state of a node: current term, vote, and the log. EVERY mutating call must be
// durable when it returns, because the node sends messages that depend on it right after (a vote
// granted and then forgotten in a crash could elect two leaders in one term).
class Storage {
 public:
  virtual ~Storage() = default;
  virtual uint64_t term() const = 0;
  virtual NodeId voted_for() const = 0;
  virtual void SetTermAndVote(uint64_t new_term, NodeId vote) = 0;
  virtual uint64_t last_index() const = 0;
  virtual uint64_t term_at(uint64_t index) const = 0;  // 0 for index 0 and for index > last
  virtual const Entry& entry(uint64_t index) const = 0;  // 1 <= index <= last
  virtual void Append(const std::vector<Entry>& entries) = 0;
  virtual void TruncateFrom(uint64_t index) = 0;  // removes index..last
};

// In-memory storage. The simulator keeps it across a simulated crash, which models a disk that
// survives the crash; the node object (all volatile state) is thrown away.
class MemoryStorage : public Storage {
 public:
  uint64_t term() const override { return term_; }
  NodeId voted_for() const override { return vote_; }
  void SetTermAndVote(uint64_t new_term, NodeId vote) override {
    term_ = new_term;
    vote_ = vote;
  }
  uint64_t last_index() const override { return log_.size(); }
  uint64_t term_at(uint64_t index) const override {
    return index == 0 || index > log_.size() ? 0 : log_[index - 1].term;
  }
  const Entry& entry(uint64_t index) const override { return log_[index - 1]; }
  void Append(const std::vector<Entry>& entries) override {
    log_.insert(log_.end(), entries.begin(), entries.end());
  }
  void TruncateFrom(uint64_t index) override { log_.resize(index - 1); }

 private:
  uint64_t term_ = 0;
  NodeId vote_ = 0;
  std::vector<Entry> log_;
};

// File-backed storage built on the Phase 7 primitives (D-049):
//   raft-meta  term + vote, replaced atomically (tmp + fsync + rename + dir fsync)
//   raft-log   framed records: APPEND(index, term, data) and TRUNCATE(from), fsync'd per call
// The whole log is also kept in memory. Recovery replays raft-log and cuts its torn tail.
class FileStorage : public Storage {
 public:
  FileStorage(const std::string& dir, SyncMode sync);
  uint64_t term() const override { return memory_.term(); }
  NodeId voted_for() const override { return memory_.voted_for(); }
  void SetTermAndVote(uint64_t new_term, NodeId vote) override;
  uint64_t last_index() const override { return memory_.last_index(); }
  uint64_t term_at(uint64_t index) const override { return memory_.term_at(index); }
  const Entry& entry(uint64_t index) const override { return memory_.entry(index); }
  void Append(const std::vector<Entry>& entries) override;
  void TruncateFrom(uint64_t index) override;
  uint64_t log_bytes() const { return log_->size(); }

 private:
  std::string dir_;
  SyncMode sync_;
  MemoryStorage memory_;
  std::unique_ptr<LogFile> log_;
};

struct Config {
  NodeId id = 1;
  std::vector<NodeId> peers;  // the OTHER members of the cluster
  // In ticks. Election timeouts are drawn uniformly from [min, max] each time the timer resets,
  // so two followers rarely time out together (D-046).
  uint32_t election_ticks_min = 10;
  uint32_t election_ticks_max = 20;
  uint32_t heartbeat_ticks = 2;
  size_t max_entries_per_message = 256;
  uint64_t seed = 1;
  bool check_quorum = true;
};

enum class Role { kFollower, kCandidate, kLeader };
const char* RoleName(Role role);

class RaftNode {
 public:
  RaftNode(Config config, Storage* storage);

  void Tick();
  void Step(const Message& m);
  // Leader only: appends the entries (durably) and starts replicating them. Returns the index of
  // the last one, or 0 if this node is not the leader (nothing was appended).
  uint64_t Propose(std::vector<std::string> data);
  uint64_t Propose(std::string data) {
    std::vector<std::string> one;
    one.push_back(std::move(data));
    return Propose(std::move(one));
  }

  std::vector<Message> TakeMessages();
  // Entries committed since the last call, in log order, as (index, entry). The state machine must
  // apply each exactly once, in this order. After a restart this starts again from index 1.
  std::vector<std::pair<uint64_t, Entry>> TakeCommitted();

  NodeId id() const noexcept { return config_.id; }
  Role role() const noexcept { return role_; }
  uint64_t term() const { return storage_->term(); }
  NodeId leader() const noexcept { return leader_; }
  uint64_t commit_index() const noexcept { return commit_; }
  uint64_t last_index() const { return storage_->last_index(); }
  uint64_t last_applied() const noexcept { return applied_; }
  uint64_t elections_started() const noexcept { return elections_started_; }

 private:
  size_t Majority() const noexcept { return (config_.peers.size() + 1) / 2 + 1; }
  void BecomeFollower(uint64_t new_term, NodeId new_leader);
  void BecomeCandidate();
  void BecomeLeader();
  void ResetElectionTimer();
  void SendAppend(NodeId peer);
  void BroadcastAppend();
  void AdvanceCommit();
  void HandleVoteRequest(const Message& m);
  void HandleVoteReply(const Message& m);
  void HandleAppend(const Message& m);
  void HandleAppendReply(const Message& m);
  bool CandidateLogIsUpToDate(uint64_t last_log_index, uint64_t last_log_term) const;
  size_t PeerSlot(NodeId peer) const;
  uint64_t NextRandom();

  Config config_;
  Storage* storage_;
  Role role_ = Role::kFollower;
  NodeId leader_ = 0;
  uint64_t commit_ = 0;   // volatile: rebuilt from the leader after a restart
  uint64_t applied_ = 0;  // volatile: the state machine is rebuilt by re-applying from 1
  uint32_t election_elapsed_ = 0;
  uint32_t election_timeout_ = 0;
  uint32_t heartbeat_elapsed_ = 0;
  uint64_t rng_state_;
  uint64_t elections_started_ = 0;
  // Candidate.
  std::vector<bool> votes_;
  // Leader, one slot per peer (same order as config_.peers).
  std::vector<uint64_t> next_index_;
  std::vector<uint64_t> match_index_;
  std::vector<bool> heard_from_;  // check-quorum: any reply during the current window
  std::vector<Message> outbox_;
};

}  // namespace lldstore::raft
