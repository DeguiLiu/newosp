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

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace osp {

// ============================================================================
// Constants
// ============================================================================

constexpr int32_t kDefaultBacklog = 128;

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
  kSetOptFailed
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
  static expected<SocketAddress, SocketError> FromIpv4(const char* ip,
                                                       uint16_t port) noexcept {
    SocketAddress sa;
    sa.addr_.sin_family = AF_INET;
    sa.addr_.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &sa.addr_.sin_addr) != 1) {
      return expected<SocketAddress, SocketError>::error(
          SocketError::kInvalidFd);
    }
    return expected<SocketAddress, SocketError>::success(sa);
  }

  /** @brief Raw pointer to the underlying sockaddr structure. */
  const sockaddr* Raw() const noexcept {
    return reinterpret_cast<const sockaddr*>(&addr_);
  }

  /** @brief Mutable raw pointer (used internally by Accept/RecvFrom). */
  sockaddr* RawMut() noexcept {
    return reinterpret_cast<sockaddr*>(&addr_);
  }

  /** @brief Size of the underlying sockaddr_in structure. */
  socklen_t Size() const noexcept {
    return static_cast<socklen_t>(sizeof(addr_));
  }

  /** @brief Return the port in host byte order. */
  uint16_t Port() const noexcept { return ntohs(addr_.sin_port); }

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
  TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

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
    int32_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    if (::connect(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kConnectFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<int32_t, SocketError> Send(const void* data, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = ::send(fd_, data, len, MSG_NOSIGNAL);
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kSendFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<int32_t, SocketError> Recv(void* buf, size_t len) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = ::recv(fd_, buf, len, 0);
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kRecvFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<void, SocketError> SetNonBlocking(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    int32_t flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    if (enable) {
      flags |= O_NONBLOCK;
    } else {
      flags &= ~O_NONBLOCK;
    }
    if (::fcntl(fd_, F_SETFL, flags) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> SetReuseAddr(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    int32_t opt = enable ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                     static_cast<socklen_t>(sizeof(opt))) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> SetNoDelay(bool enable) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    int32_t opt = enable ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt,
                     static_cast<socklen_t>(sizeof(opt))) < 0) {
      return expected<void, SocketError>::error(SocketError::kSetOptFailed);
    }
    return expected<void, SocketError>::success();
  }

  /** @brief Close the socket. Idempotent - safe to call multiple times. */
  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  int32_t Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  friend class TcpListener;

  /** @brief Construct from an already-open file descriptor (used by Accept). */
  explicit TcpSocket(int32_t fd) noexcept : fd_(fd) {}

  int32_t fd_;
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
  UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

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
    int32_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
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
    if (::bind(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kBindFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<int32_t, SocketError> SendTo(const void* data, size_t len,
                                        const SocketAddress& dest) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    auto n = ::sendto(fd_, data, len, 0, dest.Raw(), dest.Size());
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kSendFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  expected<int32_t, SocketError> RecvFrom(void* buf, size_t len,
                                          SocketAddress& src) noexcept {
    if (fd_ < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kInvalidFd);
    }
    socklen_t addr_len = src.Size();
    auto n = ::recvfrom(fd_, buf, len, 0, src.RawMut(), &addr_len);
    if (n < 0) {
      return expected<int32_t, SocketError>::error(SocketError::kRecvFailed);
    }
    return expected<int32_t, SocketError>::success(static_cast<int32_t>(n));
  }

  /** @brief Close the socket. Idempotent. */
  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  int32_t Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  explicit UdpSocket(int32_t fd) noexcept : fd_(fd) {}

  int32_t fd_;
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
  TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

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
    int32_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    if (::bind(fd_, addr.Raw(), addr.Size()) < 0) {
      return expected<void, SocketError>::error(SocketError::kBindFailed);
    }
    return expected<void, SocketError>::success();
  }

  expected<void, SocketError> Listen(int32_t backlog = kDefaultBacklog) noexcept {
    if (fd_ < 0) {
      return expected<void, SocketError>::error(SocketError::kInvalidFd);
    }
    if (::listen(fd_, backlog) < 0) {
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
    int32_t client_fd = ::accept(fd_, nullptr, nullptr);
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
    int32_t client_fd = ::accept(fd_, client_addr.RawMut(), &addr_len);
    if (client_fd < 0) {
      return expected<TcpSocket, SocketError>::error(SocketError::kAcceptFailed);
    }
    return expected<TcpSocket, SocketError>::success(TcpSocket(client_fd));
  }

  /** @brief Close the listener socket. Idempotent. */
  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /** @brief Return the raw file descriptor. */
  int32_t Fd() const noexcept { return fd_; }

  /** @brief Check whether the socket holds a valid file descriptor. */
  bool IsValid() const noexcept { return fd_ >= 0; }

 private:
  explicit TcpListener(int32_t fd) noexcept : fd_(fd) {}

  int32_t fd_;
};

}  // namespace osp

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#endif  // OSP_SOCKET_HPP_
