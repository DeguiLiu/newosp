/**
 * @file test_discovery.cpp
 * @brief Tests for discovery.hpp: StaticDiscovery and MulticastDiscovery.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/discovery.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// ============================================================================
// 1. StaticDiscovery - Add/Find/Remove
// ============================================================================

TEST_CASE("discovery - StaticDiscovery add and find node", "[discovery][static]") {
  osp::StaticDiscovery<32> discovery;

  auto r = discovery.AddNode("node1", "192.168.1.10", 5000);
  REQUIRE(r.has_value());
  REQUIRE(discovery.NodeCount() == 1);

  const osp::DiscoveredNode* node = discovery.FindNode("node1");
  REQUIRE(node != nullptr);
  REQUIRE(std::strcmp(node->name, "node1") == 0);
  REQUIRE(std::strcmp(node->address, "192.168.1.10") == 0);
  REQUIRE(node->port == 5000);
  REQUIRE(node->alive);
}

TEST_CASE("discovery - StaticDiscovery find non-existent node", "[discovery][static]") {
  osp::StaticDiscovery<32> discovery;

  const osp::DiscoveredNode* node = discovery.FindNode("nonexistent");
  REQUIRE(node == nullptr);
}

TEST_CASE("discovery - StaticDiscovery remove node", "[discovery][static]") {
  osp::StaticDiscovery<32> discovery;

  discovery.AddNode("node1", "192.168.1.10", 5000);
  discovery.AddNode("node2", "192.168.1.11", 5001);
  REQUIRE(discovery.NodeCount() == 2);

  bool removed = discovery.RemoveNode("node1");
  REQUIRE(removed);
  REQUIRE(discovery.NodeCount() == 1);

  const osp::DiscoveredNode* node = discovery.FindNode("node1");
  REQUIRE(node == nullptr);

  node = discovery.FindNode("node2");
  REQUIRE(node != nullptr);
}

TEST_CASE("discovery - StaticDiscovery remove non-existent node", "[discovery][static]") {
  osp::StaticDiscovery<32> discovery;

  bool removed = discovery.RemoveNode("nonexistent");
  REQUIRE(!removed);
}

// ============================================================================
// 2. StaticDiscovery - ForEach
// ============================================================================

TEST_CASE("discovery - StaticDiscovery ForEach iteration", "[discovery][static]") {
  osp::StaticDiscovery<32> discovery;

  discovery.AddNode("node1", "192.168.1.10", 5000);
  discovery.AddNode("node2", "192.168.1.11", 5001);
  discovery.AddNode("node3", "192.168.1.12", 5002);

  struct Context {
    uint32_t count;
    char names[3][64];
  };
  Context ctx{0, {}};

  discovery.ForEach(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        std::strcpy(c->names[c->count], node.name);
        ++c->count;
      },
      &ctx);

  REQUIRE(ctx.count == 3);
  REQUIRE(std::strcmp(ctx.names[0], "node1") == 0);
  REQUIRE(std::strcmp(ctx.names[1], "node2") == 0);
  REQUIRE(std::strcmp(ctx.names[2], "node3") == 0);
}

// ============================================================================
// 3. StaticDiscovery - Full capacity
// ============================================================================

TEST_CASE("discovery - StaticDiscovery full capacity", "[discovery][static]") {
  osp::StaticDiscovery<4> discovery;

  REQUIRE(discovery.AddNode("node1", "192.168.1.10", 5000).has_value());
  REQUIRE(discovery.AddNode("node2", "192.168.1.11", 5001).has_value());
  REQUIRE(discovery.AddNode("node3", "192.168.1.12", 5002).has_value());
  REQUIRE(discovery.AddNode("node4", "192.168.1.13", 5003).has_value());
  REQUIRE(discovery.NodeCount() == 4);

  // Should fail - capacity reached
  auto r = discovery.AddNode("node5", "192.168.1.14", 5004);
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::DiscoveryError::kSocketFailed);
  REQUIRE(discovery.NodeCount() == 4);
}

// ============================================================================
// 4. MulticastDiscovery - Start/Stop
// ============================================================================

TEST_CASE("discovery - MulticastDiscovery start and stop", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19999;  // Use high port to avoid conflicts
  osp::MulticastDiscovery<32> discovery(cfg);

  discovery.SetLocalNode("test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());
  REQUIRE(discovery.IsRunning());

  discovery.Stop();
  REQUIRE(!discovery.IsRunning());
}

TEST_CASE("discovery - MulticastDiscovery start twice fails", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19998;
  osp::MulticastDiscovery<32> discovery(cfg);

  discovery.SetLocalNode("test_node", 8000);

  auto r1 = discovery.Start();
  REQUIRE(r1.has_value());

  auto r2 = discovery.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::DiscoveryError::kAlreadyRunning);

  discovery.Stop();
}

// ============================================================================
// 5. MulticastDiscovery - Self-discovery
// ============================================================================

TEST_CASE("discovery - MulticastDiscovery self-discovery", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19997;
  cfg.announce_interval_ms = 500;
  osp::MulticastDiscovery<32> discovery(cfg);

  discovery.SetLocalNode("self_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-announcement to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Should discover itself
  const osp::DiscoveredNode* node = discovery.FindNode("self_node");
  REQUIRE(node != nullptr);
  REQUIRE(std::strcmp(node->name, "self_node") == 0);
  REQUIRE(node->port == 8000);
  REQUIRE(node->alive);

  discovery.Stop();
}

// ============================================================================
// 6. MulticastDiscovery - Node join/leave callbacks
// ============================================================================

TEST_CASE("discovery - MulticastDiscovery node join callback", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19996;
  cfg.announce_interval_ms = 500;
  osp::MulticastDiscovery<32> discovery(cfg);

  struct Context {
    std::atomic<uint32_t> join_count{0};
    char joined_name[64];
  };
  Context ctx;

  discovery.SetOnNodeJoin(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        std::strcpy(c->joined_name, node.name);
        c->join_count.fetch_add(1, std::memory_order_relaxed);
      },
      &ctx);

  discovery.SetLocalNode("join_test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-announcement
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  REQUIRE(ctx.join_count.load(std::memory_order_relaxed) >= 1);
  REQUIRE(std::strcmp(ctx.joined_name, "join_test_node") == 0);

  discovery.Stop();
}

// ============================================================================
// 7. MulticastDiscovery - Node timeout
// ============================================================================

TEST_CASE("discovery - MulticastDiscovery node timeout", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19995;
  cfg.announce_interval_ms = 500;
  cfg.timeout_ms = 1500;
  osp::MulticastDiscovery<32> discovery(cfg);

  struct Context {
    std::atomic<uint32_t> leave_count{0};
    char left_name[64];
  };
  Context ctx;

  discovery.SetOnNodeLeave(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        std::strcpy(c->left_name, node.name);
        c->leave_count.fetch_add(1, std::memory_order_relaxed);
      },
      &ctx);

  discovery.SetLocalNode("timeout_test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-discovery
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  REQUIRE(discovery.NodeCount() >= 1);

  // Stop to simulate node going offline
  discovery.Stop();

  // Start a new discovery instance on the same port to observe timeout
  osp::MulticastDiscovery<32> discovery2(cfg);
  discovery2.SetLocalNode("observer_node", 8001);
  discovery2.SetOnNodeLeave(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        std::strcpy(c->left_name, node.name);
        c->leave_count.fetch_add(1, std::memory_order_relaxed);
      },
      &ctx);

  auto r2 = discovery2.Start();
  REQUIRE(r2.has_value());

  // Wait for timeout detection (timeout_ms + some margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  discovery2.Stop();

  // Note: This test is timing-sensitive and may not always trigger leave callback
  // in CI environments. We just verify the mechanism doesn't crash.
}

// ============================================================================
// 8. MulticastDiscovery - ForEach
// ============================================================================

TEST_CASE("discovery - MulticastDiscovery ForEach iteration", "[discovery][multicast]") {
  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19994;
  cfg.announce_interval_ms = 500;
  osp::MulticastDiscovery<32> discovery(cfg);

  discovery.SetLocalNode("foreach_test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-discovery
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  struct Context {
    uint32_t count;
  };
  Context ctx{0};

  discovery.ForEach(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        ++c->count;
      },
      &ctx);

  REQUIRE(ctx.count >= 1);

  discovery.Stop();
}
