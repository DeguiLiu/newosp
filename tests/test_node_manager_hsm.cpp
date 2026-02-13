/**
 * @file test_node_manager_hsm.cpp
 * @brief Unit tests for HsmNodeManager.
 */

#include "osp/node_manager_hsm.hpp"
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
