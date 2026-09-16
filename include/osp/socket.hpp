/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file socket.hpp
 * @brief POSIX socket RAII abstractions for ARM-Linux embedded systems.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Provides TcpSocket, UdpSocket, TcpListener with RAII fd ownership,
 * and SocketAddress as a thin wrapper around sockaddr_in / sockaddr_in6.
 * All errors are returned via osp::expected<V,E>.
 */

#ifndef OSP_SOCKET_HPP_
#define OSP_SOCKET_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#if OSP_HAS_NETWORK

#include <cerrno>
#include <cstring>

#if OSP_NET_BACKEND == 1
// RT-Thread SAL / lwIP socket API. LWIP_COMPAT_SOCKETS must be 0 (its lwip
// default is 1, which would define read/write/close macros that clobber C++
// stdlib headers). Calls go through socket_api below, never through macros.
#include <lwip/sockets.h>
#elif OSP_NET_BACKEND == 3
// Winsock. CMake defines WIN32_LEAN_AND_MEAN; guard it here too so direct cl
// builds (no CMake) stay consistent. winsock2.h must precede any windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>  // sockaddr_un for the (non-functional on Winsock) AF_UNIX surface
// POSIX shutdown() how constants; Winsock uses SD_* (same numeric values).
// Provided so modules can call socket_api::Shutdown(fd, SHUT_RDWR) uniformly.
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace osp {

// ============================================================================
// Constants
// ============================================================================

constexpr int32_t kDefaultBacklog = 128;

// Flags passed to ::send() on stream sockets. MSG_NOSIGNAL suppresses SIGPIPE
// (Linux extension). Note lwIP also defines MSG_NOSIGNAL (0x20) but does not
// implement it (lwip_send only acts on MSG_MORE/MSG_DONTWAIT), so the value is
// passed through and silently ignored there. On targets that define neither,
// fall back to 0.
#ifdef MSG_NOSIGNAL
inline constexpr int32_t kSendNoSignal = static_cast<int32_t>(MSG_NOSIGNAL);
#else
inline constexpr int32_t kSendNoSignal = 0;
#endif

// ============================================================================
// Backend-neutral socket descriptor / poll descriptor types.
// POSIX fds are int; Winsock SOCKET handles are UINT_PTR (64-bit on Win64).
// INVALID_SOCKET maps to -1 when stored in SocketHandle, so `fd < 0` remains
// the invalid-handle test on every backend.
// ============================================================================

#if OSP_NET_BACKEND == 3
using SocketHandle = intptr_t;
using PollFd = WSAPOLLFD;  // struct pollfd alias whose fd is a SOCKET
#else
using SocketHandle = int32_t;
using PollFd = struct pollfd;
#endif

// ============================================================================
// socket_api -- backend-neutral socket call layer.
// The lwIP backend calls lwip_* directly; the POSIX backend calls the system
// functions. No macros involved, so C++ stdlib headers stay clean.
// ============================================================================

namespace socket_api {

#if OSP_NET_BACKEND == 3
// One-time WSAStartup before the first socket call. Static-local guard keeps
// it idempotent and -fno-exceptions safe. Winsock requires a successful
// WSAStartup (version 2.2) per process before any socket call.
inline bool EnsureWsaStarted() noexcept {
  static const bool started = []() noexcept {
    WSADATA wsa_data;
    return WSAStartup(0x0202, &wsa_data) == 0;
  }();
  return started;
}
#endif

inline SocketHandle Socket(int domain, int type, int protocol) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<SocketHandle>(lwip_socket(domain, type, protocol));
#elif OSP_NET_BACKEND == 3
  if (!EnsureWsaStarted()) {
    return static_cast<SocketHandle>(INVALID_SOCKET);
  }
  return static_cast<SocketHandle>(::socket(domain, type, protocol));
#else
  return static_cast<SocketHandle>(::socket(domain, type, protocol));
#endif
}

inline int Bind(SocketHandle fd, const sockaddr* addr, socklen_t len) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_bind(static_cast<int>(fd), addr, len);
#elif OSP_NET_BACKEND == 3
  return ::bind(static_cast<SOCKET>(fd), addr, static_cast<int>(len));
#else
  return ::bind(fd, addr, len);
#endif
}

