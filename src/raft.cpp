#include "lldstore/raft.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "lldstore/codec.h"

namespace lldstore::raft {

// ---- Wire format --------------------------------------------------------------------------

namespace {

void PutEntry(ByteWriter& w, const Entry& e) {
  w.U64(e.term);
  w.Bytes(e.data);
}

bool GetEntry(ByteReader& r, Entry* e) {
  std::string_view data;
  if (!r.U64(&e->term) || !r.Bytes(&data)) return false;
  e->data.assign(data.data(), data.size());
  return true;
}

}  // namespace

std::string EncodeMessage(const Message& m) {
  ByteWriter w;
  w.U8(static_cast<uint8_t>(m.type));
  w.U32(m.from);
  w.U32(m.to);
  w.U64(m.term);
  switch (m.type) {
    case MessageType::kVoteRequest:
      w.U64(m.last_log_index);
      w.U64(m.last_log_term);
      break;
    case MessageType::kVoteReply:
      w.Bool(m.granted);
      break;
    case MessageType::kAppend:
      w.U64(m.prev_index);
      w.U64(m.prev_term);
      w.U64(m.commit);
      w.U32(static_cast<uint32_t>(m.entries.size()));
      for (const Entry& e : m.entries) PutEntry(w, e);
      break;
    case MessageType::kAppendReply:
      w.Bool(m.success);
      w.U64(m.match_index);
      break;
  }
  return w.Take();
}

bool DecodeMessage(std::string_view data, Message* m) {
  ByteReader r(data);
  uint8_t type = 0;
  if (!r.U8(&type) || type < 1 || type > 4) return false;
  *m = Message{};
  m->type = static_cast<MessageType>(type);
  if (!r.U32(&m->from) || !r.U32(&m->to) || !r.U64(&m->term)) return false;
  switch (m->type) {
    case MessageType::kVoteRequest:
      r.U64(&m->last_log_index);
      r.U64(&m->last_log_term);
      break;
    case MessageType::kVoteReply:
      r.Bool(&m->granted);
      break;
    case MessageType::kAppend: {
      uint32_t n = 0;
      r.U64(&m->prev_index);
      r.U64(&m->prev_term);
      r.U64(&m->commit);
      if (!r.U32(&n) || n > r.remaining() / 12) return false;  // each entry needs >= 12 bytes
      m->entries.resize(n);
      for (Entry& e : m->entries) {
        if (!GetEntry(r, &e)) return false;
      }
      break;
    }
    case MessageType::kAppendReply:
      r.Bool(&m->success);
      r.U64(&m->match_index);
      break;
  }
  return r.done();
}

const char* RoleName(Role role) {
  switch (role) {
    case Role::kFollower: return "follower";
    case Role::kCandidate: return "candidate";
    case Role::kLeader: return "leader";
  }
  return "?";
}

// ---- FileStorage --------------------------------------------------------------------------

namespace {
enum : uint8_t { kLogAppend = 1, kLogTruncate = 2 };
}

FileStorage::FileStorage(const std::string& dir, SyncMode sync) : dir_(dir), sync_(sync) {
  std::filesystem::create_directories(dir_);
  std::string meta;
  if (ReadWholeFile(dir_ + "/raft-meta", &meta)) {
    ByteReader r(meta);
    uint64_t t = 0;
    uint32_t v = 0;
    if (!r.U64(&t) || !r.U32(&v) || !r.done()) {
      throw std::runtime_error("raft-meta in " + dir_ + " is damaged");
    }
    memory_.SetTermAndVote(t, v);
  }
  const std::string path = dir_ + "/raft-log";
  const LogScan scan = ScanLog(path, [&](std::string_view payload) {
    ByteReader r(payload);
    uint8_t type = 0;
    uint64_t index = 0;
    if (!r.U8(&type) || !r.U64(&index)) return false;
    if (type == kLogTruncate) {
      if (!r.done() || index == 0 || index > memory_.last_index() + 1) return false;
      memory_.TruncateFrom(index);
      return true;
    }
    Entry e;
    if (type != kLogAppend || !GetEntry(r, &e) || !r.done() || index != memory_.last_index() + 1) {
      return false;
    }
    memory_.Append({e});
    return true;
  });
  // A torn tail is a record whose write was never acknowledged (we sync before replying), so it
  // is safe to drop. LogFile::Open cuts it off durably.
  log_ = LogFile::Open(path, scan.valid_bytes);
}

void FileStorage::SetTermAndVote(uint64_t new_term, NodeId vote) {
  ByteWriter w;
  w.U64(new_term);
  w.U32(vote);
  WriteFileAtomically(dir_ + "/raft-meta", w.buffer(), sync_);
  memory_.SetTermAndVote(new_term, vote);
}

void FileStorage::Append(const std::vector<Entry>& entries) {
  if (entries.empty()) return;
  std::string batch;
  uint64_t index = memory_.last_index();
  for (const Entry& e : entries) {
    ByteWriter w;
    w.U8(kLogAppend);
    w.U64(++index);
    PutEntry(w, e);
    AppendFramedRecord(batch, w.buffer());
  }
  log_->Write(batch);
  log_->Sync(sync_);  // one sync for the whole batch
  memory_.Append(entries);
}

void FileStorage::TruncateFrom(uint64_t index) {
  ByteWriter w;
  w.U8(kLogTruncate);
  w.U64(index);
  std::string record;
  AppendFramedRecord(record, w.buffer());
  log_->Write(record);
  log_->Sync(sync_);
  memory_.TruncateFrom(index);
}

// ---- RaftNode -----------------------------------------------------------------------------

RaftNode::RaftNode(Config config, Storage* storage)
    : config_(std::move(config)),
      storage_(storage),
      rng_state_(config_.seed * 0x9e3779b97f4a7c15ULL + config_.id) {
  if (config_.election_ticks_min == 0 || config_.election_ticks_max < config_.election_ticks_min) {
    throw std::invalid_argument("raft: bad election timeout range");
  }
  if (config_.heartbeat_ticks == 0 || config_.heartbeat_ticks >= config_.election_ticks_min) {
    throw std::invalid_argument("raft: heartbeat must be shorter than the election timeout");
  }
  const size_t n = config_.peers.size();
  votes_.assign(n, false);
  next_index_.assign(n, 1);
  match_index_.assign(n, 0);
  heard_from_.assign(n, false);
  ResetElectionTimer();
}

uint64_t RaftNode::NextRandom() {  // splitmix64: deterministic per (seed, id)
  uint64_t z = (rng_state_ += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

void RaftNode::ResetElectionTimer() {
  election_elapsed_ = 0;
  const uint32_t span = config_.election_ticks_max - config_.election_ticks_min + 1;
  election_timeout_ = config_.election_ticks_min + static_cast<uint32_t>(NextRandom() % span);
}

size_t RaftNode::PeerSlot(NodeId peer) const {
  for (size_t i = 0; i < config_.peers.size(); ++i) {
    if (config_.peers[i] == peer) return i;
  }
  return config_.peers.size();  // unknown sender
}

void RaftNode::Tick() {
  if (role_ == Role::kLeader) {
    if (++heartbeat_elapsed_ >= config_.heartbeat_ticks) {
      heartbeat_elapsed_ = 0;
      BroadcastAppend();
    }
    // Check-quorum (D-048): once per election timeout, a leader that heard from fewer than a
    // majority steps down, so a leader cut off by a partition stops accepting requests it can
    // never commit, and clients look for the real leader.
    if (config_.check_quorum && ++election_elapsed_ >= config_.election_ticks_max) {
      election_elapsed_ = 0;
      size_t heard = 1;  // itself
      for (size_t i = 0; i < heard_from_.size(); ++i) {
        if (heard_from_[i]) ++heard;
        heard_from_[i] = false;
      }
      if (heard < Majority()) BecomeFollower(term(), 0);
    }
    return;
  }
  if (++election_elapsed_ >= election_timeout_) BecomeCandidate();
}

void RaftNode::BecomeFollower(uint64_t new_term, NodeId new_leader) {
  if (new_term > term()) storage_->SetTermAndVote(new_term, 0);
  role_ = Role::kFollower;
  leader_ = new_leader;
  ResetElectionTimer();
}

void RaftNode::BecomeCandidate() {
  role_ = Role::kCandidate;
  leader_ = 0;
  ++elections_started_;
  // Term and self-vote in ONE durable write: a crash in between must not leave a node that bumped
  // its term but can still vote for someone else in it.
  storage_->SetTermAndVote(term() + 1, config_.id);
  std::fill(votes_.begin(), votes_.end(), false);
  ResetElectionTimer();
  if (Majority() == 1) {  // single-node cluster
    BecomeLeader();
    return;
  }
  for (NodeId peer : config_.peers) {
    Message m;
    m.type = MessageType::kVoteRequest;
    m.from = config_.id;
    m.to = peer;
    m.term = term();
    m.last_log_index = storage_->last_index();
    m.last_log_term = storage_->term_at(m.last_log_index);
    outbox_.push_back(std::move(m));
  }
}

void RaftNode::BecomeLeader() {
  role_ = Role::kLeader;
  leader_ = config_.id;
  heartbeat_elapsed_ = 0;
  election_elapsed_ = 0;
  std::fill(heard_from_.begin(), heard_from_.end(), false);
  std::fill(next_index_.begin(), next_index_.end(), storage_->last_index() + 1);
  std::fill(match_index_.begin(), match_index_.end(), 0);
  // The no-op (D-047): a leader may only count replicas for entries of ITS term, so entries left
  // over from earlier terms become committed only once an entry of the new term commits on top of
  // them. The no-op provides that entry immediately instead of waiting for a client write.
  storage_->Append({Entry{term(), {}}});
  AdvanceCommit();  // a single-node cluster commits at once
  BroadcastAppend();
}

void RaftNode::BroadcastAppend() {
  for (NodeId peer : config_.peers) SendAppend(peer);
}

void RaftNode::SendAppend(NodeId peer) {
  const size_t slot = PeerSlot(peer);
  uint64_t& next = next_index_[slot];
  Message m;
  m.type = MessageType::kAppend;
  m.from = config_.id;
  m.to = peer;
  m.term = term();
  m.prev_index = next - 1;
  m.prev_term = storage_->term_at(m.prev_index);
  m.commit = commit_;
  const uint64_t last = storage_->last_index();
  for (uint64_t i = next; i <= last && m.entries.size() < config_.max_entries_per_message; ++i) {
    m.entries.push_back(storage_->entry(i));
  }
  // Optimistic pipelining: assume the entries arrive, so the next message carries the ones after
  // them. If they are lost, the follower rejects the next message and its hint resets `next`.
  next += m.entries.size();
  outbox_.push_back(std::move(m));
}

uint64_t RaftNode::Propose(std::vector<std::string> data) {
  if (role_ != Role::kLeader || data.empty()) return 0;
  std::vector<Entry> entries;
  entries.reserve(data.size());
  for (std::string& d : data) entries.push_back(Entry{term(), std::move(d)});
  storage_->Append(entries);  // durable before anyone is told about it
  AdvanceCommit();
  BroadcastAppend();
  return storage_->last_index();
}

void RaftNode::AdvanceCommit() {
  if (role_ != Role::kLeader) return;
  const uint64_t last = storage_->last_index();
  for (uint64_t n = last; n > commit_; --n) {
    // Figure 8 of the Raft paper: only entries of the CURRENT term are committed by counting
    // replicas. Earlier entries commit indirectly, by the Log Matching property.
    if (storage_->term_at(n) != term()) break;
    size_t replicas = 1;  // the leader has every entry it has appended (durably)
    for (uint64_t m : match_index_) {
      if (m >= n) ++replicas;
    }
    if (replicas >= Majority()) {
      commit_ = n;
      break;
    }
  }
}

void RaftNode::Step(const Message& m) {
  if (m.to != config_.id || PeerSlot(m.from) == config_.peers.size()) return;  // not for us
  if (m.term > term()) {
    // Any message from a newer term means we are out of date. Becoming a follower clears the
    // vote. For an Append we also learn who leads that term.
    BecomeFollower(m.term, m.type == MessageType::kAppend ? m.from : 0);
  }
  switch (m.type) {
    case MessageType::kVoteRequest: HandleVoteRequest(m); break;
    case MessageType::kVoteReply: HandleVoteReply(m); break;
    case MessageType::kAppend: HandleAppend(m); break;
    case MessageType::kAppendReply: HandleAppendReply(m); break;
  }
}

bool RaftNode::CandidateLogIsUpToDate(uint64_t last_log_index, uint64_t last_log_term) const {
  // Election restriction (§5.4.1): vote only for a candidate whose log is at least as up to date
  // as ours, so a leader always holds every committed entry.
  const uint64_t my_last = storage_->last_index();
  const uint64_t my_term = storage_->term_at(my_last);
  return last_log_term > my_term || (last_log_term == my_term && last_log_index >= my_last);
}

void RaftNode::HandleVoteRequest(const Message& m) {
  Message reply;
  reply.type = MessageType::kVoteReply;
  reply.from = config_.id;
  reply.to = m.from;
  const bool can_vote = storage_->voted_for() == 0 || storage_->voted_for() == m.from;
  if (m.term == term() && can_vote && role_ == Role::kFollower &&
      CandidateLogIsUpToDate(m.last_log_index, m.last_log_term)) {
    storage_->SetTermAndVote(term(), m.from);  // durable BEFORE the reply leaves
    ResetElectionTimer();                      // granting a vote counts as hearing from a peer
    reply.granted = true;
  }
  reply.term = term();
  outbox_.push_back(std::move(reply));
}

void RaftNode::HandleVoteReply(const Message& m) {
  if (role_ != Role::kCandidate || m.term != term() || !m.granted) return;
  votes_[PeerSlot(m.from)] = true;
  size_t granted = 1;  // our own vote
  for (bool v : votes_) {
    if (v) ++granted;
  }
  if (granted >= Majority()) BecomeLeader();
}

void RaftNode::HandleAppend(const Message& m) {
  Message reply;
  reply.type = MessageType::kAppendReply;
  reply.from = config_.id;
  reply.to = m.from;
  if (m.term < term()) {  // a stale leader: tell it the newer term so it steps down
    reply.term = term();
    reply.success = false;
    reply.match_index = 0;
    outbox_.push_back(std::move(reply));
    return;
  }
  // m.term == term(): m.from is the one leader of this term.
  if (role_ != Role::kFollower) BecomeFollower(term(), m.from);
  leader_ = m.from;
  ResetElectionTimer();
  reply.term = term();

  const uint64_t last = storage_->last_index();
  if (m.prev_index > last) {
    reply.success = false;
    reply.match_index = last + 1;  // hint: we are missing everything after `last`
    outbox_.push_back(std::move(reply));
    return;
  }
  if (storage_->term_at(m.prev_index) != m.prev_term) {
    // Conflict: skip back over the whole conflicting term in one round trip instead of one
    // entry per round trip.
    const uint64_t conflict_term = storage_->term_at(m.prev_index);
    uint64_t first = m.prev_index;
    while (first > 1 && storage_->term_at(first - 1) == conflict_term) --first;
    reply.success = false;
    reply.match_index = first;
    outbox_.push_back(std::move(reply));
    return;
  }
  // prev matches. Append what we do not have; on the first conflicting entry, cut our log there
  // (an uncommitted suffix from an old leader) and take the leader's. Entries we already have are
  // skipped, so a duplicated or reordered message can never truncate committed entries.
  size_t i = 0;
  for (; i < m.entries.size(); ++i) {
    const uint64_t index = m.prev_index + 1 + i;
    if (index > storage_->last_index()) break;
    if (storage_->term_at(index) != m.entries[i].term) {
      storage_->TruncateFrom(index);
      break;
    }
  }
  if (i < m.entries.size()) {
    storage_->Append(std::vector<Entry>(m.entries.begin() + static_cast<std::ptrdiff_t>(i),
                                        m.entries.end()));
  }
  const uint64_t match = m.prev_index + m.entries.size();
  if (m.commit > commit_) commit_ = std::min(m.commit, match);
  reply.success = true;
  reply.match_index = match;
  outbox_.push_back(std::move(reply));
}

void RaftNode::HandleAppendReply(const Message& m) {
  if (role_ != Role::kLeader || m.term != term()) return;
  const size_t slot = PeerSlot(m.from);
  heard_from_[slot] = true;
  if (m.success) {
    // Replies can arrive out of order or twice: only ever move match forward.
    if (m.match_index > match_index_[slot]) match_index_[slot] = m.match_index;
    if (next_index_[slot] < match_index_[slot] + 1) next_index_[slot] = match_index_[slot] + 1;
    AdvanceCommit();
    if (next_index_[slot] <= storage_->last_index()) SendAppend(m.from);
    return;
  }
  // Rejected: back off to the follower's hint (never below match + 1, never to 0) and retry now.
  next_index_[slot] = std::max<uint64_t>({1, match_index_[slot] + 1, std::min(m.match_index, next_index_[slot])});
  SendAppend(m.from);
}

std::vector<Message> RaftNode::TakeMessages() {
  std::vector<Message> out;
  out.swap(outbox_);
  return out;
}

std::vector<std::pair<uint64_t, Entry>> RaftNode::TakeCommitted() {
  std::vector<std::pair<uint64_t, Entry>> out;
  while (applied_ < commit_) {
    ++applied_;
    out.emplace_back(applied_, storage_->entry(applied_));
  }
  return out;
}

}  // namespace lldstore::raft
