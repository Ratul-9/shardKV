// kvctl: command-line client, load generator and history recorder for the cluster.
//
//   kvctl --servers=127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003 put KEY VALUE
//   kvctl --servers=... get KEY
//   kvctl --servers=... del KEY
//   kvctl --servers=... status
//   kvctl --servers=... bench [--mode=closed|open] [--clients=8] [--seconds=10] [--warmup=2]
//                             [--rate=OPS_PER_SEC_TOTAL] [--keys=10000] [--read-pct=90] [--value-size=64]
//   kvctl --servers=... history --out=FILE [--clients=4] [--seconds=30] [--keys=5]
//
// bench: closed loop reports maximum throughput only; open loop reports latency percentiles from
// each request's INTENDED start time (no coordinated omission), as in Phases 0-2.
// history: every client records each operation's invocation/response time and result (or
// "unknown" on timeout) into FILE, for `lincheck`. Run it while killing nodes (scripts/chaos_local.sh).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "flags.h"
#include "lldstore/kv_client.h"
#include "lldstore/linearizability.h"
#include "lldstore/multi_client.h"

namespace {

using namespace lldstore;

KvClientOptions ClientOptions(app::Flags& flags) {
  KvClientOptions o;
  o.servers = app::Split(flags.Get("servers", "127.0.0.1:8001,127.0.0.1:8002,127.0.0.1:8003"), ',');
  o.attempt_timeout = std::chrono::milliseconds(flags.Int("attempt-timeout-ms", 1000));
  o.total_timeout = std::chrono::milliseconds(flags.Int("timeout-ms", 10000));
  return o;
}

int Simple(app::Flags& flags, const std::vector<std::string>& args) {
  KvClient client(ClientOptions(flags));
  const std::string& cmd = args[0];
  KvClient::Reply r;
  if (cmd == "put" && args.size() == 3) {
    r = client.Put(args[1], args[2]);
  } else if (cmd == "get" && args.size() == 2) {
    r = client.Get(args[1]);
  } else if (cmd == "del" && args.size() == 2) {
    r = client.Delete(args[1]);
  } else {
    std::fprintf(stderr, "usage: see the top of apps/kvctl.cpp\n");
    return 2;
  }
  if (r.outcome == KvClient::Outcome::kUnknown) {
    std::printf("UNKNOWN (no answer in time; the operation may or may not have happened)\n");
    return 3;
  }
  if (cmd == "get") {
    if (r.ok) std::printf("%s\n", r.value.c_str());
    else std::printf("(not found)\n");
    return r.ok ? 0 : 1;
  }
  std::printf(cmd == "del" ? (r.ok ? "deleted\n" : "(not found)\n") : "ok\n");
  return 0;
}

int Status(app::Flags& flags) {
  KvClientOptions o = ClientOptions(flags);
  KvClient client(o);
  for (size_t i = 0; i < o.servers.size(); ++i) {
    const std::string s = client.Status(i);
    std::printf("%-22s %s\n", o.servers[i].c_str(), s.empty() ? "DOWN (no answer)" : s.c_str());
  }
  return 0;
}

int Bench(app::Flags& flags) {
  const KvClientOptions base = ClientOptions(flags);
  const std::string mode = flags.Get("mode", "closed");
  const int clients = static_cast<int>(flags.Int("clients", 8));
  const auto seconds = std::chrono::seconds(flags.Int("seconds", 10));
  const auto warmup = std::chrono::seconds(flags.Int("warmup", 2));
  const uint64_t keys = static_cast<uint64_t>(flags.Int("keys", 10000));
  const int read_pct = static_cast<int>(flags.Int("read-pct", 90));
  const std::string value(static_cast<size_t>(flags.Int("value-size", 64)), 'v');
  const double rate = flags.Double("rate", 2000);
  if (!flags.Unused().empty()) {
    std::fprintf(stderr, "unknown flag --%s\n", flags.Unused().c_str());
    return 2;
  }
  std::atomic<uint64_t> unknown{0};
  // One KvClient (one connection set, one session) per load-generator thread.
  auto make_op = [&](int id) {
    auto client = std::make_shared<KvClient>(base);
    auto rng = std::make_shared<std::mt19937_64>(static_cast<uint64_t>(id) * 7919 + 1);
    return [client, rng, keys, read_pct, &value, &unknown]() {
      const std::string key = "key:" + std::to_string((*rng)() % keys);
      const bool read = static_cast<int>((*rng)() % 100) < read_pct;
      const KvClient::Reply r = read ? client->Get(key) : client->Put(key, value);
      if (r.outcome == KvClient::Outcome::kUnknown) unknown.fetch_add(1);
    };
  };
  std::printf("# bench mode=%s clients=%d seconds=%lld warmup=%lld keys=%llu read_pct=%d value=%zu servers=%zu\n",
              mode.c_str(), clients, static_cast<long long>(seconds.count()),
              static_cast<long long>(warmup.count()), static_cast<unsigned long long>(keys), read_pct,
              value.size(), base.servers.size());
  if (mode == "closed") {
    ClosedLoopConfig cfg;
    cfg.warmup = warmup;
    cfg.duration = seconds;
    const ClosedLoopResult r = RunClosedLoop(cfg, clients, make_op);
    std::printf("throughput_ops_per_sec=%.0f ops=%llu unknown=%llu\n", r.ops_per_sec,
                static_cast<unsigned long long>(r.measured_ops), static_cast<unsigned long long>(unknown.load()));
    return 0;
  }
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = rate;
  cfg.warmup = warmup;
  cfg.duration = seconds;
  const MultiClientResult r = RunMultiClientOpenLoop(cfg, clients, make_op);
  auto us = [&](double p) { return static_cast<double>(r.latency.ValueAtPercentile(p)) / 1000.0; };
  std::printf("offered_ops_per_sec=%.0f achieved_ops_per_sec=%.0f ops=%llu unknown=%llu\n", rate,
              r.achieved_ops_per_sec, static_cast<unsigned long long>(r.measured_ops),
              static_cast<unsigned long long>(unknown.load()));
  std::printf("latency_us p50=%.1f p90=%.1f p99=%.1f p999=%.1f max=%.1f\n", us(50), us(90), us(99), us(99.9),
              static_cast<double>(r.latency.Max()) / 1000.0);
  return 0;
}

int History(app::Flags& flags) {
  const KvClientOptions base = ClientOptions(flags);
  const std::string out = flags.Get("out");
  const int clients = static_cast<int>(flags.Int("clients", 4));
  const auto seconds = std::chrono::seconds(flags.Int("seconds", 30));
  const uint64_t keys = static_cast<uint64_t>(flags.Int("keys", 5));
  if (out.empty() || !flags.Unused().empty()) {
    std::fprintf(stderr, "history needs --out=FILE (unknown flag: %s)\n", flags.Unused().c_str());
    return 2;
  }
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto end = start + seconds;
  auto now_us = [&] {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  };
  std::mutex mu;
  std::vector<lin::Operation> history;
  std::vector<std::thread> threads;
  for (int c = 0; c < clients; ++c) {
    threads.emplace_back([&, c] {
      KvClient client(base);
      std::mt19937_64 rng(static_cast<uint64_t>(c) + 1);
      std::vector<lin::Operation> mine;
      for (uint64_t i = 0; Clock::now() < end; ++i) {
        lin::Operation op;
        op.client = client.client_id();
        op.key = "h" + std::to_string(rng() % keys);
        const uint64_t r = rng() % 10;
        op.call = now_us();
        KvClient::Reply reply;
        if (r < 4) {
          op.type = lin::OpType::kPut;
          op.value = std::to_string(c) + "-" + std::to_string(i);
          reply = client.Put(op.key, op.value);
        } else if (r < 9) {
          op.type = lin::OpType::kGet;
          reply = client.Get(op.key);
        } else {
          op.type = lin::OpType::kDelete;
          reply = client.Delete(op.key);
        }
        if (reply.outcome == KvClient::Outcome::kOk) {
          op.ret = now_us();
          op.found = reply.ok;
          op.output = reply.value;
        } else {
          op.known = false;
          op.ret = lin::kNoReturn;
        }
        mine.push_back(std::move(op));
      }
      std::lock_guard<std::mutex> lock(mu);
      history.insert(history.end(), mine.begin(), mine.end());
    });
  }
  for (auto& t : threads) t.join();
  std::ofstream file(out);
  file << lin::FormatHistory(history);
  size_t unknown = 0;
  for (const auto& op : history) unknown += op.known ? 0 : 1;
  std::printf("recorded %zu operations (%zu unknown) to %s\n", history.size(), unknown, out.c_str());
  return file.good() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    app::Flags flags(argc, argv);
    const auto& args = flags.positional();
    if (args.empty()) {
      std::fprintf(stderr, "usage: see the top of apps/kvctl.cpp\n");
      return 2;
    }
    if (args[0] == "status") return Status(flags);
    if (args[0] == "bench") return Bench(flags);
    if (args[0] == "history") return History(flags);
    return Simple(flags, args);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "kvctl: %s\n", e.what());
    return 1;
  }
}
