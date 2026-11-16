#pragma once

// Phases 8 and 10: a deterministic simulation of a Raft cluster with clients, written by Claude
// at Ratul's request (D-052, D-058).
//
// Everything (message delays, drops, reordering, partitions, crashes, restarts, client timing)
// comes from one seeded PRNG and one logical clock, so a failing run is reproduced exactly by
// re-running its seed. Nodes are RaftNode objects; a crash destroys the object (volatile state)
// and keeps its MemoryStorage (the disk). Clients record a history that the linearizability
// checker verifies at the end.
//
// Invariants checked continuously:
//   election safety     at most one leader per term
//   state machine safety no two nodes apply different entries at the same index
//   leader completeness a committed entry is never lost (follows from the above + convergence)
// and at the end, after healing everything: every node converges to the same state.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "lldstore/kv_command.h"
#include "lldstore/linearizability.h"
#include "lldstore/raft.h"

namespace lldstore::sim {

struct SimOptions {
  int nodes = 3;
  uint64_t seed = 1;
  // Network
  double drop_probability = 0.0;
  uint32_t min_delay = 1;  // ticks
  uint32_t max_delay = 3;
  // Faults, per tick
  double crash_probability = 0.0;    // crash a random live node
  uint32_t min_downtime = 20;
  uint32_t max_downtime = 80;
  double partition_probability = 0.0;  // split the cluster at random
  double heal_probability = 0.0;       // remove any partition (0: only Heal()/Quiesce() heal)
  // Clients
  int clients = 0;
  int keys = 4;
  uint32_t attempt_timeout = 40;    // ticks before a client tries another node
  uint32_t give_up_after = 400;     // ticks before an op is recorded as "unknown"
  raft::Config raft;                // id/peers/seed are filled in per node
};

class Cluster {
 public:
  explicit Cluster(SimOptions options) : o_(std::move(options)), rng_(o_.seed) {
    const int n = o_.nodes;
    storage_.resize(static_cast<size_t>(n));
    nodes_.resize(static_cast<size_t>(n));
    machines_.resize(static_cast<size_t>(n));
    pending_.resize(static_cast<size_t>(n));
    restart_at_.assign(static_cast<size_t>(n), -1);
    side_.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
      storage_[static_cast<size_t>(i)] = std::make_unique<raft::MemoryStorage>();
      Start(i);
    }
    for (int c = 0; c < o_.clients; ++c) {
      Client client;
      client.id = static_cast<uint64_t>(c + 1);
      clients_.push_back(client);
    }
  }

  // ---- Driving ------------------------------------------------------------------------------

  void Run(int64_t ticks) {
    for (int64_t i = 0; i < ticks; ++i) Tick();
  }

