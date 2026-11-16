#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "lldstore/flat_store.h"
#include "lldstore/wal.h"

namespace lldstore {

struct DurableStoreOptions {
  WalOptions wal;
  // Take a checkpoint (snapshot + new log) once the current log exceeds this. 0 = only on request.
  uint64_t checkpoint_log_bytes = 64u << 20;
};

// Phase 7: a key-value store whose acknowledged writes survive a crash (kill -9, power loss with
// an honest disk). Written by Claude at Ratul's request. Design: docs/DESIGN.md D-042 and D-043.
//
// Directory layout:
//   snapshot              all entries as of a checkpoint, written atomically (tmp + rename)
//   wal-<generation>.log  every Put/Delete since that checkpoint, in order
//
// Put/Delete: append a record to the log, apply it to the in-memory FlatStore, then wait for the
// group commit that covers the record. Only then return (acknowledge).
// Get: read memory, then wait until every write that was applied before the read is durable,
// because a reader must not observe a write that could still be lost (D-042).
// Recovery (constructor): load the snapshot, replay the logs from the snapshot's generation on,
// truncate the torn tail of the last log, delete logs older than the snapshot.
//
// Thread-safe: one mutex around memory + log append (short), while the expensive part, waiting
// for the fsync, happens outside it, which is what lets concurrent writers share one sync.
class DurableStore {
 public:
  struct RecoveryInfo {
    uint64_t snapshot_entries = 0;
    uint64_t log_records = 0;
    uint64_t torn_bytes_dropped = 0;
    uint64_t generation = 0;
    uint64_t stale_logs_deleted = 0;
  };

  explicit DurableStore(std::string dir, DurableStoreOptions options = {});
  ~DurableStore();

  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  void Put(std::string_view key, std::string_view value);
  bool Get(std::string_view key, std::string* out);
  bool Delete(std::string_view key);
  size_t Size() const;

  // Writes a snapshot of everything and starts a new, empty log. Blocks all operations while it
  // runs (stop-the-world, D-043).
  void Checkpoint();

  const RecoveryInfo& recovery() const noexcept { return recovery_; }
  GroupCommitLog::Stats wal_stats() const;

 private:
  void Recover();
  void CheckpointLocked();
  void MaybeCheckpointLocked();
  std::string LogPath(uint64_t generation) const;

  const std::string dir_;
  const DurableStoreOptions options_;
  mutable std::mutex mutex_;
  FlatStore memory_;
  std::shared_ptr<GroupCommitLog> log_;  // shared: a waiter may outlive a checkpoint's log swap
  uint64_t last_ticket_ = 0;             // ticket of the last write applied to memory_ (in log_)
  uint64_t generation_ = 0;
  RecoveryInfo recovery_;
};

}  // namespace lldstore
