// kvnode: one member of the replicated lldstore cluster (Phase 8 + 11).
//
//   kvnode --id=1 \
//          --raft=1=127.0.0.1:7001,2=127.0.0.1:7002,3=127.0.0.1:7003 \
//          --client=1=127.0.0.1:8001,2=127.0.0.1:8002,3=127.0.0.1:8003 \
//          --data=/tmp/lldstore/1 [--metrics=127.0.0.1:9101] [--listen-host=0.0.0.0]
//          [--tick-ms=10] [--election-ticks=15,30] [--heartbeat-ticks=3] [--sync=data|none]
//
// Stops cleanly on SIGINT/SIGTERM. kill -9 is the crash test: everything acknowledged survives.
#include <csignal>
#include <cstdio>
#include <exception>
#include <thread>

#include "flags.h"
#include "lldstore/kv_node.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }  // async-signal-safe: only set a flag (CS:APP 8.5.5)

std::map<lldstore::raft::NodeId, std::string> ParseIdMap(const std::string& text) {
  std::map<lldstore::raft::NodeId, std::string> out;
  for (const std::string& item : lldstore::app::Split(text, ',')) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) throw std::invalid_argument("expected id=host:port, got " + item);
    out[static_cast<lldstore::raft::NodeId>(std::stoul(item.substr(0, eq)))] = item.substr(eq + 1);
  }
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace lldstore;
  try {
    app::Flags flags(argc, argv);
    KvNodeOptions o;
    o.id = static_cast<raft::NodeId>(flags.Int("id", 0));
    o.raft_addresses = ParseIdMap(flags.Get("raft"));
    o.client_addresses = ParseIdMap(flags.Get("client"));
    o.data_dir = flags.Get("data");
    o.metrics_address = flags.Get("metrics");
    o.listen_host = flags.Get("listen-host");
    o.tick = std::chrono::milliseconds(flags.Int("tick-ms", 10));
    const auto election = app::Split(flags.Get("election-ticks", "15,30"), ',');
    o.raft.election_ticks_min = static_cast<uint32_t>(std::stoul(election.at(0)));
    o.raft.election_ticks_max = static_cast<uint32_t>(std::stoul(election.at(1)));
    o.raft.heartbeat_ticks = static_cast<uint32_t>(flags.Int("heartbeat-ticks", 3));
    const std::string sync = flags.Get("sync", "data");
    o.sync = sync == "none" ? SyncMode::kNone : SyncMode::kData;
    if (!flags.Unused().empty()) {
      std::fprintf(stderr, "unknown flag --%s\n", flags.Unused().c_str());
      return 2;
    }
    if (o.id == 0 || o.data_dir.empty()) {
      std::fprintf(stderr, "need --id and --data (see the comment at the top of apps/kvnode.cpp)\n");
      return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);  // a vanished peer must surface as an error, not kill us
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    KvNode node(o);
    node.Start();
    std::fprintf(stderr, "kvnode %u up: raft %s, clients %s, data %s, sync=%s\n", o.id,
                 o.raft_addresses.at(o.id).c_str(), o.client_addresses.at(o.id).c_str(),
                 o.data_dir.c_str(), sync.c_str());
    raft::Role last_role = raft::Role::kFollower;
    uint64_t last_term = 0;
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (node.role() != last_role || node.term() != last_term) {
        last_role = node.role();
        last_term = node.term();
        std::fprintf(stderr, "kvnode %u: %s in term %llu (leader %u)\n", o.id, raft::RoleName(last_role),
                     static_cast<unsigned long long>(last_term), node.leader());
      }
    }
    std::fprintf(stderr, "kvnode %u stopping\n", o.id);
    node.Stop();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "kvnode: %s\n", e.what());
    return 1;
  }
}
