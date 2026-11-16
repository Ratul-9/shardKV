#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "lldstore/latency_histogram.h"

namespace lldstore {

// Phase 11: process metrics in the Prometheus text format. Written by Claude at Ratul's request.
// Design: docs/DESIGN.md D-060.
//
// Counters and gauges are single atomics (cheap enough for the request path). A histogram is the
// Phase 0 log-linear LatencyHistogram behind a mutex, exported as a Prometheus SUMMARY (p50, p90,
// p99, p99.9, sum, count) computed in this process.
//
// A percentile can NOT be averaged across nodes or time windows: the cluster p99 is not the mean
// of the nodes' p99s. Each node's summary is its own; cluster-wide percentiles need the raw
// distributions (merge histograms, as the load generator does), never averaged quantiles.
class Counter {
 public:
  void Inc(uint64_t n = 1) noexcept { v_.fetch_add(n, std::memory_order_relaxed); }
  uint64_t value() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> v_{0};
};

class Gauge {
 public:
  void Set(int64_t v) noexcept { v_.store(v, std::memory_order_relaxed); }
  int64_t value() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> v_{0};
};

class Histogram {
 public:
  void Record(uint64_t v) {
    std::lock_guard<std::mutex> lock(mu_);
    h_.Record(v);
  }
  LatencyHistogram Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return h_;
  }

 private:
  mutable std::mutex mu_;
  LatencyHistogram h_;
};

class MetricsRegistry {
 public:
  // Returned references stay valid for the registry's lifetime. Calling twice with the same name
  // returns the same metric.
  Counter& counter(const std::string& name, const std::string& help) { return Get(counters_, name, help); }
  Gauge& gauge(const std::string& name, const std::string& help) { return Get(gauges_, name, help); }
  Histogram& histogram(const std::string& name, const std::string& help) {
    return Get(histograms_, name, help);
  }

  std::string RenderPrometheus() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out;
    char line[256];
    for (const auto& [name, m] : counters_) {
      out += "# HELP " + name + " " + m.help + "\n# TYPE " + name + " counter\n";
      std::snprintf(line, sizeof(line), "%s %llu\n", name.c_str(),
                    static_cast<unsigned long long>(m.metric->value()));
      out += line;
    }
    for (const auto& [name, m] : gauges_) {
      out += "# HELP " + name + " " + m.help + "\n# TYPE " + name + " gauge\n";
      std::snprintf(line, sizeof(line), "%s %lld\n", name.c_str(), static_cast<long long>(m.metric->value()));
      out += line;
    }
    for (const auto& [name, m] : histograms_) {
      const LatencyHistogram h = m.metric->Snapshot();
      out += "# HELP " + name + " " + m.help + "\n# TYPE " + name + " summary\n";
      for (double q : {50.0, 90.0, 99.0, 99.9}) {
        std::snprintf(line, sizeof(line), "%s{quantile=\"%g\"} %llu\n", name.c_str(), q / 100.0,
                      static_cast<unsigned long long>(h.ValueAtPercentile(q)));
        out += line;
      }
      std::snprintf(line, sizeof(line), "%s_sum %.0f\n%s_count %llu\n", name.c_str(),
                    h.Mean() * static_cast<double>(h.Count()), name.c_str(),
                    static_cast<unsigned long long>(h.Count()));
      out += line;
    }
    return out;
  }

 private:
  template <typename M>
  struct Entry {
    std::string help;
    std::unique_ptr<M> metric;
  };

  template <typename M>
  M& Get(std::map<std::string, Entry<M>>& map, const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& e = map[name];
    if (!e.metric) {
      e.metric = std::make_unique<M>();
      e.help = help;
    }
    return *e.metric;
  }

  mutable std::mutex mu_;
  std::map<std::string, Entry<Counter>> counters_;
  std::map<std::string, Entry<Gauge>> gauges_;
  std::map<std::string, Entry<Histogram>> histograms_;
};

}  // namespace lldstore
