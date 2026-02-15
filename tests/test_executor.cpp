/**
 * @file test_executor.cpp
 * @brief Catch2 tests for osp::SingleThreadExecutor, StaticExecutor,
 *        and PinnedExecutor.
 */

#include "osp/executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <variant>

// ============================================================================
// Test message types
// ============================================================================

// NOTE: structs must be >= 8 bytes to avoid GCC 14 wide-read optimization
// triggering ASan stack-buffer-overflow (8-byte memcpy on smaller struct).
struct TestMsg {
  int value;
  uint32_t reserved = 0;
};

using TestPayload = std::variant<TestMsg>;
using TestBus = osp::AsyncBus<TestPayload>;

// ============================================================================
// Bus reset helper
// ============================================================================

struct ExecutorBusFixture {
  ExecutorBusFixture() { TestBus::Instance().Reset(); }
  ~ExecutorBusFixture() { TestBus::Instance().Reset(); }
};

// ============================================================================
// SingleThreadExecutor tests
// ============================================================================

TEST_CASE("executor - SingleThreadExecutor add nodes", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node_a("node_a", 1);
  osp::Node<TestPayload> node_b("node_b", 2);

  osp::SingleThreadExecutor<TestPayload> exec;

  SECTION("add nodes successfully") {
    REQUIRE(exec.AddNode(node_a));
    REQUIRE(exec.AddNode(node_b));
    REQUIRE(exec.NodeCount() == 2);
  }

  SECTION("duplicate node rejected") {
    REQUIRE(exec.AddNode(node_a));
    REQUIRE_FALSE(exec.AddNode(node_a));
    REQUIRE(exec.NodeCount() == 1);
  }

  SECTION("returns false when full") {
    // Fill up with unique nodes
    osp::Node<TestPayload> pool[OSP_EXECUTOR_MAX_NODES] = {
        osp::Node<TestPayload>("n0", 100),
        osp::Node<TestPayload>("n1", 101),
        osp::Node<TestPayload>("n2", 102),
        osp::Node<TestPayload>("n3", 103),
        osp::Node<TestPayload>("n4", 104),
        osp::Node<TestPayload>("n5", 105),
        osp::Node<TestPayload>("n6", 106),
        osp::Node<TestPayload>("n7", 107),
        osp::Node<TestPayload>("n8", 108),
        osp::Node<TestPayload>("n9", 109),
        osp::Node<TestPayload>("n10", 110),
        osp::Node<TestPayload>("n11", 111),
        osp::Node<TestPayload>("n12", 112),
        osp::Node<TestPayload>("n13", 113),
        osp::Node<TestPayload>("n14", 114),
        osp::Node<TestPayload>("n15", 115),
    };
    for (uint32_t i = 0; i < OSP_EXECUTOR_MAX_NODES; ++i) {
      REQUIRE(exec.AddNode(pool[i]));
    }
    REQUIRE(exec.NodeCount() == OSP_EXECUTOR_MAX_NODES);

    osp::Node<TestPayload> extra("extra", 999);
    REQUIRE_FALSE(exec.AddNode(extra));
  }
}

TEST_CASE("executor - SingleThreadExecutor SpinOnce processes messages",
          "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> sender("sender", 1);
  osp::Node<TestPayload> receiver("receiver", 2);

  std::atomic<int> received_value{0};
  auto sub_result =
      receiver.Subscribe<TestMsg>([&](const TestMsg& msg, const auto&) {
        received_value.store(msg.value, std::memory_order_relaxed);
      });
  REQUIRE(sub_result.has_value());

  osp::SingleThreadExecutor<TestPayload> exec;
  exec.AddNode(sender);
  exec.AddNode(receiver);

  // Publish a message
  REQUIRE(sender.Publish(TestMsg{42}));

  // Process it
  uint32_t count = exec.SpinOnce();
  REQUIRE(count == 1);
  REQUIRE(received_value.load(std::memory_order_relaxed) == 42);
}

TEST_CASE("executor - SingleThreadExecutor Stop terminates Spin",
          "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node("node", 1);

  osp::SingleThreadExecutor<TestPayload> exec;
  exec.AddNode(node);

  REQUIRE_FALSE(exec.IsRunning());

  // Start Spin in a separate thread
  std::thread spin_thread([&exec]() { exec.Spin(); });

  // Give it a moment to start
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(exec.IsRunning());

  // Stop it
  exec.Stop();
  spin_thread.join();

  REQUIRE_FALSE(exec.IsRunning());
}

