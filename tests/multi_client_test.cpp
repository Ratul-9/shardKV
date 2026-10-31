#include "lldstore/multi_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace lldstore {
namespace {

using namespace std::chrono_literals;

TEST(MultiClientOpenLoop, SplitsRateAcrossClientsAndRunsEachOnItsOwnThread) {
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = 4000;  // 1000 per client
  cfg.duration = 200ms;
  std::atomic<int> made{0};
  const auto r = RunMultiClientOpenLoop(cfg, 4, [&](int) {
    ++made;
    return [] {};
  });
  EXPECT_EQ(made.load(), 4);
  EXPECT_EQ(r.measured_ops, 800u);  // 4 clients x 200 ops
  EXPECT_EQ(r.latency.Count(), 800u);
}

TEST(MultiClientClosedLoop, CountsOperationsFromAllClients) {
  ClosedLoopConfig cfg;
  cfg.duration = 200ms;
  std::atomic<uint64_t> executed{0};
  const auto r = RunClosedLoop(cfg, 4, [&](int) {
    return [&] { executed.fetch_add(1, std::memory_order_relaxed); };
  });
  EXPECT_GT(r.measured_ops, 0u);
  EXPECT_EQ(r.measured_ops % 64, 0u);        // counted in batches of 64
  EXPECT_LE(r.measured_ops, executed.load());  // warm-up-free here, but never over-counts
  EXPECT_GT(r.ops_per_sec, 0.0);
}

}  // namespace
}  // namespace lldstore