inline int Listen(SocketHandle fd, int backlog) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_listen(static_cast<int>(fd), backlog);
#elif OSP_NET_BACKEND == 3
  return ::listen(static_cast<SOCKET>(fd), backlog);
#else
  return ::listen(fd, backlog);
#endif
}

inline SocketHandle Accept(SocketHandle fd, sockaddr* addr, socklen_t* len) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<SocketHandle>(lwip_accept(static_cast<int>(fd), addr, len));
#elif OSP_NET_BACKEND == 3
  return static_cast<SocketHandle>(::accept(static_cast<SOCKET>(fd), addr, len));
#else
  return static_cast<SocketHandle>(::accept(fd, addr, len));
#endif
}

inline int Connect(SocketHandle fd, const sockaddr* addr, socklen_t len) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_connect(static_cast<int>(fd), addr, len);
#elif OSP_NET_BACKEND == 3
  return ::connect(static_cast<SOCKET>(fd), addr, static_cast<int>(len));
#else
  return ::connect(fd, addr, len);
#endif
}

inline int Send(SocketHandle fd, const void* data, size_t len, int flags) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<int>(lwip_send(static_cast<int>(fd), data, len, flags));
#elif OSP_NET_BACKEND == 3
  return ::send(static_cast<SOCKET>(fd), static_cast<const char*>(data), static_cast<int>(len), flags);
#else
  return static_cast<int>(::send(fd, data, len, flags));
#endif
}

inline int Recv(SocketHandle fd, void* buf, size_t len, int flags) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<int>(lwip_recv(static_cast<int>(fd), buf, len, flags));
#elif OSP_NET_BACKEND == 3
  return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(len), flags);
#else
  return static_cast<int>(::recv(fd, buf, len, flags));
#endif
}

inline int SendTo(SocketHandle fd, const void* data, size_t len, int flags, const sockaddr* dest, socklen_t dest_len) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<int>(lwip_sendto(static_cast<int>(fd), data, len, flags, dest, dest_len));
#elif OSP_NET_BACKEND == 3
  return ::sendto(static_cast<SOCKET>(fd), static_cast<const char*>(data), static_cast<int>(len), flags, dest, static_cast<int>(dest_len));
#else
  return static_cast<int>(::sendto(fd, data, len, flags, dest, dest_len));
#endif
}

inline int RecvFrom(SocketHandle fd, void* buf, size_t len, int flags, sockaddr* src, socklen_t* src_len) noexcept {
#if OSP_NET_BACKEND == 1
  return static_cast<int>(lwip_recvfrom(static_cast<int>(fd), buf, len, flags, src, src_len));
#elif OSP_NET_BACKEND == 3
  return ::recvfrom(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(len), flags, src, src_len);
#else
  return static_cast<int>(::recvfrom(fd, buf, len, flags, src, src_len));
#endif
}

inline int Close(SocketHandle fd) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_close(static_cast<int>(fd));
#elif OSP_NET_BACKEND == 3
  return ::closesocket(static_cast<SOCKET>(fd));
#else
  return ::close(fd);
#endif
}

inline int SetSockOpt(SocketHandle fd, int level, int optname, const void* optval, socklen_t optlen) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_setsockopt(static_cast<int>(fd), level, optname, optval, optlen);
#elif OSP_NET_BACKEND == 3
  return ::setsockopt(static_cast<SOCKET>(fd), level, optname, static_cast<const char*>(optval), static_cast<int>(optlen));
#else
  return ::setsockopt(fd, level, optname, optval, optlen);
#endif
}

inline int GetSockOpt(SocketHandle fd, int level, int optname, void* optval, socklen_t* optlen) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_getsockopt(static_cast<int>(fd), level, optname, optval, optlen);
#elif OSP_NET_BACKEND == 3
  return ::getsockopt(static_cast<SOCKET>(fd), level, optname, static_cast<char*>(optval), optlen);
#else
  return ::getsockopt(fd, level, optname, optval, optlen);
#endif
}

inline int ParseIpv4(const char* ip, void* out) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_inet_pton(AF_INET, ip, out);
#else
  return ::inet_pton(AF_INET, ip, out);
