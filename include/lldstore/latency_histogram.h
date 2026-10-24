#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lldstore {

// Fixed-memory latency histogram with log-linear buckets (the idea behind HdrHistogram).
//
// Values are non-negative integers (we use nanoseconds). Values below 128 get one bucket
// each. Above that, each power-of-two range [2^k, 2^(k+1)) is split into 64 equal
// sub-buckets, so the relative error of any reported value is at most 1/64 (~1.6%).
// Recording is O(1) with no allocation, so it is cheap enough to call on a hot path.
class LatencyHistogram {
 public:
  static constexpr int kSubBucketBits = 7;
  static constexpr uint64_t kSubBucketCount = uint64_t{1} << kSubBucketBits;  // 128
  static constexpr uint64_t kHalfCount = kSubBucketCount / 2;                 // 64
  static constexpr uint64_t kMaxValue = (uint64_t{1} << 40) - 1;              // ~18 minutes in ns

  LatencyHistogram() : counts_(IndexOf(kMaxValue) + 1, 0) {}

  // Values above kMaxValue are clamped, never dropped.
  void Record(uint64_t value) {
    value = std::min(value, kMaxValue);
    ++counts_[IndexOf(value)];
    ++total_;
    sum_ += static_cast<double>(value);
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
  }

  // Smallest bucket upper bound such that at least p% of samples are <= it.
  // Never reports more than the largest value actually recorded.
  uint64_t ValueAtPercentile(double p) const {
    if (total_ == 0) return 0;
    p = std::clamp(p, 0.0, 100.0);
    const auto target = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_))));
    uint64_t seen = 0;
    for (size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen >= target) return std::min(HighestEquivalentValue(i), max_);
    }
    return max_;
  }

  void Merge(const LatencyHistogram& other) {
    for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }

  uint64_t Count() const { return total_; }
  uint64_t Min() const { return total_ == 0 ? 0 : min_; }
  uint64_t Max() const { return max_; }
  double Mean() const { return total_ == 0 ? 0.0 : sum_ / static_cast<double>(total_); }

 private:
  // Bucket index for a value. For v < 128 it is v itself. Otherwise we find the top bit,
  // keep its 7 leading bits (64..127) and use how far we shifted to pick the power-of-two range.
  static constexpr size_t IndexOf(uint64_t v) {
    if (v < kSubBucketCount) return static_cast<size_t>(v);
    const int msb = std::bit_width(v) - 1;
    const int shift = msb - (kSubBucketBits - 1);
    return static_cast<size_t>(kSubBucketCount + static_cast<uint64_t>(shift - 1) * kHalfCount +
                               ((v >> shift) - kHalfCount));
  }

  // Largest value that maps to bucket `index` (inverse of IndexOf).
  static constexpr uint64_t HighestEquivalentValue(size_t index) {
    if (index < kSubBucketCount) return index;
    const uint64_t j = index - kSubBucketCount;
    const uint64_t shift = j / kHalfCount + 1;
    const uint64_t sub = j % kHalfCount + kHalfCount;
    return ((sub + 1) << shift) - 1;
  }

  std::vector<uint64_t> counts_;
  uint64_t total_ = 0;
  double sum_ = 0.0;
  uint64_t min_ = UINT64_MAX;
  uint64_t max_ = 0;
};

}  // namespace lldstore
