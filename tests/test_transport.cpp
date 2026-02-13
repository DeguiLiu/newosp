/**
 * @file test_transport.cpp
 * @brief Tests for transport.hpp: Endpoint, FrameCodec, Serializer,
 *        TcpTransport, NetworkNode.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/transport.hpp"

#include <cstring>
#include <thread>
#include <chrono>
#include <variant>

// ============================================================================
// Test payload types
// ============================================================================

struct SensorData {
  float temperature;
  uint32_t sensor_id;
};

struct MotorCmd {
  int32_t speed;
  uint8_t direction;
};

using TestPayload = std::variant<SensorData, MotorCmd>;

// ============================================================================
// Helper: create a TcpListener on port 0 and return the OS-assigned port
// ============================================================================

static uint16_t SetupListener(osp::TcpListener& listener) {
  auto listener_r = osp::TcpListener::Create();
  REQUIRE(listener_r.has_value());
  listener = static_cast<osp::TcpListener&&>(listener_r.value());

  int opt = 1;
  ::setsockopt(listener.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt,
               static_cast<socklen_t>(sizeof(opt)));

  auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  REQUIRE(addr_r.has_value());
  auto bind_r = listener.Bind(addr_r.value());
  REQUIRE(bind_r.has_value());

  sockaddr_in bound_addr{};
  socklen_t addr_len = sizeof(bound_addr);
  ::getsockname(listener.Fd(), reinterpret_cast<sockaddr*>(&bound_addr),
                &addr_len);
  uint16_t port = ntohs(bound_addr.sin_port);
  REQUIRE(port > 0);

  auto listen_r = listener.Listen(4);
  REQUIRE(listen_r.has_value());

  return port;
}

// ============================================================================
// 1. Endpoint::FromString
// ============================================================================

TEST_CASE("transport - Endpoint::FromString", "[transport][endpoint]") {
  auto ep = osp::Endpoint::FromString("192.168.1.100", 5555);
  REQUIRE(std::strcmp(ep.host, "192.168.1.100") == 0);
  REQUIRE(ep.port == 5555);
}

TEST_CASE("transport - Endpoint::FromString truncates long host",
          "[transport][endpoint]") {
  // Create a 70-char string (exceeds 63-char host buffer)
  char long_host[70];
  std::memset(long_host, 'a', sizeof(long_host));
  long_host[69] = '\0';

  auto ep = osp::Endpoint::FromString(long_host, 80);
  REQUIRE(std::strlen(ep.host) == 63);
  REQUIRE(ep.port == 80);
}

TEST_CASE("transport - Endpoint::FromString nullptr host",
          "[transport][endpoint]") {
  auto ep = osp::Endpoint::FromString(nullptr, 1234);
  REQUIRE(ep.host[0] == '\0');
  REQUIRE(ep.port == 1234);
}

// ============================================================================
// 2. FrameCodec encode/decode roundtrip
// ============================================================================

TEST_CASE("transport - FrameCodec encode decode roundtrip",
          "[transport][codec]") {
  osp::FrameHeader original;
  original.magic = osp::kFrameMagic;
  original.length = 128;
  original.type_index = 3;
  original.sender_id = 42;

  uint8_t buf[osp::FrameCodec::kHeaderSize];
  uint32_t written = osp::FrameCodec::EncodeHeader(original, buf, sizeof(buf));
  REQUIRE(written == osp::FrameCodec::kHeaderSize);

  osp::FrameHeader decoded;
  bool ok = osp::FrameCodec::DecodeHeader(buf, sizeof(buf), decoded);
  REQUIRE(ok);
  REQUIRE(decoded.magic == original.magic);
  REQUIRE(decoded.length == original.length);
  REQUIRE(decoded.type_index == original.type_index);
  REQUIRE(decoded.sender_id == original.sender_id);
}

TEST_CASE("transport - FrameCodec encode fails on small buffer",
          "[transport][codec]") {
  osp::FrameHeader hdr;
  hdr.magic = osp::kFrameMagic;
  hdr.length = 0;
  hdr.type_index = 0;
  hdr.sender_id = 0;

  uint8_t small_buf[4];
  uint32_t written =
      osp::FrameCodec::EncodeHeader(hdr, small_buf, sizeof(small_buf));
  REQUIRE(written == 0);
}

TEST_CASE("transport - FrameCodec decode fails on small buffer",
          "[transport][codec]") {
  uint8_t small_buf[4] = {0};
  osp::FrameHeader hdr;
  bool ok = osp::FrameCodec::DecodeHeader(small_buf, sizeof(small_buf), hdr);
  REQUIRE(!ok);
}

// ============================================================================
// 3. Serializer<T> POD roundtrip
// ============================================================================

TEST_CASE("transport - Serializer POD roundtrip", "[transport][serializer]") {
  SensorData original{25.5f, 7};

  uint8_t buf[64];
  uint32_t len =
      osp::Serializer<SensorData>::Serialize(original, buf, sizeof(buf));
  REQUIRE(len == sizeof(SensorData));

  SensorData restored;
  bool ok = osp::Serializer<SensorData>::Deserialize(buf, len, restored);
  REQUIRE(ok);
  REQUIRE(restored.temperature == original.temperature);
  REQUIRE(restored.sensor_id == original.sensor_id);
}

TEST_CASE("transport - Serializer fails on undersized buffer",
          "[transport][serializer]") {
  SensorData data{1.0f, 1};

  // Serialize into too-small buffer
  uint8_t tiny_buf[1];
  uint32_t len =
      osp::Serializer<SensorData>::Serialize(data, tiny_buf, sizeof(tiny_buf));
  REQUIRE(len == 0);

  // Deserialize from too-small buffer
  SensorData out;
  bool ok = osp::Serializer<SensorData>::Deserialize(tiny_buf, 1, out);
  REQUIRE(!ok);
}

// ============================================================================
// 4. TcpTransport Connect/SendFrame/RecvFrame loopback
// ============================================================================

TEST_CASE("transport - TcpTransport SendFrame RecvFrame loopback",
          "[transport][tcp][integration]") {
  // Set up a listener on port 0
  osp::TcpListener listener;
  uint16_t port = SetupListener(listener);

  // Client connects and sends a frame in a separate thread
  SensorData send_data{36.6f, 99};
  std::thread client_thread([port, &send_data]() {
    osp::TcpTransport client;
    auto ep = osp::Endpoint::FromString("127.0.0.1", port);
    auto r = client.Connect(ep);
    if (!r.has_value()) return;

    uint8_t payload_buf[64];
    uint32_t len = osp::Serializer<SensorData>::Serialize(
        send_data, payload_buf, sizeof(payload_buf));
    if (len == 0) return;

    (void)client.SendFrame(0, 42, payload_buf, len);
  });

  // Accept connection on server side
  auto accept_r = listener.Accept();
  REQUIRE(accept_r.has_value());

  osp::TcpTransport server;
  server.AcceptFrom(static_cast<osp::TcpSocket&&>(accept_r.value()));
  REQUIRE(server.IsConnected());

  // Receive the frame
  osp::FrameHeader hdr;
  uint8_t recv_buf[256];
  auto recv_r = server.RecvFrame(hdr, recv_buf, sizeof(recv_buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(hdr.magic == osp::kFrameMagic);
  REQUIRE(hdr.type_index == 0);
  REQUIRE(hdr.sender_id == 42);
  REQUIRE(recv_r.value() == sizeof(SensorData));

  // Deserialize and verify
  SensorData received;
  bool ok = osp::Serializer<SensorData>::Deserialize(
      recv_buf, recv_r.value(), received);
  REQUIRE(ok);
  REQUIRE(received.temperature == send_data.temperature);
  REQUIRE(received.sensor_id == send_data.sensor_id);

  client_thread.join();
}

// ============================================================================
// 5. TcpTransport not connected returns error
// ============================================================================

TEST_CASE("transport - TcpTransport not connected returns error",
          "[transport][tcp]") {
  osp::TcpTransport transport;
  REQUIRE(!transport.IsConnected());

  // SendFrame should fail
  uint8_t buf[4] = {1, 2, 3, 4};
  auto send_r = transport.SendFrame(0, 0, buf, 4);
  REQUIRE(!send_r.has_value());
  REQUIRE(send_r.get_error() == osp::TransportError::kNotConnected);

  // RecvFrame should fail
  osp::FrameHeader hdr;
  auto recv_r = transport.RecvFrame(hdr, buf, sizeof(buf));
  REQUIRE(!recv_r.has_value());
  REQUIRE(recv_r.get_error() == osp::TransportError::kNotConnected);
}

// ============================================================================
// 6. FrameHeader magic validation
// ============================================================================

TEST_CASE("transport - FrameHeader magic validation",
          "[transport][tcp][integration]") {
  osp::TcpListener listener;
  uint16_t port = SetupListener(listener);

  // Client sends a frame with bad magic
  std::thread client_thread([port]() {
    auto sock_r = osp::TcpSocket::Create();
    if (!sock_r.has_value()) return;
    osp::TcpSocket sock = static_cast<osp::TcpSocket&&>(sock_r.value());

    auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    if (!addr_r.has_value()) return;
    auto conn_r = sock.Connect(addr_r.value());
    if (!conn_r.has_value()) return;

    // Build a header with wrong magic
    osp::FrameHeader bad_hdr;
    bad_hdr.magic = 0xDEADBEEF;
    bad_hdr.length = 0;
    bad_hdr.type_index = 0;
    bad_hdr.sender_id = 0;

    uint8_t hdr_buf[osp::FrameCodec::kHeaderSize];
    osp::FrameCodec::EncodeHeader(bad_hdr, hdr_buf, sizeof(hdr_buf));
    (void)sock.Send(hdr_buf, osp::FrameCodec::kHeaderSize);
  });

  auto accept_r = listener.Accept();
  REQUIRE(accept_r.has_value());

  osp::TcpTransport server;
  server.AcceptFrom(static_cast<osp::TcpSocket&&>(accept_r.value()));

  osp::FrameHeader hdr;
  uint8_t buf[64];
  auto recv_r = server.RecvFrame(hdr, buf, sizeof(buf));
  REQUIRE(!recv_r.has_value());
  REQUIRE(recv_r.get_error() == osp::TransportError::kInvalidFrame);

  client_thread.join();
}

// ============================================================================
// 7. NetworkNode construction
// ============================================================================

TEST_CASE("transport - NetworkNode construction",
          "[transport][network_node]") {
  osp::NetworkNode<TestPayload> node("test_net_node", 10);
  REQUIRE(std::strcmp(node.Name(), "test_net_node") == 0);
  REQUIRE(node.Id() == 10);
  REQUIRE(node.RemotePublisherCount() == 0);
  REQUIRE(node.RemoteSubscriberCount() == 0);
  REQUIRE(!node.IsListening());
}

// ============================================================================
// 8. NetworkNode AdvertiseTo (connect to local listener)
// ============================================================================

TEST_CASE("transport - NetworkNode AdvertiseTo connects to listener",
          "[transport][network_node][integration]") {
  // Reset the bus to avoid cross-test interference
  osp::AsyncBus<TestPayload>::Instance().Reset();

  // Set up a raw listener for the remote side
  osp::TcpListener listener;
  uint16_t port = SetupListener(listener);

  osp::NetworkNode<TestPayload> node("advertiser", 1);
  auto start_r = node.Start();
  REQUIRE(start_r.has_value());

  // Accept in background (AdvertiseTo will connect)
  osp::TcpSocket accepted_sock;
  std::thread accept_thread([&listener, &accepted_sock]() {
    auto r = listener.Accept();
    if (r.has_value()) {
      accepted_sock = static_cast<osp::TcpSocket&&>(r.value());
    }
  });

  auto ep = osp::Endpoint::FromString("127.0.0.1", port);
  auto adv_r = node.AdvertiseTo<SensorData>(ep);
  REQUIRE(adv_r.has_value());
  REQUIRE(node.RemotePublisherCount() == 1);

  accept_thread.join();
  REQUIRE(accepted_sock.IsValid());
}

// ============================================================================
// 9. NetworkNode Listen and AcceptOne
// ============================================================================

TEST_CASE("transport - NetworkNode Listen and AcceptOne",
          "[transport][network_node][integration]") {
  osp::AsyncBus<TestPayload>::Instance().Reset();

  osp::NetworkNode<TestPayload> node("listener_node", 2);

  // Listen on port 0
  auto listen_r = node.Listen(0);
  REQUIRE(listen_r.has_value());
  REQUIRE(node.IsListening());

  // Get the assigned port via getsockname
  sockaddr_in bound_addr{};
  socklen_t addr_len = sizeof(bound_addr);
  ::getsockname(node.ListenerFd(),
                reinterpret_cast<sockaddr*>(&bound_addr), &addr_len);
  uint16_t port = ntohs(bound_addr.sin_port);
  REQUIRE(port > 0);

  // Connect a client
  std::thread client_thread([port]() {
    auto sock_r = osp::TcpSocket::Create();
    if (!sock_r.has_value()) return;
    osp::TcpSocket sock = static_cast<osp::TcpSocket&&>(sock_r.value());
    auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", port);
    if (!addr_r.has_value()) return;
    (void)sock.Connect(addr_r.value());
    // Keep connection alive briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });

  auto accept_r = node.AcceptOne();
  REQUIRE(accept_r.has_value());
  REQUIRE(node.RemoteSubscriberCount() == 1);

  client_thread.join();
}

// ============================================================================
// 10. Full roundtrip: NetworkNode A -> NetworkNode B via TCP
// ============================================================================

TEST_CASE("transport - full roundtrip NetworkNode A to B via TCP",
          "[transport][network_node][integration]") {
  osp::AsyncBus<TestPayload>::Instance().Reset();

  // Node B: the receiver. Listens and accepts.
  osp::NetworkNode<TestPayload> node_b("receiver", 20);
  auto start_b = node_b.Start();
  REQUIRE(start_b.has_value());

  // Set up a raw listener for Node B's inbound connection
  osp::TcpListener listener_b;
  uint16_t port_b = SetupListener(listener_b);

  // Node A: the sender. Advertises SensorData to Node B.
  osp::NetworkNode<TestPayload> node_a("sender", 10);
  auto start_a = node_a.Start();
  REQUIRE(start_a.has_value());

  // Accept from Node A in background
  osp::TcpSocket accepted_sock;
  std::thread accept_thread([&listener_b, &accepted_sock]() {
    auto r = listener_b.Accept();
    if (r.has_value()) {
      accepted_sock = static_cast<osp::TcpSocket&&>(r.value());
    }
  });

  auto ep_b = osp::Endpoint::FromString("127.0.0.1", port_b);
  auto adv_r = node_a.AdvertiseTo<SensorData>(ep_b);
  REQUIRE(adv_r.has_value());

  accept_thread.join();
  REQUIRE(accepted_sock.IsValid());

  // Wrap the accepted socket into a TcpTransport for Node B's receiving side
  osp::TcpTransport recv_transport;
  recv_transport.AcceptFrom(static_cast<osp::TcpSocket&&>(accepted_sock));
  REQUIRE(recv_transport.IsConnected());

  // Node A publishes a SensorData message.
  // The local Subscribe callback (from AdvertiseTo) will forward it over TCP.
  SensorData outgoing{42.0f, 777};
  bool published = node_a.Publish(SensorData{42.0f, 777});
  REQUIRE(published);

  // Process the bus so the subscription callback fires (sends via transport)
  node_a.SpinOnce();

  // Receive on Node B's transport
  osp::FrameHeader hdr;
  uint8_t payload_buf[256];
  auto recv_r = recv_transport.RecvFrame(hdr, payload_buf, sizeof(payload_buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(hdr.magic == osp::kFrameMagic);
  REQUIRE(hdr.sender_id == 10);

  SensorData received;
  bool ok = osp::Serializer<SensorData>::Deserialize(
      payload_buf, recv_r.value(), received);
  REQUIRE(ok);
  REQUIRE(received.temperature == outgoing.temperature);
  REQUIRE(received.sensor_id == outgoing.sensor_id);
}