#endif
}

inline uint16_t Htons(uint16_t v) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_htons(v);
#else
  return ::htons(v);
#endif
}

inline uint16_t Ntohs(uint16_t v) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_ntohs(v);
#else
  return ::ntohs(v);
#endif
}

inline uint32_t Htonl(uint32_t v) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_htonl(v);
#else
  return ::htonl(v);
#endif
}

inline uint32_t Ntohl(uint32_t v) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_ntohl(v);
#else
  return ::ntohl(v);
#endif
}

inline int Shutdown(SocketHandle fd, int32_t how) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_shutdown(static_cast<int>(fd), how);
#elif OSP_NET_BACKEND == 3
  return ::shutdown(static_cast<SOCKET>(fd), how);
#else
  return ::shutdown(fd, how);
#endif
}

inline int GetSockName(SocketHandle fd, sockaddr* addr, socklen_t* len) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_getsockname(static_cast<int>(fd), addr, len);
#elif OSP_NET_BACKEND == 3
  return ::getsockname(static_cast<SOCKET>(fd), addr, len);
#else
  return ::getsockname(fd, addr, len);
#endif
}

inline int GetPeerName(SocketHandle fd, sockaddr* addr, socklen_t* len) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_getpeername(static_cast<int>(fd), addr, len);
#elif OSP_NET_BACKEND == 3
  return ::getpeername(static_cast<SOCKET>(fd), addr, len);
#else
  return ::getpeername(fd, addr, len);
#endif
}

inline const char* InetNtop(int32_t af, const void* src, char* dst, socklen_t size) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_inet_ntop(af, src, dst, size);
#else
  return ::inet_ntop(af, src, dst, size);
#endif
}

// Select on socket file descriptors. Uses the backend's own fd_set/timeval
// types. Returns the select() result (>0 ready, 0 timeout, -1 error).
inline int Select(int nfds, fd_set* readset, fd_set* writeset, fd_set* exceptset, timeval* timeout) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_select(nfds, readset, writeset, exceptset, timeout);
#elif OSP_NET_BACKEND == 3
  // Winsock ignores nfds; pass 0 explicitly to document that.
  return ::select(0, readset, writeset, exceptset, timeout);
#else
  return ::select(nfds, readset, writeset, exceptset, timeout);
#endif
}

// Poll on socket descriptors. On Winsock this is WSAPoll (PollFd is WSAPOLLFD
// whose fd is a SOCKET); on POSIX/lwIP it is poll() over struct pollfd.
inline int Poll(PollFd* fds, uint32_t nfds, int32_t timeout_ms) noexcept {
#if OSP_NET_BACKEND == 1
  return lwip_poll(fds, static_cast<unsigned long>(nfds), timeout_ms);
#elif OSP_NET_BACKEND == 3
  return ::WSAPoll(fds, static_cast<ULONG>(nfds), timeout_ms);
#else
  return ::poll(fds, static_cast<nfds_t>(nfds), timeout_ms);
#endif
}

// Report whether the last Send/Recv-style call failed with "would block".
// Winsock surfaces it via WSAGetLastError() == WSAEWOULDBLOCK instead of errno.
inline bool IsWouldBlock() noexcept {
#if OSP_NET_BACKEND == 1
  return errno == EAGAIN || errno == EWOULDBLOCK;
#elif OSP_NET_BACKEND == 3
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

}  // namespace socket_api

// Parse dotted-decimal IPv4 text into *out (an in_addr). Returns 1 on success,
// 0 on invalid text.
inline int ParseIpv4Text(const char* ip, void* out) noexcept {
  return socket_api::ParseIpv4(ip, out);
}

// Set or clear non-blocking mode on a socket handle. Returns 0 on success,
// -1 on failure.
inline int SetFdNonBlocking(SocketHandle fd, bool enable) noexcept {
#if OSP_NET_BACKEND == 1
  // FIONBIO expects an int* arg; lwip_ioctl reads it as *(int*)argp. Using int
  // (not unsigned long) keeps the value correct on big-endian targets.
  int mode = enable ? 1 : 0;
  return lwip_ioctl(static_cast<int>(fd), FIONBIO, &mode);
#elif OSP_NET_BACKEND == 3
  u_long mode = enable ? 1UL : 0UL;
  return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
#else
  int32_t flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, flags);
