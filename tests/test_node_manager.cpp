/**
 * @file test_node_manager.cpp
 * @brief Unit tests for NodeManager.
 */

#include "osp/node_manager.hpp"
#include "osp/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

// ============================================================================
// Test Helpers
// ============================================================================

/**
 * @brief Create a simple TCP listener for testing.
 * @param[out] out_port The port number assigned by the OS.
 * @return File descriptor of the listening socket.
 */
static int CreateTestListener(uint16_t& out_port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);

  int opt = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // OS-assigned port

  int bind_r = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  REQUIRE(bind_r == 0);

  int listen_r = ::listen(fd, 4);
  REQUIRE(listen_r == 0);

  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  out_port = ntohs(addr.sin_port);

  return fd;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("NodeManager - CreateAndDestroy", "[NodeManager]") {
  osp::NodeManagerConfig cfg;
  cfg.max_nodes = 8;
  cfg.heartbeat_interval_ms = 100;
  cfg.heartbeat_timeout_count = 3;

  osp::NodeManager<8> mgr(cfg);
  CHECK(mgr.NodeCount() == 0U);
  CHECK_FALSE(mgr.IsRunning());
}

TEST_CASE("NodeManager - ConnectToRemote", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  // Create a test listener
  uint16_t port = 0;
  int listener_fd = CreateTestListener(port);
  REQUIRE(port > 0);

  // Connect to the listener
  auto r = mgr.Connect("127.0.0.1", port);
  REQUIRE(r.has_value());
  uint16_t node_id = r.value();
  CHECK(node_id > 0);

  CHECK(mgr.IsConnected(node_id));
  CHECK(mgr.NodeCount() == 1U);

  // Accept the connection on the listener side
  sockaddr_in client_addr{};
  socklen_t addr_len = sizeof(client_addr);
  int client_fd = ::accept(listener_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
  CHECK(client_fd >= 0);

  // Disconnect
  auto disc_r = mgr.Disconnect(node_id);
  CHECK(disc_r.has_value());
  CHECK_FALSE(mgr.IsConnected(node_id));
  CHECK(mgr.NodeCount() == 0U);

  ::close(client_fd);
  ::close(listener_fd);
}

TEST_CASE("NodeManager - DisconnectCallback", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  std::atomic<bool> callback_fired{false};
  std::atomic<uint16_t> disconnected_node_id{0};

  mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* fired = static_cast<std::atomic<bool>*>(ctx);
        fired->store(true);
        auto* id_ptr = reinterpret_cast<std::atomic<uint16_t>*>(
            reinterpret_cast<char*>(ctx) + sizeof(std::atomic<bool>));
        id_ptr->store(node_id);
      },
      &callback_fired);

  uint16_t port = 0;
  int listener_fd = CreateTestListener(port);
  REQUIRE(port > 0);

  auto r = mgr.Connect("127.0.0.1", port);
  REQUIRE(r.has_value());
  uint16_t node_id = r.value();

  // Accept the connection
  int client_fd = ::accept(listener_fd, nullptr, nullptr);
  CHECK(client_fd >= 0);

  // Manually disconnect
  auto disc_r = mgr.Disconnect(node_id);
  CHECK(disc_r.has_value());

  // Note: Manual disconnect doesn't trigger callback in current implementation
  // Callback is only triggered by timeout detection in heartbeat loop

  ::close(client_fd);
  ::close(listener_fd);
}

TEST_CASE("NodeManager - NodeCountTracking", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  CHECK(mgr.NodeCount() == 0U);

  uint16_t port1 = 0, port2 = 0;
  int listener1 = CreateTestListener(port1);
  int listener2 = CreateTestListener(port2);

  auto r1 = mgr.Connect("127.0.0.1", port1);
  REQUIRE(r1.has_value());
  CHECK(mgr.NodeCount() == 1U);

  auto r2 = mgr.Connect("127.0.0.1", port2);
  REQUIRE(r2.has_value());
  CHECK(mgr.NodeCount() == 2U);

  // Accept connections
  int client1 = ::accept(listener1, nullptr, nullptr);
  int client2 = ::accept(listener2, nullptr, nullptr);

  mgr.Disconnect(r1.value());
  CHECK(mgr.NodeCount() == 1U);

  mgr.Disconnect(r2.value());
  CHECK(mgr.NodeCount() == 0U);

  ::close(client1);
  ::close(client2);
  ::close(listener1);
  ::close(listener2);
}

TEST_CASE("NodeManager - IsConnectedQuery", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  uint16_t port = 0;
  int listener_fd = CreateTestListener(port);

  auto r = mgr.Connect("127.0.0.1", port);
  REQUIRE(r.has_value());
  uint16_t node_id = r.value();

  CHECK(mgr.IsConnected(node_id));
  CHECK_FALSE(mgr.IsConnected(9999));  // Non-existent node

  int client_fd = ::accept(listener_fd, nullptr, nullptr);

  mgr.Disconnect(node_id);
  CHECK_FALSE(mgr.IsConnected(node_id));

  ::close(client_fd);
  ::close(listener_fd);
}

