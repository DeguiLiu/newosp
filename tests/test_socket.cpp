/**
 * @file test_socket.cpp
 * @brief Tests for socket.hpp: SocketAddress, TcpSocket, UdpSocket, TcpListener.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/socket.hpp"

#include <cstring>
#include <thread>
#include <unistd.h>

// ============================================================================
// TcpSocket::Create
// ============================================================================

TEST_CASE("socket - TcpSocket::Create succeeds", "[socket][tcp]") {
  auto result = osp::TcpSocket::Create();
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
  REQUIRE(result.value().Fd() >= 0);
}

// ============================================================================
// UdpSocket::Create
// ============================================================================

TEST_CASE("socket - UdpSocket::Create succeeds", "[socket][udp]") {
  auto result = osp::UdpSocket::Create();
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
  REQUIRE(result.value().Fd() >= 0);
}

// ============================================================================
// TcpListener::Create
// ============================================================================

TEST_CASE("socket - TcpListener::Create succeeds", "[socket][tcp]") {
  auto result = osp::TcpListener::Create();
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
  REQUIRE(result.value().Fd() >= 0);
}

// ============================================================================
// SocketAddress::FromIpv4
// ============================================================================

TEST_CASE("socket - SocketAddress::FromIpv4 valid", "[socket][address]") {
  auto result = osp::SocketAddress::FromIpv4("127.0.0.1", 8080);
  REQUIRE(result.has_value());
  REQUIRE(result.value().Raw() != nullptr);
  REQUIRE(result.value().Size() == sizeof(sockaddr_in));
  REQUIRE(result.value().Port() == 8080);
}

TEST_CASE("socket - SocketAddress::FromIpv4 invalid returns error",
          "[socket][address]") {
  auto result = osp::SocketAddress::FromIpv4("not.an.ip.address", 80);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::SocketError::kInvalidFd);
}

// ============================================================================
// TcpSocket move semantics
// ============================================================================

TEST_CASE("socket - TcpSocket move semantics", "[socket][tcp]") {
  auto result = osp::TcpSocket::Create();
  REQUIRE(result.has_value());

  osp::TcpSocket a = static_cast<osp::TcpSocket&&>(result.value());
  REQUIRE(a.IsValid());
  int original_fd = a.Fd();

  // Move construct
  osp::TcpSocket b(static_cast<osp::TcpSocket&&>(a));
  REQUIRE(!a.IsValid());
  REQUIRE(a.Fd() < 0);
  REQUIRE(b.IsValid());
  REQUIRE(b.Fd() == original_fd);

  // Move assign
  osp::TcpSocket c;
  c = static_cast<osp::TcpSocket&&>(b);
  REQUIRE(!b.IsValid());
  REQUIRE(c.IsValid());
  REQUIRE(c.Fd() == original_fd);
}

// ============================================================================
// TcpSocket Close idempotent
// ============================================================================

TEST_CASE("socket - TcpSocket Close is idempotent", "[socket][tcp]") {
  auto result = osp::TcpSocket::Create();
  REQUIRE(result.has_value());

  osp::TcpSocket sock = static_cast<osp::TcpSocket&&>(result.value());
  REQUIRE(sock.IsValid());

  sock.Close();
  REQUIRE(!sock.IsValid());

  // Second close should be a no-op, no crash
  sock.Close();
  REQUIRE(!sock.IsValid());
}

// ============================================================================
// TcpSocket SetReuseAddr
// ============================================================================

TEST_CASE("socket - TcpSocket SetReuseAddr", "[socket][tcp]") {
  auto result = osp::TcpSocket::Create();
  REQUIRE(result.has_value());

  osp::TcpSocket sock = static_cast<osp::TcpSocket&&>(result.value());
  auto r = sock.SetReuseAddr(true);
  REQUIRE(r.has_value());

  r = sock.SetReuseAddr(false);
  REQUIRE(r.has_value());
}

// ============================================================================
// TcpSocket SetNonBlocking
// ============================================================================

TEST_CASE("socket - TcpSocket SetNonBlocking", "[socket][tcp]") {
  auto result = osp::TcpSocket::Create();
  REQUIRE(result.has_value());

  osp::TcpSocket sock = static_cast<osp::TcpSocket&&>(result.value());

  auto r = sock.SetNonBlocking(true);
  REQUIRE(r.has_value());

  // Verify the flag is actually set via fcntl
  int flags = ::fcntl(sock.Fd(), F_GETFL, 0);
  REQUIRE((flags & O_NONBLOCK) != 0);

  r = sock.SetNonBlocking(false);
  REQUIRE(r.has_value());

  flags = ::fcntl(sock.Fd(), F_GETFL, 0);
  REQUIRE((flags & O_NONBLOCK) == 0);
}

// ============================================================================
// TcpListener bind + listen + accept (loopback)
// ============================================================================

TEST_CASE("socket - TcpListener bind listen accept loopback",
          "[socket][tcp][integration]") {
  // Create listener
  auto listener_r = osp::TcpListener::Create();
  REQUIRE(listener_r.has_value());
  osp::TcpListener listener =
      static_cast<osp::TcpListener&&>(listener_r.value());

  // Set SO_REUSEADDR on the listener fd directly
  int opt = 1;
  ::setsockopt(listener.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt,
               static_cast<socklen_t>(sizeof(opt)));

  // Bind to loopback on port 0 (OS picks a free port)
  auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  REQUIRE(addr_r.has_value());
  auto bind_r = listener.Bind(addr_r.value());
  REQUIRE(bind_r.has_value());

  // Retrieve the actual port assigned by the OS
  sockaddr_in bound_addr{};
  socklen_t addr_len = sizeof(bound_addr);
  ::getsockname(listener.Fd(), reinterpret_cast<sockaddr*>(&bound_addr),
                &addr_len);
  uint16_t port = ntohs(bound_addr.sin_port);
  REQUIRE(port > 0);

  auto listen_r = listener.Listen(4);
  REQUIRE(listen_r.has_value());

  // Connect from a client in a separate thread
  const char* msg = "hello";
  std::thread client_thread([port, msg]() {
    auto client_r = osp::TcpSocket::Create();
    if (!client_r.has_value()) {
      return;
    }
    osp::TcpSocket client = static_cast<osp::TcpSocket&&>(client_r.value());
    auto server_addr = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    if (!server_addr.has_value()) {
      return;
    }
    auto conn = client.Connect(server_addr.value());
    if (!conn.has_value()) {
      return;
    }
    (void)client.Send(msg, std::strlen(msg));
    client.Close();
  });

  // Accept the connection
  osp::SocketAddress client_addr;
  auto accept_r = listener.Accept(client_addr);
  REQUIRE(accept_r.has_value());

  osp::TcpSocket accepted = static_cast<osp::TcpSocket&&>(accept_r.value());
  REQUIRE(accepted.IsValid());

  // Receive data
  char buf[64]{};
  auto recv_r = accepted.Recv(buf, sizeof(buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() == static_cast<int32_t>(std::strlen(msg)));
  REQUIRE(std::strncmp(buf, msg, std::strlen(msg)) == 0);

  client_thread.join();
}

// ============================================================================
// UdpSocket SendTo / RecvFrom loopback
// ============================================================================

TEST_CASE("socket - UdpSocket SendTo RecvFrom loopback",
          "[socket][udp][integration]") {
  // Create receiver
  auto recv_sock_r = osp::UdpSocket::Create();
  REQUIRE(recv_sock_r.has_value());
  osp::UdpSocket recv_sock =
      static_cast<osp::UdpSocket&&>(recv_sock_r.value());

  // Bind receiver to loopback port 0
  auto bind_addr = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  REQUIRE(bind_addr.has_value());
  auto bind_r = recv_sock.Bind(bind_addr.value());
  REQUIRE(bind_r.has_value());

  // Retrieve the actual port
  sockaddr_in bound_addr{};
  socklen_t addr_len = sizeof(bound_addr);
  ::getsockname(recv_sock.Fd(), reinterpret_cast<sockaddr*>(&bound_addr),
                &addr_len);
  uint16_t port = ntohs(bound_addr.sin_port);
  REQUIRE(port > 0);

  // Create sender
  auto send_sock_r = osp::UdpSocket::Create();
  REQUIRE(send_sock_r.has_value());
  osp::UdpSocket send_sock =
      static_cast<osp::UdpSocket&&>(send_sock_r.value());

  // Send data to receiver
  auto dest_addr = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(dest_addr.has_value());

  const char* payload = "udp_test";
  auto send_r =
      send_sock.SendTo(payload, std::strlen(payload), dest_addr.value());
  REQUIRE(send_r.has_value());
  REQUIRE(send_r.value() == static_cast<int32_t>(std::strlen(payload)));

  // Receive data
  char buf[64]{};
  osp::SocketAddress src_addr;
  auto recv_r = recv_sock.RecvFrom(buf, sizeof(buf), src_addr);
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() == static_cast<int32_t>(std::strlen(payload)));
  REQUIRE(std::strncmp(buf, payload, std::strlen(payload)) == 0);
}

// ============================================================================
// UnixAddress
// ============================================================================

TEST_CASE("socket - UnixAddress::FromPath succeeds", "[socket][unix]") {
  auto result = osp::UnixAddress::FromPath("/tmp/osp_test.sock");
  REQUIRE(result.has_value());
  REQUIRE(std::strcmp(result.value().Path(), "/tmp/osp_test.sock") == 0);
}

TEST_CASE("socket - UnixAddress::FromPath rejects too-long path", "[socket][unix]") {
  // sockaddr_un::sun_path is typically 108 bytes
  char long_path[256];
  std::memset(long_path, 'a', sizeof(long_path) - 1);
  long_path[sizeof(long_path) - 1] = '\0';
  auto result = osp::UnixAddress::FromPath(long_path);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::SocketError::kPathTooLong);
}

// ============================================================================
// UnixSocket + UnixListener echo
// ============================================================================

TEST_CASE("socket - UnixSocket Create succeeds", "[socket][unix]") {
  auto result = osp::UnixSocket::Create();
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
  REQUIRE(result.value().Fd() >= 0);
}

TEST_CASE("socket - UnixListener Create succeeds", "[socket][unix]") {
  auto result = osp::UnixListener::Create();
  REQUIRE(result.has_value());
  REQUIRE(result.value().IsValid());
}

TEST_CASE("socket - Unix domain echo round-trip", "[socket][unix]") {
  const char* path = "/tmp/osp_test_echo.sock";

  // Cleanup any leftover socket file
  ::unlink(path);

  // Setup listener
  auto addr_r = osp::UnixAddress::FromPath(path);
  REQUIRE(addr_r.has_value());
  auto& addr = addr_r.value();

  auto listener_r = osp::UnixListener::Create();
  REQUIRE(listener_r.has_value());
  auto& listener = listener_r.value();

  auto bind_r = listener.Bind(addr);
  REQUIRE(bind_r.has_value());
  auto listen_r = listener.Listen();
  REQUIRE(listen_r.has_value());

  // Client thread - create own address to avoid data race on addr
  std::atomic<bool> client_ok{false};
  std::thread client([path, &client_ok]() {
    auto client_addr_r = osp::UnixAddress::FromPath(path);
    if (!client_addr_r.has_value()) return;

    auto sock_r = osp::UnixSocket::Create();
    if (!sock_r.has_value()) return;
    auto& sock = sock_r.value();

    auto conn_r = sock.Connect(client_addr_r.value());
    if (!conn_r.has_value()) return;

    const char msg[] = "hello unix";
    auto send_r = sock.Send(msg, sizeof(msg));
    if (!send_r.has_value()) return;

    char buf[64] = {};
    auto recv_r = sock.Recv(buf, sizeof(buf));
    if (!recv_r.has_value()) return;

    if (std::strcmp(buf, "hello unix") == 0) {
      client_ok.store(true, std::memory_order_release);
    }
  });

  // Server: accept, echo back
  auto accepted_r = listener.Accept();
  REQUIRE(accepted_r.has_value());
  auto& conn = accepted_r.value();

  char buf[64] = {};
  auto recv_r = conn.Recv(buf, sizeof(buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() > 0);

  auto send_r = conn.Send(buf, static_cast<size_t>(recv_r.value()));
  REQUIRE(send_r.has_value());

  client.join();
  REQUIRE(client_ok.load(std::memory_order_acquire));

  // Cleanup
  ::unlink(path);
}

TEST_CASE("socket - UnixSocket move semantics", "[socket][unix]") {
  auto result = osp::UnixSocket::Create();
  REQUIRE(result.has_value());
  int32_t fd = result.value().Fd();

  osp::UnixSocket moved(std::move(result.value()));
  REQUIRE(moved.Fd() == fd);
  REQUIRE(moved.IsValid());
  // Original should be invalidated
  REQUIRE_FALSE(result.value().IsValid());
}

TEST_CASE("socket - UnixListener move semantics", "[socket][unix]") {
  auto result = osp::UnixListener::Create();
  REQUIRE(result.has_value());
  int32_t fd = result.value().Fd();

  osp::UnixListener moved(std::move(result.value()));
  REQUIRE(moved.Fd() == fd);
  REQUIRE(moved.IsValid());
  REQUIRE_FALSE(result.value().IsValid());
}

TEST_CASE("socket - UnixSocket SetNonBlocking", "[socket][unix]") {
  auto result = osp::UnixSocket::Create();
  REQUIRE(result.has_value());
  auto nb = result.value().SetNonBlocking(true);
  REQUIRE(nb.has_value());
}