  void Tick() {
    ++now_;
    if (faults_enabled_) InjectFaults();
    for (size_t i = 0; i < restart_at_.size(); ++i) {
      if (restart_at_[i] == now_) Start(static_cast<int>(i));
    }
    DeliverDue();
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (nodes_[i]) nodes_[i]->Tick();
    }
    for (size_t i = 0; i < nodes_.size(); ++i) Collect(static_cast<int>(i));
    CheckElectionSafety();
    if (clients_enabled_) DriveClients();
  }

  // Stop injecting faults, heal the network, restart every crashed node, finish client ops.
  void Quiesce(int64_t ticks) {
    faults_enabled_ = false;
    Heal();
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (!nodes_[i]) Start(static_cast<int>(i));
    }
    Run(ticks);
  }

  void StopClients() { clients_enabled_ = false; }

  // ---- Faults -------------------------------------------------------------------------------

  void Crash(int i) {
    nodes_[static_cast<size_t>(i)].reset();
    machines_[static_cast<size_t>(i)] = std::make_unique<KvStateMachine>();
    applied_[static_cast<size_t>(i)].clear();
    pending_[static_cast<size_t>(i)].clear();
    ++crashes_;
  }

  void Start(int i) {
    raft::Config c = o_.raft;
    c.id = static_cast<raft::NodeId>(i + 1);
    c.peers.clear();
    for (int j = 0; j < o_.nodes; ++j) {
      if (j != i) c.peers.push_back(static_cast<raft::NodeId>(j + 1));
    }
    c.seed = o_.seed * 1000003 + static_cast<uint64_t>(restarts_++);
    nodes_[static_cast<size_t>(i)] =
        std::make_unique<raft::RaftNode>(c, storage_[static_cast<size_t>(i)].get());
    machines_[static_cast<size_t>(i)] = std::make_unique<KvStateMachine>();
    applied_.resize(nodes_.size());
    applied_[static_cast<size_t>(i)].clear();
    restart_at_[static_cast<size_t>(i)] = -1;
  }

  // Nodes on different sides cannot exchange messages.
  void Partition(const std::vector<int>& side_one) {
    std::fill(side_.begin(), side_.end(), 0);
    for (int i : side_one) side_[static_cast<size_t>(i)] = 1;
    ++partitions_;
  }
  void Heal() { std::fill(side_.begin(), side_.end(), 0); }

  // ---- Inspection ---------------------------------------------------------------------------

  // The live leader with the highest term, or -1.
  int Leader() const {
    int best = -1;
    uint64_t best_term = 0;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (nodes_[i] && nodes_[i]->role() == raft::Role::kLeader && nodes_[i]->term() >= best_term) {
        best = static_cast<int>(i);
        best_term = nodes_[i]->term();
      }
    }
    return best;
  }

  raft::RaftNode* node(int i) { return nodes_[static_cast<size_t>(i)].get(); }
  const KvStateMachine& machine(int i) const { return *machines_[static_cast<size_t>(i)]; }
  raft::MemoryStorage& storage(int i) { return *storage_[static_cast<size_t>(i)]; }
  int64_t now() const { return now_; }
  const std::vector<std::string>& violations() const { return violations_; }
  const std::vector<lin::Operation>& history() const { return history_; }
  uint64_t crashes() const { return crashes_; }
  uint64_t partitions() const { return partitions_; }
  size_t committed_entries() const { return committed_.size(); }

  // Proposes directly on node i (tests that do not use simulated clients).
  uint64_t ProposeOn(int i, const std::string& data) { return nodes_[static_cast<size_t>(i)]->Propose(data); }

  // True if every node has applied the same entries and holds the same data.
  bool Converged(std::string* why) const {
    std::optional<std::vector<std::pair<uint64_t, raft::Entry>>> reference;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (!nodes_[i]) {
        *why = "node " + std::to_string(i + 1) + " is down";
        return false;
      }
      if (!reference) {
        reference = applied_[i];
      } else if (applied_[i] != *reference) {
        *why = "node " + std::to_string(i + 1) + " applied " + std::to_string(applied_[i].size()) +
               " entries, node 1 applied " + std::to_string(reference->size());
        return false;
      }
    }
    return true;
  }

 private:
  struct Envelope {
    raft::Message message;
  };

  struct Client {
    uint64_t id = 0;
    uint64_t seq = 0;
    bool busy = false;
    KvCommand command;
    lin::Operation op;
    int target = 0;
    int64_t attempt_deadline = 0;
    int64_t give_up_at = 0;
    uint64_t attempt = 0;  // distinguishes replies to old attempts
    int64_t idle_until = 0;
  };

  struct ClientEvent {  // a request arriving at a node, or a reply arriving at a client
    bool to_node = true;
    int node = 0;
    size_t client = 0;
    uint64_t attempt = 0;
    bool not_leader = false;
    raft::NodeId leader_hint = 0;
    KvResult result;
  };

  struct Pending {  // a client proposal on a node, waiting to be applied
    uint64_t term;
    size_t client;
    uint64_t attempt;
  };

  uint32_t Delay() {
    return o_.min_delay + static_cast<uint32_t>(rng_() % (o_.max_delay - o_.min_delay + 1));
  }
  // Portable: std::uniform_real_distribution differs between libstdc++ and libc++, which would
  // make the same seed produce different runs on the Mac and on Linux. mt19937_64 itself does not.
  bool Chance(double p) { return p > 0 && static_cast<double>(rng_() >> 11) * 0x1.0p-53 < p; }
  bool Connected(int a, int b) const { return side_[static_cast<size_t>(a)] == side_[static_cast<size_t>(b)]; }

  void InjectFaults() {
    if (Chance(o_.crash_probability)) {
      const int i = static_cast<int>(rng_() % static_cast<uint64_t>(o_.nodes));
      if (nodes_[static_cast<size_t>(i)]) {
        Crash(i);
        restart_at_[static_cast<size_t>(i)] =
            now_ + o_.min_downtime + static_cast<int64_t>(rng_() % (o_.max_downtime - o_.min_downtime + 1));
      }
    }
    if (Chance(o_.partition_probability)) {
      std::vector<int> side;
      for (int i = 0; i < o_.nodes; ++i) {
        if (rng_() % 2) side.push_back(i);
      }
      Partition(side);
    }
    if (Chance(o_.heal_probability)) Heal();
  }

  void Collect(int i) {
    raft::RaftNode* n = nodes_[static_cast<size_t>(i)].get();
    if (!n) return;
    for (raft::Message& m : n->TakeMessages()) {
      if (Chance(o_.drop_probability)) continue;
      // Every message goes through the codec, so the simulator also tests the wire format.
      raft::Message decoded;
      if (!raft::DecodeMessage(raft::EncodeMessage(m), &decoded)) {
        violations_.push_back("message failed to round-trip through the codec");
        continue;
      }
      network_.emplace(std::make_pair(now_ + Delay(), seq_++), Envelope{std::move(decoded)});
    }
    for (auto& [index, entry] : n->TakeCommitted()) {
      auto [it, inserted] = committed_.emplace(index, entry);
      if (!inserted && !(it->second == entry)) {
        violations_.push_back("state machine safety: index " + std::to_string(index) +
                              " applied with different entries (node " + std::to_string(i + 1) + ")");
      }
      applied_[static_cast<size_t>(i)].emplace_back(index, entry);
      KvResult result;
      if (!entry.data.empty()) {
        KvCommand cmd;
        if (!DecodeKvCommand(entry.data, &cmd)) {
          violations_.push_back("undecodable command at index " + std::to_string(index));
          continue;
        }
        result = machines_[static_cast<size_t>(i)]->Apply(cmd);
      }
      auto& pend = pending_[static_cast<size_t>(i)];
      auto p = pend.find(index);
      if (p != pend.end()) {
        if (p->second.term == entry.term) {  // our proposal is what committed at this index
          ClientEvent reply;
          reply.to_node = false;
          reply.client = p->second.client;
          reply.attempt = p->second.attempt;
          reply.result = result;
          client_events_.emplace(std::make_pair(now_ + Delay(), seq_++), reply);
        }
        pend.erase(p);
      }
    }
  }

  void DeliverDue() {
    while (!network_.empty() && network_.begin()->first.first <= now_) {
      Envelope env = std::move(network_.begin()->second);
      network_.erase(network_.begin());
      const int from = static_cast<int>(env.message.from) - 1;
      const int to = static_cast<int>(env.message.to) - 1;
      if (!nodes_[static_cast<size_t>(to)] || !Connected(from, to)) continue;  // lost
      nodes_[static_cast<size_t>(to)]->Step(env.message);
    }
    while (!client_events_.empty() && client_events_.begin()->first.first <= now_) {
      ClientEvent e = client_events_.begin()->second;
      client_events_.erase(client_events_.begin());
      if (e.to_node) {
        HandleClientRequest(e);
      } else {
        HandleClientReply(e);
      }
    }
  }

  void CheckElectionSafety() {
    for (size_t i = 0; i < nodes_.size(); ++i) {
      if (!nodes_[i] || nodes_[i]->role() != raft::Role::kLeader) continue;
      auto [it, inserted] = leader_of_term_.emplace(nodes_[i]->term(), static_cast<int>(i));
      if (!inserted && it->second != static_cast<int>(i)) {
        violations_.push_back("election safety: two leaders in term " + std::to_string(nodes_[i]->term()));
      }
    }
  }

  // ---- Simulated clients --------------------------------------------------------------------

  void DriveClients() {
    for (size_t c = 0; c < clients_.size(); ++c) {
      Client& cl = clients_[c];
      if (!cl.busy) {
        if (now_ >= cl.idle_until) StartOp(c);
        continue;
      }
      if (now_ >= cl.give_up_at) {  // outcome unknown: it may still commit later
        cl.op.known = false;
        cl.op.ret = lin::kNoReturn;
        history_.push_back(cl.op);
        cl.busy = false;
        cl.idle_until = now_ + static_cast<int64_t>(rng_() % 5);
        continue;
      }
      if (now_ >= cl.attempt_deadline) {
        cl.target = (cl.target + 1) % o_.nodes;
        SendAttempt(c);
      }
    }
  }

  void StartOp(size_t c) {
    Client& cl = clients_[c];
    cl.busy = true;
    ++cl.seq;
    KvCommand cmd;
    cmd.client_id = cl.id;
    cmd.seq = cl.seq;
    cmd.key = "k" + std::to_string(rng_() % static_cast<uint64_t>(o_.keys));
    const uint64_t r = rng_() % 10;
    lin::Operation op;
    op.client = cl.id;
    op.key = cmd.key;
    op.call = now_;
    if (r < 4) {
      cmd.op = KvCommand::Op::kPut;
      cmd.value = "c" + std::to_string(cl.id) + "-" + std::to_string(cl.seq);
      op.type = lin::OpType::kPut;
      op.value = cmd.value;
    } else if (r < 9) {
      cmd.op = KvCommand::Op::kGet;
      op.type = lin::OpType::kGet;
    } else {
      cmd.op = KvCommand::Op::kDelete;
      op.type = lin::OpType::kDelete;
    }
    cl.command = cmd;
    cl.op = op;
    cl.give_up_at = now_ + o_.give_up_after;
    SendAttempt(c);
  }

  void SendAttempt(size_t c) {
    Client& cl = clients_[c];
    ++cl.attempt;
    cl.attempt_deadline = now_ + o_.attempt_timeout;
    ClientEvent e;
    e.to_node = true;
    e.node = cl.target;
    e.client = c;
    e.attempt = cl.attempt;
    client_events_.emplace(std::make_pair(now_ + Delay(), seq_++), e);
  }

  void HandleClientRequest(const ClientEvent& e) {
    raft::RaftNode* n = nodes_[static_cast<size_t>(e.node)].get();
    if (!n) return;  // the request is lost; the client times out
    if (n->role() != raft::Role::kLeader) {
      ClientEvent reply = e;
      reply.to_node = false;
      reply.not_leader = true;
      reply.leader_hint = n->leader();
      client_events_.emplace(std::make_pair(now_ + Delay(), seq_++), reply);
      return;
    }
    const uint64_t index = n->Propose(EncodeKvCommand(clients_[e.client].command));
    pending_[static_cast<size_t>(e.node)][index] = Pending{n->term(), e.client, e.attempt};
  }

  void HandleClientReply(const ClientEvent& e) {
    Client& cl = clients_[e.client];
    if (!cl.busy || e.attempt != cl.attempt) return;  // a reply to an attempt we gave up on
    if (e.not_leader) {
      cl.target = e.leader_hint != 0 ? static_cast<int>(e.leader_hint) - 1 : (cl.target + 1) % o_.nodes;
      SendAttempt(e.client);
      return;
    }
    cl.op.ret = now_;
    cl.op.known = true;
    cl.op.found = e.result.ok;
    cl.op.output = e.result.value;
    history_.push_back(cl.op);
    cl.busy = false;
    cl.idle_until = now_ + static_cast<int64_t>(rng_() % 5);
  }

  SimOptions o_;
  std::mt19937_64 rng_;
  int64_t now_ = 0;
  uint64_t seq_ = 0;
  uint64_t restarts_ = 0;
  uint64_t crashes_ = 0;
  uint64_t partitions_ = 0;
  bool faults_enabled_ = true;
  bool clients_enabled_ = true;
  std::vector<std::unique_ptr<raft::MemoryStorage>> storage_;
  std::vector<std::unique_ptr<raft::RaftNode>> nodes_;
  std::vector<std::unique_ptr<KvStateMachine>> machines_;  // FlatStore cannot be moved
  std::vector<std::vector<std::pair<uint64_t, raft::Entry>>> applied_;
  std::vector<std::map<uint64_t, Pending>> pending_;
  std::vector<int64_t> restart_at_;
  std::vector<int> side_;
  std::map<std::pair<int64_t, uint64_t>, Envelope> network_;
  std::map<std::pair<int64_t, uint64_t>, ClientEvent> client_events_;
  std::map<uint64_t, raft::Entry> committed_;
  std::map<uint64_t, int> leader_of_term_;
  std::vector<Client> clients_;
  std::vector<lin::Operation> history_;
  std::vector<std::string> violations_;
};

}  // namespace lldstore::sim
