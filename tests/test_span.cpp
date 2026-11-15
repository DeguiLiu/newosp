/**
 * @file test_span.cpp
 * @brief Catch2 tests for osp::span and the byte-view overloads of the I/O APIs.
 *
 * The span semantics (empty view, array deduction, qualification conversion,
 * byte reinterpretation) are checked directly. The byte-view overloads of the
 * socket and serial transports are exercised on closed handles, so the tests
 * need no real device and no network.
 */

#include "osp/serial_transport.hpp"
#include "osp/socket.hpp"
#include "osp/span.hpp"

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("span - default view is empty", "[span]") {
  osp::span<const uint8_t> s;
  REQUIRE(s.empty());
  REQUIRE(s.data() == nullptr);
  REQUIRE(s.size() == 0U);
  REQUIRE(s.begin() == s.end());
  REQUIRE(s.size_u32() == 0U);
}

TEST_CASE("span - make_span deduces the array length", "[span]") {
  uint8_t buf[4] = {1U, 2U, 3U, 4U};
  auto s = osp::make_span(buf);
  REQUIRE(s.size() == 4U);
  REQUIRE(s.size_bytes() == 4U);
  REQUIRE_FALSE(s.empty());
  REQUIRE(s[2] == 3U);
  REQUIRE(s.end() == s.data() + 4);
}

TEST_CASE("span - qualification conversion preserves pointer and size", "[span]") {
  uint8_t buf[3] = {7U, 8U, 9U};
  osp::span<uint8_t> mutable_view(buf);
  osp::span<const uint8_t> const_view = mutable_view;
  REQUIRE(const_view.data() == buf);
  REQUIRE(const_view.size() == 3U);
  REQUIRE(const_view[1] == 8U);
}

TEST_CASE("span - as_bytes views the same storage as bytes", "[span]") {
  uint32_t words[2] = {0x11223344U, 0x55667788U};
  auto bytes = osp::as_bytes(osp::make_span(words));
  REQUIRE(bytes.size() == sizeof(words));
  REQUIRE(bytes.data() == reinterpret_cast<const std::byte*>(words));

  auto writable = osp::as_writable_bytes(osp::make_span(words));
  REQUIRE(writable.size() == sizeof(words));
  writable[0] = std::byte{0xAB};
  REQUIRE(reinterpret_cast<const uint8_t*>(words)[0] == 0xABU);
}

#if OSP_HAS_NETWORK

TEST_CASE("span - socket byte-view overloads report invalid fd", "[span][socket]") {
  uint8_t buf[8] = {};
  osp::SocketAddress addr;

  osp::TcpSocket tcp;
  auto tx = tcp.Send(osp::as_bytes(osp::make_span(buf)));
  REQUIRE_FALSE(tx.has_value());
  REQUIRE(tx.get_error() == osp::SocketError::kInvalidFd);

  auto rx = tcp.Recv(osp::as_writable_bytes(osp::make_span(buf)));
  REQUIRE_FALSE(rx.has_value());
  REQUIRE(rx.get_error() == osp::SocketError::kInvalidFd);

  osp::UdpSocket udp;
  auto us = udp.SendTo(osp::as_bytes(osp::make_span(buf)), addr);
  REQUIRE_FALSE(us.has_value());
  REQUIRE(us.get_error() == osp::SocketError::kInvalidFd);

  auto ur = udp.RecvFrom(osp::as_writable_bytes(osp::make_span(buf)), addr);
  REQUIRE_FALSE(ur.has_value());
  REQUIRE(ur.get_error() == osp::SocketError::kInvalidFd);
}

#endif  // OSP_HAS_NETWORK

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

TEST_CASE("span - serial byte-view overload reports a closed port", "[span][serial]") {
  osp::SerialConfig cfg;
  osp::SerialTransport transport(cfg);
  uint8_t buf[4] = {};
  auto r = transport.Send(1U, osp::as_bytes(osp::make_span(buf)));
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.get_error() == osp::SerialError::kPortNotOpen);
}

#endif  // OSP_PLATFORM_LINUX || OSP_PLATFORM_MACOS
