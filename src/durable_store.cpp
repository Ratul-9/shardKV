#include "lldstore/durable_store.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "lldstore/codec.h"

namespace lldstore {
namespace {

namespace fs = std::filesystem;

enum : uint8_t { kOpPut = 1, kOpDelete = 2, kSnapshotHeader = 10, kSnapshotTrailer = 11 };
constexpr uint64_t kSnapshotMagic = 0x6c6c6473746f7265ULL;  // "lldstore"
constexpr uint32_t kFormatVersion = 1;

std::string EncodePut(std::string_view key, std::string_view value) {
  ByteWriter w;
  w.U8(kOpPut);
  w.Bytes(key);
  w.Bytes(value);
  return w.Take();
}

std::string EncodeDelete(std::string_view key) {
  ByteWriter w;
  w.U8(kOpDelete);
  w.Bytes(key);
  return w.Take();
}

// Applies one log/snapshot record to `memory`. Returns false if it does not decode, which the
// scanner treats like a bad checksum (stop there).
bool ApplyRecord(FlatStore& memory, std::string_view payload) {
  ByteReader r(payload);
  uint8_t op = 0;
  std::string_view key, value;
  if (!r.U8(&op)) return false;
  if (op == kOpPut) {
    if (!r.Bytes(&key) || !r.Bytes(&value) || !r.done()) return false;
    memory.Put(key, value);
    return true;
  }
  if (op == kOpDelete) {
    if (!r.Bytes(&key) || !r.done()) return false;
    memory.Delete(key);  // idempotent: replaying a delete of a missing key is fine
    return true;
  }
  return false;
}

// Generation numbers of every wal-*.log file in `dir`, ascending.
std::vector<uint64_t> ListLogGenerations(const std::string& dir) {
  std::vector<uint64_t> gens;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    unsigned long long g = 0;
    char tail = 0;
    if (std::sscanf(name.c_str(), "wal-%20llu.lo%c", &g, &tail) == 2 && tail == 'g' &&
        name.size() == 4 + 20 + 4) {
      gens.push_back(static_cast<uint64_t>(g));
    }
  }
  std::sort(gens.begin(), gens.end());
  return gens;
}

}  // namespace

DurableStore::DurableStore(std::string dir, DurableStoreOptions options)
    : dir_(std::move(dir)), options_(options) {
  fs::create_directories(dir_);
  Recover();
}

DurableStore::~DurableStore() = default;  // log_ destructor flushes and syncs

std::string DurableStore::LogPath(uint64_t generation) const {
  char name[32];
  std::snprintf(name, sizeof(name), "wal-%020" PRIu64 ".log", generation);
  return (fs::path(dir_) / name).string();
}

void DurableStore::Recover() {
  // 1. Snapshot. It is written atomically, so it is either absent or complete; a damaged one is
  //    real corruption (or tampering), and we refuse to start rather than silently lose data.
  std::string snapshot;
  uint64_t first_generation = 0;
  if (ReadWholeFile((fs::path(dir_) / "snapshot").string(), &snapshot)) {
    bool header = false, trailer = false;
    uint64_t entries = 0;
    const LogScan scan = ScanLogBytes(snapshot, [&](std::string_view payload) {
      ByteReader r(payload);
      uint8_t type = 0;
      if (!r.U8(&type) || trailer) return false;
      if (!header) {
        uint64_t magic = 0;
        uint32_t version = 0;
        if (type != kSnapshotHeader || !r.U64(&magic) || !r.U32(&version) ||
            !r.U64(&first_generation) || !r.done() || magic != kSnapshotMagic ||
            version != kFormatVersion)
          return false;
        header = true;
        return true;
      }
      if (type == kSnapshotTrailer) {
        uint64_t count = 0;
        if (!r.U64(&count) || !r.done() || count != entries) return false;
        trailer = true;
        return true;
      }
      ++entries;
      return ApplyRecord(memory_, payload);
    });
    if (!header || !trailer || scan.torn_tail()) {
      throw std::runtime_error("DurableStore: snapshot in " + dir_ + " is damaged");
    }
    recovery_.snapshot_entries = entries;
  }

  // 2. Logs from the snapshot's generation on, in order. Only the newest may have a torn tail
  //    (it was being written when we crashed); a torn tail in an older one means records after it
  //    are missing, which is corruption.
  std::vector<uint64_t> gens = ListLogGenerations(dir_);
  uint64_t valid_size = 0;
  generation_ = first_generation;
  for (size_t i = 0; i < gens.size(); ++i) {
    if (gens[i] < first_generation) continue;
    const LogScan scan = ScanLog(LogPath(gens[i]), [&](std::string_view payload) {
      return ApplyRecord(memory_, payload);
    });
    recovery_.log_records += scan.records;
    const bool newest = i + 1 == gens.size();
    if (scan.torn_tail() && !newest) {
      throw std::runtime_error("DurableStore: " + LogPath(gens[i]) +
                               " is damaged before its end and a newer log exists");
    }
    recovery_.torn_bytes_dropped += scan.file_bytes - scan.valid_bytes;
    generation_ = gens[i];
    valid_size = scan.valid_bytes;
  }

  // 3. Logs older than the snapshot are fully contained in it (a crash between writing the
  //    snapshot and deleting them leaves them behind). Delete them.
  bool deleted = false;
  for (uint64_t g : gens) {
    if (g < first_generation) {
      fs::remove(LogPath(g));
      ++recovery_.stale_logs_deleted;
      deleted = true;
    }
  }
  if (deleted) SyncDirectory(dir_, options_.wal.sync);

  // 4. Continue appending to the newest log, cut back to its valid prefix (idempotent: recovering
  //    twice gives the same state, because the cut is made durable before anything is appended).
  log_ = std::make_shared<GroupCommitLog>(LogFile::Open(LogPath(generation_), valid_size),
                                          options_.wal);
  recovery_.generation = generation_;
}

