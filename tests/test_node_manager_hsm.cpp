/**
 * @file test_node_manager_hsm.cpp
 * @brief Unit tests for HsmNodeManager.
 */

#include "osp/node_manager_hsm.hpp"
#include "osp/node_manager.hpp"
#include "osp/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("node_manager_hsm - Add and remove node", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;

  CHECK(mgr.NodeCount() == 0U);

  bool added = mgr.AddNode(1);
  CHECK(added);
  CHECK(mgr.NodeCount() == 1U);
  CHECK(mgr.IsConnected(1));

  bool removed = mgr.RemoveNode(1);
  CHECK(removed);
  CHECK(mgr.NodeCount() == 0U);
  CHECK_FALSE(mgr.IsConnected(1));
}

TEST_CASE("node_manager_hsm - Heartbeat keeps connected", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Send heartbeats to keep connection alive
  for (int i = 0; i < 5; ++i) {
    mgr.OnHeartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
    CHECK(mgr.GetMissedHeartbeats(1) == 0U);
  }
}

TEST_CASE("node_manager_hsm - Timeout transitions to Suspect", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Wait for timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  mgr.CheckTimeouts();

  CHECK(std::strcmp(mgr.GetNodeState(1), "Suspect") == 0);
  CHECK(mgr.GetMissedHeartbeats(1) >= 1U);
}

TEST_CASE("node_manager_hsm - Multiple timeouts transition to Disconnected", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

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

  mgr.AddNode(1, 3);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Simulate multiple timeouts
  for (int i = 0; i < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.CheckTimeouts();
  }

  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);
  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == 1);
}

TEST_CASE("node_manager_hsm - Heartbeat in Suspect returns to Connected", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Transition to Suspect
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  mgr.CheckTimeouts();
  CHECK(std::strcmp(mgr.GetNodeState(1), "Suspect") == 0);

  // Send heartbeat to recover
  mgr.OnHeartbeat(1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  CHECK(mgr.GetMissedHeartbeats(1) == 0U);
}

TEST_CASE("node_manager_hsm - Disconnect callback fires", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

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

  mgr.AddNode(1, 2);

  // Trigger disconnect via timeouts
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.CheckTimeouts();
  }

  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == 1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);
}

TEST_CASE("node_manager_hsm - Reconnect from Disconnected", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 2);

  // Transition to Disconnected
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.CheckTimeouts();
  }
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);

  // Reconnect
  mgr.RequestReconnect(1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  CHECK(mgr.GetMissedHeartbeats(1) == 0U);
}

TEST_CASE("node_manager_hsm - GetNodeState returns correct state", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);

  // Initial state
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Transition to Suspect
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  mgr.CheckTimeouts();
  CHECK(std::strcmp(mgr.GetNodeState(1), "Suspect") == 0);

  // Transition to Disconnected
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.CheckTimeouts();
  }
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);

  // Non-existent node
  CHECK(std::strcmp(mgr.GetNodeState(999), "") == 0);
}

TEST_CASE("node_manager_hsm - Multiple nodes independent state", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  mgr.AddNode(2, 3);
  mgr.AddNode(3, 3);

  CHECK(mgr.NodeCount() == 3U);

  // All start in Connected
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(2), "Connected") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(3), "Connected") == 0);

  // Keep node 1 alive, let nodes 2 and 3 timeout
  for (int i = 0; i < 5; ++i) {
    mgr.OnHeartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Now trigger timeout check - nodes 2 and 3 should be timed out
  mgr.OnHeartbeat(1);  // Refresh node 1 right before check
  mgr.CheckTimeouts();

  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(2), "Suspect") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(3), "Suspect") == 0);

  // Continue - let nodes 2 and 3 reach Disconnected
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.OnHeartbeat(1);
    mgr.CheckTimeouts();
  }

  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(2), "Disconnected") == 0);
  CHECK(std::strcmp(mgr.GetNodeState(3), "Disconnected") == 0);
}

TEST_CASE("node_manager_hsm - Node count tracking", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;

  CHECK(mgr.NodeCount() == 0U);

  mgr.AddNode(1);
  CHECK(mgr.NodeCount() == 1U);

  mgr.AddNode(2);
  CHECK(mgr.NodeCount() == 2U);

  mgr.AddNode(3);
  CHECK(mgr.NodeCount() == 3U);

  mgr.RemoveNode(2);
  CHECK(mgr.NodeCount() == 2U);

  mgr.RemoveNode(1);
  CHECK(mgr.NodeCount() == 1U);

  mgr.RemoveNode(3);
  CHECK(mgr.NodeCount() == 0U);
}

TEST_CASE("node_manager_hsm - Start and stop monitoring thread", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  CHECK_FALSE(mgr.IsRunning());

  bool started = mgr.Start();
  CHECK(started);
  CHECK(mgr.IsRunning());

  // Starting again should fail
  bool started_again = mgr.Start();
  CHECK_FALSE(started_again);

  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Can start again after stop
  bool restarted = mgr.Start();
  CHECK(restarted);
  CHECK(mgr.IsRunning());

  mgr.Stop();
}

