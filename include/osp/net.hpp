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
 * @file net.hpp
 * @brief sockpp integration layer for network transport.
 *
 * Provides a thin wrapper around sockpp with osp::expected error handling.
 * Only available when OSP_HAS_SOCKPP is defined.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_NET_HPP_
#define OSP_NET_HPP_

#include "osp/platform.hpp"

#if OSP_HAS_NETWORK

#ifdef OSP_HAS_SOCKPP

#include "osp/vocabulary.hpp"

#include <sockpp/inet_address.h>
#include <sockpp/tcp_acceptor.h>
#include <sockpp/tcp_connector.h>
#include <sockpp/tcp_socket.h>
#include <sockpp/udp_socket.h>

#include <cstring>

namespace osp {
namespace net {

// ============================================================================
// NetError
// ============================================================================

enum class NetError : uint8_t {
  kConnectFailed = 0,
  kBindFailed,
  kListenFailed,
  kAcceptFailed,
  kSendFailed,
  kRecvFailed,
  kTimeout,
  kClosed,
  kInvalidAddress,
};

// ============================================================================
// TcpClient - TCP client connection
// ============================================================================

/**
 * @brief TCP client connection wrapper around sockpp::tcp_socket.
 *
 * Provides RAII socket management with osp::expected error handling.
 * Move-only type.
 */
class TcpClient {
 public:
  TcpClient() noexcept = default;

  /**
   * @brief Connect to a remote TCP server.
   * @param host Hostname or IP address (e.g. "127.0.0.1")
   * @param port Port number in host byte order
   * @param timeout_ms Connection timeout in milliseconds (0 = blocking)
   * @return TcpClient on success, NetError on failure
   */
  static expected<TcpClient, NetError> Connect(const char* host, uint16_t port,
                                                int32_t timeout_ms = 5000) noexcept {
    // Create address
    auto addr_res = sockpp::inet_address::create(host, port);
    if (!addr_res) {
      return expected<TcpClient, NetError>::error(NetError::kInvalidAddress);
    }

    // Connect with timeout using tcp_connector
    sockpp::tcp_connector conn;
    sockpp::result<> res;
    if (timeout_ms > 0) {
      res = conn.connect(addr_res.value(), std::chrono::milliseconds(timeout_ms));
    } else {
      res = conn.connect(addr_res.value());
    }

    if (!res) {
      return expected<TcpClient, NetError>::error(NetError::kConnectFailed);
    }

    // Move the connector into a TcpClient
    TcpClient client;
    client.sock_ = std::move(conn);
    return expected<TcpClient, NetError>::success(std::move(client));
  }

  /**
   * @brief Send data over the connection.
   * @param data Pointer to data buffer
   * @param len Number of bytes to send
   * @return Number of bytes sent on success, NetError on failure
   */
  expected<size_t, NetError> Send(const void* data, size_t len) noexcept {
    if (!sock_.is_open()) {
      return expected<size_t, NetError>::error(NetError::kClosed);
    }

    auto res = sock_.write(data, len);
    if (!res) {
      return expected<size_t, NetError>::error(NetError::kSendFailed);
    }

    return expected<size_t, NetError>::success(res.value());
  }

  /**
   * @brief Receive data from the connection.
   * @param buf Pointer to receive buffer
   * @param len Maximum number of bytes to receive
   * @return Number of bytes received on success, NetError on failure
   */
  expected<size_t, NetError> Recv(void* buf, size_t len) noexcept {
    if (!sock_.is_open()) {
      return expected<size_t, NetError>::error(NetError::kClosed);
    }

    auto res = sock_.read(buf, len);
    if (!res) {
      return expected<size_t, NetError>::error(NetError::kRecvFailed);
    }

    return expected<size_t, NetError>::success(res.value());
  }

  /**
   * @brief Get the underlying file descriptor.
   * @return File descriptor, or -1 if not open
   */
  int Fd() const noexcept { return sock_.handle(); }

