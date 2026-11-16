#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lldstore::net {

// Phase 8: minimal blocking TCP helpers for the cluster (POSIX sockets, Linux and macOS).
// Written by Claude at Ratul's request. Design: docs/DESIGN.md D-053.
//
// Framing: every message is a u32 little-endian length followed by that many bytes. TCP is a byte
// stream with no message boundaries, so without a length prefix the reader cannot tell where one
// message ends. The length is capped so a corrupt or hostile peer cannot make us allocate 4 GiB.

struct HostPort {
  std::string host;
  uint16_t port = 0;
  std::string ToString() const { return host + ":" + std::to_string(port); }
};

bool ParseHostPort(std::string_view text, HostPort* out);

// Throws std::system_error. SO_REUSEADDR is set so a restarted node can rebind at once.
int Listen(const HostPort& where, int backlog = 128);
// Returns -1 on failure or timeout.
int Accept(int listen_fd);
int Connect(const HostPort& where, int timeout_ms);

// Disables Nagle's algorithm (small request/response messages must not wait to be batched) and
// SIGPIPE on macOS; bounds a blocking send so a stuck peer cannot block the sender forever.
void ConfigureConnection(int fd, int send_timeout_ms);

constexpr uint32_t kMaxFrame = 64u << 20;

bool SendFrame(int fd, std::string_view payload);

enum class RecvStatus { kOk, kClosed, kTimeout, kError };
// timeout_ms < 0 waits forever. The timeout applies to the whole frame.
RecvStatus RecvFrame(int fd, std::string* payload, int timeout_ms);

void Close(int fd);
// Wakes any thread blocked reading or writing `fd` (used to stop reader threads).
void Shutdown(int fd);

}  // namespace lldstore::net
