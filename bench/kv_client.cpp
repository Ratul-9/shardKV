// gRPC load generator for the key-value server.
//
// Usage: kv_client [--target=127.0.0.1:50051] [--clients=N] [--mode=open|closed]
//                  [--rate=OPS_PER_SEC_TOTAL] [--seconds=S] [--warmup=S] [--keys=K] [--read-pct=P]
//   --mode=closed  every client issues one request after another: MAXIMUM THROUGHPUT, no latency
//   --mode=open    each client schedules requests at rate/clients and measures from the intended
//                  send time, so latency percentiles are not distorted by coordinated omission
//
// Each client is a thread with its OWN channel (its own TCP connection) and a synchronous stub, so
// "N clients" means N connections with at most one request outstanding each. Failed RPCs are
// counted and reported: a run with errors is not a valid measurement.
#include <benchmark/benchmark.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "bench_util.h"
#include "kv.grpc.pb.h"
#include "lldstore/multi_client.h"

namespace {

using namespace lldstore;
using namespace lldstore::bench;

struct Options {
  std::string target = "127.0.0.1:50051";
  int clients = 4;
  std::string mode = "open";
  double rate = 20'000;
  int seconds = 5;
  int warmup = 1;
  uint64_t keys = 1'000'000;
  int read_pct = 90;
};

std::atomic<uint64_t> g_errors{0};

bool ParseFlag(const char* arg, const char* name, std::string* value) {
  const std::string prefix = std::string("--") + name + "=";
  if (std::strncmp(arg, prefix.c_str(), prefix.size()) != 0) return false;
  *value = arg + prefix.size();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    if (ParseFlag(argv[i], "target", &v)) o.target = v;
    else if (ParseFlag(argv[i], "clients", &v)) o.clients = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "mode", &v)) o.mode = v;
    else if (ParseFlag(argv[i], "rate", &v)) o.rate = std::atof(v.c_str());
    else if (ParseFlag(argv[i], "seconds", &v)) o.seconds = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "warmup", &v)) o.warmup = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "keys", &v)) o.keys = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "read-pct", &v)) o.read_pct = std::atoi(v.c_str());
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (o.clients < 1 || o.keys < 1 || (o.mode != "open" && o.mode != "closed")) {
    std::fprintf(stderr, "invalid clients, keys or mode\n");
    return 2;
  }

  // Runs on each client's own thread, before the start barrier: connecting is not measured.
  auto make_op = [&](int id) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);  // force a separate connection per client
    auto channel =
        grpc::CreateCustomChannel(o.target, grpc::InsecureChannelCredentials(), args);
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
      std::fprintf(stderr, "client %d could not connect to %s\n", id, o.target.c_str());
      std::exit(1);
    }
    std::shared_ptr<lldstore::v1::KV::Stub> stub = lldstore::v1::KV::NewStub(channel);
    return [stub, channel, &o, rng = XorShift64(42 + static_cast<uint64_t>(id)),
            get_req = lldstore::v1::GetRequest(), get_resp = lldstore::v1::GetResponse(),
            put_req = lldstore::v1::PutRequest(),
            put_resp = lldstore::v1::PutResponse()]() mutable {
      char buf[kKeySize];
      const uint64_t x = rng.Next();
      const auto key = FormatKey((x >> 8) % o.keys, buf);
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      grpc::Status status;
      if (x % 100 < static_cast<uint64_t>(o.read_pct)) {
        get_req.set_key(key.data(), key.size());
        status = stub->Get(&context, get_req, &get_resp);
        benchmark::DoNotOptimize(get_resp);
      } else {
        put_req.set_key(key.data(), key.size());
        put_req.set_value(Value().data(), Value().size());
        status = stub->Put(&context, put_req, &put_resp);
      }
      if (!status.ok()) g_errors.fetch_add(1, std::memory_order_relaxed);
    };
  };

  std::printf("target=%s clients=%d mode=%s keys=%llu read_pct=%d\n", o.target.c_str(), o.clients,
              o.mode.c_str(), static_cast<unsigned long long>(o.keys), o.read_pct);

  if (o.mode == "closed") {
    ClosedLoopConfig cfg;
    cfg.warmup = std::chrono::seconds(o.warmup);
    cfg.duration = std::chrono::seconds(o.seconds);
    const auto r = RunClosedLoop(cfg, o.clients, make_op);
    std::printf("throughput=%.0f ops/s (measured_ops=%llu)\n", r.ops_per_sec,
                static_cast<unsigned long long>(r.measured_ops));
  } else {
    OpenLoopConfig cfg;
    cfg.target_ops_per_sec = o.rate;
    cfg.warmup = std::chrono::seconds(o.warmup);
    cfg.duration = std::chrono::seconds(o.seconds);
    const auto r = RunMultiClientOpenLoop(cfg, o.clients, make_op);
    std::printf("offered=%.0f achieved=%.0f ops/s measured_ops=%llu\n", o.rate,
                r.achieved_ops_per_sec, static_cast<unsigned long long>(r.measured_ops));
    std::printf("latency ns: min=%llu p50=%llu p90=%llu p99=%llu p999=%llu max=%llu mean=%.0f\n",
                static_cast<unsigned long long>(r.latency.Min()),
                static_cast<unsigned long long>(r.latency.ValueAtPercentile(50)),
                static_cast<unsigned long long>(r.latency.ValueAtPercentile(90)),
                static_cast<unsigned long long>(r.latency.ValueAtPercentile(99)),
                static_cast<unsigned long long>(r.latency.ValueAtPercentile(99.9)),
                static_cast<unsigned long long>(r.latency.Max()), r.latency.Mean());
  }
  std::printf("rpc_errors=%llu%s\n", static_cast<unsigned long long>(g_errors.load()),
              g_errors.load() == 0 ? "" : "  (INVALID RUN)");
  return g_errors.load() == 0 ? 0 : 3;
}
