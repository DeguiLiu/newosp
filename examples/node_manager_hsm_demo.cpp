/**
 * @file node_manager_hsm_demo.cpp
 * @brief Demonstrates NodeManager with HSM-driven heartbeat state machine.
 *
 * Shows:
 *   - HSM states for node connection: Connected -> Suspect -> Disconnected
 *   - Heartbeat events driving state transitions
 *   - Disconnect callback notification
 *   - Reconnection handling
 *   - Multiple nodes with independent state machines
 */

#include "osp/hsm.hpp"
#include "osp/node_manager.hpp"
#include "osp/log.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

// ============================================================================
// HSM Events
// ============================================================================

enum EventId : uint32_t {
  kHeartbeatOk = 1,
  kHeartbeatMiss = 2,
  kDisconnect = 3,
  kReconnect = 4,
};

// ============================================================================
// Node Context - Per-node state machine context
// ============================================================================

struct NodeContext {
  uint16_t node_id;
  uint32_t missed_heartbeats;
  uint32_t total_heartbeats;
  bool connected;

  NodeContext() noexcept
      : node_id(0), missed_heartbeats(0), total_heartbeats(0), connected(false) {}
};

// ============================================================================
// State Handlers
// ============================================================================

// Forward declarations for state indices
static int32_t g_state_connected = -1;
static int32_t g_state_suspect = -1;
static int32_t g_state_disconnected = -1;

// --- Connected State ---

static void OnEnterConnected(NodeContext& ctx) {
  std::printf("[Node %u] -> Connected\n", ctx.node_id);
  ctx.connected = true;
  ctx.missed_heartbeats = 0;
}

static void OnExitConnected(NodeContext& ctx) {
  std::printf("[Node %u] <- Connected\n", ctx.node_id);
}

static osp::TransitionResult HandleConnected(NodeContext& ctx, const osp::Event& event) {
  switch (event.id) {
    case kHeartbeatOk:
      ctx.total_heartbeats++;
      std::printf("[Node %u] Connected: heartbeat OK (total=%u)\n",
                  ctx.node_id, ctx.total_heartbeats);
      return osp::TransitionResult::kHandled;

    case kHeartbeatMiss:
      ctx.missed_heartbeats++;
      std::printf("[Node %u] Connected: heartbeat MISS (count=%u)\n",
                  ctx.node_id, ctx.missed_heartbeats);
      if (ctx.missed_heartbeats >= 2) {
        std::printf("[Node %u] Connected: too many misses, transitioning to Suspect\n",
                    ctx.node_id);
        return osp::TransitionResult::kTransition;
      }
      return osp::TransitionResult::kHandled;

    case kDisconnect:
      std::printf("[Node %u] Connected: disconnect event\n", ctx.node_id);
      return osp::TransitionResult::kTransition;

    default:
      return osp::TransitionResult::kUnhandled;
  }
}

// --- Suspect State ---

static void OnEnterSuspect(NodeContext& ctx) {
  std::printf("[Node %u] -> Suspect (missed=%u)\n", ctx.node_id, ctx.missed_heartbeats);
}

static void OnExitSuspect(NodeContext& ctx) {
  std::printf("[Node %u] <- Suspect\n", ctx.node_id);
}

static osp::TransitionResult HandleSuspect(NodeContext& ctx, const osp::Event& event) {
  switch (event.id) {
    case kHeartbeatOk:
      ctx.total_heartbeats++;
      ctx.missed_heartbeats = 0;
      std::printf("[Node %u] Suspect: heartbeat OK, recovering to Connected\n",
                  ctx.node_id);
      return osp::TransitionResult::kTransition;

    case kHeartbeatMiss:
      ctx.missed_heartbeats++;
      std::printf("[Node %u] Suspect: heartbeat MISS (count=%u)\n",
                  ctx.node_id, ctx.missed_heartbeats);
      if (ctx.missed_heartbeats >= 5) {
        std::printf("[Node %u] Suspect: timeout, transitioning to Disconnected\n",
                    ctx.node_id);
        return osp::TransitionResult::kTransition;
      }
      return osp::TransitionResult::kHandled;

    case kDisconnect:
      std::printf("[Node %u] Suspect: disconnect event\n", ctx.node_id);
      return osp::TransitionResult::kTransition;

    default:
      return osp::TransitionResult::kUnhandled;
  }
}

