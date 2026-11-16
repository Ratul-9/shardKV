#include "lldstore/kv_node.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <stdexcept>

#include "lldstore/kv_protocol.h"

namespace lldstore {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxPeerQueue = 10000;  // frames; beyond this the oldest are dropped
constexpr int kSendTimeoutMs = 1000;

uint64_t NanosSince(Clock::time_point t) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
}

net::HostPort MustParse(const std::string& text) {
  net::HostPort hp;
  if (!net::ParseHostPort(text, &hp)) throw std::invalid_argument("bad address: " + text);
  return hp;
}

// Waits up to `ms` for a listening socket to have a connection; lets accept loops notice Stop().
bool ReadableWithin(int fd, int ms) {
  pollfd p{fd, POLLIN, 0};
  return ::poll(&p, 1, ms) == 1;
}

}  // namespace

KvNode::KvNode(KvNodeOptions options) : options_(std::move(options)) {
  if (options_.raft_addresses.count(options_.id) == 0 || options_.client_addresses.count(options_.id) == 0) {
    throw std::invalid_argument("this node's id is not in the cluster address lists");
  }
  raft::Config config = options_.raft;
  config.id = options_.id;
  config.peers.clear();
  for (const auto& [id, addr] : options_.raft_addresses) {
    if (id == options_.id) continue;
    config.peers.push_back(id);
    auto peer = std::make_unique<Peer>();
    peer->id = id;
    peer->address = MustParse(addr);
    peers_.push_back(std::move(peer));
  }
  config.seed = config.seed * 31 + options_.id;
  storage_ = std::make_unique<raft::FileStorage>(options_.data_dir, options_.sync);
  node_ = std::make_unique<raft::RaftNode>(config, storage_.get());

  requests_ = &metrics_.counter("lldstore_requests_total", "Client commands received");
  not_leader_ = &metrics_.counter("lldstore_not_leader_total", "Commands refused: not the leader");
  retries_ = &metrics_.counter("lldstore_retry_total", "Commands answered 'retry' after a leadership change");
  batches_ = &metrics_.counter("lldstore_proposal_batches_total", "Raft proposals (one durable append each)");
  batched_commands_ = &metrics_.counter("lldstore_proposed_commands_total", "Commands proposed");
  peer_messages_in_ = &metrics_.counter("lldstore_raft_messages_in_total", "Raft messages received");
  peer_messages_dropped_ = &metrics_.counter("lldstore_raft_messages_dropped_total", "Raft messages dropped by full or broken links");
  term_gauge_ = &metrics_.gauge("lldstore_raft_term", "Current Raft term");
  role_gauge_ = &metrics_.gauge("lldstore_raft_role", "0 follower, 1 candidate, 2 leader");
  commit_gauge_ = &metrics_.gauge("lldstore_raft_commit_index", "Highest committed log index");
  applied_gauge_ = &metrics_.gauge("lldstore_raft_applied_index", "Highest applied log index");
  log_gauge_ = &metrics_.gauge("lldstore_raft_last_index", "Last log index");
  elections_gauge_ = &metrics_.gauge("lldstore_raft_elections_started", "Elections started by this node since start");
  keys_gauge_ = &metrics_.gauge("lldstore_keys", "Keys in the state machine");
  latency_ = &metrics_.histogram("lldstore_request_latency_ns", "Command latency inside the node, receive to reply (ns)");
  append_latency_ = &metrics_.histogram("lldstore_raft_append_latency_ns", "Leader durable append of one proposal batch (ns)");
}

KvNode::~KvNode() { Stop(); }