TEST_CASE("node_manager_hsm - Automatic timeout detection with thread", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

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

  mgr.AddNode(1, 2);
  mgr.Start();

  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Wait for automatic timeout detection (2 missed * 100ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(350));

  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == 1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);

  mgr.Stop();
}

TEST_CASE("node_manager_hsm - Request disconnect", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;

  std::atomic<bool> callback_fired{false};

  mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* fired = static_cast<std::atomic<bool>*>(ctx);
        fired->store(true);
      },
      &callback_fired);

  mgr.AddNode(1, 3);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Request disconnect
  mgr.RequestDisconnect(1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);
  CHECK(callback_fired.load());
}

TEST_CASE("node_manager_hsm - Table full", "[HsmNodeManager]") {
  osp::HsmNodeManager<4> mgr;  // Small capacity

  CHECK(mgr.AddNode(1));
  CHECK(mgr.AddNode(2));
  CHECK(mgr.AddNode(3));
  CHECK(mgr.AddNode(4));

  // Fifth node should fail
  CHECK_FALSE(mgr.AddNode(5));
  CHECK(mgr.NodeCount() == 4U);
}

TEST_CASE("node_manager_hsm - IsConnected query", "[HsmNodeManager]") {
  osp::HsmNodeManager<64> mgr;
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  CHECK(mgr.IsConnected(1));

  // Transition to Suspect
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  mgr.CheckTimeouts();
  CHECK_FALSE(mgr.IsConnected(1));  // Not in Connected state

  // Recover
  mgr.OnHeartbeat(1);
  CHECK(mgr.IsConnected(1));

  // Non-existent node
  CHECK_FALSE(mgr.IsConnected(999));
}

// ============================================================================
// Phase 2: TimerScheduler Injection Mode Tests
// ============================================================================

#include "osp/timer.hpp"

TEST_CASE("node_manager_hsm - Scheduler injection basic lifecycle", "[HsmNodeManager][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  CHECK_FALSE(mgr.IsRunning());

  bool started = mgr.Start();
  CHECK(started);
  CHECK(mgr.IsRunning());

  // Starting again should fail
  bool started_again = mgr.Start();
  CHECK_FALSE(started_again);

  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Can restart
  bool restarted = mgr.Start();
  CHECK(restarted);
  CHECK(mgr.IsRunning());

  mgr.Stop();
  sched.Stop();
}

TEST_CASE("node_manager_hsm - Scheduler injection auto timeout", "[HsmNodeManager][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  std::atomic<bool> callback_fired{false};
  std::atomic<uint16_t> disconnected_node_id{0};

  struct Context {
    std::atomic<bool>* fired;
    std::atomic<uint16_t>* id;
  };
  Context context{&callback_fired, &disconnected_node_id};

  mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* c = static_cast<Context*>(ctx);
        c->fired->store(true);
        c->id->store(node_id);
      },
      &context);

  mgr.AddNode(1, 2);
  mgr.Start();

  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Wait for automatic timeout detection (2 missed * 100ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == 1);
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);

  mgr.Stop();
  sched.Stop();
}

TEST_CASE("node_manager_hsm - Scheduler injection heartbeat keeps connected", "[HsmNodeManager][scheduler]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  mgr.AddNode(1, 3);
  mgr.Start();

  // Send heartbeats to keep connection alive
  for (int i = 0; i < 5; ++i) {
    mgr.OnHeartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);
  }

  mgr.Stop();
  sched.Stop();
}

// ============================================================================
// Edge Case Tests: Scheduler Injection
// ============================================================================

TEST_CASE("node_manager_hsm - Scheduler injection multiple modules share scheduler", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr1(&sched);
  osp::HsmNodeManager<64> mgr2(&sched);
  mgr1.SetHeartbeatInterval(100);
  mgr2.SetHeartbeatInterval(100);

  std::atomic<uint32_t> mgr1_disconnects{0};
  std::atomic<uint32_t> mgr2_disconnects{0};

  mgr1.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* counter = static_cast<std::atomic<uint32_t>*>(ctx);
        counter->fetch_add(1, std::memory_order_release);
      },
      &mgr1_disconnects);

  mgr2.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* counter = static_cast<std::atomic<uint32_t>*>(ctx);
        counter->fetch_add(1, std::memory_order_release);
      },
      &mgr2_disconnects);

  // Add nodes to both managers
  mgr1.AddNode(1, 2);
  mgr2.AddNode(2, 2);

  // Start both managers
  bool started1 = mgr1.Start();
  bool started2 = mgr2.Start();
  CHECK(started1);
  CHECK(started2);

  // Wait for timeout detection (2 missed * 100ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Both should detect timeouts independently
  CHECK(mgr1_disconnects.load(std::memory_order_acquire) >= 1);
  CHECK(mgr2_disconnects.load(std::memory_order_acquire) >= 1);
  CHECK(std::strcmp(mgr1.GetNodeState(1), "Disconnected") == 0);
  CHECK(std::strcmp(mgr2.GetNodeState(2), "Disconnected") == 0);

  mgr1.Stop();
  mgr2.Stop();
  sched.Stop();
}

