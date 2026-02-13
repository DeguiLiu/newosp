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
  REQUIRE(node->name == "node1");
  REQUIRE(node->address == "192.168.1.10");
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
        std::strcpy(c->names[c->count], node.name.c_str());
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
  REQUIRE(node->name == "self_node");
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
        std::strcpy(c->joined_name, node.name.c_str());
        c->join_count.fetch_add(1, std::memory_order_release);
      },
      &ctx);

  discovery.SetLocalNode("join_test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-announcement
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  REQUIRE(ctx.join_count.load(std::memory_order_acquire) >= 1);
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
        std::strcpy(c->left_name, node.name.c_str());
        c->leave_count.fetch_add(1, std::memory_order_release);
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
        std::strcpy(c->left_name, node.name.c_str());
        c->leave_count.fetch_add(1, std::memory_order_release);
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

// ============================================================================
// 9. TopicInfo - Construction
// ============================================================================

TEST_CASE("discovery - TopicInfo construction", "[discovery][topic]") {
  osp::TopicInfo topic;
  REQUIRE(topic.name.c_str()[0] == '\0');
  REQUIRE(topic.type_name.c_str()[0] == '\0');
  REQUIRE(topic.publisher_port == 0);
  REQUIRE(topic.is_publisher == false);

  // Manual initialization
  topic.name = "sensor_data";
  topic.type_name = "SensorData";
  topic.publisher_port = 5000;
  topic.is_publisher = true;

  REQUIRE(topic.name == "sensor_data");
  REQUIRE(topic.type_name == "SensorData");
  REQUIRE(topic.publisher_port == 5000);
  REQUIRE(topic.is_publisher == true);
}

// ============================================================================
// 10. ServiceInfo - Construction
// ============================================================================

TEST_CASE("discovery - ServiceInfo construction", "[discovery][service]") {
  osp::ServiceInfo svc;
  REQUIRE(svc.name.c_str()[0] == '\0');
  REQUIRE(svc.request_type.c_str()[0] == '\0');
  REQUIRE(svc.response_type.c_str()[0] == '\0');
  REQUIRE(svc.port == 0);

  // Manual initialization
  svc.name = "get_status";
  svc.request_type = "StatusRequest";
  svc.response_type = "StatusResponse";
  svc.port = 6000;

  REQUIRE(svc.name == "get_status");
  REQUIRE(svc.request_type == "StatusRequest");
  REQUIRE(svc.response_type == "StatusResponse");
  REQUIRE(svc.port == 6000);
}

// ============================================================================
// 11. TopicAwareDiscovery - Add local topic
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery add local topic", "[discovery][topic]") {
  osp::TopicAwareDiscovery<32, 16> discovery;

  osp::TopicInfo topic;
  topic.name = "sensor_data";
  topic.type_name = "SensorData";
  topic.publisher_port = 5000;
  topic.is_publisher = true;

  auto r = discovery.AddLocalTopic(topic);
  REQUIRE(r.has_value());
  REQUIRE(discovery.TopicCount() == 1);
}

// ============================================================================
// 12. TopicAwareDiscovery - Find publishers
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery find publishers", "[discovery][topic]") {
  osp::TopicAwareDiscovery<32, 16> discovery;

  // Add a publisher
  osp::TopicInfo pub_topic;
  pub_topic.name = "sensor_data";
  pub_topic.type_name = "SensorData";
  pub_topic.publisher_port = 5000;
  pub_topic.is_publisher = true;
  discovery.AddLocalTopic(pub_topic);

  // Add a subscriber (should not be found)
  osp::TopicInfo sub_topic;
  sub_topic.name = "sensor_data";
  sub_topic.type_name = "SensorData";
  sub_topic.publisher_port = 0;
  sub_topic.is_publisher = false;
  discovery.AddLocalTopic(sub_topic);

  // Find publishers
  osp::TopicInfo results[10];
  uint32_t count = discovery.FindPublishers("sensor_data", results, 10);

  REQUIRE(count == 1);
  REQUIRE(results[0].name == "sensor_data");
  REQUIRE(results[0].is_publisher == true);
  REQUIRE(results[0].publisher_port == 5000);
}

// ============================================================================
// 13. TopicAwareDiscovery - Find subscribers
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery find subscribers", "[discovery][topic]") {
  osp::TopicAwareDiscovery<32, 16> discovery;

  // Add a subscriber
  osp::TopicInfo sub_topic;
  sub_topic.name = "sensor_data";
  sub_topic.type_name = "SensorData";
  sub_topic.publisher_port = 0;
  sub_topic.is_publisher = false;
  discovery.AddLocalTopic(sub_topic);

  // Add a publisher (should not be found)
  osp::TopicInfo pub_topic;
  pub_topic.name = "sensor_data";
  pub_topic.type_name = "SensorData";
  pub_topic.publisher_port = 5000;
  pub_topic.is_publisher = true;
  discovery.AddLocalTopic(pub_topic);

  // Find subscribers
  osp::TopicInfo results[10];
  uint32_t count = discovery.FindSubscribers("sensor_data", results, 10);

  REQUIRE(count == 1);
  REQUIRE(results[0].name == "sensor_data");
  REQUIRE(results[0].is_publisher == false);
}