void KvNode::Start() {
  auto bind_addr = [&](const std::string& advertised) {
    net::HostPort hp = MustParse(advertised);
    if (!options_.listen_host.empty()) hp.host = options_.listen_host;
    return hp;
  };
  peer_listen_fd_ = net::Listen(bind_addr(options_.raft_addresses.at(options_.id)));
  client_listen_fd_ = net::Listen(bind_addr(options_.client_addresses.at(options_.id)));
  if (!options_.metrics_address.empty()) metrics_listen_fd_ = net::Listen(bind_addr(options_.metrics_address));

  loop_thread_ = std::thread([this] { LoopMain(); });
  for (auto& peer : peers_) {
    Peer* p = peer.get();
    p->thread = std::thread([this, p] { SenderMain(p); });
  }
  accept_threads_.emplace_back([this] { AcceptMain(peer_listen_fd_, true); });
  accept_threads_.emplace_back([this] { AcceptMain(client_listen_fd_, false); });
  if (metrics_listen_fd_ >= 0) accept_threads_.emplace_back([this] { MetricsMain(metrics_listen_fd_); });
}

void KvNode::Stop() {
  if (stopping_.exchange(true)) return;
  inbox_cv_.notify_all();
  if (loop_thread_.joinable()) loop_thread_.join();
  for (auto& peer : peers_) {
    peer->cv.notify_all();
    if (peer->thread.joinable()) peer->thread.join();
  }
  for (auto& t : accept_threads_) t.join();  // they poll stopping_ every 100 ms
  accept_threads_.clear();
  {
    std::lock_guard<std::mutex> lock(conns_mu_);
    for (int fd : conn_fds_) net::Shutdown(fd);  // wakes the blocked readers
  }
  for (auto& t : conn_threads_) t.join();
  conn_threads_.clear();
  for (int fd : conn_fds_) net::Close(fd);
  conn_fds_.clear();
  net::Close(peer_listen_fd_);
  net::Close(client_listen_fd_);
  net::Close(metrics_listen_fd_);
}

void KvNode::Push(Event e) {
  {
    std::lock_guard<std::mutex> lock(inbox_mu_);
    inbox_.push_back(std::move(e));
  }
  inbox_cv_.notify_one();
}

void KvNode::TrackConnection(int fd, std::thread t) {
  std::lock_guard<std::mutex> lock(conns_mu_);
  conn_fds_.push_back(fd);
  conn_threads_.push_back(std::move(t));
}

std::string KvNode::LeaderHint() const {
  const raft::NodeId l = leader_.load();
  auto it = options_.client_addresses.find(l);
  return it == options_.client_addresses.end() ? std::string() : it->second;
}

std::string KvNode::StatusLine() const {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "id=%u role=%s term=%llu leader=%u", options_.id,
                raft::RoleName(static_cast<raft::Role>(role_.load())),
                static_cast<unsigned long long>(term_.load()), leader_.load());
  return buf;
}

// ---- Loop ---------------------------------------------------------------------------------

