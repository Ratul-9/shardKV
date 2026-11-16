#include "lldstore/log_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

#include "lldstore/codec.h"
#include "lldstore/crc32c.h"

namespace lldstore {
namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

uint32_t LoadU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

void SyncFd(int fd, SyncMode mode, const std::string& what) {
  if (mode == SyncMode::kNone) return;
#if defined(__APPLE__)
  // macOS fsync() only pushes data to the drive, which may keep it in a volatile cache.
  // F_FULLFSYNC asks the drive to flush that cache too. Some filesystems refuse it; fall back.
  if (fcntl(fd, F_FULLFSYNC) == 0) return;
  if (fsync(fd) != 0) ThrowErrno("fsync " + what);
#else
  // fdatasync skips metadata that is not needed to read the data back (e.g. mtime). The file size
  // IS needed, so appends that grow the file are still covered.
  if (fdatasync(fd) != 0) ThrowErrno("fdatasync " + what);
#endif
}

std::string DirOf(const std::string& path) {
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  return parent.empty() ? std::string(".") : parent.string();
}

}  // namespace

void AppendFramedRecord(std::string& out, std::string_view payload) {
  char length[4];
  const auto n = static_cast<uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) length[i] = static_cast<char>((n >> (8 * i)) & 0xff);
  const uint32_t crc = Crc32c(payload, Crc32c(std::string_view(length, 4)));
  out.append(length, 4);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  out.append(payload.data(), payload.size());
}

std::unique_ptr<LogFile> LogFile::Open(const std::string& path, uint64_t valid_size) {
  const bool existed = std::filesystem::exists(path);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) ThrowErrno("open " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    ThrowErrno("fstat " + path);
  }
  if (static_cast<uint64_t>(st.st_size) != valid_size) {
    // Cut off a torn tail before appending, or new records would follow garbage and be lost on the
    // next recovery. The truncation must itself be durable before anything is appended.
    if (::ftruncate(fd, static_cast<off_t>(valid_size)) != 0) {
      ::close(fd);
      ThrowErrno("ftruncate " + path);
    }
    SyncFd(fd, SyncMode::kData, path);
  }
  if (::lseek(fd, static_cast<off_t>(valid_size), SEEK_SET) < 0) {
    ::close(fd);
    ThrowErrno("lseek " + path);
  }
  if (!existed) SyncDirectory(DirOf(path), SyncMode::kData);
  return std::unique_ptr<LogFile>(new LogFile(fd, path, valid_size));
}

LogFile::~LogFile() { ::close(fd_); }

void LogFile::Write(std::string_view bytes) {
  const char* p = bytes.data();
  size_t left = bytes.size();
  while (left > 0) {
    const ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("write " + path_);
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  size_ += bytes.size();
}

void LogFile::Sync(SyncMode mode) { SyncFd(fd_, mode, path_); }

LogScan ScanLogBytes(std::string_view data, const std::function<bool(std::string_view)>& on_record) {
  LogScan scan;
  scan.file_bytes = data.size();
  size_t pos = 0;
  while (data.size() - pos >= kRecordHeaderSize) {
    const uint32_t length = LoadU32(data.data() + pos);
    const uint32_t stored_crc = LoadU32(data.data() + pos + 4);
    if (length > kMaxRecordSize || data.size() - pos - kRecordHeaderSize < length) break;
    const std::string_view payload = data.substr(pos + kRecordHeaderSize, length);
    const uint32_t crc = Crc32c(payload, Crc32c(data.substr(pos, 4)));
    if (crc != stored_crc) break;
    if (!on_record(payload)) break;
    pos += kRecordHeaderSize + length;
    ++scan.records;
  }
  scan.valid_bytes = pos;
  return scan;
}

LogScan ScanLog(const std::string& path, const std::function<bool(std::string_view)>& on_record) {
  std::string data;
  if (!ReadWholeFile(path, &data)) return LogScan{};
  return ScanLogBytes(data, on_record);
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return false;
    ThrowErrno("open " + path);
  }
  out->clear();
  char buf[1 << 16];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      const int saved = errno;
      ::close(fd);
      errno = saved;
      ThrowErrno("read " + path);
    }
    if (n == 0) break;
    out->append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return true;
}

void WriteFileAtomically(const std::string& path, std::string_view data, SyncMode mode) {
  const std::string tmp = path + ".tmp";
  {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) ThrowErrno("open " + tmp);
    LogFile file(fd, tmp, 0);  // closes fd on every path
    file.Write(data);
    file.Sync(mode);  // the data must be durable BEFORE the rename makes it visible
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) ThrowErrno("rename " + tmp);
  SyncDirectory(DirOf(path), mode);  // makes the rename itself durable
}

void SyncDirectory(const std::string& dir, SyncMode mode) {
  if (mode == SyncMode::kNone) return;
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) ThrowErrno("open dir " + dir);
  const int rc = ::fsync(fd);  // F_FULLFSYNC is not supported on directories
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    ThrowErrno("fsync dir " + dir);
  }
}

}  // namespace lldstore