// ============================================================================
// StaticExecutor tests
// ============================================================================

TEST_CASE("executor - StaticExecutor Start/Stop lifecycle", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node("node", 1);

  osp::StaticExecutor<TestPayload> exec;
  exec.AddNode(node);

  REQUIRE_FALSE(exec.IsRunning());

  exec.Start();
  REQUIRE(exec.IsRunning());

  // Allow the dispatch thread to run briefly
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  exec.Stop();
  REQUIRE_FALSE(exec.IsRunning());
}

TEST_CASE("executor - StaticExecutor processes messages in background",
          "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> sender("sender", 1);
  osp::Node<TestPayload> receiver("receiver", 2);

  std::atomic<int> received_count{0};
  auto sub_result =
      receiver.Subscribe<TestMsg>([&](const TestMsg&, const auto&) {
        received_count.fetch_add(1, std::memory_order_relaxed);
      });
  REQUIRE(sub_result.has_value());

  osp::StaticExecutor<TestPayload> exec;
  exec.AddNode(sender);
  exec.AddNode(receiver);

  exec.Start();

  // Publish several messages
  for (int i = 0; i < 5; ++i) {
    sender.Publish(TestMsg{i});
  }

  // Wait for dispatch thread to process
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (received_count.load(std::memory_order_relaxed) < 5 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  exec.Stop();

  REQUIRE(received_count.load(std::memory_order_relaxed) == 5);
}

// ============================================================================
// PinnedExecutor tests
// ============================================================================

TEST_CASE("executor - PinnedExecutor Start/Stop lifecycle", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node("node", 1);

  // Use core 0 (should be valid on any system)
  osp::PinnedExecutor<TestPayload> exec(0);
  exec.AddNode(node);

  REQUIRE_FALSE(exec.IsRunning());

  exec.Start();
  REQUIRE(exec.IsRunning());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  exec.Stop();
  REQUIRE_FALSE(exec.IsRunning());
}

// ============================================================================
// RemoveNode tests
// ============================================================================

TEST_CASE("executor - RemoveNode works correctly", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node_a("node_a", 1);
  osp::Node<TestPayload> node_b("node_b", 2);
  osp::Node<TestPayload> node_c("node_c", 3);

  osp::SingleThreadExecutor<TestPayload> exec;
  exec.AddNode(node_a);
  exec.AddNode(node_b);
  exec.AddNode(node_c);

  REQUIRE(exec.NodeCount() == 3);

  SECTION("remove middle node") {
    REQUIRE(exec.RemoveNode(node_b));
    REQUIRE(exec.NodeCount() == 2);
  }

  SECTION("remove first node") {
    REQUIRE(exec.RemoveNode(node_a));
    REQUIRE(exec.NodeCount() == 2);
  }

  SECTION("remove last node") {
    REQUIRE(exec.RemoveNode(node_c));
    REQUIRE(exec.NodeCount() == 2);
  }

  SECTION("remove nonexistent node") {
    osp::Node<TestPayload> other("other", 99);
    REQUIRE_FALSE(exec.RemoveNode(other));
    REQUIRE(exec.NodeCount() == 3);
  }

  SECTION("remove same node twice") {
    REQUIRE(exec.RemoveNode(node_b));
    REQUIRE_FALSE(exec.RemoveNode(node_b));
    REQUIRE(exec.NodeCount() == 2);
  }
}

// ============================================================================
// NodeCount tests
// ============================================================================

TEST_CASE("executor - NodeCount returns correct value", "[executor]") {
  ExecutorBusFixture fix;

  osp::SingleThreadExecutor<TestPayload> exec;
  REQUIRE(exec.NodeCount() == 0);

  osp::Node<TestPayload> node_a("a", 1);
  osp::Node<TestPayload> node_b("b", 2);
  osp::Node<TestPayload> node_c("c", 3);

  exec.AddNode(node_a);
  REQUIRE(exec.NodeCount() == 1);

  exec.AddNode(node_b);
  REQUIRE(exec.NodeCount() == 2);

  exec.AddNode(node_c);
  REQUIRE(exec.NodeCount() == 3);

  exec.RemoveNode(node_b);
  REQUIRE(exec.NodeCount() == 2);

  exec.RemoveNode(node_a);
  REQUIRE(exec.NodeCount() == 1);

  exec.RemoveNode(node_c);
  REQUIRE(exec.NodeCount() == 0);
}

// ============================================================================
// RealtimeExecutor tests
// ============================================================================

TEST_CASE("executor - RealtimeConfig default values", "[executor]") {
  osp::RealtimeConfig cfg;
  REQUIRE(cfg.sched_policy == 0);
  REQUIRE(cfg.sched_priority == 0);
  REQUIRE(cfg.lock_memory == false);
  REQUIRE(cfg.stack_size == 0);
  REQUIRE(cfg.cpu_affinity == -1);
}

TEST_CASE("executor - RealtimeExecutor Start/Stop lifecycle", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node("node", 1);

  osp::RealtimeConfig cfg;
  osp::RealtimeExecutor<TestPayload> exec(cfg);
  exec.AddNode(node);

  REQUIRE_FALSE(exec.IsRunning());

  exec.Start();
  REQUIRE(exec.IsRunning());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  exec.Stop();
  REQUIRE_FALSE(exec.IsRunning());
}

TEST_CASE("executor - RealtimeExecutor processes messages", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> sender("sender", 1);
  osp::Node<TestPayload> receiver("receiver", 2);

  std::atomic<int> received_count{0};
  auto sub_result =
      receiver.Subscribe<TestMsg>([&](const TestMsg&, const auto&) {
        received_count.fetch_add(1, std::memory_order_relaxed);
      });
  REQUIRE(sub_result.has_value());

  osp::RealtimeConfig cfg;
  osp::RealtimeExecutor<TestPayload> exec(cfg);
  exec.AddNode(sender);
  exec.AddNode(receiver);

  exec.Start();

  // Publish several messages
  for (int i = 0; i < 5; ++i) {
    sender.Publish(TestMsg{i});
  }

  // Wait for dispatch thread to process
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (received_count.load(std::memory_order_relaxed) < 5 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  exec.Stop();

  REQUIRE(received_count.load(std::memory_order_relaxed) == 5);
}

TEST_CASE("executor - RealtimeExecutor AddNode", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node_a("node_a", 1);
  osp::Node<TestPayload> node_b("node_b", 2);

  osp::RealtimeConfig cfg;
  osp::RealtimeExecutor<TestPayload> exec(cfg);

  SECTION("add nodes successfully") {
    REQUIRE(exec.AddNode(node_a));
    REQUIRE(exec.AddNode(node_b));
    REQUIRE(exec.NodeCount() == 2);
  }

  SECTION("duplicate node rejected") {
    REQUIRE(exec.AddNode(node_a));
    REQUIRE_FALSE(exec.AddNode(node_a));
    REQUIRE(exec.NodeCount() == 1);
  }
}

TEST_CASE("executor - RealtimeExecutor GetConfig returns configured values",
          "[executor]") {
  ExecutorBusFixture fix;

  osp::RealtimeConfig cfg;
  cfg.sched_policy = 1;
  cfg.sched_priority = 50;
  cfg.lock_memory = true;
  cfg.stack_size = 1024 * 1024;
  cfg.cpu_affinity = 2;

  osp::RealtimeExecutor<TestPayload> exec(cfg);

  const auto& retrieved_cfg = exec.GetConfig();
  REQUIRE(retrieved_cfg.sched_policy == 1);
  REQUIRE(retrieved_cfg.sched_priority == 50);
  REQUIRE(retrieved_cfg.lock_memory == true);
  REQUIRE(retrieved_cfg.stack_size == 1024 * 1024);
  REQUIRE(retrieved_cfg.cpu_affinity == 2);
}

TEST_CASE("executor - RealtimeExecutor with CPU affinity", "[executor]") {
  ExecutorBusFixture fix;

  osp::Node<TestPayload> node("node", 1);

  osp::RealtimeConfig cfg;
  cfg.cpu_affinity = 0;  // Bind to core 0

  osp::RealtimeExecutor<TestPayload> exec(cfg);
  exec.AddNode(node);

  REQUIRE_FALSE(exec.IsRunning());

  exec.Start();
  REQUIRE(exec.IsRunning());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  exec.Stop();
  REQUIRE_FALSE(exec.IsRunning());
}