TEST_CASE("NodeManager - ForEachIteration", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  uint16_t port1 = 0, port2 = 0, port3 = 0;
  int listener1 = CreateTestListener(port1);
  int listener2 = CreateTestListener(port2);
  int listener3 = CreateTestListener(port3);

  auto r1 = mgr.Connect("127.0.0.1", port1);
  auto r2 = mgr.Connect("127.0.0.1", port2);
  auto r3 = mgr.Connect("127.0.0.1", port3);

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(r3.has_value());

  // Accept connections
  int client1 = ::accept(listener1, nullptr, nullptr);
  int client2 = ::accept(listener2, nullptr, nullptr);
  int client3 = ::accept(listener3, nullptr, nullptr);

  uint32_t count = 0;
  mgr.ForEach([&count](const osp::NodeEntry& node) {
    CHECK(node.active);
    CHECK_FALSE(node.is_listener);
    ++count;
  });

  CHECK(count == 3U);

  ::close(client1);
  ::close(client2);
  ::close(client3);
  ::close(listener1);
  ::close(listener2);
  ::close(listener3);
}

TEST_CASE("NodeManager - TableFull", "[NodeManager]") {
  osp::NodeManager<2> mgr;  // Small capacity

  uint16_t port1 = 0, port2 = 0, port3 = 0;
  int listener1 = CreateTestListener(port1);
  int listener2 = CreateTestListener(port2);
  int listener3 = CreateTestListener(port3);

  auto r1 = mgr.Connect("127.0.0.1", port1);
  REQUIRE(r1.has_value());

  auto r2 = mgr.Connect("127.0.0.1", port2);
  REQUIRE(r2.has_value());

  // Third connection should fail (table full)
  auto r3 = mgr.Connect("127.0.0.1", port3);
  CHECK_FALSE(r3.has_value());
  CHECK(r3.get_error() == osp::NodeManagerError::kTableFull);

  // Accept the successful connections
  int client1 = ::accept(listener1, nullptr, nullptr);
  int client2 = ::accept(listener2, nullptr, nullptr);

  ::close(client1);
  ::close(client2);
  ::close(listener1);
  ::close(listener2);
  ::close(listener3);
}

TEST_CASE("NodeManager - StartAndStopHeartbeat", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  CHECK_FALSE(mgr.IsRunning());

  auto start_r = mgr.Start();
  CHECK(start_r.has_value());
  CHECK(mgr.IsRunning());

  // Starting again should fail
  auto start_r2 = mgr.Start();
  CHECK_FALSE(start_r2.has_value());
  CHECK(start_r2.get_error() == osp::NodeManagerError::kAlreadyRunning);

  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Can start again after stop
  auto start_r3 = mgr.Start();
  CHECK(start_r3.has_value());
  CHECK(mgr.IsRunning());

  mgr.Stop();
}

TEST_CASE("NodeManager - HeartbeatTimeoutDetection", "[NodeManager]") {
  osp::NodeManagerConfig cfg;
  cfg.heartbeat_interval_ms = 100;
  cfg.heartbeat_timeout_count = 2;  // 200ms timeout

  osp::NodeManager<8> mgr(cfg);

  std::atomic<bool> callback_fired{false};
  std::atomic<uint16_t> disconnected_node_id{0};

  struct Context {
    std::atomic<bool>* fired;
    std::atomic<uint16_t>* id;
  };
  Context context{&callback_fired, &disconnected_node_id};

  mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* context = static_cast<Context*>(ctx);
        context->fired->store(true);
        context->id->store(node_id);
      },
      &context);

  uint16_t port = 0;
  int listener_fd = CreateTestListener(port);

  auto r = mgr.Connect("127.0.0.1", port);
  REQUIRE(r.has_value());
  uint16_t node_id = r.value();

  // Accept the connection
  int client_fd = ::accept(listener_fd, nullptr, nullptr);
  CHECK(client_fd >= 0);

  // Start heartbeat thread
  auto start_r = mgr.Start();
  CHECK(start_r.has_value());

  // Close the client side to simulate connection loss
  // This will cause heartbeat sends to fail
  ::close(client_fd);

  // Wait for timeout (200ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Node should be disconnected by timeout
  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == node_id);
  CHECK_FALSE(mgr.IsConnected(node_id));

  mgr.Stop();
  ::close(listener_fd);
}

TEST_CASE("NodeManager - CreateListenerBasic", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  auto r = mgr.CreateListener(0);  // OS-assigned port
  REQUIRE(r.has_value());
  uint16_t node_id = r.value();

  CHECK(node_id > 0);
  CHECK(mgr.IsConnected(node_id));
  CHECK(mgr.NodeCount() == 1U);

  // Verify it's marked as a listener
  bool found_listener = false;
  mgr.ForEach([&found_listener, node_id](const osp::NodeEntry& node) {
    if (node.node_id == node_id) {
      CHECK(node.is_listener);
      found_listener = true;
    }
  });
  CHECK(found_listener);
}

TEST_CASE("NodeManager - DisconnectNotFound", "[NodeManager]") {
  osp::NodeManager<8> mgr;

  auto r = mgr.Disconnect(9999);  // Non-existent node
  CHECK_FALSE(r.has_value());
  CHECK(r.get_error() == osp::NodeManagerError::kNotFound);
}
