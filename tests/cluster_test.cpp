// Phase 8 end-to-end: three real KvNodes in this process, talking TCP over localhost, with real
// files (sync off for speed: this test is about the protocol and failover, not the disk).
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/kv_client.h"
#include "lldstore/kv_node.h"
#include "lldstore/linearizability.h"
#include "test_dir.h"

namespace lldstore {
namespace {

using namespace std::chrono_literals;

class LocalCluster {
 public:
  explicit LocalCluster(int n) : n_(n) {
    // Ports derived from the pid so parallel ctest runs do not collide.
    const int base = 30000 + (static_cast<int>(::getpid()) % 2000) * 10;
    for (int i = 1; i <= n; ++i) {
      raft_[static_cast<raft::NodeId>(i)] = "127.0.0.1:" + std::to_string(base + i);
      client_[static_cast<raft::NodeId>(i)] = "127.0.0.1:" + std::to_string(base + 5 + i);
      servers_.push_back(client_[static_cast<raft::NodeId>(i)]);
    }
    nodes_.resize(static_cast<size_t>(n));
    for (int i = 1; i <= n; ++i) Start(i);
  }

  void Start(int i) {
    KvNodeOptions o;
    o.id = static_cast<raft::NodeId>(i);
    o.raft_addresses = raft_;
    o.client_addresses = client_;
    o.data_dir = dir_.path() + "/node" + std::to_string(i);
    o.sync = SyncMode::kNone;
    o.tick = 5ms;
    o.raft.election_ticks_min = 20;
    o.raft.election_ticks_max = 40;
    o.raft.heartbeat_ticks = 4;
    auto node = std::make_unique<KvNode>(o);
    node->Start();
    nodes_[static_cast<size_t>(i - 1)] = std::move(node);
  }

  void Stop(int i) { nodes_[static_cast<size_t>(i - 1)].reset(); }

  int WaitForLeader(std::chrono::milliseconds limit = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      for (int i = 1; i <= n_; ++i) {
        auto& node = nodes_[static_cast<size_t>(i - 1)];
        if (node && node->role() == raft::Role::kLeader) return i;
      }
      std::this_thread::sleep_for(10ms);
    }
    return 0;
  }

  KvClientOptions ClientOptions() const {
    KvClientOptions o;
    o.servers = servers_;
    o.attempt_timeout = 300ms;
    o.total_timeout = 10s;
    return o;
  }

 private:
  int n_;
  testing::TempDir dir_;
  std::map<raft::NodeId, std::string> raft_, client_;
  std::vector<std::string> servers_;
  std::vector<std::unique_ptr<KvNode>> nodes_;
};

TEST(Cluster, PutGetDeleteThroughTheLeader) {
  LocalCluster cluster(3);
  ASSERT_GT(cluster.WaitForLeader(), 0);
  KvClient client(cluster.ClientOptions());
  ASSERT_EQ(client.Put("a", "1").outcome, KvClient::Outcome::kOk);
  const auto got = client.Get("a");
  ASSERT_EQ(got.outcome, KvClient::Outcome::kOk);
  EXPECT_TRUE(got.ok);
  EXPECT_EQ(got.value, "1");
  EXPECT_TRUE(client.Delete("a").ok);
  EXPECT_FALSE(client.Get("a").ok);
}

TEST(Cluster, SurvivesLosingTheLeaderWithoutLosingAcknowledgedWrites) {
  LocalCluster cluster(3);
  const int leader = cluster.WaitForLeader();
  ASSERT_GT(leader, 0);
  KvClient client(cluster.ClientOptions());
  for (int i = 0; i < 50; ++i) {
    ASSERT_EQ(client.Put("k" + std::to_string(i), "v" + std::to_string(i)).outcome, KvClient::Outcome::kOk);
  }
  const auto killed_at = std::chrono::steady_clock::now();
  cluster.Stop(leader);
  ASSERT_GT(cluster.WaitForLeader(), 0);
  const auto failover = std::chrono::steady_clock::now() - killed_at;
  std::printf("[cluster] failover took %lld ms\n",
              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(failover).count()));
  for (int i = 0; i < 50; ++i) {
    const auto r = client.Get("k" + std::to_string(i));
    ASSERT_EQ(r.outcome, KvClient::Outcome::kOk) << i;
    ASSERT_TRUE(r.ok) << i;
    EXPECT_EQ(r.value, "v" + std::to_string(i));
  }
  cluster.Start(leader);  // rejoins from its files and catches up
  ASSERT_EQ(client.Put("after", "x").outcome, KvClient::Outcome::kOk);
}

TEST(Cluster, HistoryWithAKilledLeaderIsLinearizable) {
  LocalCluster cluster(3);
  ASSERT_GT(cluster.WaitForLeader(), 0);
  std::vector<lin::Operation> history;
  std::mutex mu;
  const auto start = std::chrono::steady_clock::now();
  auto now = [&] {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  };
  std::atomic<bool> stop{false};
  std::vector<std::thread> clients;
  for (int c = 0; c < 3; ++c) {
    clients.emplace_back([&, c] {
      KvClient client(cluster.ClientOptions());
      std::vector<lin::Operation> mine;
      for (int i = 0; !stop.load(); ++i) {
        lin::Operation op;
        op.client = client.client_id();
        op.key = "x" + std::to_string(i % 2);
        op.call = now();
        KvClient::Reply r;
        if (i % 3 == 0) {
          op.type = lin::OpType::kPut;
          op.value = std::to_string(c) + "-" + std::to_string(i);
          r = client.Put(op.key, op.value);
        } else {
          op.type = lin::OpType::kGet;
          r = client.Get(op.key);
        }
        if (r.outcome == KvClient::Outcome::kOk) {
          op.ret = now();
          op.found = r.ok;
          op.output = r.value;
        } else {
          op.known = false;
          op.ret = lin::kNoReturn;
        }
        mine.push_back(op);
      }
      std::lock_guard<std::mutex> lock(mu);
      history.insert(history.end(), mine.begin(), mine.end());
    });
  }
  std::this_thread::sleep_for(500ms);
  const int leader = cluster.WaitForLeader();
  cluster.Stop(leader);
  std::this_thread::sleep_for(1500ms);
  cluster.Start(leader);
  std::this_thread::sleep_for(500ms);
  stop = true;
  for (auto& t : clients) t.join();
  ASSERT_GT(history.size(), 50u);
  const auto result = lin::Check(history);
  EXPECT_EQ(result.verdict, lin::Verdict::kLinearizable) << "key " << result.key;
}

}  // namespace
}  // namespace lldstore
