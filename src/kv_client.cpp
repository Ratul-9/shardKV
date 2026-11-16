#include "lldstore/kv_client.h"

#include <random>
#include <stdexcept>
#include <thread>

#include "lldstore/kv_protocol.h"

namespace lldstore {

KvClient::KvClient(KvClientOptions options) : options_(std::move(options)) {
  if (options_.servers.empty()) throw std::invalid_argument("KvClient: no servers");
  for (const std::string& s : options_.servers) {
    net::HostPort hp;
    if (!net::ParseHostPort(s, &hp)) throw std::invalid_argument("KvClient: bad address " + s);
    addresses_.push_back(hp);
  }
  fds_.assign(addresses_.size(), -1);
  client_id_ = options_.client_id;
  if (client_id_ == 0) {
    std::random_device rd;
    client_id_ = (uint64_t{rd()} << 32 | rd()) | 1;  // never 0 (0 means "no session")
  }
  leader_guess_ = client_id_ % addresses_.size();  // spread first contacts across nodes
}

KvClient::~KvClient() {
  for (size_t i = 0; i < fds_.size(); ++i) Disconnect(i);
}

bool KvClient::EnsureConnected(size_t server) {
  if (fds_[server] >= 0) return true;
  const int fd = net::Connect(addresses_[server], static_cast<int>(options_.attempt_timeout.count()));
  if (fd < 0) return false;
  net::ConfigureConnection(fd, static_cast<int>(options_.attempt_timeout.count()));
  fds_[server] = fd;
  return true;
}

void KvClient::Disconnect(size_t server) {
  net::Close(fds_[server]);
  fds_[server] = -1;
}

KvClient::Reply KvClient::Put(std::string_view key, std::string_view value) {
  KvCommand c;
  c.op = KvCommand::Op::kPut;
  c.key = std::string(key);
  c.value = std::string(value);
  return Execute(std::move(c));
}

KvClient::Reply KvClient::Get(std::string_view key) {
  KvCommand c;
  c.op = KvCommand::Op::kGet;
  c.key = std::string(key);
  return Execute(std::move(c));
}

KvClient::Reply KvClient::Delete(std::string_view key) {
  KvCommand c;
  c.op = KvCommand::Op::kDelete;
  c.key = std::string(key);
  return Execute(std::move(c));
}

KvClient::Reply KvClient::Execute(KvCommand command) {
  command.client_id = client_id_;
  command.seq = ++seq_;  // the same seq for every retry of THIS request
  const auto deadline = std::chrono::steady_clock::now() + options_.total_timeout;
  size_t target = leader_guess_;
  while (std::chrono::steady_clock::now() < deadline) {
    ++attempts_;
    if (!EnsureConnected(target)) {
      target = (target + 1) % addresses_.size();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    proto::Request req;
    req.kind = proto::RequestKind::kCommand;
    req.id = ++request_id_;
    req.command = command;
    std::string frame;
    proto::Response resp;
    if (!net::SendFrame(fds_[target], proto::Encode(req)) ||
        net::RecvFrame(fds_[target], &frame, static_cast<int>(options_.attempt_timeout.count())) !=
            net::RecvStatus::kOk ||
        !proto::Decode(frame, &resp) || resp.id != req.id) {
      // Timed out or broken. Drop the connection, so a late reply to this attempt can never be
      // read as the reply to a later one, and try the next node.
      Disconnect(target);
      target = (target + 1) % addresses_.size();
      continue;
    }
    switch (resp.status) {
      case proto::Status::kOk:
        leader_guess_ = target;
        return Reply{Outcome::kOk, resp.ok, std::move(resp.value)};
      case proto::Status::kNotLeader: {
        size_t next = (target + 1) % addresses_.size();
        for (size_t i = 0; i < options_.servers.size(); ++i) {
          if (!resp.leader_hint.empty() && options_.servers[i] == resp.leader_hint) next = i;
        }
        if (next == target) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        target = next;
        break;
      }
      case proto::Status::kRetry:
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // an election is under way
        break;
      case proto::Status::kBadRequest:
        throw std::runtime_error("KvClient: the server rejected the request as malformed");
    }
  }
  return Reply{};  // kUnknown
}

std::string KvClient::Status(size_t server) {
  if (server >= addresses_.size() || !EnsureConnected(server)) return {};
  proto::Request req;
  req.kind = proto::RequestKind::kStatus;
  req.id = ++request_id_;
  std::string frame;
  proto::Response resp;
  if (!net::SendFrame(fds_[server], proto::Encode(req)) ||
      net::RecvFrame(fds_[server], &frame, static_cast<int>(options_.attempt_timeout.count())) !=
          net::RecvStatus::kOk ||
      !proto::Decode(frame, &resp) || resp.id != req.id) {
    Disconnect(server);
    return {};
  }
  return resp.info;
}

}  // namespace lldstore