#endif
}

// ============================================================================
// SocketError
// ============================================================================

enum class SocketError : uint8_t {
  kInvalidFd = 0,
  kBindFailed,
  kListenFailed,
  kConnectFailed,
  kSendFailed,
  kRecvFailed,
  kAcceptFailed,
  kAlreadyClosed,
  kSetOptFailed,
  kPathTooLong,
  kWouldBlock,    ///< EAGAIN/EWOULDBLOCK (or WSAEWOULDBLOCK) -- transient, caller may retry.
  kNotSupported   ///< Operation not available on this backend/platform (e.g. AF_UNIX on Winsock).
};

// ============================================================================
// SocketAddress
// ============================================================================

/**
 * @brief Simple wrapper for sockaddr_in / sockaddr_in6.
 *
 * Currently supports IPv4 only via the FromIpv4 factory.
 */
class SocketAddress {
 public:
  SocketAddress() noexcept { std::memset(&addr_, 0, sizeof(addr_)); }

  /**
   * @brief Create an IPv4 socket address from a dotted-decimal string and port.
   *
   * @param ip   Dotted-decimal IPv4 string (e.g. "127.0.0.1")
   * @param port Port number in host byte order
   * @return expected<SocketAddress, SocketError> on success; kInvalidFd on bad ip
   */
  static expected<SocketAddress, SocketError> FromIpv4(const char* ip, uint16_t port) noexcept {
    SocketAddress sa;
    sa.addr_.sin_family = AF_INET;
    sa.addr_.sin_port = socket_api::Htons(port);
    if (ParseIpv4Text(ip, &sa.addr_.sin_addr) != 1) {
      return expected<SocketAddress, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<SocketAddress, SocketError>::success(sa);
  }

  /** @brief Raw pointer to the underlying sockaddr structure. */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- MISRA 5-2-8 deviation: POSIX sockaddr cast
  const sockaddr* Raw() const noexcept { return reinterpret_cast<const sockaddr*>(&addr_); }

  /** @brief Mutable raw pointer (used internally by Accept/RecvFrom). */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- MISRA 5-2-8 deviation: POSIX sockaddr cast
  sockaddr* RawMut() noexcept { return reinterpret_cast<sockaddr*>(&addr_); }

  /** @brief Size of the underlying sockaddr_in structure. */
  socklen_t Size() const noexcept { return static_cast<socklen_t>(sizeof(addr_)); }

  /** @brief Return the port in host byte order. */
  uint16_t Port() const noexcept { return socket_api::Ntohs(addr_.sin_port); }

 private:
  sockaddr_in addr_;
};

// Forward declaration so TcpListener::Accept can construct TcpSocket from fd.
class TcpSocket;

// ============================================================================
// TcpSocket
// ============================================================================

/**
 * @brief RAII TCP stream socket.
 *
 * Owns a file descriptor. Movable but not copyable.
 * On destruction (or explicit Close()), the fd is closed.
 */
class TcpSocket {
 public:
  TcpSocket() noexcept : fd_(-1) {}

  ~TcpSocket() { Close(); }

  // Move-only ---------------------------------------------------------------
  TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  TcpSocket& operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  // Factory -----------------------------------------------------------------

  /**
   * @brief Create a TCP (SOCK_STREAM) socket.
   * @return TcpSocket on success, SocketError::kInvalidFd on failure.
   */
  static expected<TcpSocket, SocketError> Create() noexcept {
    SocketHandle fd = socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<TcpSocket, SocketError>::success(TcpSocket(fd));
  }

  // Operations --------------------------------------------------------------

