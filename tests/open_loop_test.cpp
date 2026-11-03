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

// Regression test for a harness bug: sleep_for can overshoot by a fraction of the requested time
// (about 25-30% on macOS), so sleeping until a short margin before a LONG interval's deadline made
// every operation start late, and that lateness was charged as latency. At 20 ms intervals an
// idle no-op loop must still start operations close to their intended time. The overshoot was
// systematic (every operation ~25% late), so the median is asserted: unlike p99 it is not
// sensitive to a single scheduler hiccup on a loaded machine.
TEST(OpenLoop, LongIntervalsDoNotAddSleepOvershootToLatency) {
  OpenLoopConfig cfg;
  cfg.target_ops_per_sec = 50;  // one operation every 20 ms
  cfg.duration = 1s;
  const auto r = RunOpenLoop(cfg, [] {});
  EXPECT_EQ(r.measured_ops, 50u);
  EXPECT_LT(r.latency.ValueAtPercentile(50), 2'000'000u);  // < 2 ms (the old code gave ~5 ms)
}

}  // namespace
}  // namespace lldstore