void DurableStore::Put(std::string_view key, std::string_view value) {
  std::shared_ptr<GroupCommitLog> log;
  uint64_t ticket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ticket = log_->Append(EncodePut(key, value));  // log first ...
    memory_.Put(key, value);                         // ... then memory, in the same order
    last_ticket_ = ticket;
    log = log_;
    MaybeCheckpointLocked();
  }
  log->WaitDurable(ticket);  // acknowledge only after the sync (outside the lock: group commit)
}

bool DurableStore::Delete(std::string_view key) {
  std::shared_ptr<GroupCommitLog> log;
  uint64_t ticket;
  bool existed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    existed = memory_.Get(key).has_value();
    if (existed) {
      ticket = log_->Append(EncodeDelete(key));
      memory_.Delete(key);
      last_ticket_ = ticket;
    } else {
      ticket = last_ticket_;  // nothing to log, but "absent" may come from an undurable delete
    }
    log = log_;
    if (existed) MaybeCheckpointLocked();
  }
  log->WaitDurable(ticket);
  return existed;
}

bool DurableStore::Get(std::string_view key, std::string* out) {
  std::shared_ptr<GroupCommitLog> log;
  uint64_t ticket;
  bool found;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto v = memory_.Get(key);
    found = v.has_value();
    if (found) out->assign(v->data(), v->size());
    ticket = last_ticket_;
    log = log_;
  }
  // The value read may come from a write that is applied but not yet durable. Returning it before
  // that write is durable could show a client a value that a crash then erases. Wait for it.
  log->WaitDurable(ticket);
  return found;
}

size_t DurableStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return memory_.Size();
}

void DurableStore::Checkpoint() {
  std::lock_guard<std::mutex> lock(mutex_);
  CheckpointLocked();
}

GroupCommitLog::Stats DurableStore::wal_stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return log_->stats();
}

void DurableStore::MaybeCheckpointLocked() {
  if (options_.checkpoint_log_bytes != 0 && log_->bytes_on_disk() >= options_.checkpoint_log_bytes) {
    CheckpointLocked();
  }
}

void DurableStore::CheckpointLocked() {
  // Everything in memory_ must be in the snapshot, and the snapshot must not claim anything that
  // was never logged; both hold because we hold mutex_ (no write is between log and memory).
  log_->WaitDurable(last_ticket_);
  const uint64_t next = generation_ + 1;

  ByteWriter header;
  header.U8(kSnapshotHeader);
  header.U64(kSnapshotMagic);
  header.U32(kFormatVersion);
  header.U64(next);  // the first log generation NOT contained in this snapshot
  std::string data;
  AppendFramedRecord(data, header.buffer());
  uint64_t count = 0;
  memory_.ForEach([&](std::string_view k, std::string_view v) {
    AppendFramedRecord(data, EncodePut(k, v));
    ++count;
  });
  ByteWriter trailer;
  trailer.U8(kSnapshotTrailer);
  trailer.U64(count);
  AppendFramedRecord(data, trailer.buffer());

  // Order matters (D-043): (1) snapshot durable under its final name, (2) new empty log,
  // (3) old logs deleted. A crash after any step recovers correctly.
  WriteFileAtomically((fs::path(dir_) / "snapshot").string(), data, options_.wal.sync);
  auto fresh = std::make_shared<GroupCommitLog>(LogFile::Open(LogPath(next), 0), options_.wal);
  const uint64_t old_generation = generation_;
  log_ = std::move(fresh);  // waiters holding the old log still finish on it (already durable)
  generation_ = next;
  last_ticket_ = 0;
  for (uint64_t g : ListLogGenerations(dir_)) {
    if (g <= old_generation) fs::remove(LogPath(g));
  }
  SyncDirectory(dir_, options_.wal.sync);
}

}  // namespace lldstore