// --- Disconnected State ---

static void OnEnterDisconnected(NodeContext& ctx) {
  std::printf("[Node %u] -> Disconnected\n", ctx.node_id);
  ctx.connected = false;
}

static void OnExitDisconnected(NodeContext& ctx) {
  std::printf("[Node %u] <- Disconnected\n", ctx.node_id);
}

static osp::TransitionResult HandleDisconnected(NodeContext& ctx, const osp::Event& event) {
  switch (event.id) {
    case kReconnect:
      std::printf("[Node %u] Disconnected: reconnect event\n", ctx.node_id);
      ctx.missed_heartbeats = 0;
      return osp::TransitionResult::kTransition;

    case kHeartbeatOk:
    case kHeartbeatMiss:
      std::printf("[Node %u] Disconnected: ignoring heartbeat event\n", ctx.node_id);
      return osp::TransitionResult::kHandled;

    default:
      return osp::TransitionResult::kUnhandled;
  }
}

// ============================================================================
// State Machine Factory
// ============================================================================

using NodeHSM = osp::StateMachine<NodeContext, 8>;

static void BuildNodeHSM(NodeHSM& sm) {
  // Add states
  g_state_connected = sm.AddState({
      "Connected",
      -1,  // root state
      HandleConnected,
      OnEnterConnected,
      OnExitConnected,
      nullptr
  });

  g_state_suspect = sm.AddState({
      "Suspect",
      -1,  // root state
      HandleSuspect,
      OnEnterSuspect,
      OnExitSuspect,
      nullptr
  });

  g_state_disconnected = sm.AddState({
      "Disconnected",
      -1,  // root state
      HandleDisconnected,
      OnEnterDisconnected,
      OnExitDisconnected,
      nullptr
  });

  sm.SetInitialState(g_state_connected);
}

// ============================================================================
// Transition Logic
// ============================================================================

static void ProcessTransition(NodeHSM& sm, NodeContext& ctx, const osp::Event& event) {
  int32_t current = sm.CurrentState();

  if (current == g_state_connected) {
    if (event.id == kHeartbeatMiss && ctx.missed_heartbeats >= 2) {
      sm.RequestTransition(g_state_suspect);
    } else if (event.id == kDisconnect) {
      sm.RequestTransition(g_state_disconnected);
    }
  } else if (current == g_state_suspect) {
    if (event.id == kHeartbeatOk) {
      sm.RequestTransition(g_state_connected);
    } else if (event.id == kHeartbeatMiss && ctx.missed_heartbeats >= 5) {
      sm.RequestTransition(g_state_disconnected);
    } else if (event.id == kDisconnect) {
      sm.RequestTransition(g_state_disconnected);
    }
  } else if (current == g_state_disconnected) {
    if (event.id == kReconnect) {
      sm.RequestTransition(g_state_connected);
    }
  }
}

// ============================================================================
// Demo Scenario
// ============================================================================