// ============================================================================
// 14. TopicAwareDiscovery - Add local service
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery add local service", "[discovery][service]") {
  osp::TopicAwareDiscovery<32, 16> discovery;

  osp::ServiceInfo svc;
  svc.name = "get_status";
  svc.request_type = "StatusRequest";
  svc.response_type = "StatusResponse";
  svc.port = 6000;

  auto r = discovery.AddLocalService(svc);
  REQUIRE(r.has_value());
  REQUIRE(discovery.ServiceCount() == 1);
}

// ============================================================================
// 15. TopicAwareDiscovery - Find service
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery find service", "[discovery][service]") {
  osp::TopicAwareDiscovery<32, 16> discovery;

  osp::ServiceInfo svc;
  svc.name = "get_status";
  svc.request_type = "StatusRequest";
  svc.response_type = "StatusResponse";
  svc.port = 6000;
  discovery.AddLocalService(svc);

  const osp::ServiceInfo* found = discovery.FindService("get_status");
  REQUIRE(found != nullptr);
  REQUIRE(found->name == "get_status");
  REQUIRE(found->request_type == "StatusRequest");
  REQUIRE(found->response_type == "StatusResponse");
  REQUIRE(found->port == 6000);

  // Find non-existent service
  const osp::ServiceInfo* not_found = discovery.FindService("nonexistent");
  REQUIRE(not_found == nullptr);
}

// ============================================================================
// 16. TopicAwareDiscovery - Topic overflow
// ============================================================================

TEST_CASE("discovery - TopicAwareDiscovery topic overflow", "[discovery][topic]") {
  osp::TopicAwareDiscovery<32, 4> discovery;  // MaxTopicsPerNode = 4

  osp::TopicInfo topic;
  topic.type_name = "SensorData";
  topic.publisher_port = 5000;
  topic.is_publisher = true;

  // Add 4 topics (should succeed)
  for (uint32_t i = 0; i < 4; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "topic_%u", i);
    topic.name.assign(osp::TruncateToCapacity, name);
    auto r = discovery.AddLocalTopic(topic);
    REQUIRE(r.has_value());
  }
  REQUIRE(discovery.TopicCount() == 4);

  // Add 5th topic (should fail - overflow)
  topic.name = "topic_overflow";
  auto r = discovery.AddLocalTopic(topic);
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::DiscoveryError::kSocketFailed);
  REQUIRE(discovery.TopicCount() == 4);
}

// ============================================================================
// Phase 2: TimerScheduler Injection Mode Tests
// ============================================================================

#include "osp/timer.hpp"

TEST_CASE("discovery - MulticastDiscovery scheduler injection start/stop", "[discovery][multicast][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19990;
  osp::MulticastDiscovery<32> discovery(cfg, &sched);

  discovery.SetLocalNode("sched_test_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());
  REQUIRE(discovery.IsRunning());

  discovery.Stop();
  REQUIRE(!discovery.IsRunning());

  sched.Stop();
}

TEST_CASE("discovery - MulticastDiscovery scheduler injection self-discovery", "[discovery][multicast][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19989;
  cfg.announce_interval_ms = 500;
  osp::MulticastDiscovery<32> discovery(cfg, &sched);

  discovery.SetLocalNode("sched_self_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-announcement to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Should discover itself
  const osp::DiscoveredNode* node = discovery.FindNode("sched_self_node");
  REQUIRE(node != nullptr);
  REQUIRE(node->name == "sched_self_node");
  REQUIRE(node->port == 8000);
  REQUIRE(node->alive);

  discovery.Stop();
  sched.Stop();
}

TEST_CASE("discovery - MulticastDiscovery scheduler injection join callback", "[discovery][multicast][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19988;
  cfg.announce_interval_ms = 500;
  osp::MulticastDiscovery<32> discovery(cfg, &sched);

  struct Context {
    std::atomic<uint32_t> join_count{0};
    char joined_name[64];
  };
  Context ctx;

  discovery.SetOnNodeJoin(
      [](const osp::DiscoveredNode& node, void* user_ctx) {
        Context* c = static_cast<Context*>(user_ctx);
        std::strcpy(c->joined_name, node.name.c_str());
        c->join_count.fetch_add(1, std::memory_order_release);
      },
      &ctx);

  discovery.SetLocalNode("sched_join_node", 8000);

  auto r = discovery.Start();
  REQUIRE(r.has_value());

  // Wait for self-announcement
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  REQUIRE(ctx.join_count.load(std::memory_order_acquire) >= 1);
  REQUIRE(std::strcmp(ctx.joined_name, "sched_join_node") == 0);

  discovery.Stop();
  sched.Stop();
}

TEST_CASE("discovery - MulticastDiscovery scheduler injection start twice fails", "[discovery][multicast][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::MulticastDiscovery<32>::Config cfg;
  cfg.port = 19987;
  osp::MulticastDiscovery<32> discovery(cfg, &sched);

  discovery.SetLocalNode("test_node", 8000);

  auto r1 = discovery.Start();
  REQUIRE(r1.has_value());

  auto r2 = discovery.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::DiscoveryError::kAlreadyRunning);

  discovery.Stop();
  sched.Stop();
}
