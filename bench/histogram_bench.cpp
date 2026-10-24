#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>

#include "lldstore/latency_histogram.h"

namespace {

// Cost of recording one sample. Every later benchmark pays this per operation, so we measure it.
void BM_HistogramRecord(benchmark::State& state) {
  lldstore::LatencyHistogram h;
  uint64_t v = 1;
  for (auto _ : state) {
    v = (v * 1103515245 + 12345) & 0x3FFFFFFF;  // cheap LCG, values up to ~1 s in ns
    h.Record(v);
    benchmark::ClobberMemory();  // force the store to the histogram to be materialised
  }
  benchmark::DoNotOptimize(h.Count());
}
BENCHMARK(BM_HistogramRecord);

void BM_HistogramPercentile(benchmark::State& state) {
  lldstore::LatencyHistogram h;
  for (uint64_t v = 1; v <= 1'000'000; ++v) h.Record(v * 37);
  for (auto _ : state) benchmark::DoNotOptimize(h.ValueAtPercentile(99.9));
}
BENCHMARK(BM_HistogramPercentile);

// Timer resolution/overhead of the clock the open-loop harness uses.
void BM_SteadyClockNow(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(std::chrono::steady_clock::now());
}
BENCHMARK(BM_SteadyClockNow);

}  // namespace