  expected<void, SocketError> Connect(const SocketAddress& addr) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Connect(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kConnectFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<int32_t, SocketError> Send(const void* data, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = socket_api::Send(fd_, data, len, kSendNoSignal);
    if (n < 0) {
      if (socket_api::IsWouldBlock()) {
        return expected<int32_t, SocketError>::error(SocketError::kWouldBlock);
      }
      return expected<int32_t, SocketError>::error(SocketError::kSendFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<int32_t, SocketError> Recv(void* buf, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = socket_api::Recv(fd_, buf, len, 0);
    if (n < 0) {
      if (socket_api::IsWouldBlock()) {
        return expected<int32_t, SocketError>::error(SocketError::kWouldBlock);
      }
      return expected<int32_t, SocketError>::error(SocketError::kRecvFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<void, SocketError> SetNonBlocking(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (SetFdNonBlocking(fd_, enable) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> SetReuseAddr(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    int32_t opt = enable ? 1 : 0;
    if (socket_api::SetSockOpt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof(opt))) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> SetNoDelay(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    int32_t opt = enable ? 1 : 0;
    if (socket_api::SetSockOpt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, static_cast<socklen_t>(sizeof(opt))) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  /** @brief Close the socket. Idempotent - safe to call multiple times. */
  void Close() noexcept {
    if (fd_ >= 0) {
      socket_api::Close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  SocketHandle Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  friend class TcpListener;

  /** @brief Construct from an already-open file descriptor (used by Accept). */
  explicit TcpSocket(SocketHandle fd) noexcept : fd_(fd) {}

  SocketHandle fd_;
};

// ============================================================================
// UdpSocket
// ============================================================================

/**
 * @brief RAII UDP datagram socket.
 *
 * Owns a file descriptor. Movable but not copyable.
 */
class UdpSocket {
 public:
  UdpSocket() noexcept : fd_(-1) {}

  ~UdpSocket() { Close(); }

  // Move-only ---------------------------------------------------------------
  UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  UdpSocket& operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Factory -----------------------------------------------------------------

  /**
   * @brief Create a UDP (SOCK_DGRAM) socket.
   * @return UdpSocket on success, SocketError::kInvalidFd on failure.
   */
  static expected<UdpSocket, SocketError> Create() noexcept {
    SocketHandle fd = socket_api::Socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      return expected<UdpSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<UdpSocket, SocketError>::success(UdpSocket(fd));
  }

  // Operations --------------------------------------------------------------

  expected<void, SocketError> Bind(const SocketAddress& addr) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Bind(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kBindFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<int32_t, SocketError> SendTo(const void* data, size_t len, const SocketAddress& dest) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = socket_api::SendTo(fd_, data, len, 0, dest.Raw(), dest.Size());
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kSendFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<int32_t, SocketError> RecvFrom(void* buf, size_t len, SocketAddress& src) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    socklen_t addr_len = src.Size();
    auto n = socket_api::RecvFrom(fd_, buf, len, 0, src.RawMut(), &addr_len);
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kRecvFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  /** @brief Close the socket. Idempotent. */
  void Close() noexcept {
    if (fd_ >= 0) {
      socket_api::Close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  SocketHandle Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  explicit UdpSocket(SocketHandle fd) noexcept : fd_(fd) {}

  SocketHandle fd_;
};

// ============================================================================
// TcpListener
// ============================================================================

/**
 * @brief RAII TCP listener (server) socket.
 *
 * Binds to an address, listens for incoming connections, and accepts them
 * as TcpSocket instances.
 */
class TcpListener {
 public:
  TcpListener() noexcept : fd_(-1) {}

  ~TcpListener() { Close(); }

  // Move-only ---------------------------------------------------------------
  TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  TcpListener& operator=(TcpListener&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Factory -----------------------------------------------------------------

  /**
   * @brief Create a TCP listener socket.
   * @return TcpListener on success, SocketError::kInvalidFd on failure.
   */
  static expected<TcpListener, SocketError> Create() noexcept {
    SocketHandle fd = socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return expected<TcpListener, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<TcpListener, SocketError>::success(TcpListener(fd));
  }

  // Operations --------------------------------------------------------------

  expected<void, SocketError> Bind(const SocketAddress& addr) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Bind(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kBindFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> Listen(int32_t backlog = kDefaultBacklog) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Listen(fd_, backlog) < 0) {
      return expected<void, SocketError>::error(SocketError::kListenFailed);
    }
    return expected<void, SocketError>::success();
  }

  /**
   * @brief Accept an incoming connection.
   * @return A connected TcpSocket on success.
   */
  expected<TcpSocket, SocketError> Accept() noexcept {
    if (fd_ < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    SocketHandle client_fd = socket_api::Accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kAcceptFailed);
    }
    return expected<TcpSocket, SocketError>::success(TcpSocket(client_fd));
  }

  /**
   * @brief Accept an incoming connection and fill the client address.
   * @param[out] client_addr Filled with the connecting peer's address.
   * @return A connected TcpSocket on success.
   */
  expected<TcpSocket, SocketError> Accept(SocketAddress& client_addr) noexcept {
    if (fd_ < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    socklen_t addr_len = client_addr.Size();
    SocketHandle client_fd = socket_api::Accept(fd_, client_addr.RawMut(), &addr_len);
    if (client_fd < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kAcceptFailed);
    }
    return expected<TcpSocket, SocketError>::success(TcpSocket(client_fd));
  }

  /** @brief Close the listener socket. Idempotent. */
  void Close() noexcept {
    if (fd_ >= 0) {
      socket_api::Close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  SocketHandle Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  explicit TcpListener(SocketHandle fd) noexcept : fd_(fd) {}

  SocketHandle fd_;
};

// ============================================================================
// UnixAddress
// ============================================================================

#if OSP_NET_BACKEND == 0 || OSP_NET_BACKEND == 3
/**
 * @brief Wrapper for Unix Domain Socket address (sockaddr_un).
 *
 * Provides a type-safe interface for Unix socket paths.
 */
class UnixAddress {
 public:
  UnixAddress() noexcept {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sun_family = AF_UNIX;
  }

  /**
   * @brief Create a Unix socket address from a filesystem path.
   *
   * @param path Filesystem path for the Unix socket
   * @return expected<UnixAddress, SocketError> on success; kPathTooLong if path exceeds limit
   */
  static expected<UnixAddress, SocketError> FromPath(const char* path) noexcept {
    UnixAddress ua;
    size_t len = std::strlen(path);
    if (len >= sizeof(ua.addr_.sun_path)) {
      return expected<UnixAddress, SocketError>::error(SocketError::kPathTooLong);
    }
    std::memcpy(ua.addr_.sun_path, path, len + 1);
    return expected<UnixAddress, SocketError>::success(ua);
  }

  /** @brief Return the socket path. */
  const char* Path() const noexcept { return addr_.sun_path; }

  /** @brief Raw pointer to the underlying sockaddr structure. */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- MISRA 5-2-8 deviation: POSIX sockaddr cast
  const sockaddr* Raw() const noexcept { return reinterpret_cast<const sockaddr*>(&addr_); }

  /** @brief Mutable raw pointer (used internally by Accept). */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- MISRA 5-2-8 deviation: POSIX sockaddr cast
  sockaddr* RawMut() noexcept { return reinterpret_cast<sockaddr*>(&addr_); }

  /** @brief Size of the underlying sockaddr_un structure. */
  socklen_t Size() const noexcept { return static_cast<socklen_t>(sizeof(addr_)); }

 private:
  sockaddr_un addr_;
};

// Forward declaration so UnixListener::Accept can construct UnixSocket from fd.
class UnixSocket;

// ============================================================================
// UnixSocket
// ============================================================================

/**
 * @brief RAII Unix Domain Socket stream socket.
 *
 * Owns a file descriptor. Movable but not copyable.
 * On destruction (or explicit Close()), the fd is closed.
 */
class UnixSocket {
 public:
  UnixSocket() noexcept : fd_(-1) {}

  ~UnixSocket() { Close(); }

  // Move-only ---------------------------------------------------------------
  UnixSocket(UnixSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  UnixSocket& operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  UnixSocket(const UnixSocket&) = delete;
  UnixSocket& operator=(const UnixSocket&) = delete;

  // Factory -----------------------------------------------------------------

  /**
   * @brief Create a Unix Domain Socket (SOCK_STREAM).
   * @return UnixSocket on success, SocketError::kInvalidFd on failure.
   */
  static expected<UnixSocket, SocketError> Create() noexcept {
    SocketHandle fd = socket_api::Socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return expected<UnixSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<UnixSocket, SocketError>::success(UnixSocket(fd));
  }

  // Operations --------------------------------------------------------------

  expected<void, SocketError> Connect(const UnixAddress& addr) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Connect(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kConnectFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<int32_t, SocketError> Send(const void* data, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = socket_api::Send(fd_, data, len, kSendNoSignal);
    if (n < 0) {
      if (socket_api::IsWouldBlock()) {
        return expected<int32_t, SocketError>::error(SocketError::kWouldBlock);
      }
      return expected<int32_t, SocketError>::error(SocketError::kSendFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<int32_t, SocketError> Recv(void* buf, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = socket_api::Recv(fd_, buf, len, 0);
    if (n < 0) {
      if (socket_api::IsWouldBlock()) {
        return expected<int32_t, SocketError>::error(SocketError::kWouldBlock);
      }
      return expected<int32_t, SocketError>::error(SocketError::kRecvFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<void, SocketError> SetNonBlocking(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (SetFdNonBlocking(fd_, enable) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  /** @brief Close the socket. Idempotent - safe to call multiple times. */
  void Close() noexcept {
    if (fd_ >= 0) {
      socket_api::Close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  SocketHandle Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  friend class UnixListener;

  /** @brief Construct from an already-open file descriptor (used by Accept). */
  explicit UnixSocket(SocketHandle fd) noexcept : fd_(fd) {}

  SocketHandle fd_;
};

// ============================================================================
// UnixListener
// ============================================================================

/**
 * @brief RAII Unix Domain Socket listener (server) socket.
 *
 * Binds to a filesystem path, listens for incoming connections, and accepts them
 * as UnixSocket instances.
 */
class UnixListener {
 public:
  UnixListener() noexcept : fd_(-1) {}

  ~UnixListener() { Close(); }

  // Move-only ---------------------------------------------------------------
  UnixListener(UnixListener&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  UnixListener& operator=(UnixListener&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  UnixListener(const UnixListener&) = delete;
  UnixListener& operator=(const UnixListener&) = delete;

  // Factory -----------------------------------------------------------------

  /**
   * @brief Create a Unix Domain Socket listener.
   * @return UnixListener on success, SocketError::kInvalidFd on failure.
   */
  static expected<UnixListener, SocketError> Create() noexcept {
    SocketHandle fd = socket_api::Socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return expected<UnixListener, SocketError>::error(SocketError::kInvalidFd);
    }
    return expected<UnixListener, SocketError>::success(UnixListener(fd));
  }

  // Operations --------------------------------------------------------------

  expected<void, SocketError> Bind(const UnixAddress& addr) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
#if OSP_NET_BACKEND == 3
    // Winsock has no ::unlink and AF_UNIX path semantics differ; reject.
    (void)addr;
    return expected<void, SocketError>::error(SocketError::kNotSupported);
#else
    ::unlink(addr.Path());  // Remove stale socket file
    if (socket_api::Bind(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kBindFailed);
    }
    return expected<void, SocketError>::success();
#endif
  }

  expected<void, SocketError> Listen(int32_t backlog = kDefaultBacklog) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (socket_api::Listen(fd_, backlog) < 0) {
      return expected<void, SocketError>::error(SocketError::kListenFailed);
    }
    return expected<void, SocketError>::success();
  }

  /**
   * @brief Accept an incoming connection.
   * @return A connected UnixSocket on success.
   */
  expected<UnixSocket, SocketError> Accept() noexcept {
    if (fd_ < 0) {
      return expected<UnixSocket, SocketError>::error(SocketError::kInvalidFd);
    }
    SocketHandle client_fd = socket_api::Accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
      return expected<UnixSocket, SocketError>::error(SocketError::kAcceptFailed);
    }
    return expected<UnixSocket, SocketError>::success(UnixSocket(client_fd));
  }

  /** @brief Close the listener socket. Idempotent. */
  void Close() noexcept {
    if (fd_ >= 0) {
      socket_api::Close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  SocketHandle Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  explicit UnixListener(SocketHandle fd) noexcept : fd_(fd) {}

  SocketHandle fd_;
};

#endif  // OSP_NET_BACKEND == 0 || OSP_NET_BACKEND == 3 (AF_UNIX unsupported by lwIP/Winsock)

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_SOCKET_HPP_
