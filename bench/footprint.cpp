// Memory footprint of the store: bytes requested from operator new, and process RSS.
// Usage: footprint [num_keys=1000000] [map|flat]
//
// Replaces global operator new/delete with counting versions. Each block carries a 16-byte header
// holding its size so delete can subtract it. This counts *requested* bytes; the allocator adds its
// own per-block overhead, which only RSS shows. We report both.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "bench_util.h"
#include "lldstore/flat_store.h"
#include "lldstore/store.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>

#include <fstream>
#endif

namespace {
size_t g_live_bytes = 0;
size_t g_alloc_count = 0;
constexpr size_t kHeader = 16;  // keeps the user pointer 16-byte aligned

size_t CurrentRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS)
    return 0;
  return info.resident_size;
#elif defined(__linux__)
  std::ifstream f("/proc/self/statm");
  size_t total_pages = 0, resident_pages = 0;
  f >> total_pages >> resident_pages;
  return resident_pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}
}  // namespace

void* operator new(size_t n) {
  void* p = std::malloc(n + kHeader);
  if (p == nullptr) throw std::bad_alloc();
  *static_cast<size_t*>(p) = n;
  g_live_bytes += n;
  ++g_alloc_count;
  return static_cast<char*>(p) + kHeader;
}

void operator delete(void* p) noexcept {
  if (p == nullptr) return;
  void* base = static_cast<char*>(p) - kHeader;
  g_live_bytes -= *static_cast<size_t*>(base);
  std::free(base);
}

void operator delete(void* p, size_t) noexcept { operator delete(p); }

void* operator new(size_t n, std::align_val_t align) {
  const auto a = static_cast<size_t>(align);
  const size_t header = a > kHeader ? a : kHeader;
  const size_t total = (n + header + a - 1) / a * a;
  void* p = std::aligned_alloc(a, total);
  if (p == nullptr) throw std::bad_alloc();
  *static_cast<size_t*>(p) = n;
  g_live_bytes += n;
  ++g_alloc_count;
  return static_cast<char*>(p) + header;
}

void operator delete(void* p, std::align_val_t align) noexcept {
  if (p == nullptr) return;
  const auto a = static_cast<size_t>(align);
  void* base = static_cast<char*>(p) - (a > kHeader ? a : kHeader);
  g_live_bytes -= *static_cast<size_t*>(base);
  std::free(base);
}

void operator delete(void* p, size_t, std::align_val_t align) noexcept {
  operator delete(p, align);
}

template <typename S>
int Measure(const char* table, uint64_t n) {
  using namespace lldstore::bench;
  const size_t rss_before = CurrentRssBytes();
  const size_t live_before = g_live_bytes;
  const size_t allocs_before = g_alloc_count;

  S store;
  Prefill(store, n);

  const size_t rss_after = CurrentRssBytes();
  const double entries = static_cast<double>(n);
  const double payload = static_cast<double>(kKeySize + kValueSize);
  const double requested = static_cast<double>(g_live_bytes - live_before) / entries;
  const double rss = static_cast<double>(rss_after - rss_before) / entries;

  std::printf("table=%s keys=%llu stored=%zu payload/entry=%.0f B\n", table,
              static_cast<unsigned long long>(n), store.Size(), payload);
  std::printf("requested bytes/entry=%.1f (overhead %.2fx)  allocations/entry=%.2f\n", requested,
              requested / payload,
              static_cast<double>(g_alloc_count - allocs_before) / entries);
  std::printf("RSS delta/entry=%.1f B (overhead %.2fx)\n", rss, rss / payload);
  return 0;
}

int main(int argc, char** argv) {
  const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const char* table = argc > 2 ? argv[2] : "map";
  if (std::strcmp(table, "map") == 0) return Measure<lldstore::Store>(table, n);
  if (std::strcmp(table, "flat") == 0) return Measure<lldstore::FlatStore>(table, n);
  std::fprintf(stderr, "table must be map or flat\n");
  return 2;
}
