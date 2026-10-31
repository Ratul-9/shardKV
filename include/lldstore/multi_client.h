#pragma once

#include <chrono>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

#include "lldstore/latency_histogram.h"
#include "lldstore/open_loop.h"

namespace lldstore {

// Multi-client load generation. `make_op(client_id)` is called on each client's own thread and
// returns the callable executed once per operation.

struct MultiClientResult {
  LatencyHistogram latency;  // nanoseconds, from each operation's INTENDED start time
  uint64_t measured_ops = 0;
  double achieved_ops_per_sec = 0;
};

// Open loop: each of `clients` threads schedules operations at total_rate / clients, independent
// of whether earlier operations finished, and measures from the scheduled time. This is the one to
// use for latency: it does not suffer coordinated omission. If the system cannot keep up,
// achieved_ops_per_sec falls below the offered rate and the latencies grow.
template <typename MakeOp>
MultiClientResult RunMultiClientOpenLoop(const OpenLoopConfig& total, int clients,
                                         MakeOp&& make_op) {
  OpenLoopConfig per_client = total;
  per_client.target_ops_per_sec = total.target_ops_per_sec / clients;

  std::vector<OpenLoopResult> results(static_cast<size_t>(clients));
  std::latch ready(clients);
  std::vector<std::thread> threads;
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&, i] {
      auto op = make_op(i);
      ready.arrive_and_wait();  // all clients start together
      results[static_cast<size_t>(i)] = RunOpenLoop(per_client, op);
    });
  }
  for (auto& t : threads) t.join();

  MultiClientResult merged;
  for (const auto& r : results) {
    merged.latency.Merge(r.latency);
    merged.measured_ops += r.measured_ops;
    merged.achieved_ops_per_sec += r.achieved_ops_per_sec;
  }
  return merged;
}

struct ClosedLoopConfig {
  std::chrono::nanoseconds warmup{0};
  std::chrono::nanoseconds duration{std::chrono::seconds(1)};
};

struct ClosedLoopResult {
  uint64_t measured_ops = 0;
  double ops_per_sec = 0;
};

// Closed loop: every client issues operations back to back as fast as it can. This measures
// MAXIMUM THROUGHPUT (the scaling curve). It deliberately records no latency: a closed loop waits
// for each operation, so it suffers coordinated omission and its latencies would flatter the system.
template <typename MakeOp>
ClosedLoopResult RunClosedLoop(const ClosedLoopConfig& cfg, int clients, MakeOp&& make_op) {
  using Clock = std::chrono::steady_clock;
  constexpr int kBatch = 64;  // check the clock once per batch, not per operation

  std::vector<uint64_t> counts(static_cast<size_t>(clients), 0);
  std::latch ready(clients);
  std::vector<std::thread> threads;
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&, i] {
      auto op = make_op(i);
      ready.arrive_and_wait();
      const auto start = Clock::now();
      const auto measure_from = start + cfg.warmup;
      const auto end = measure_from + cfg.duration;
      uint64_t ops = 0;
      for (;;) {
        const auto batch_start = Clock::now();
        if (batch_start >= end) break;
        for (int k = 0; k < kBatch; ++k) op();
        if (batch_start >= measure_from) ops += kBatch;
      }
      counts[static_cast<size_t>(i)] = ops;
    });
  }
  for (auto& t : threads) t.join();

  ClosedLoopResult r;
  for (uint64_t c : counts) r.measured_ops += c;
  r.ops_per_sec = static_cast<double>(r.measured_ops) /
                  std::chrono::duration<double>(cfg.duration).count();
  return r;
}

}  // namespace lldstore
