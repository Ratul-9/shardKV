#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "lldstore/log_file.h"

namespace lldstore {

struct WalOptions {
  SyncMode sync = SyncMode::kData;
  // Group commit window. 0 = start syncing as soon as anything is pending: batches still form
  // naturally, because everything appended WHILE a sync runs goes into the next batch. A small
  // delay (e.g. 100 us) trades latency at low load for bigger batches at high load (D-040).
  std::chrono::microseconds max_batch_delay{0};
  size_t max_batch_bytes = 4u << 20;  // a batch stops growing at this size
};

// Phase 7: a write-ahead log with GROUP COMMIT. Written by Claude at Ratul's request.
// Design decisions: docs/DESIGN.md D-040 and D-041.
//
//   writer threads --Append()--> pending buffer --flusher thread--> write() + fdatasync()
//                  <--WaitDurable(ticket) returns once the sync covering the ticket has finished
//
// Append is cheap (frame + copy under a mutex) and returns a ticket. WaitDurable(ticket) blocks
// until the record and everything before it is on stable storage. One fdatasync covers every
// record in the batch, so N concurrent writers pay roughly one sync between them instead of N.
//
// The rule a caller must keep: ACKNOWLEDGE A WRITE ONLY AFTER WaitDurable(its ticket) RETURNS.
//
// Failure: if a write or sync fails, the log enters a failed state. Every waiter and every later
// Append throws. After an fsync error the page cache state is unknown (the kernel may have dropped
// the dirty pages), so retrying the fsync is NOT safe; the only safe recovery is a restart that
// replays the log from disk (D-041).
class GroupCommitLog {
 public:
  struct Stats {
    uint64_t records = 0;
    uint64_t batches = 0;  // = number of syncs
    uint64_t bytes = 0;
    uint64_t sync_nanos = 0;
    uint64_t max_batch_records = 0;
  };

  GroupCommitLog(std::unique_ptr<LogFile> file, WalOptions options);
  // Flushes and syncs everything appended, then stops the flusher.
  ~GroupCommitLog();

  GroupCommitLog(const GroupCommitLog&) = delete;
  GroupCommitLog& operator=(const GroupCommitLog&) = delete;

  // Thread-safe. Returns the record's ticket (1, 2, 3, ...). Throws if the log has failed.
  uint64_t Append(std::string_view payload);
  // Thread-safe. Returns once ticket `t` is durable; t == 0 returns at once. Throws on failure.
  void WaitDurable(uint64_t ticket);
  uint64_t Sync() {  // Append nothing, wait for everything so far
    uint64_t t;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      t = appended_;
    }
    WaitDurable(t);
    return t;
  }

  uint64_t bytes_on_disk() const;  // file size including pending bytes
  Stats stats() const;

 private:
  void FlusherLoop();

  const WalOptions options_;
  std::unique_ptr<LogFile> file_;  // touched only by the flusher (and the destructor after join)

  mutable std::mutex mutex_;
  std::condition_variable work_cv_;     // flusher waits for pending data or stop
  std::condition_variable durable_cv_;  // writers wait for durable_ to pass their ticket
  std::string pending_;                 // framed records not yet handed to the flusher
  uint64_t appended_ = 0;               // last ticket given out
  uint64_t durable_ = 0;                // last ticket known to be on stable storage
  uint64_t bytes_total_ = 0;
  bool stop_ = false;
  std::string error_;  // non-empty once failed
  Stats stats_;
  std::thread flusher_;
};

}  // namespace lldstore
