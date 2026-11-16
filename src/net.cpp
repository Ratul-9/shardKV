#include "lldstore/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>

namespace lldstore::net {
namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // Linux: no SIGPIPE when the peer has gone
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set per socket instead
#endif

bool Resolve(const HostPort& where, sockaddr_storage* addr, socklen_t* len) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(where.port);
  if (::getaddrinfo(where.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) return false;
  std::memcpy(addr, res->ai_addr, res->ai_addrlen);
  *len = static_cast<socklen_t>(res->ai_addrlen);
  ::freeaddrinfo(res);
  return true;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Waits until `fd` is readable. Returns 1 ready, 0 timeout, -1 error.
int WaitReadable(int fd, int64_t deadline_ms) {
  for (;;) {
    int timeout = -1;
    if (deadline_ms >= 0) {
      const int64_t left = deadline_ms - NowMs();
      if (left <= 0) return 0;
      timeout = static_cast<int>(left);
    }
    pollfd p{fd, POLLIN, 0};
    const int rc = ::poll(&p, 1, timeout);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) return -1;
    return rc == 0 ? 0 : 1;
  }
}

RecvStatus ReadExactly(int fd, char* buf, size_t n, int64_t deadline_ms) {
  size_t got = 0;
  while (got < n) {
    const int ready = WaitReadable(fd, deadline_ms);
    if (ready == 0) return RecvStatus::kTimeout;
    if (ready < 0) return RecvStatus::kError;
    const ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r == 0) return RecvStatus::kClosed;
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      return RecvStatus::kError;
    }
    got += static_cast<size_t>(r);
  }
  return RecvStatus::kOk;
}

}  // namespace

bool ParseHostPort(std::string_view text, HostPort* out) {
  const size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) return false;
  unsigned long port = 0;
  for (char c : text.substr(colon + 1)) {
    if (c < '0' || c > '9') return false;
    port = port * 10 + static_cast<unsigned long>(c - '0');
    if (port > 65535) return false;
  }
  out->host = std::string(text.substr(0, colon));
  out->port = static_cast<uint16_t>(port);
  return true;
}

int Listen(const HostPort& where, int backlog) {
  sockaddr_storage addr{};
  socklen_t len = 0;
  if (!Resolve(where, &addr, &len)) {
    throw std::system_error(EINVAL, std::generic_category(), "resolve " + where.ToString());
  }
  const int fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(fd, backlog) != 0) {
    const int err = errno;
    ::close(fd);
    throw std::system_error(err, std::generic_category(), "bind/listen " + where.ToString());
  }
  return fd;
}

int Accept(int listen_fd) {
  for (;;) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd >= 0) return fd;
    if (errno == EINTR || errno == ECONNABORTED) continue;
    return -1;  // the listener was shut down
  }
}

int Connect(const HostPort& where, int timeout_ms) {
  sockaddr_storage addr{};
  socklen_t len = 0;
  if (!Resolve(where, &addr, &len)) return -1;
  const int fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  // Non-blocking connect so the timeout is ours, not the kernel's (which can be minutes).
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), len);
  if (rc != 0 && errno == EINPROGRESS) {
    pollfd p{fd, POLLOUT, 0};
    rc = ::poll(&p, 1, timeout_ms) == 1 ? 0 : -1;
    int err = 0;
    socklen_t err_len = sizeof(err);
    if (rc == 0 && (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0)) rc = -1;
  }
  if (rc != 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFL, flags);
  return fd;
}

void ConfigureConnection(int fd, int send_timeout_ms) {
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  timeval tv{};
  tv.tv_sec = send_timeout_ms / 1000;
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>((send_timeout_ms % 1000) * 1000);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool SendFrame(int fd, std::string_view payload) {
  if (payload.size() > kMaxFrame) return false;
  std::string frame;
  frame.reserve(4 + payload.size());
  const auto n = static_cast<uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
  frame.append(payload.data(), payload.size());
  size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t r = ::send(fd, frame.data() + sent, frame.size() - sent, kSendFlags);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;  // includes EAGAIN after SO_SNDTIMEO: the peer is not reading
    }
    sent += static_cast<size_t>(r);
  }
  return true;
}

RecvStatus RecvFrame(int fd, std::string* payload, int timeout_ms) {
  const int64_t deadline = timeout_ms < 0 ? -1 : NowMs() + timeout_ms;
  char header[4];
  RecvStatus s = ReadExactly(fd, header, 4, deadline);
  if (s != RecvStatus::kOk) return s;
  uint32_t n = 0;
  for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<uint8_t>(header[i])) << (8 * i);
  if (n > kMaxFrame) return RecvStatus::kError;
  payload->resize(n);
  return n == 0 ? RecvStatus::kOk : ReadExactly(fd, payload->data(), n, deadline);
}

void Close(int fd) {
  if (fd >= 0) ::close(fd);
}

void Shutdown(int fd) {
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

}  // namespace lldstore::net
