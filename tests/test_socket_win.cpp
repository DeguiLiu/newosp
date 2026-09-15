/**
 * @file test_socket_win.cpp
 * @brief Catch2 tests for osp::socket.hpp on the Winsock backend (OSP_NET_BACKEND==3).
 *
 * Verifies TcpSocket/TcpListener over a local 127.0.0.1 loopback with a random
 * ephemeral port, plus the socket_api Winsock glue (GetSockName / connect-refused /
 * setsockopt / FIONBIO). Guarded by OSP_PLATFORM_WINDOWS.
 */

#include "osp/socket.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#if defined(OSP_PLATFORM_WINDOWS)

namespace {

/// @brief Actual bound port of a listening socket via socket_api::GetSockName.
uint16_t BoundPort(const osp::TcpListener& l) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  socklen_t len = sizeof(addr);
  if (osp::socket_api::GetSockName(l.Fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return 0;
  }
  return osp::socket_api::Ntohs(addr.sin_port);
}

/// @brief Receive exactly `len` bytes (TCP is a byte stream).
bool RecvExact(osp::TcpSocket& s, void* buf, size_t len) {
  char* p = static_cast<char*>(buf);
  size_t got = 0;
  while (got < len) {
    auto r = s.Recv(p + got, len - got);
    if (!r.has_value() || r.value() <= 0) {
      return false;
    }
    got += static_cast<size_t>(r.value());
  }
  return true;
}

}  // namespace

// ============================================================================
// Loopback TCP roundtrip
// ============================================================================

TEST_CASE("socket_win - TCP loopback roundtrip", "[socket][tcp][windows]") {
  auto lr = osp::TcpListener::Create();
  REQUIRE(lr.has_value());
  osp::TcpListener listener = std::move(lr.value());

  auto ar = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  REQUIRE(ar.has_value());
  REQUIRE(listener.Bind(ar.value()).has_value());
  REQUIRE(listener.Listen(4).has_value());

  const uint16_t port = BoundPort(listener);
  REQUIRE(port != 0);

  auto cr = osp::TcpSocket::Create();
  REQUIRE(cr.has_value());
  osp::TcpSocket client = std::move(cr.value());

  auto ca = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(ca.has_value());
  REQUIRE(client.Connect(ca.value()).has_value());

  auto srv = listener.Accept();
  REQUIRE(srv.has_value());
  osp::TcpSocket server = std::move(srv.value());

  // client -> server
  const char msg[] = "ping";
  auto sr = server.Send(msg, 4);
  REQUIRE(sr.has_value());
  REQUIRE(sr.value() == 4);

  char buf[8] = {0};
  REQUIRE(RecvExact(client, buf, 4));
  REQUIRE(std::memcmp(buf, msg, 4) == 0);

  // server -> client
  const char msg2[] = "pong";
  REQUIRE(client.Send(msg2, 4).has_value());
  REQUIRE(RecvExact(server, buf, 4));
  REQUIRE(std::memcmp(buf, msg2, 4) == 0);
}

// ============================================================================
// Connect to a closed port -> kConnectFailed (WSAECONNREFUSED)
// ============================================================================

TEST_CASE("socket_win - connect to closed port fails", "[socket][tcp][windows]") {
  auto lr = osp::TcpListener::Create();
  REQUIRE(lr.has_value());
  osp::TcpListener listener = std::move(lr.value());
  auto ar = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  REQUIRE(ar.has_value());
  REQUIRE(listener.Bind(ar.value()).has_value());
  REQUIRE(listener.Listen(1).has_value());
  const uint16_t port = BoundPort(listener);
  REQUIRE(port != 0);
  listener.Close();  // no listening socket any more

  auto cr = osp::TcpSocket::Create();
  REQUIRE(cr.has_value());
  osp::TcpSocket client = std::move(cr.value());
  auto ca = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(ca.has_value());
  auto cc = client.Connect(ca.value());
  REQUIRE_FALSE(cc.has_value());
  REQUIRE(cc.get_error() == osp::SocketError::kConnectFailed);
}

// ============================================================================
// Winsock socket options (setsockopt / ioctlsocket paths)
// ============================================================================

TEST_CASE("socket_win - SetReuseAddr / SetNoDelay / SetNonBlocking", "[socket][tcp][windows]") {
  auto cr = osp::TcpSocket::Create();
  REQUIRE(cr.has_value());
  osp::TcpSocket sock = std::move(cr.value());

  REQUIRE(sock.SetReuseAddr(true).has_value());
  REQUIRE(sock.SetNoDelay(true).has_value());
  REQUIRE(sock.SetNonBlocking(true).has_value());
  REQUIRE(sock.SetNonBlocking(false).has_value());
}

// ============================================================================
// SocketAddress parsing
// ============================================================================

TEST_CASE("socket_win - SocketAddress::FromIpv4", "[socket][address][windows]") {
  auto ok = osp::SocketAddress::FromIpv4("127.0.0.1", 8080);
  REQUIRE(ok.has_value());
  REQUIRE(ok.value().Port() == 8080);
  REQUIRE(ok.value().Size() == sizeof(sockaddr_in));

  auto bad = osp::SocketAddress::FromIpv4("999.1.1.1", 80);
  REQUIRE_FALSE(bad.has_value());
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("socket_win - Windows socket tests are Windows-only", "[socket][windows]") {
  SKIP("socket_win tests only run on Windows (Winsock backend)");
}

#endif  // OSP_PLATFORM_WINDOWS
