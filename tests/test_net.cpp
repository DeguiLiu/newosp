/**
 * @file test_net.cpp
 * @brief Tests for osp::net (sockpp integration layer).
 */

#include <catch2/catch_test_macros.hpp>

#ifdef OSP_HAS_SOCKPP

#include "osp/net.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>

using namespace osp::net;

TEST_CASE("TcpServer can listen and accept connections", "[net][tcp]") {
  // Create server
  auto server_r = TcpServer::Listen(0);  // OS-assigned port
  REQUIRE(server_r.has_value());
  auto server = std::move(server_r.value());
  REQUIRE(server.IsOpen());

  // Get the actual port
  int server_fd = server.Fd();
  REQUIRE(server_fd >= 0);

  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(server_fd, reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);
  REQUIRE(port > 0);

  // Connect from client in a separate thread
  std::atomic<bool> client_success{true};
  std::thread client_thread([port, &client_success]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto client_r = TcpClient::Connect("127.0.0.1", port, 1000);
    if (!client_r.has_value()) {
      client_success.store(false, std::memory_order_relaxed);
      return;
    }
    auto client = std::move(client_r.value());
    if (!client.IsOpen()) {
      client_success.store(false, std::memory_order_relaxed);
      return;
    }

    // Send a message
    const char* msg = "Hello";
    auto send_r = client.Send(msg, std::strlen(msg));
    if (!send_r.has_value() || send_r.value() != std::strlen(msg)) {
      client_success.store(false, std::memory_order_relaxed);
      return;
    }

    // Receive response
    char buf[64] = {};
    auto recv_r = client.Recv(buf, sizeof(buf));
    if (!recv_r.has_value() || recv_r.value() != 5 || std::strcmp(buf, "World") != 0) {
      client_success.store(false, std::memory_order_relaxed);
      return;
    }
  });

  // Accept connection
  auto accepted_r = server.Accept();
  REQUIRE(accepted_r.has_value());
  auto accepted = std::move(accepted_r.value());
  REQUIRE(accepted.IsOpen());

  // Receive message
  char buf[64] = {};
  auto recv_r = accepted.Recv(buf, sizeof(buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() == 5);
  REQUIRE(std::strcmp(buf, "Hello") == 0);

  // Send response
  const char* response = "World";
  auto send_r = accepted.Send(response, std::strlen(response));
  REQUIRE(send_r.has_value());
  REQUIRE(send_r.value() == std::strlen(response));

  client_thread.join();
  REQUIRE(client_success.load(std::memory_order_relaxed));
}

TEST_CASE("TcpClient can connect to server", "[net][tcp]") {
  // Start server
  auto server_r = TcpServer::Listen(0);
  REQUIRE(server_r.has_value());
  auto server = std::move(server_r.value());

  // Get port
  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(server.Fd(), reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);

  // Connect
  auto client_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());
  REQUIRE(client.IsOpen());
  REQUIRE(client.Fd() >= 0);
}

TEST_CASE("TcpClient fails to connect to invalid port", "[net][tcp]") {
  // Try to connect to a port that's not listening
  auto client_r = TcpClient::Connect("127.0.0.1", 1, 100);
  REQUIRE(!client_r.has_value());
  REQUIRE(client_r.get_error() == NetError::kConnectFailed);
}

TEST_CASE("TcpClient can send and receive data", "[net][tcp]") {
  // Start server
  auto server_r = TcpServer::Listen(0);
  REQUIRE(server_r.has_value());
  auto server = std::move(server_r.value());

  // Get port
  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(server.Fd(), reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);

  // Server thread
  std::atomic<bool> server_success{true};
  std::thread server_thread([&server, &server_success]() {
    auto accepted_r = server.Accept();
    if (!accepted_r.has_value()) {
      server_success.store(false, std::memory_order_relaxed);
      return;
    }
    auto accepted = std::move(accepted_r.value());

    // Echo server
    char buf[1024];
    auto recv_r = accepted.Recv(buf, sizeof(buf));
    if (!recv_r.has_value()) {
      server_success.store(false, std::memory_order_relaxed);
      return;
    }
    size_t n = recv_r.value();

    auto send_r = accepted.Send(buf, n);
    if (!send_r.has_value() || send_r.value() != n) {
      server_success.store(false, std::memory_order_relaxed);
      return;
    }
  });

  // Client
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto client_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  const char* msg = "Test message 123";
  auto send_r = client.Send(msg, std::strlen(msg));
  REQUIRE(send_r.has_value());
  REQUIRE(send_r.value() == std::strlen(msg));

  char buf[1024] = {};
  auto recv_r = client.Recv(buf, sizeof(buf));
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() == std::strlen(msg));
  REQUIRE(std::strcmp(buf, msg) == 0);

  server_thread.join();
  REQUIRE(server_success.load(std::memory_order_relaxed));
}