  /**
   * @brief Check if the socket is open.
   * @return true if open, false otherwise
   */
  bool IsOpen() const noexcept { return sock_.is_open(); }

  /**
   * @brief Close the connection.
   */
  void Close() noexcept { sock_.close(); }

  // Move-only
  TcpClient(TcpClient&& other) noexcept = default;
  TcpClient& operator=(TcpClient&& other) noexcept = default;

  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

 private:
  friend class TcpServer;

  // Private constructor from tcp_socket (used by TcpServer::Accept)
  explicit TcpClient(sockpp::tcp_socket&& sock) noexcept
      : sock_(std::move(sock)) {}

  sockpp::tcp_socket sock_;
};

// ============================================================================
// TcpServer - TCP server listener
// ============================================================================

/**
 * @brief TCP server listener wrapper around sockpp::tcp_acceptor.
 *
 * Binds to a port and accepts incoming connections.
 * Move-only type.
 */
class TcpServer {
 public:
  TcpServer() noexcept = default;

  /**
   * @brief Create a TCP server listening on the specified port.
   * @param port Port number in host byte order (0 for OS-assigned)
   * @param backlog Maximum pending connection queue size
   * @return TcpServer on success, NetError on failure
   */
  static expected<TcpServer, NetError> Listen(uint16_t port,
                                               int32_t backlog = 16) noexcept {
    TcpServer server;

    // Create address (bind to all interfaces)
    auto addr_res = sockpp::inet_address::create("0.0.0.0", port);
    if (!addr_res) {
      return expected<TcpServer, NetError>::error(NetError::kInvalidAddress);
    }

    // Open acceptor
    auto open_res = server.acc_.open(addr_res.value(), backlog);
    if (!open_res) {
      return expected<TcpServer, NetError>::error(NetError::kListenFailed);
    }

    return expected<TcpServer, NetError>::success(std::move(server));
  }

  /**
   * @brief Accept an incoming connection (blocking).
   * @return TcpClient on success, NetError on failure
   */
  expected<TcpClient, NetError> Accept() noexcept {
    if (!acc_.is_open()) {
      return expected<TcpClient, NetError>::error(NetError::kClosed);
    }

    auto res = acc_.accept();
    if (!res) {
      return expected<TcpClient, NetError>::error(NetError::kAcceptFailed);
    }

    // Create TcpClient from the accepted tcp_socket
    // Use release() to move out of result
    return expected<TcpClient, NetError>::success(
        TcpClient(res.release()));
  }

  /**
   * @brief Get the underlying file descriptor.
   * @return File descriptor, or -1 if not open
   */
  int Fd() const noexcept { return acc_.handle(); }

  /**
   * @brief Check if the server is listening.
   * @return true if listening, false otherwise
   */
  bool IsOpen() const noexcept { return acc_.is_open(); }

  /**
   * @brief Close the server socket.
   */
  void Close() noexcept { acc_.close(); }

  // Move-only
  TcpServer(TcpServer&& other) noexcept = default;
  TcpServer& operator=(TcpServer&& other) noexcept = default;

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

 private:
  sockpp::tcp_acceptor acc_;
};

// ============================================================================
// UdpPeer - UDP socket
// ============================================================================

/**
 * @brief UDP socket wrapper around sockpp::udp_socket.
 *
 * Can be used for both sending and receiving UDP datagrams.
 * Move-only type.
 */
class UdpPeer {
 public:
  UdpPeer() noexcept = default;

  /**
   * @brief Create a UDP socket bound to a specific port.
   * @param port Port number in host byte order
   * @return UdpPeer on success, NetError on failure
   */
  static expected<UdpPeer, NetError> Bind(uint16_t port) noexcept {
    UdpPeer peer;

    auto addr_res = sockpp::inet_address::create("0.0.0.0", port);
    if (!addr_res) {
      return expected<UdpPeer, NetError>::error(NetError::kInvalidAddress);
    }

    auto bind_res = peer.sock_.bind(addr_res.value());
    if (!bind_res) {
      return expected<UdpPeer, NetError>::error(NetError::kBindFailed);
    }

    return expected<UdpPeer, NetError>::success(std::move(peer));
  }

