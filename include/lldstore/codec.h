#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lldstore {

// Little-endian binary encoding shared by the WAL, the Raft log and the network protocol.
// Written by Claude at Ratul's request (Phases 7-8, D-036).
//
// Fixed-width integers are written byte by byte with shifts, never with memcpy of a host integer,
// so the format is the same on every machine whatever its byte order. Strings are a u32 length
// followed by the bytes.
class ByteWriter {
 public:
  void U8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
  void U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
  void Bool(bool v) { U8(v ? 1 : 0); }
  void Bytes(std::string_view s) {
    U32(static_cast<uint32_t>(s.size()));
    buf_.append(s.data(), s.size());
  }

  const std::string& buffer() const noexcept { return buf_; }
  std::string Take() { return std::move(buf_); }
  size_t size() const noexcept { return buf_.size(); }

 private:
  std::string buf_;
};

// Every read is bounds-checked: on malformed input it returns false and the reader stays failed,
// so a decoder can read all fields and check ok() once at the end. Bytes() returns a view into
// the input, valid as long as the input is.
class ByteReader {
 public:
  explicit ByteReader(std::string_view data) : data_(data) {}

  bool U8(uint8_t* v) {
    if (!Need(1)) return false;
    *v = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }
  bool U32(uint32_t* v) {
    if (!Need(4)) return false;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= static_cast<uint32_t>(Byte(pos_ + static_cast<size_t>(i))) << (8 * i);
    pos_ += 4;
    *v = r;
    return true;
  }
  bool U64(uint64_t* v) {
    if (!Need(8)) return false;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= static_cast<uint64_t>(Byte(pos_ + static_cast<size_t>(i))) << (8 * i);
    pos_ += 8;
    *v = r;
    return true;
  }
  bool Bool(bool* v) {
    uint8_t b = 0;
    if (!U8(&b)) return false;
    if (b > 1) return Fail();
    *v = b == 1;
    return true;
  }
  bool Bytes(std::string_view* v) {
    uint32_t n = 0;
    if (!U32(&n) || !Need(n)) return false;
    *v = data_.substr(pos_, n);
    pos_ += n;
    return true;
  }

  bool ok() const noexcept { return ok_; }
  bool done() const noexcept { return ok_ && pos_ == data_.size(); }
  size_t remaining() const noexcept { return data_.size() - pos_; }

 private:
  bool Need(size_t n) {
    if (!ok_ || data_.size() - pos_ < n) return Fail();
    return true;
  }
  bool Fail() {
    ok_ = false;
    return false;
  }
  uint8_t Byte(size_t i) const { return static_cast<uint8_t>(data_[i]); }

  std::string_view data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace lldstore
