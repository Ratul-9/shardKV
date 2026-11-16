#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lldstore/codec.h"
#include "lldstore/crc32c.h"
#include "lldstore/durable_store.h"
#include "lldstore/log_file.h"
#include "lldstore/wal.h"
#include "test_dir.h"

namespace lldstore {
namespace {

using testing::TempDir;
namespace fs = std::filesystem;

// ---- Codec and checksum ---------------------------------------------------------------------

TEST(Crc32c, KnownVectorsAndChaining) {
  EXPECT_EQ(Crc32c(""), 0u);
  EXPECT_EQ(Crc32c("123456789"), 0xE3069283u);
  EXPECT_EQ(Crc32c("56789", Crc32c("1234")), Crc32c("123456789"));
  EXPECT_NE(Crc32c("123456788"), Crc32c("123456789"));
}

TEST(Codec, RoundTripAndRejectsTruncation) {
  ByteWriter w;
  w.U8(7);
  w.U32(0xdeadbeef);
  w.U64(0x0123456789abcdefULL);
  w.Bool(true);
  w.Bytes(std::string("a\0b", 3));
  const std::string data = w.Take();
  {
    ByteReader r(data);
    uint8_t a;
    uint32_t b;
    uint64_t c;
    bool d;
    std::string_view e;
    ASSERT_TRUE(r.U8(&a) && r.U32(&b) && r.U64(&c) && r.Bool(&d) && r.Bytes(&e));
    EXPECT_TRUE(r.done());
    EXPECT_EQ(a, 7);
    EXPECT_EQ(b, 0xdeadbeefu);
    EXPECT_EQ(c, 0x0123456789abcdefULL);
    EXPECT_TRUE(d);
    EXPECT_EQ(e, std::string_view("a\0b", 3));
  }
  for (size_t cut = 0; cut < data.size(); ++cut) {
    ByteReader r(std::string_view(data).substr(0, cut));
    uint8_t a;
    uint32_t b;
    uint64_t c;
    bool d;
    std::string_view e;
    EXPECT_FALSE(r.U8(&a) && r.U32(&b) && r.U64(&c) && r.Bool(&d) && r.Bytes(&e)) << cut;
  }
}

// ---- Record framing and recovery scan -------------------------------------------------------

std::vector<std::string> SamplePayloads() {
  std::vector<std::string> p;
  for (int i = 0; i < 12; ++i) p.push_back(std::string(static_cast<size_t>(i * 7), static_cast<char>('a' + i)));
  p.push_back(std::string(1000, '\0'));
  return p;
}

TEST(LogScan, ReadsBackEveryRecord) {
  std::string log;
  const auto payloads = SamplePayloads();
  for (const auto& p : payloads) AppendFramedRecord(log, p);
  std::vector<std::string> got;
  const LogScan scan = ScanLogBytes(log, [&](std::string_view p) {
    got.emplace_back(p);
    return true;
  });
  EXPECT_EQ(got, payloads);
  EXPECT_FALSE(scan.torn_tail());
  EXPECT_EQ(scan.valid_bytes, log.size());
}

// The Phase 7 test the plan asks for: cut the log at EVERY byte offset and check recovery keeps
// exactly the records that are complete in the prefix, and nothing else.
TEST(LogScan, TruncationAtEveryByteKeepsExactlyTheCompleteRecords) {
  std::string log;
  std::vector<size_t> ends;
  const auto payloads = SamplePayloads();
  for (const auto& p : payloads) {
    AppendFramedRecord(log, p);
    ends.push_back(log.size());
  }
  for (size_t cut = 0; cut <= log.size(); ++cut) {
    size_t expected = 0;
    while (expected < ends.size() && ends[expected] <= cut) ++expected;
    size_t got = 0;
    const LogScan scan = ScanLogBytes(std::string_view(log).substr(0, cut), [&](std::string_view p) {
      EXPECT_EQ(p, payloads[got]);
      ++got;
      return true;
    });
    ASSERT_EQ(got, expected) << "cut at " << cut;
    ASSERT_EQ(scan.valid_bytes, expected == 0 ? 0 : ends[expected - 1]) << cut;
  }
}

TEST(LogScan, AFlippedBitStopsRecoveryAtThatRecord) {
  std::string log;
  std::vector<size_t> starts;
  const auto payloads = SamplePayloads();
  for (const auto& p : payloads) {
    starts.push_back(log.size());
    AppendFramedRecord(log, p);
  }
  for (size_t k = 0; k < payloads.size(); ++k) {
    for (size_t offset : {size_t{0}, size_t{3}, size_t{5}, size_t{8}}) {  // length, crc, payload
      std::string damaged = log;
      const size_t at = starts[k] + offset;
      if (at >= (k + 1 < starts.size() ? starts[k + 1] : log.size())) continue;
      damaged[at] = static_cast<char>(damaged[at] ^ 0x10);
      size_t got = 0;
      const LogScan scan = ScanLogBytes(damaged, [&](std::string_view) {
        ++got;
        return true;
      });
      EXPECT_EQ(got, k) << "record " << k << " offset " << offset;
      EXPECT_EQ(scan.valid_bytes, starts[k]);
    }
  }
}

TEST(LogScan, AnAbsurdLengthIsCorruptionNotAnAllocation) {
  std::string log;
  AppendFramedRecord(log, "ok");
  log += std::string("\xff\xff\xff\xff\0\0\0\0", 8);
  const LogScan scan = ScanLogBytes(log, [](std::string_view) { return true; });
  EXPECT_EQ(scan.records, 1u);
  EXPECT_TRUE(scan.torn_tail());
}

// ---- Group commit ---------------------------------------------------------------------------

TEST(GroupCommitLog, EverythingAppendedIsOnDiskAfterShutdown) {
  TempDir dir;
  {
    GroupCommitLog log(LogFile::Open(dir.File("log"), 0), WalOptions{});
    for (int i = 0; i < 100; ++i) log.Append("record-" + std::to_string(i));
  }
  int i = 0;
  const LogScan scan = ScanLog(dir.File("log"), [&](std::string_view p) {
    EXPECT_EQ(p, "record-" + std::to_string(i++));
    return true;
  });
  EXPECT_EQ(scan.records, 100u);
}

TEST(GroupCommitLog, ConcurrentWritersShareSyncs) {
  TempDir dir;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  GroupCommitLog::Stats stats;
  {
    GroupCommitLog log(LogFile::Open(dir.File("log"), 0), WalOptions{});
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          const uint64_t ticket = log.Append(std::to_string(t) + ":" + std::to_string(i));
          log.WaitDurable(ticket);  // ack only after the covering sync
        }
      });
    }
    for (auto& th : threads) th.join();
    stats = log.stats();
  }
  EXPECT_EQ(stats.records, uint64_t{kThreads} * kPerThread);
  EXPECT_LE(stats.batches, stats.records);
  // Each thread's records appear in its own order, and all of them are there.
  std::map<int, int> next;
  const LogScan scan = ScanLog(dir.File("log"), [&](std::string_view p) {
    const auto colon = p.find(':');
    const int t = std::stoi(std::string(p.substr(0, colon)));
    const int i = std::stoi(std::string(p.substr(colon + 1)));
    EXPECT_EQ(i, next[t]++);
    return true;
  });
  EXPECT_EQ(scan.records, uint64_t{kThreads} * kPerThread);
}