TEST_CASE("UdpPeer can bind to a port", "[net][udp]") {
  auto peer_r = UdpPeer::Bind(0);  // OS-assigned port
  REQUIRE(peer_r.has_value());
  auto peer = std::move(peer_r.value());
  REQUIRE(peer.Fd() >= 0);
}

TEST_CASE("UdpPeer can create unbound socket", "[net][udp]") {
  auto peer_r = UdpPeer::Create();
  REQUIRE(peer_r.has_value());
  auto peer = std::move(peer_r.value());
  REQUIRE(peer.Fd() >= 0);
}

TEST_CASE("UdpPeer can send and receive datagrams", "[net][udp]") {
  // Create receiver
  auto receiver_r = UdpPeer::Bind(0);
  REQUIRE(receiver_r.has_value());
  auto receiver = std::move(receiver_r.value());

  // Get receiver port
  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(receiver.Fd(), reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);

  // Create sender
  auto sender_r = UdpPeer::Create();
  REQUIRE(sender_r.has_value());
  auto sender = std::move(sender_r.value());

  // Send datagram
  const char* msg = "UDP test";
  auto send_r = sender.SendTo(msg, std::strlen(msg), "127.0.0.1", port);
  REQUIRE(send_r.has_value());
  REQUIRE(send_r.value() == std::strlen(msg));

  // Receive datagram
  char buf[1024] = {};
  char from_host[64] = {};
  uint16_t from_port = 0;
  auto recv_r = receiver.RecvFrom(buf, sizeof(buf), from_host, sizeof(from_host),
                                   &from_port);
  REQUIRE(recv_r.has_value());
  REQUIRE(recv_r.value() == std::strlen(msg));
  REQUIRE(std::strcmp(buf, msg) == 0);
  REQUIRE(from_port > 0);
}

TEST_CASE("TcpClient move semantics", "[net][tcp]") {
  auto server_r = TcpServer::Listen(0);
  REQUIRE(server_r.has_value());
  auto server = std::move(server_r.value());

  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(server.Fd(), reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);

  auto client1_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client1_r.has_value());
  auto client1 = std::move(client1_r.value());
  REQUIRE(client1.IsOpen());

  int fd1 = client1.Fd();
  REQUIRE(fd1 >= 0);

  // Move construct
  TcpClient client2(std::move(client1));
  REQUIRE(client2.IsOpen());
  REQUIRE(client2.Fd() == fd1);
  REQUIRE(!client1.IsOpen());  // Moved-from state

  // Move assign
  TcpClient client3;
  client3 = std::move(client2);
  REQUIRE(client3.IsOpen());
  REQUIRE(client3.Fd() == fd1);
  REQUIRE(!client2.IsOpen());
}

TEST_CASE("TcpServer move semantics", "[net][tcp]") {
  auto server1_r = TcpServer::Listen(0);
  REQUIRE(server1_r.has_value());
  auto server1 = std::move(server1_r.value());
  REQUIRE(server1.IsOpen());

  int fd1 = server1.Fd();
  REQUIRE(fd1 >= 0);

  // Move construct
  TcpServer server2(std::move(server1));
  REQUIRE(server2.IsOpen());
  REQUIRE(server2.Fd() == fd1);
  REQUIRE(!server1.IsOpen());

  // Move assign
  TcpServer server3;
  server3 = std::move(server2);
  REQUIRE(server3.IsOpen());
  REQUIRE(server3.Fd() == fd1);
  REQUIRE(!server2.IsOpen());
}

TEST_CASE("UdpPeer move semantics", "[net][udp]") {
  auto peer1_r = UdpPeer::Bind(0);
  REQUIRE(peer1_r.has_value());
  auto peer1 = std::move(peer1_r.value());

  int fd1 = peer1.Fd();
  REQUIRE(fd1 >= 0);

  // Move construct
  UdpPeer peer2(std::move(peer1));
  REQUIRE(peer2.Fd() == fd1);

  // Move assign
  UdpPeer peer3;
  peer3 = std::move(peer2);
  REQUIRE(peer3.Fd() == fd1);
}

TEST_CASE("Multiple clients can connect to server", "[net][tcp]") {
  auto server_r = TcpServer::Listen(0);
  REQUIRE(server_r.has_value());
  auto server = std::move(server_r.value());

  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  REQUIRE(::getsockname(server.Fd(), reinterpret_cast<sockaddr*>(&addr),
                        &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);

  // Connect 3 clients
  auto client1_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client1_r.has_value());

  auto client2_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client2_r.has_value());

  auto client3_r = TcpClient::Connect("127.0.0.1", port, 1000);
  REQUIRE(client3_r.has_value());

  // Accept all 3
  auto accepted1_r = server.Accept();
  REQUIRE(accepted1_r.has_value());

  auto accepted2_r = server.Accept();
  REQUIRE(accepted2_r.has_value());

  auto accepted3_r = server.Accept();
  REQUIRE(accepted3_r.has_value());
}

#endif  // OSP_HAS_SOCKPP
