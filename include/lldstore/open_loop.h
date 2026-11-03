#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include "lldstore/latency_histogram.h"

namespace lldstore {

struct OpenLoopConfig {
  double target_ops_per_sec = 10000;
  std::chrono::nanoseconds warmup{0};  // operations in this window run but are not recorded
  std::chrono::nanoseconds duration{std::chrono::seconds(1)};
};

struct OpenLoopResult {
  LatencyHistogram latency;  // nanoseconds, measured from the INTENDED send time
  uint64_t measured_ops = 0;
  uint64_t warmup_ops = 0;
  double achieved_ops_per_sec = 0;
};

namespace detail {
using Clock = std::chrono::steady_clock;

// Waits until `t`. sleep_for can overshoot by a FRACTION of the requested time (measured on macOS:
// about 25-30%, so a 12.7 ms sleep returns ~3 ms late), not by a fixed amount. A single sleep up
// to a fixed margin before the deadline therefore overshoots badly on long waits. Instead, sleep
// for only HALF of the remaining time and re-check: the overshoot (< half of what we asked for)
// can never carry us past the deadline, and the remaining time shrinks geometrically. Spin only
// for the final stretch, where sleeping is too coarse.
inline void WaitUntil(Clock::time_point t) {
  using namespace std::chrono_literals;
  constexpr auto kSpinBelow = 50us;
  for (;;) {
    const auto now = Clock::now();
    if (now >= t) return;
    const auto remaining = t - now;
    if (remaining > kSpinBelow) std::this_thread::sleep_for(remaining / 2);
  }
}
}  // namespace detail

// Single-threaded open-loop runner. Operation i is *scheduled* at start + i / rate whether or
// not the previous one finished. Latency is measured from the scheduled time, so if the system
// stalls, every operation that should have started during the stall is charged its queueing
// delay. This is what avoids coordinated omission: a closed loop (send, wait, send) would record
// one slow sample for the stall; this records one per missed slot.
template <typename Op>
OpenLoopResult RunOpenLoop(const OpenLoopConfig& cfg, Op&& op) {
  using detail::Clock;
  const auto interval = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / cfg.target_ops_per_sec));
  const auto total = cfg.warmup + cfg.duration;
  const auto start = Clock::now();
  const auto measure_from = start + cfg.warmup;

  OpenLoopResult result;
  for (int64_t i = 0;; ++i) {
    const auto intended = start + interval * i;
    if (intended - start >= total) break;
    detail::WaitUntil(intended);
    op();
    const auto done = Clock::now();
    if (intended < measure_from) {
      ++result.warmup_ops;
    } else {
      result.latency.Record(
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(done - intended).count()));
      ++result.measured_ops;
    }
  }
  const std::chrono::duration<double> elapsed = Clock::now() - measure_from;
  result.achieved_ops_per_sec = static_cast<double>(result.measured_ops) / elapsed.count();
  return result;
}

}  // namespace lldstore