TEST(GroupCommitLog, ReopeningCutsATornTailBeforeAppending) {
  TempDir dir;
  {
    GroupCommitLog log(LogFile::Open(dir.File("log"), 0), WalOptions{});
    log.WaitDurable(log.Append("one"));
    log.WaitDurable(log.Append("two"));
  }
  fs::resize_file(dir.File("log"), fs::file_size(dir.File("log")) - 2);  // tear "two"
  const LogScan scan = ScanLog(dir.File("log"), [](std::string_view) { return true; });
  ASSERT_EQ(scan.records, 1u);
  {
    GroupCommitLog log(LogFile::Open(dir.File("log"), scan.valid_bytes), WalOptions{});
    log.WaitDurable(log.Append("three"));
  }
  std::vector<std::string> got;
  ScanLog(dir.File("log"), [&](std::string_view p) {
    got.emplace_back(p);
    return true;
  });
  EXPECT_EQ(got, (std::vector<std::string>{"one", "three"}));
}

// ---- DurableStore ---------------------------------------------------------------------------

TEST(DurableStore, SurvivesReopen) {
  TempDir dir;
  {
    DurableStore store(dir.path());
    store.Put("a", "1");
    store.Put("b", "2");
    store.Put("a", "3");
    EXPECT_TRUE(store.Delete("b"));
    EXPECT_FALSE(store.Delete("missing"));
  }
  DurableStore store(dir.path());
  std::string out;
  ASSERT_TRUE(store.Get("a", &out));
  EXPECT_EQ(out, "3");
  EXPECT_FALSE(store.Get("b", &out));
  EXPECT_EQ(store.Size(), 1u);
  EXPECT_EQ(store.recovery().log_records, 4u);
}