void KvNode::LoopMain() {
  auto next_tick = Clock::now() + options_.tick;
  std::deque<Event> events;
  std::vector<std::string> proposals;
  std::vector<Event*> proposers;

  auto reply = [&](const std::shared_ptr<ClientConn>& conn, proto::Response r, Clock::time_point received) {
    latency_->Record(NanosSince(received));
    conn->Send(proto::Encode(r));  // a failed send means the client went away; it will retry
  };
  auto refuse = [&](Event& e, proto::Status status) {
    proto::Response r;
    r.status = status;
    r.id = e.request_id;
    r.leader_hint = LeaderHint();
    if (status == proto::Status::kNotLeader) not_leader_->Inc();
    reply(e.conn, std::move(r), e.received);
  };
  auto flush = [&] {
    if (proposals.empty()) return;
    const size_t n = proposals.size();
    const auto t0 = Clock::now();
    const uint64_t last = node_->Propose(std::move(proposals));
    proposals.clear();
    if (last == 0) {
      for (Event* e : proposers) refuse(*e, proto::Status::kNotLeader);
    } else {
      append_latency_->Record(NanosSince(t0));
      batches_->Inc();
      batched_commands_->Inc(n);
      const uint64_t first = last - n + 1;
      for (size_t k = 0; k < n; ++k) {
        Event* e = proposers[k];
        pending_[first + k] = PendingRequest{node_->term(), e->conn, e->request_id, e->command.op, e->received};
      }
    }
    proposers.clear();
  };

  while (!stopping_.load()) {
    {
      std::unique_lock<std::mutex> lock(inbox_mu_);
      inbox_cv_.wait_until(lock, next_tick, [&] { return !inbox_.empty() || stopping_.load(); });
      events.swap(inbox_);
    }
    for (Event& e : events) {
      if (e.kind == Event::Kind::kPeer) {
        peer_messages_in_->Inc();
        node_->Step(e.message);
        continue;
      }
      requests_->Inc();
      if (node_->role() != raft::Role::kLeader) {
        refuse(e, proto::Status::kNotLeader);
        continue;
      }
      proposals.push_back(EncodeKvCommand(e.command));
      proposers.push_back(&e);
      if (proposals.size() >= options_.max_batch) flush();
    }
    flush();
    events.clear();

    if (Clock::now() >= next_tick) {
      node_->Tick();
      next_tick += options_.tick;
      if (next_tick < Clock::now()) next_tick = Clock::now() + options_.tick;  // fell behind
    }

    for (raft::Message& m : node_->TakeMessages()) {
      for (auto& peer : peers_) {
        if (peer->id != m.to) continue;
        {
          std::lock_guard<std::mutex> lock(peer->mu);
          if (peer->queue.size() >= kMaxPeerQueue) {
            peer->queue.pop_front();
            peer_messages_dropped_->Inc();
          }
          peer->queue.push_back(raft::EncodeMessage(m));
        }
        peer->cv.notify_one();
      }
    }

    for (auto& [index, entry] : node_->TakeCommitted()) {
      KvResult result;
      if (!entry.data.empty()) {
        KvCommand cmd;
        if (DecodeKvCommand(entry.data, &cmd)) result = machine_.Apply(cmd);
      }
      auto it = pending_.find(index);
      if (it == pending_.end()) continue;
      proto::Response r;
      r.id = it->second.request_id;
      if (it->second.term == entry.term) {  // our proposal is the one that committed here
        r.status = proto::Status::kOk;
        r.ok = result.ok;
        r.value = std::move(result.value);
      } else {
        r.status = proto::Status::kRetry;
        retries_->Inc();
      }
      reply(it->second.conn, std::move(r), it->second.received);
      pending_.erase(it);
    }

    // Lost leadership: the pending commands may or may not commit under the next leader. Say
    // "retry"; the client resends with the same (client_id, seq), and the session table makes
    // that safe whichever way it went.
    if (node_->role() != raft::Role::kLeader && !pending_.empty()) {
      for (auto& [index, p] : pending_) {
        proto::Response r;
        r.status = proto::Status::kRetry;
        r.id = p.request_id;
        r.leader_hint = LeaderHint();
        retries_->Inc();
        reply(p.conn, std::move(r), p.received);
      }
      pending_.clear();
    }

    role_.store(static_cast<int>(node_->role()));
    term_.store(node_->term());
    leader_.store(node_->leader());
    term_gauge_->Set(static_cast<int64_t>(node_->term()));
    role_gauge_->Set(static_cast<int64_t>(node_->role()));
    commit_gauge_->Set(static_cast<int64_t>(node_->commit_index()));
    applied_gauge_->Set(static_cast<int64_t>(node_->last_applied()));
    log_gauge_->Set(static_cast<int64_t>(node_->last_index()));
    elections_gauge_->Set(static_cast<int64_t>(node_->elections_started()));
    keys_gauge_->Set(static_cast<int64_t>(machine_.Size()));
  }
  // Shutting down: anyone still waiting gets "retry" rather than silence.
  for (auto& [index, p] : pending_) {
    proto::Response r;
    r.status = proto::Status::kRetry;
    r.id = p.request_id;
    p.conn->Send(proto::Encode(r));
  }
  pending_.clear();
}

