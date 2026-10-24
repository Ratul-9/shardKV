#include "lldstore/open_loop.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace lldstore {
namespace {

using namespace std::chrono_literals;

TEST(OpenLoop, SchedulesExactlyRateTimesDuration) {
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = 2000;
  cfg.duration = 200ms;
  const auto r = RunOpenLoop(cfg, [] {});
  EXPECT_EQ(r.measured_ops, 400u);
  EXPECT_EQ(r.warmup_ops, 0u);
}

TEST(OpenLoop, WarmupOperationsAreNotRecorded) {
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = 1000;
  cfg.warmup = 100ms;
  cfg.duration = 200ms;
  const auto r = RunOpenLoop(cfg, [] {});
  EXPECT_EQ(r.warmup_ops, 100u);
  EXPECT_EQ(r.measured_ops, 200u);
  EXPECT_EQ(r.latency.Count(), 200u);
}

// The coordinated-omission demonstration. One operation stalls for 200 ms. A closed loop would
// record a single 200 ms sample out of ~1000 and report a p99 near zero. The open loop charges
// every slot that was due during the stall, so roughly 20% of samples are late and p99 is large.
TEST(OpenLoop, StallIsChargedToEveryMissedSlot) {
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = 1000;
  cfg.duration = 1s;
  int calls = 0;
  const auto r = RunOpenLoop(cfg, [&] {
    if (++calls == 100) std::this_thread::sleep_for(200ms);
  });
  EXPECT_EQ(r.measured_ops, 1000u);
  EXPECT_GT(r.latency.ValueAtPercentile(99), 100'000'000u);  // > 100 ms
  EXPECT_LT(r.latency.ValueAtPercentile(50), 20'000'000u);   // < 20 ms: unaffected ops stay fast
}

}  // namespace
}  // namespace lldstore