TEST(DurableStore, CheckpointThenMoreWritesThenReopen) {
  TempDir dir;
  {
    DurableStore store(dir.path());
    for (int i = 0; i < 100; ++i) store.Put("k" + std::to_string(i), "v" + std::to_string(i));
    store.Checkpoint();
    store.Delete("k5");
    store.Put("k6", "changed");
  }
  DurableStore store(dir.path());
  EXPECT_EQ(store.recovery().snapshot_entries, 100u);
  EXPECT_EQ(store.recovery().log_records, 2u);
  EXPECT_EQ(store.Size(), 99u);
  std::string out;
  EXPECT_FALSE(store.Get("k5", &out));
  ASSERT_TRUE(store.Get("k6", &out));
  EXPECT_EQ(out, "changed");
  size_t logs = 0;
  for (const auto& e : fs::directory_iterator(dir.path()))
    if (e.path().filename().string().rfind("wal-", 0) == 0) ++logs;
  EXPECT_EQ(logs, 1u);  // the pre-checkpoint log was deleted
}

TEST(DurableStore, AutomaticCheckpointsKeepTheLogBounded) {
  TempDir dir;
  DurableStoreOptions options;
  options.checkpoint_log_bytes = 4096;
  {
    DurableStore store(dir.path(), options);
    for (int i = 0; i < 2000; ++i) store.Put("k" + std::to_string(i % 50), std::string(40, 'x'));
  }
  for (const auto& e : fs::directory_iterator(dir.path())) {
    if (e.path().filename().string().rfind("wal-", 0) == 0) {
      EXPECT_LT(fs::file_size(e.path()), 4096u + 200u);
    }
  }
  DurableStore store(dir.path(), options);
  EXPECT_EQ(store.Size(), 50u);
}

TEST(DurableStore, TruncatingTheLogAtEveryByteRecoversAPrefixOfTheWrites) {
  TempDir dir;
  std::vector<std::pair<std::string, std::string>> writes;
  for (int i = 0; i < 20; ++i) writes.emplace_back("key" + std::to_string(i % 7), "value" + std::to_string(i));
  {
    DurableStore store(dir.path());
    for (const auto& [k, v] : writes) store.Put(k, v);
  }
  std::string log;
  std::string log_name;
  for (const auto& e : fs::directory_iterator(dir.path()))
    if (e.path().filename().string().rfind("wal-", 0) == 0) log_name = e.path().filename().string();
  ASSERT_TRUE(ReadWholeFile(dir.File(log_name), &log));
  for (size_t cut = 0; cut <= log.size(); ++cut) {
    TempDir copy;
    {
      std::string prefix = log.substr(0, cut);
      WriteFileAtomically(copy.File(log_name), prefix, SyncMode::kNone);
    }
    DurableStore store(copy.path(), DurableStoreOptions{WalOptions{SyncMode::kNone}, 0});
    const size_t applied = store.recovery().log_records;
    std::map<std::string, std::string> model;
    for (size_t i = 0; i < applied; ++i) model[writes[i].first] = writes[i].second;
    ASSERT_EQ(store.Size(), model.size()) << cut;
    for (const auto& [k, v] : model) {
      std::string out;
      ASSERT_TRUE(store.Get(k, &out)) << cut;
      ASSERT_EQ(out, v) << cut;
    }
  }
}

