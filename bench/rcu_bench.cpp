// Phase 9 benchmark: read throughput while ONE writer keeps updating, for
//   rcu     RcuMap (lock-free readers, epoch reclamation)
//   shared  std::unordered_map behind std::shared_mutex (readers share, writer excludes)
//   mutex   std::unordered_map behind std::mutex
// Usage: rcu_bench [--map=rcu|shared|mutex] [--readers=4] [--seconds=3] [--keys=100000]
//                  [--writer=1]   (0 = no writer, to see the pure read cost)
// Prints reads/s summed over readers and writes/s. Closed loop: throughput only, no latency.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bench_util.h"
#include "lldstore/rcu_map.h"

namespace {

using namespace lldstore;

struct Options {
  std::string map = "rcu";
  int readers = 4;
  int seconds = 3;
  uint64_t keys = 100000;
  bool writer = true;
};

template <typename Lock>
struct LockedMap {
  mutable Lock lock;
  std::unordered_map<std::string, std::string> map;
};

int Run(const Options& o) {
  RcuMap rcu;
  LockedMap<std::shared_mutex> shared;
  LockedMap<std::mutex> plain;
  const std::string value(64, 'v');
  char buf[bench::kKeySize];
  for (uint64_t i = 0; i < o.keys; ++i) {
    const std::string k(bench::FormatKey(i, buf));
    rcu.Put(k, value);
    shared.map[k] = value;
    plain.map[k] = value;
  }
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0}, writes{0};
  std::vector<std::thread> threads;
  for (int r = 0; r < o.readers; ++r) {
    threads.emplace_back([&, r] {
      bench::XorShift64 rng(static_cast<uint64_t>(r) + 1);
      char kb[bench::kKeySize];
      std::string out;
      uint64_t n = 0;
      const size_t slot = o.map == "rcu" ? rcu.RegisterReader() : 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string_view k = bench::FormatKey(rng.Next() % o.keys, kb);
        if (o.map == "rcu") {
          auto guard = rcu.Pin(slot);
          if (auto v = rcu.Get(guard, k)) out.assign(v->data(), v->size());
        } else if (o.map == "shared") {
          std::shared_lock<std::shared_mutex> lock(shared.lock);
          auto it = shared.map.find(std::string(k));
          if (it != shared.map.end()) out = it->second;
        } else {
          std::lock_guard<std::mutex> lock(plain.lock);
          auto it = plain.map.find(std::string(k));
          if (it != plain.map.end()) out = it->second;
        }
        ++n;
      }
      reads.fetch_add(n);
    });
  }
  if (o.writer) {
    threads.emplace_back([&] {
      bench::XorShift64 rng(777);
      char kb[bench::kKeySize];
      uint64_t n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string_view k = bench::FormatKey(rng.Next() % o.keys, kb);
        if (o.map == "rcu") {
          rcu.Put(k, value);
        } else if (o.map == "shared") {
          std::unique_lock<std::shared_mutex> lock(shared.lock);
          shared.map[std::string(k)] = value;
        } else {
          std::lock_guard<std::mutex> lock(plain.lock);
          plain.map[std::string(k)] = value;
        }
        ++n;
      }
      writes.fetch_add(n);
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(o.seconds));
  stop = true;
  for (auto& t : threads) t.join();
  std::printf("map=%s readers=%d writer=%d keys=%llu seconds=%d reads_per_sec=%.0f writes_per_sec=%.0f\n",
              o.map.c_str(), o.readers, o.writer ? 1 : 0, static_cast<unsigned long long>(o.keys), o.seconds,
              static_cast<double>(reads.load()) / o.seconds, static_cast<double>(writes.load()) / o.seconds);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--map=", 6) == 0) o.map = a + 6;
    else if (std::strncmp(a, "--readers=", 10) == 0) o.readers = std::atoi(a + 10);
    else if (std::strncmp(a, "--seconds=", 10) == 0) o.seconds = std::atoi(a + 10);
    else if (std::strncmp(a, "--keys=", 7) == 0) o.keys = std::strtoull(a + 7, nullptr, 10);
    else if (std::strncmp(a, "--writer=", 9) == 0) o.writer = std::atoi(a + 9) != 0;
    else {
      std::fprintf(stderr, "unknown argument %s\n", a);
      return 2;
    }
  }
  if (o.map != "rcu" && o.map != "shared" && o.map != "mutex") {
    std::fprintf(stderr, "--map must be rcu, shared or mutex\n");
    return 2;
  }
  return Run(o);
}
