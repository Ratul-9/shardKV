// Phase 7 benchmark: what group commit buys, on THIS disk.
// Usage: wal_bench [--dir=/tmp/lldstore-walbench] [--writers=1,2,4,8,16,32] [--seconds=3]
//                  [--sync=data|none] [--delay-us=0] [--value-size=64]
// For each writer count: N threads each loop { Append; WaitDurable } (an acknowledged write each),
// reporting acknowledged writes/s, syncs/s, records per sync, and the mean sync time.
// sync=none shows the cost without fsync (page cache only) for comparison.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/log_file.h"
#include "lldstore/wal.h"

int main(int argc, char** argv) {
  using namespace lldstore;
  std::string dir = "/tmp/lldstore-walbench";
  std::string writers_list = "1,2,4,8,16,32";
  int seconds = 3;
  SyncMode sync = SyncMode::kData;
  long delay_us = 0;
  size_t value_size = 64;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--dir=", 6) == 0) dir = a + 6;
    else if (std::strncmp(a, "--writers=", 10) == 0) writers_list = a + 10;
    else if (std::strncmp(a, "--seconds=", 10) == 0) seconds = std::atoi(a + 10);
    else if (std::strcmp(a, "--sync=none") == 0) sync = SyncMode::kNone;
    else if (std::strcmp(a, "--sync=data") == 0) sync = SyncMode::kData;
    else if (std::strncmp(a, "--delay-us=", 11) == 0) delay_us = std::atol(a + 11);
    else if (std::strncmp(a, "--value-size=", 13) == 0) value_size = std::strtoull(a + 13, nullptr, 10);
    else {
      std::fprintf(stderr, "unknown argument %s\n", a);
      return 2;
    }
  }
  std::filesystem::create_directories(dir);
  const std::string payload(value_size, 'x');
  std::printf("# wal_bench dir=%s sync=%s delay_us=%ld record=%zu B\n", dir.c_str(),
              sync == SyncMode::kNone ? "none" : "data", delay_us, value_size);
  size_t start = 0;
  while (start <= writers_list.size()) {
    const size_t comma = writers_list.find(',', start);
    const int writers = std::atoi(writers_list.substr(start, comma - start).c_str());
    start = comma == std::string::npos ? writers_list.size() + 1 : comma + 1;
    const std::string path = dir + "/bench.log";
    std::filesystem::remove(path);
    WalOptions options;
    options.sync = sync;
    options.max_batch_delay = std::chrono::microseconds(delay_us);
    GroupCommitLog log(LogFile::Open(path, 0), options);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> acked{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w) {
      threads.emplace_back([&] {
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          log.WaitDurable(log.Append(payload));
          ++n;
        }
        acked.fetch_add(n);
      });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto& t : threads) t.join();
    const GroupCommitLog::Stats s = log.stats();
    const double batches = static_cast<double>(s.batches == 0 ? 1 : s.batches);
    std::printf("writers=%d acked_writes_per_sec=%.0f syncs_per_sec=%.0f records_per_sync=%.1f mean_sync_us=%.1f\n",
                writers, static_cast<double>(acked.load()) / seconds, static_cast<double>(s.batches) / seconds,
                static_cast<double>(s.records) / batches, static_cast<double>(s.sync_nanos) / batches / 1000.0);
  }
  return 0;
}