TEST(DurableStore, RecoveryIsIdempotent) {
  TempDir dir;
  {
    DurableStore store(dir.path());
    store.Put("x", "1");
    store.Put("y", "2");
  }
  // Tear the last record, then recover three times.
  std::string name;
  for (const auto& e : fs::directory_iterator(dir.path()))
    if (e.path().filename().string().rfind("wal-", 0) == 0) name = e.path().filename().string();
  fs::resize_file(dir.File(name), fs::file_size(dir.File(name)) - 1);
  for (int i = 0; i < 3; ++i) {
    DurableStore store(dir.path());
    EXPECT_EQ(store.Size(), 1u);
    EXPECT_EQ(store.recovery().torn_bytes_dropped, i == 0 ? 1u : 0u) << i;
  }
}

TEST(DurableStore, ADamagedSnapshotRefusesToStart) {
  TempDir dir;
  {
    DurableStore store(dir.path());
    store.Put("a", "b");
    store.Checkpoint();
  }
  std::string snap;
  ASSERT_TRUE(ReadWholeFile(dir.File("snapshot"), &snap));
  snap[snap.size() / 2] = static_cast<char>(snap[snap.size() / 2] ^ 1);
  WriteFileAtomically(dir.File("snapshot"), snap, SyncMode::kNone);
  EXPECT_THROW(DurableStore store(dir.path()), std::runtime_error);
}

// ---- The crash test: kill -9 at a random moment under load ----------------------------------
//
// A child process writes with several threads and reports each ACKNOWLEDGED Put to the parent
// through a pipe (8-byte writes are atomic on a pipe). The parent SIGKILLs it at a random time,
// reopens the store and checks: every acknowledged write is present with its value, and no key
// holds a value that was never written. Small checkpoint threshold, so kills also land inside
// checkpoints.
void RunChild(const std::string& dir, int ack_fd) {
  DurableStoreOptions options;
  options.checkpoint_log_bytes = 16 * 1024;
  DurableStore store(dir, options);
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (uint32_t i = 0;; ++i) {
        const uint64_t id = (uint64_t{t} << 32) | i;
        store.Put("t" + std::to_string(t) + "-" + std::to_string(i % 300), std::to_string(id));
        if (::write(ack_fd, &id, sizeof(id)) != sizeof(id)) ::_exit(3);
      }
    });
  }
  for (auto& th : threads) th.join();
}

TEST(DurableStoreCrash, KillNineNeverLosesAnAcknowledgedWrite) {
  std::mt19937 rng(12345);
  for (int round = 0; round < 20; ++round) {
    TempDir dir;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      ::close(fds[0]);
      RunChild(dir.path(), fds[1]);
      ::_exit(0);
    }
    ::close(fds[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(20 + rng() % 200));
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    // Last acknowledged value per key (each thread's writes to a key are in order).
    std::map<std::string, uint64_t> acked;
    uint64_t id;
    while (::read(fds[0], &id, sizeof(id)) == sizeof(id)) {
      const uint32_t t = static_cast<uint32_t>(id >> 32), i = static_cast<uint32_t>(id);
      acked["t" + std::to_string(t) + "-" + std::to_string(i % 300)] = id;
    }
    ::close(fds[0]);

    DurableStore store(dir.path());
    for (const auto& [key, last_acked] : acked) {
      std::string out;
      ASSERT_TRUE(store.Get(key, &out)) << "round " << round << ": lost " << key;
      const uint64_t found = std::stoull(out);
      // The store may hold a LATER write than the last ack (written, synced, killed before the ack
      // reached the pipe), but never an earlier one, and always one this key really received.
      ASSERT_GE(found, last_acked) << "round " << round << " key " << key;
      ASSERT_EQ(found >> 32, last_acked >> 32) << key;
      ASSERT_EQ(static_cast<uint32_t>(found) % 300, static_cast<uint32_t>(last_acked) % 300) << key;
    }
  }
}

}  // namespace
}  // namespace lldstore
