#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lldstore {

// Phase 7: the on-disk record format and the file primitives under the write-ahead log.
// Written by Claude at Ratul's request. Design decisions: docs/DESIGN.md D-037 to D-039.
//
// Record framing (all little-endian):
//
//     +-------------+-------------+------------------+
//     | length: u32 | crc32c: u32 | payload (length) |
//     +-------------+-------------+------------------+
//
// The CRC covers the 4 length bytes AND the payload, so a corrupted length is detected too (a
// flipped bit in the length would otherwise make the reader take the wrong bytes as the payload).
// A record is valid only if all of it is present and the CRC matches.
//
// Recovery rule: replay records from the start and STOP at the first invalid one. Everything from
// there on is discarded (truncated). A record after a bad one is never trusted, because records
// are written in order: if record k is torn, a later record can only be there if the disk reordered
// writes, and then it was never acknowledged either (D-038).

constexpr size_t kRecordHeaderSize = 8;
constexpr uint32_t kMaxRecordSize = 64u << 20;  // a larger length field is treated as corruption

// Appends one framed record to `out`.
void AppendFramedRecord(std::string& out, std::string_view payload);

enum class SyncMode {
  kNone,  // never sync: data reaches the page cache only. For measuring, not for durability.
  kData,  // fdatasync on Linux; F_FULLFSYNC on macOS (plain fsync there does not flush the drive)
};

// An append-only file. Not thread-safe: GroupCommitLog's flusher thread is its only user.
class LogFile {
 public:
  // Opens `path` for appending, creating it if needed, after truncating it to `valid_size` bytes
  // (recovery decides how much of an existing file is valid). Syncs the directory when the file
  // is new, so the file itself survives a crash. Throws std::system_error on failure.
  static std::unique_ptr<LogFile> Open(const std::string& path, uint64_t valid_size);

  // Adopts an open file descriptor positioned at `size` (closed by the destructor).
  LogFile(int fd, std::string path, uint64_t size) : fd_(fd), path_(std::move(path)), size_(size) {}
  ~LogFile();
  LogFile(const LogFile&) = delete;
  LogFile& operator=(const LogFile&) = delete;

  void Write(std::string_view bytes);  // all or throw
  void Sync(SyncMode mode);            // throws on failure
  uint64_t size() const noexcept { return size_; }
  const std::string& path() const noexcept { return path_; }

 private:
  int fd_;
  std::string path_;
  uint64_t size_;
};

struct LogScan {
  uint64_t valid_bytes = 0;  // length of the longest valid prefix
  uint64_t file_bytes = 0;
  uint64_t records = 0;
  bool torn_tail() const noexcept { return valid_bytes != file_bytes; }
};

// Calls `on_record` for each valid record in order and stops at the first invalid one. A missing
// file is an empty log. `on_record` returning false stops the scan as if the record were invalid
// (used when a payload decodes badly). Throws std::system_error if the file cannot be read.
LogScan ScanLog(const std::string& path, const std::function<bool(std::string_view)>& on_record);
LogScan ScanLogBytes(std::string_view data, const std::function<bool(std::string_view)>& on_record);

// Durable whole-file replacement: write `path.tmp`, sync it, rename over `path`, sync the
// directory. After a crash either the old or the new file is there, never a mix (D-039).
void WriteFileAtomically(const std::string& path, std::string_view data, SyncMode mode);

// Reads a whole file. Returns false if it does not exist; throws on other errors.
bool ReadWholeFile(const std::string& path, std::string* out);

// Makes a create, rename or unlink in `dir` durable.
void SyncDirectory(const std::string& dir, SyncMode mode);

}  // namespace lldstore
