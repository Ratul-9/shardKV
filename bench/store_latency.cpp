// Open-loop latency measurement for the in-process store: 90% Get / 10% Put at a fixed offered rate.
// Usage: store_latency [num_keys=1000000] [ops_per_sec=200000] [seconds=5] [warmup_seconds=1]
//
// Reported latency includes key formatting, the store call, and one steady_clock read (~13 ns),
// measured from the intended start time, so it is safe against coordinated omission.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "bench_util.h"
#include "lldstore/open_loop.h"
#include "lldstore/store.h"

int main(int argc, char** argv) {
  using namespace lldstore;
  using namespace lldstore::bench;
  const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const double rate = argc > 2 ? std::strtod(argv[2], nullptr) : 200'000;
  const long seconds = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 5;
  const long warmup = argc > 4 ? std::strtol(argv[4], nullptr, 10) : 1;

  Store store;
  Prefill(store, n);

  XorShift64 rng(42);
  char buf[kKeySize];
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = rate;
  cfg.warmup = std::chrono::seconds(warmup);
  cfg.duration = std::chrono::seconds(seconds);

  const auto r = RunOpenLoop(cfg, [&] {
    const uint64_t x = rng.Next();
    const auto key = FormatKey((x >> 8) % n, buf);
    if (x % 10 == 0) {
      store.Put(key, Value());
      benchmark::ClobberMemory();
    } else {
      auto v = store.Get(key);
      benchmark::DoNotOptimize(v);
    }
  });

  std::printf("keys=%llu offered=%.0f ops/s achieved=%.0f ops/s measured_ops=%llu\n",
              static_cast<unsigned long long>(n), rate, r.achieved_ops_per_sec,
              static_cast<unsigned long long>(r.measured_ops));
  std::printf("latency ns: min=%llu p50=%llu p90=%llu p99=%llu p999=%llu max=%llu mean=%.0f\n",
              static_cast<unsigned long long>(r.latency.Min()),
              static_cast<unsigned long long>(r.latency.ValueAtPercentile(50)),
              static_cast<unsigned long long>(r.latency.ValueAtPercentile(90)),
              static_cast<unsigned long long>(r.latency.ValueAtPercentile(99)),
              static_cast<unsigned long long>(r.latency.ValueAtPercentile(99.9)),
              static_cast<unsigned long long>(r.latency.Max()), r.latency.Mean());
  return 0;
}