  /**
   * @brief Create an unbound UDP socket (for sending only).
   * @return UdpPeer on success, NetError on failure
   */
  static expected<UdpPeer, NetError> Create() noexcept {
    UdpPeer peer;

    // Create UDP socket manually
    auto addr_res = sockpp::inet_address::create("0.0.0.0", 0);
    if (!addr_res) {
      return expected<UdpPeer, NetError>::error(NetError::kInvalidAddress);
    }

    // Just create the socket without binding
    peer.sock_ = sockpp::udp_socket(addr_res.value());
    if (!peer.sock_.is_open()) {
      return expected<UdpPeer, NetError>::error(NetError::kBindFailed);
    }

    return expected<UdpPeer, NetError>::success(std::move(peer));
  }

  /**
   * @brief Send a datagram to a specific address.
   * @param data Pointer to data buffer
   * @param len Number of bytes to send
   * @param host Destination hostname or IP address
   * @param port Destination port number
   * @return Number of bytes sent on success, NetError on failure
   */
  expected<size_t, NetError> SendTo(const void* data, size_t len,
                                     const char* host, uint16_t port) noexcept {
    if (!sock_.is_open()) {
      return expected<size_t, NetError>::error(NetError::kClosed);
    }

    auto addr_res = sockpp::inet_address::create(host, port);
    if (!addr_res) {
      return expected<size_t, NetError>::error(NetError::kInvalidAddress);
    }

    auto res = sock_.send_to(data, len, addr_res.value());
    if (!res) {
      return expected<size_t, NetError>::error(NetError::kSendFailed);
    }

    return expected<size_t, NetError>::success(res.value());
  }

  /**
   * @brief Receive a datagram and get the sender's address.
   * @param buf Pointer to receive buffer
   * @param len Maximum number of bytes to receive
   * @param from_host Buffer to store sender's hostname (can be nullptr)
   * @param host_len Size of from_host buffer
   * @param from_port Pointer to store sender's port (can be nullptr)
   * @return Number of bytes received on success, NetError on failure
   */
  expected<size_t, NetError> RecvFrom(void* buf, size_t len, char* from_host,
                                       size_t host_len,
                                       uint16_t* from_port) noexcept {
    if (!sock_.is_open()) {
      return expected<size_t, NetError>::error(NetError::kClosed);
    }

    sockpp::inet_address src_addr;
    auto res = sock_.recv_from(buf, len, &src_addr);
    if (!res) {
      return expected<size_t, NetError>::error(NetError::kRecvFailed);
    }

    // Fill in source address info if requested
    if (from_host != nullptr && host_len > 0) {
      auto addr_str = src_addr.to_string();
      size_t copy_len = addr_str.size();
      if (copy_len >= host_len) {
        copy_len = host_len - 1;
      }
      std::memcpy(from_host, addr_str.c_str(), copy_len);
      from_host[copy_len] = '\0';
    }

    if (from_port != nullptr) {
      *from_port = src_addr.port();
    }

    return expected<size_t, NetError>::success(res.value());
  }

  /**
   * @brief Get the underlying file descriptor.
   * @return File descriptor, or -1 if not open
   */
  int Fd() const noexcept { return sock_.handle(); }

  /**
   * @brief Close the socket.
   */
  void Close() noexcept { sock_.close(); }

  // Move-only
  UdpPeer(UdpPeer&& other) noexcept = default;
  UdpPeer& operator=(UdpPeer&& other) noexcept = default;

  UdpPeer(const UdpPeer&) = delete;
  UdpPeer& operator=(const UdpPeer&) = delete;

 private:
  sockpp::udp_socket sock_;
};

}  // namespace net
}  // namespace osp

#endif  // OSP_HAS_SOCKPP

#endif  // OSP_HAS_NETWORK

#endif  // OSP_NET_HPP_
