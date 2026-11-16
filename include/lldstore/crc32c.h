#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lldstore {

// CRC-32C (Castagnoli polynomial), the checksum used by ext4, iSCSI, LevelDB and RocksDB.
// Written by Claude at Ratul's request (Phase 7, D-037).
//
// Table-driven, one byte at a time: about 1-2 GB/s, far faster than the fsync it protects.
// SSE4.2 and ARMv8 have a CRC32C instruction that is roughly 10x faster; not used, to keep this
// portable and simple, and because the log is bound by the disk, not by the checksum (unmeasured).
namespace detail {
constexpr std::array<uint32_t, 256> MakeCrc32cTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;  // reflected poly
    table[i] = c;
  }
  return table;
}
inline constexpr std::array<uint32_t, 256> kCrc32cTable = MakeCrc32cTable();
}  // namespace detail

// `crc` lets a checksum be extended over several pieces: Crc32c(b, Crc32c(a)) == Crc32c(a + b).
constexpr uint32_t Crc32c(std::string_view data, uint32_t crc = 0) noexcept {
  crc = ~crc;
  for (char ch : data) {
    crc = detail::kCrc32cTable[(crc ^ static_cast<uint8_t>(ch)) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

static_assert(Crc32c("123456789") == 0xE3069283u, "CRC-32C check value");

}  // namespace lldstore