TEST_CASE("node_manager_hsm - Scheduler injection Stop before Start", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  CHECK_FALSE(mgr.IsRunning());

  // Call Stop() without ever calling Start() - should not crash
  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  sched.Stop();
}

TEST_CASE("node_manager_hsm - Scheduler injection restart cycle", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  // Cycle 1
  CHECK_FALSE(mgr.IsRunning());
  bool started1 = mgr.Start();
  CHECK(started1);
  CHECK(mgr.IsRunning());
  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Cycle 2
  bool started2 = mgr.Start();
  CHECK(started2);
  CHECK(mgr.IsRunning());
  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Cycle 3
  bool started3 = mgr.Start();
  CHECK(started3);
  CHECK(mgr.IsRunning());
  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  sched.Stop();
}

TEST_CASE("HsmNodeManager: scheduler injection - timer cleanup on Stop", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  uint32_t initial_count = sched.TaskCount();

  bool started = mgr.Start();
  REQUIRE(started);
  CHECK(mgr.IsRunning());

  // Verify timer registered (TaskCount increased by 1)
  CHECK(sched.TaskCount() == initial_count + 1);

  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Verify timer removed (TaskCount back to original)
  CHECK(sched.TaskCount() == initial_count);

  sched.Stop();
}

TEST_CASE("HsmNodeManager: scheduler injection - double Stop safe", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  bool started = mgr.Start();
  REQUIRE(started);

  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  // Stop twice - should not crash
  mgr.Stop();
  CHECK_FALSE(mgr.IsRunning());

  sched.Stop();
}

TEST_CASE("HsmNodeManager: scheduler injection - timeout detection works", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> mgr(&sched);
  mgr.SetHeartbeatInterval(100);

  std::atomic<bool> callback_fired{false};
  std::atomic<uint16_t> disconnected_node_id{0};

  struct Context {
    std::atomic<bool>* fired;
    std::atomic<uint16_t>* id;
  };
  Context context{&callback_fired, &disconnected_node_id};

  mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* c = static_cast<Context*>(ctx);
        c->fired->store(true);
        c->id->store(node_id);
      },
      &context);

  mgr.AddNode(1, 2);
  mgr.Start();

  CHECK(std::strcmp(mgr.GetNodeState(1), "Connected") == 0);

  // Don't send heartbeat, wait for timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // Should transition to Suspect
  CHECK(std::strcmp(mgr.GetNodeState(1), "Suspect") == 0);

  // Wait more for Disconnected
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should transition to Disconnected
  CHECK(std::strcmp(mgr.GetNodeState(1), "Disconnected") == 0);
  CHECK(callback_fired.load());
  CHECK(disconnected_node_id.load() == 1);

  mgr.Stop();
  sched.Stop();
}

TEST_CASE("HsmNodeManager: scheduler injection - shared scheduler with NodeManager", "[HsmNodeManager][scheduler][edge]") {
  osp::TimerScheduler<> sched;
  sched.Start();

  osp::HsmNodeManager<64> hsm_mgr(&sched);
  hsm_mgr.SetHeartbeatInterval(100);

  osp::NodeManagerConfig cfg;
  cfg.heartbeat_interval_ms = 100;
  cfg.heartbeat_timeout_count = 2;
  osp::NodeManager<8> node_mgr(cfg, &sched);

  std::atomic<uint32_t> hsm_disconnects{0};
  std::atomic<uint32_t> node_disconnects{0};

  hsm_mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* counter = static_cast<std::atomic<uint32_t>*>(ctx);
        counter->fetch_add(1, std::memory_order_release);
      },
      &hsm_disconnects);

  node_mgr.OnDisconnect(
      [](uint16_t node_id, void* ctx) {
        auto* counter = static_cast<std::atomic<uint32_t>*>(ctx);
        counter->fetch_add(1, std::memory_order_release);
      },
      &node_disconnects);

  // Add node to HSM manager
  hsm_mgr.AddNode(1, 2);

  // Start both managers
  bool hsm_started = hsm_mgr.Start();
  auto node_started = node_mgr.Start();
  CHECK(hsm_started);
  CHECK(node_started.has_value());

  // Both should be running
  CHECK(hsm_mgr.IsRunning());
  CHECK(node_mgr.IsRunning());

  // Wait for HSM timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // HSM should detect timeout
  CHECK(hsm_disconnects.load(std::memory_order_acquire) >= 1);
  CHECK(std::strcmp(hsm_mgr.GetNodeState(1), "Disconnected") == 0);

  // Both Stop cleanly
  hsm_mgr.Stop();
  node_mgr.Stop();
  CHECK_FALSE(hsm_mgr.IsRunning());
  CHECK_FALSE(node_mgr.IsRunning());

  sched.Stop();
}