// ---- Peers --------------------------------------------------------------------------------

void KvNode::SenderMain(Peer* peer) {
  int fd = -1;
  std::deque<std::string> batch;
  while (!stopping_.load()) {
    {
      std::unique_lock<std::mutex> lock(peer->mu);
      peer->cv.wait_for(lock, std::chrono::milliseconds(100),
                        [&] { return !peer->queue.empty() || stopping_.load(); });
      batch.swap(peer->queue);
    }
    if (batch.empty()) continue;
    if (fd < 0) {
      fd = net::Connect(peer->address, 200);
      if (fd < 0) {
        peer_messages_dropped_->Inc(batch.size());  // peer down: Raft will resend what matters
        batch.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      net::ConfigureConnection(fd, kSendTimeoutMs);
    }
    for (const std::string& frame : batch) {
      if (!net::SendFrame(fd, frame)) {
        net::Close(fd);
        fd = -1;
        peer_messages_dropped_->Inc();
        break;
      }
    }
    batch.clear();
  }
  net::Close(fd);
}

void KvNode::AcceptMain(int listen_fd, bool peers) {
  while (!stopping_.load()) {
    if (!ReadableWithin(listen_fd, 100)) continue;
    const int fd = net::Accept(listen_fd);
    if (fd < 0) continue;
    net::ConfigureConnection(fd, kSendTimeoutMs);
    if (peers) {
      TrackConnection(fd, std::thread([this, fd] { PeerReaderMain(fd); }));
    } else {
      auto conn = std::make_shared<ClientConn>();
      conn->fd = fd;
      TrackConnection(fd, std::thread([this, conn] { ClientReaderMain(conn); }));
    }
  }
}

void KvNode::PeerReaderMain(int fd) {
  std::string frame;
  while (!stopping_.load() && net::RecvFrame(fd, &frame, -1) == net::RecvStatus::kOk) {
    Event e;
    e.kind = Event::Kind::kPeer;
    if (!raft::DecodeMessage(frame, &e.message)) break;  // garbage: drop the connection
    Push(std::move(e));
  }
}

void KvNode::ClientReaderMain(std::shared_ptr<ClientConn> conn) {
  std::string frame;
  while (!stopping_.load() && net::RecvFrame(conn->fd, &frame, -1) == net::RecvStatus::kOk) {
    proto::Request req;
    if (!proto::Decode(frame, &req)) {
      proto::Response r;
      r.status = proto::Status::kBadRequest;
      conn->Send(proto::Encode(r));
      break;
    }
    if (req.kind == proto::RequestKind::kStatus) {
      proto::Response r;
      r.id = req.id;
      r.ok = true;
      r.info = StatusLine();
      r.leader_hint = LeaderHint();
      conn->Send(proto::Encode(r));
      continue;
    }
    Event e;
    e.kind = Event::Kind::kClient;
    e.conn = conn;
    e.request_id = req.id;
    e.command = std::move(req.command);
    e.received = Clock::now();
    Push(std::move(e));
  }
}

// ---- Metrics endpoint (HTTP/1.0, GET anything -> Prometheus text) --------------------------

void KvNode::MetricsMain(int listen_fd) {
  while (!stopping_.load()) {
    if (!ReadableWithin(listen_fd, 100)) continue;
    const int fd = net::Accept(listen_fd);
    if (fd < 0) continue;
    net::ConfigureConnection(fd, kSendTimeoutMs);
    char request[2048];
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 1000) == 1) (void)::recv(fd, request, sizeof(request), 0);
    const std::string body = metrics_.RenderPrometheus();
    const std::string response = "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < response.size()) {
      const ssize_t n = ::send(fd, response.data() + sent, response.size() - sent, 0);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    net::Close(fd);
  }
}

}  // namespace lldstore
