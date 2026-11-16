/**
 * @file test_io_poller_win.cpp
 * @brief Catch2 tests for osp::io_poller.hpp on Winsock (WSAPoll fallback,
 *        OSP_IO_POLLER_USE_WSAPOLL==1).
 *
 * The fallback poller tracks fds in an internal array and dispatches to
 * socket_api::Poll (::WSAPoll). These tests verify that a connected TCP socket
 * becomes readable when data arrives, that Wait times out with no events, and
 * that Add/Modify/Remove work on the Winsock path. Guarded by
 * OSP_PLATFORM_WINDOWS.
 */

#include "osp/io_poller.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(OSP_PLATFORM_WINDOWS)

namespace {

/// @brief Connected loopback TCP pair for poller tests.
struct TcpPair {
  osp::TcpListener listener;
  osp::TcpSocket server;
  osp::TcpSocket client;

  bool Setup() {
    auto lr = osp::TcpListener::Create();
    if (!lr.has_value()) {
      return false;
    }
    listener = std::move(lr.value());

    auto ar = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
    if (!ar.has_value() || !listener.Bind(ar.value()).has_value() || !listener.Listen(1).has_value()) {
      return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    socklen_t len = sizeof(addr);
    if (osp::socket_api::GetSockName(listener.Fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      return false;
    }
    const uint16_t port = osp::socket_api::Ntohs(addr.sin_port);

    auto ca = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    auto cr = osp::TcpSocket::Create();
    if (!ca.has_value() || !cr.has_value()) {
      return false;
    }
    client = std::move(cr.value());
    if (!client.Connect(ca.value()).has_value()) {
      return false;
    }

    auto srv = listener.Accept();
    if (!srv.has_value()) {
      return false;
    }
    server = std::move(srv.value());
    return true;
  }
};

}  // namespace

// ============================================================================
// Default construction
// ============================================================================

TEST_CASE("io_poller_win - default construction is valid", "[io_poller][windows]") {
  osp::IoPoller poller;
  REQUIRE(poller.IsValid());
}

// ============================================================================
// Readable event on a connected socket
// ============================================================================

TEST_CASE("io_poller_win - WSAPoll reports readable socket", "[io_poller][windows]") {
  TcpPair pair;
  REQUIRE(pair.Setup());

  osp::IoPoller poller;
  REQUIRE(poller.IsValid());

  auto add_r = poller.Add(static_cast<int32_t>(pair.server.Fd()),
                          static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  const char msg[] = "hi";
  REQUIRE(pair.client.Send(msg, 2).has_value());

  osp::PollResult results[4];
  auto w = poller.Wait(results, 4, 500);
  REQUIRE(w.has_value());
  REQUIRE(w.value() >= 1);
  REQUIRE(results[0].fd == static_cast<int32_t>(pair.server.Fd()));
  REQUIRE((results[0].events & static_cast<uint8_t>(osp::IoEvent::kReadable)) != 0);

  char buf[8] = {0};
  (void)pair.server.Recv(buf, sizeof(buf));
}

// ============================================================================
// Timeout with no events
// ============================================================================

TEST_CASE("io_poller_win - Wait times out with no data", "[io_poller][windows]") {
  TcpPair pair;
  REQUIRE(pair.Setup());

  osp::IoPoller poller;
  auto add_r = poller.Add(static_cast<int32_t>(pair.server.Fd()),
                          static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  osp::PollResult results[4];
  auto w = poller.Wait(results, 4, 50);
  REQUIRE(w.has_value());
  REQUIRE(w.value() == 0);
}

// ============================================================================
// Modify and Remove
// ============================================================================

TEST_CASE("io_poller_win - Modify and Remove on a socket fd", "[io_poller][windows]") {
  TcpPair pair;
  REQUIRE(pair.Setup());

  osp::IoPoller poller;
  auto add_r = poller.Add(static_cast<int32_t>(pair.server.Fd()),
                          static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  auto mod_r = poller.Modify(static_cast<int32_t>(pair.server.Fd()),
                             static_cast<uint8_t>(osp::IoEvent::kWritable));
  REQUIRE(mod_r.has_value());

  auto rm_r = poller.Remove(static_cast<int32_t>(pair.server.Fd()));
  REQUIRE(rm_r.has_value());

  // Removing an fd that is no longer registered fails.
  auto rm2 = poller.Remove(static_cast<int32_t>(pair.server.Fd()));
  REQUIRE_FALSE(rm2.has_value());
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("io_poller_win - Windows poller tests are Windows-only", "[io_poller][windows]") {
  SKIP("io_poller_win tests only run on Windows (WSAPoll)");
}

#endif  // OSP_PLATFORM_WINDOWS
