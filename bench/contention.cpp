// Lock-contention measurement for the Phase 2 mutex baseline, in process (no network).
//
// Isolates the cost of the lock from gRPC overhead: N client threads call the store directly.
//
// Usage: contention [--store=none|mutex|shared] [--clients=N] [--mode=open|closed]
//                   [--rate=OPS_PER_SEC_TOTAL] [--seconds=S] [--warmup=S] [--keys=K] [--read-pct=P]
//   --mode=closed  maximum throughput (ops/s); no latency is reported
//   --mode=open    latency percentiles at a fixed offered rate (coordinated-omission safe)
//   --store=none   the bare single-threaded Store, valid only with --clients=1 (the reference)
//
// Every Get copies the value into a reused buffer, so all three store kinds do the same work.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "bench_util.h"
#include "lldstore/locked_store.h"
#include "lldstore/multi_client.h"
#include "lldstore/store.h"

namespace {

using namespace lldstore;
using namespace lldstore::bench;

struct Options {
  std::string store = "mutex";
  int clients = 4;
  std::string mode = "open";
  double rate = 200'000;
  int seconds = 5;
  int warmup = 1;
  uint64_t keys = 1'000'000;
  int read_pct = 90;
};

// The unlocked Store with the same Get(key, string*) interface as the locked wrappers.
class UnlockedStore {
 public:
  void Put(std::string_view k, std::string_view v) { store_.Put(k, v); }
  bool Get(std::string_view k, std::string* out) const {
    const auto v = store_.Get(k);
    if (!v) return false;
    out->assign(v->data(), v->size());
    return true;
  }

 private:
  Store store_;
};

template <typename S>
int Run(S& store, const Options& o) {
  for (uint64_t i = 0; i < o.keys; ++i) {
    char buf[kKeySize];
    store.Put(FormatKey(i, buf), Value());
  }

  // Each client gets its own RNG and its own output buffer; the store is the only shared state.
  auto make_op = [&](int id) {
    return [&store, &o, rng = XorShift64(42 + static_cast<uint64_t>(id)),
            out = std::string()]() mutable {
      char buf[kKeySize];
      const uint64_t x = rng.Next();
      const auto key = FormatKey((x >> 8) % o.keys, buf);
      if (x % 100 < static_cast<uint64_t>(o.read_pct)) {
        benchmark::DoNotOptimize(store.Get(key, &out));
      } else {
        store.Put(key, Value());
        benchmark::ClobberMemory();
      }
    };
  };

  std::printf("store=%s clients=%d mode=%s keys=%llu read_pct=%d\n", o.store.c_str(), o.clients,
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
  return 0;
}

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
    if (ParseFlag(argv[i], "store", &v)) o.store = v;
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

  if (o.store == "none") {
    if (o.clients != 1) {
      std::fprintf(stderr, "--store=none is not thread-safe; use --clients=1\n");
      return 2;
    }
    UnlockedStore s;
    return Run(s, o);
  }
  if (o.store == "mutex") {
    MutexStore s;
    return Run(s, o);
  }
  if (o.store == "shared") {
    SharedMutexStore s;
    return Run(s, o);
  }
  std::fprintf(stderr, "unknown --store=%s\n", o.store.c_str());
  return 2;
}
