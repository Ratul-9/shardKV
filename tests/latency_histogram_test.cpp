#include "lldstore/latency_histogram.h"

#include <gtest/gtest.h>

namespace lldstore {
namespace {

TEST(LatencyHistogram, EmptyReportsZeros) {
  LatencyHistogram h;
  EXPECT_EQ(h.Count(), 0u);
  EXPECT_EQ(h.ValueAtPercentile(99), 0u);
  EXPECT_EQ(h.Min(), 0u);
  EXPECT_EQ(h.Max(), 0u);
  EXPECT_EQ(h.Mean(), 0.0);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (uint64_t v = 0; v < 128; ++v) h.Record(v);
  EXPECT_EQ(h.Count(), 128u);
  EXPECT_EQ(h.Min(), 0u);
  EXPECT_EQ(h.Max(), 127u);
  EXPECT_EQ(h.ValueAtPercentile(50), 63u);
  EXPECT_EQ(h.ValueAtPercentile(100), 127u);
}

TEST(LatencyHistogram, UniformPercentilesWithinRelativeError) {
  LatencyHistogram h;
  for (uint64_t v = 1; v <= 100000; ++v) h.Record(v);
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const double expected = p / 100.0 * 100000;
    const double got = static_cast<double>(h.ValueAtPercentile(p));
    EXPECT_GE(got, expected) << "p" << p;  // bucket upper bound never under-reports
    EXPECT_LE(got, expected * (1.0 + 1.0 / 64)) << "p" << p;
  }
}

TEST(LatencyHistogram, BucketErrorBoundHoldsAcrossMagnitudes) {
  const uint64_t sentinel = uint64_t{1} << 39;
  for (uint64_t v = 200; v < sentinel; v = v * 3 + 1) {
    LatencyHistogram h;
    h.Record(v);
    h.Record(sentinel);
    const uint64_t got = h.ValueAtPercentile(50);  // falls in v's bucket
    EXPECT_GE(got, v);
    EXPECT_LE(static_cast<double>(got - v) / static_cast<double>(v), 1.0 / 64) << "v=" << v;
  }
}

TEST(LatencyHistogram, ValuesAboveMaxAreClampedNotDropped) {
  LatencyHistogram h;
  h.Record(UINT64_MAX);
  EXPECT_EQ(h.Count(), 1u);
  EXPECT_EQ(h.Max(), LatencyHistogram::kMaxValue);
}

TEST(LatencyHistogram, MergeCombinesCountsAndExtremes) {
  LatencyHistogram a, b;
  for (uint64_t v = 1; v <= 50; ++v) a.Record(v);
  for (uint64_t v = 51; v <= 100; ++v) b.Record(v);
  a.Merge(b);
  EXPECT_EQ(a.Count(), 100u);
  EXPECT_EQ(a.Min(), 1u);
  EXPECT_EQ(a.Max(), 100u);
  EXPECT_EQ(a.ValueAtPercentile(50), 50u);
  EXPECT_DOUBLE_EQ(a.Mean(), 50.5);
}

}  // namespace
}  // namespace lldstore