static void SimulateHeartbeats(NodeHSM& sm, NodeContext& ctx,
                                const char* scenario_name,
                                const uint32_t* events, uint32_t count) {
  std::printf("\n=== Scenario: %s ===\n", scenario_name);

  for (uint32_t i = 0; i < count; ++i) {
    osp::Event evt{events[i], nullptr};
    std::printf("\n[Step %u] Dispatching event %u\n", i + 1, events[i]);

    // Dispatch event
    sm.Dispatch(evt);

    // Check if transition is needed
    ProcessTransition(sm, ctx, evt);

    std::printf("  State: %s, Missed: %u, Total: %u\n",
                sm.CurrentStateName(), ctx.missed_heartbeats, ctx.total_heartbeats);
  }
}

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kInfo);

  std::printf("=== NodeManager HSM Demo ===\n");
  std::printf("Demonstrates heartbeat state machine with 3 states:\n");
  std::printf("  Connected -> Suspect -> Disconnected\n\n");

  // -------------------------------------------------------------------------
  // Node 1: Normal operation
  // -------------------------------------------------------------------------

  NodeContext ctx1;
  ctx1.node_id = 101;
  NodeHSM sm1(ctx1);
  BuildNodeHSM(sm1);
  sm1.Start();

  uint32_t scenario1[] = {
      kHeartbeatOk, kHeartbeatOk, kHeartbeatOk, kHeartbeatOk
  };
  SimulateHeartbeats(sm1, ctx1, "Node 101 - Normal Operation",
                     scenario1, sizeof(scenario1) / sizeof(scenario1[0]));

  // -------------------------------------------------------------------------
  // Node 2: Missed heartbeats -> Suspect -> Recovery
  // -------------------------------------------------------------------------

  NodeContext ctx2;
  ctx2.node_id = 102;
  NodeHSM sm2(ctx2);
  BuildNodeHSM(sm2);
  sm2.Start();

  uint32_t scenario2[] = {
      kHeartbeatOk,
      kHeartbeatMiss,
      kHeartbeatMiss,  // -> Suspect
      kHeartbeatMiss,
      kHeartbeatOk,    // -> Connected (recovery)
      kHeartbeatOk
  };
  SimulateHeartbeats(sm2, ctx2, "Node 102 - Suspect and Recovery",
                     scenario2, sizeof(scenario2) / sizeof(scenario2[0]));

  // -------------------------------------------------------------------------
  // Node 3: Timeout -> Disconnect -> Reconnect
  // -------------------------------------------------------------------------

  NodeContext ctx3;
  ctx3.node_id = 103;
  NodeHSM sm3(ctx3);
  BuildNodeHSM(sm3);
  sm3.Start();

  uint32_t scenario3[] = {
      kHeartbeatOk,
      kHeartbeatMiss,
      kHeartbeatMiss,  // -> Suspect
      kHeartbeatMiss,
      kHeartbeatMiss,
      kHeartbeatMiss,  // -> Disconnected
      kHeartbeatMiss,  // ignored
      kReconnect,      // -> Connected
      kHeartbeatOk
  };
  SimulateHeartbeats(sm3, ctx3, "Node 103 - Timeout and Reconnect",
                     scenario3, sizeof(scenario3) / sizeof(scenario3[0]));

  // -------------------------------------------------------------------------
  // Node 4: Immediate disconnect
  // -------------------------------------------------------------------------

  NodeContext ctx4;
  ctx4.node_id = 104;
  NodeHSM sm4(ctx4);
  BuildNodeHSM(sm4);
  sm4.Start();

  uint32_t scenario4[] = {
      kHeartbeatOk,
      kHeartbeatOk,
      kDisconnect,     // -> Disconnected
      kReconnect,      // -> Connected
      kHeartbeatOk
  };
  SimulateHeartbeats(sm4, ctx4, "Node 104 - Immediate Disconnect",
                     scenario4, sizeof(scenario4) / sizeof(scenario4[0]));

  // -------------------------------------------------------------------------
  // Summary
  // -------------------------------------------------------------------------

  std::printf("\n=== Summary ===\n");
  std::printf("Node 101: State=%s, Connected=%d, Total HB=%u\n",
              sm1.CurrentStateName(), ctx1.connected, ctx1.total_heartbeats);
  std::printf("Node 102: State=%s, Connected=%d, Total HB=%u\n",
              sm2.CurrentStateName(), ctx2.connected, ctx2.total_heartbeats);
  std::printf("Node 103: State=%s, Connected=%d, Total HB=%u\n",
              sm3.CurrentStateName(), ctx3.connected, ctx3.total_heartbeats);
  std::printf("Node 104: State=%s, Connected=%d, Total HB=%u\n",
              sm4.CurrentStateName(), ctx4.connected, ctx4.total_heartbeats);

  osp::log::Shutdown();
  return 0;
}
