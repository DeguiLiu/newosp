/**
 * @file test_lwip_backend.cpp
 * @brief osp network tests over a real lwIP stack (OSP_NET_BACKEND=1).
 * Requires LWIP_COMPAT_SOCKETS=0; built only when OSP_WITH_LWIP is enabled.
 */

#include "osp/io_poller.hpp"
#include "osp/socket.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <thread>

// ============================================================================
// TCP loopback over the lwIP loopback netif (127.0.0.1)
// ============================================================================

TEST_CASE("lwip backend - TCP loopback send/recv", "[lwip][socket]") {
  const uint16_t port = 19100;

  auto listener_res = osp::TcpListener::Create();
  REQUIRE(listener_res.has_value());
  auto listener = static_cast<osp::TcpListener&&>(listener_res.value());

  auto addr_res = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(addr_res.has_value());

  REQUIRE(listener.Bind(addr_res.value()).has_value());
  REQUIRE(listener.Listen(4).has_value());

  std::thread client_thread([port]() {
    auto sock_res = osp::TcpSocket::Create();
    if (!sock_res.has_value()) {
      return;
    }
    auto sock = static_cast<osp::TcpSocket&&>(sock_res.value());
    auto ca_res = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    if (!ca_res.has_value()) {
      return;
    }
    auto cr = sock.Connect(ca_res.value());
    if (!cr.has_value()) {
      return;
    }
    const char msg[] = "hello lwip";
    auto sr = sock.Send(msg, static_cast<size_t>(sizeof(msg)));
    if (!sr.has_value()) {
      sock.Close();
      return;
    }
    char buf[64];
    auto rr = sock.Recv(buf, sizeof(buf));
    if (rr.has_value() && rr.value() > 0) {
      std::printf("client recv: %.*s\n", rr.value(), buf);
    }
    sock.Close();
  });

  auto acc_res = listener.Accept();
  REQUIRE(acc_res.has_value());
  auto server_sock = static_cast<osp::TcpSocket&&>(acc_res.value());

  char buf[64];
  auto n = server_sock.Recv(buf, sizeof(buf));
  REQUIRE(n.has_value());
  REQUIRE(n.value() == 11);
  REQUIRE(std::memcmp(buf, "hello lwip", 10) == 0);

  const char reply[] = "pong";
  auto snd = server_sock.Send(reply, static_cast<size_t>(sizeof(reply)));
  REQUIRE(snd.has_value());

  client_thread.join();
}

// ============================================================================
// UDP loopback over the lwIP loopback netif
// ============================================================================

TEST_CASE("lwip backend - UDP loopback sendto/recvfrom", "[lwip][socket]") {
  const uint16_t port = 19101;

  auto recv_res = osp::UdpSocket::Create();
  REQUIRE(recv_res.has_value());
  auto recv_sock = static_cast<osp::UdpSocket&&>(recv_res.value());

  auto addr_res = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(addr_res.has_value());
  REQUIRE(recv_sock.Bind(addr_res.value()).has_value());

  auto send_res = osp::UdpSocket::Create();
  REQUIRE(send_res.has_value());
  auto send_sock = static_cast<osp::UdpSocket&&>(send_res.value());

  const char payload[] = "udp hello";
  auto sr = send_sock.SendTo(payload, static_cast<size_t>(sizeof(payload)), addr_res.value());
  REQUIRE(sr.has_value());
  REQUIRE(sr.value() == static_cast<int32_t>(sizeof(payload)));

  char buf[64];
  osp::SocketAddress src;
  auto rr = recv_sock.RecvFrom(buf, sizeof(buf), src);
  REQUIRE(rr.has_value());
  REQUIRE(rr.value() == static_cast<int32_t>(sizeof(payload)));
  REQUIRE(std::memcmp(buf, "udp hello", 9) == 0);
}

// ============================================================================
// IoPoller (poll-based path) over lwIP: a pending connection makes the
// listener fd readable.
// ============================================================================

TEST_CASE("lwip backend - IoPoller reports listener readiness", "[lwip][io_poller]") {
  const uint16_t port = 19102;

  auto listener_res = osp::TcpListener::Create();
  REQUIRE(listener_res.has_value());
  auto listener = static_cast<osp::TcpListener&&>(listener_res.value());

  auto addr_res = osp::SocketAddress::FromIpv4("127.0.0.1", port);
  REQUIRE(addr_res.has_value());
  REQUIRE(listener.Bind(addr_res.value()).has_value());
  REQUIRE(listener.Listen(4).has_value());

  osp::IoPoller poller;
  REQUIRE(poller.IsValid());
  auto add_r = poller.Add(listener.Fd(), static_cast<uint8_t>(osp::IoEvent::kReadable));
  REQUIRE(add_r.has_value());

  std::thread client_thread([port]() {
    auto sock_res = osp::TcpSocket::Create();
    if (!sock_res.has_value()) {
      return;
    }
    auto sock = static_cast<osp::TcpSocket&&>(sock_res.value());
    auto ca_res = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    if (!ca_res.has_value()) {
      return;
    }
    auto cr = sock.Connect(ca_res.value());
    (void)cr;
    sock.Close();
  });

  // Give the client a moment to start the connect, then wait for readability.
  bool seen = false;
  int attempts = 0;
  while (!seen && attempts < 20) {
    osp::PollResult results[4];
    auto r = poller.Wait(results, 4, 200);
    REQUIRE(r.has_value());
    for (uint32_t i = 0; i < r.value(); ++i) {
      if (results[i].fd == listener.Fd()) {
        seen = true;
      }
    }
    ++attempts;
  }

  client_thread.join();
  REQUIRE(seen);
}
