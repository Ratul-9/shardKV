#include "lldstore/wal.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace lldstore {

GroupCommitLog::GroupCommitLog(std::unique_ptr<LogFile> file, WalOptions options)
    : options_(options), file_(std::move(file)), bytes_total_(file_->size()) {
  flusher_ = std::thread([this] { FlusherLoop(); });
}

GroupCommitLog::~GroupCommitLog() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_one();
  flusher_.join();
}

uint64_t GroupCommitLog::Append(std::string_view payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!error_.empty()) throw std::runtime_error("WAL failed: " + error_);
  if (stop_) throw std::logic_error("WAL used after shutdown");
  const size_t before = pending_.size();
  AppendFramedRecord(pending_, payload);
  bytes_total_ += pending_.size() - before;
  ++appended_;
  // Wake the flusher only when the buffer goes from empty to non-empty: while it is non-empty the
  // flusher is either already awake or will look at it right after its current sync.
  if (before == 0) work_cv_.notify_one();
  return appended_;
}

void GroupCommitLog::WaitDurable(uint64_t ticket) {
  std::unique_lock<std::mutex> lock(mutex_);
  durable_cv_.wait(lock, [&] { return durable_ >= ticket || !error_.empty(); });
  if (durable_ < ticket) throw std::runtime_error("WAL failed: " + error_);
}

uint64_t GroupCommitLog::bytes_on_disk() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytes_total_;
}

GroupCommitLog::Stats GroupCommitLog::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void GroupCommitLog::FlusherLoop() {
  std::string batch;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    work_cv_.wait(lock, [&] { return !pending_.empty() || stop_; });
    if (pending_.empty()) return;  // stopped and fully flushed
    if (options_.max_batch_delay.count() > 0 && !stop_) {
      // Optional window: let more writers join this batch before paying for the sync.
      const auto deadline = std::chrono::steady_clock::now() + options_.max_batch_delay;
      work_cv_.wait_until(lock, deadline,
                          [&] { return stop_ || pending_.size() >= options_.max_batch_bytes; });
    }
    batch.swap(pending_);  // take the whole batch; writers keep appending into a fresh buffer
    pending_.clear();
    const uint64_t batch_last = appended_;
    const uint64_t batch_records = batch_last - durable_;
    lock.unlock();

    std::string failure;
    const auto start = std::chrono::steady_clock::now();
    try {
      file_->Write(batch);
      file_->Sync(options_.sync);
    } catch (const std::exception& e) {
      failure = e.what();
    }
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    batch.clear();

    lock.lock();
    if (!failure.empty()) {
      error_ = failure;  // permanent: see the class comment
      durable_cv_.notify_all();
      return;
    }
    durable_ = batch_last;
    ++stats_.batches;
    stats_.records += batch_records;
    stats_.bytes = file_->size();
    stats_.sync_nanos += static_cast<uint64_t>(nanos);
    stats_.max_batch_records = std::max(stats_.max_batch_records, batch_records);
    durable_cv_.notify_all();
  }
}

}  // namespace lldstore
