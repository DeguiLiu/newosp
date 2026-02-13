/**
 * @file hsm_protocol_demo.cpp
 * @brief Demonstrates HSM for a communication protocol state machine.
 *
 * Models a simplified connection protocol (TCP-lite / Modbus-like) with
 * hierarchical states showing the power of state inheritance:
 *
 * Operational (root)
 * ├── Disconnected (initial)
 * ├── Connecting
 * ├── Connected (parent state)
 * │   ├── Idle (initial child)
 * │   └── Active
 * └── Disconnecting
 *
 * Key feature: Connected state handles DISCONNECT for both Idle and Active
 * children, demonstrating hierarchical event handling.
 */

#include "osp/hsm.hpp"
#include <cstdio>
#include <cstring>

// ============================================================================
// Protocol Events
// ============================================================================

enum ProtocolEvent : uint32_t {
  CONNECT = 1,
  SYN_ACK = 2,
  DISCONNECT = 3,
  FIN_ACK = 4,
  TIMEOUT = 5,
  DATA_READY = 6,
  DATA_SENT = 7,
  ERROR = 8
};

// ============================================================================
// Protocol Context
// ============================================================================

struct ProtocolContext {
  int syn_count = 0;
  int ack_count = 0;
  int data_sent_count = 0;
  int error_count = 0;
  bool connected = false;
  char last_action[64] = {0};

  void SetAction(const char* action) {
    strncpy(last_action, action, sizeof(last_action) - 1);
    last_action[sizeof(last_action) - 1] = '\0';
  }
};

// ============================================================================
// State Indices and State Machine Pointer
// ============================================================================

static int32_t s_operational;
static int32_t s_disconnected;
static int32_t s_connecting;
static int32_t s_connected;
static int32_t s_idle;
static int32_t s_active;
static int32_t s_disconnecting;

static osp::StateMachine<ProtocolContext, 16>* g_sm = nullptr;

// ============================================================================
// State Handlers
// ============================================================================

// --- Disconnected ---

osp::TransitionResult OnDisconnected(ProtocolContext& ctx,
                                      const osp::Event& event) {
  if (event.id == CONNECT) {
    printf("  [Disconnected] CONNECT received -> Connecting\n");
    return g_sm->RequestTransition(s_connecting);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterDisconnected(ProtocolContext& ctx) {
  printf("  [Disconnected] Entry: connection closed\n");
  ctx.connected = false;
  ctx.SetAction("disconnected");
}

// --- Connecting ---

osp::TransitionResult OnConnecting(ProtocolContext& ctx,
                                    const osp::Event& event) {
  if (event.id == SYN_ACK) {
    printf("  [Connecting] SYN_ACK received -> Connected/Idle\n");
    ctx.ack_count++;
    return g_sm->RequestTransition(s_idle);
  }
  if (event.id == TIMEOUT) {
    printf("  [Connecting] TIMEOUT -> Disconnected\n");
    return g_sm->RequestTransition(s_disconnected);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterConnecting(ProtocolContext& ctx) {
  printf("  [Connecting] Entry: sending SYN...\n");
  ctx.syn_count++;
  ctx.SetAction("connecting");
}

// --- Connected (parent state) ---

osp::TransitionResult OnConnected(ProtocolContext& ctx,
                                   const osp::Event& event) {
  // Connected handles DISCONNECT for both Idle and Active children
  if (event.id == DISCONNECT) {
    printf("  [Connected] DISCONNECT received -> Disconnecting\n");
    return g_sm->RequestTransition(s_disconnecting);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterConnected(ProtocolContext& ctx) {
  printf("  [Connected] Entry: connection established\n");
  ctx.connected = true;
  ctx.SetAction("connected");
}

void OnExitConnected(ProtocolContext& ctx) {
  printf("  [Connected] Exit: leaving connected state\n");
}

// --- Idle (child of Connected) ---

osp::TransitionResult OnIdle(ProtocolContext& ctx, const osp::Event& event) {
  if (event.id == DATA_READY) {
    printf("  [Idle] DATA_READY -> Active\n");
    return g_sm->RequestTransition(s_active);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterIdle(ProtocolContext& ctx) {
  printf("  [Idle] Entry: waiting for data\n");
  ctx.SetAction("idle");
}

// --- Active (child of Connected) ---

osp::TransitionResult OnActive(ProtocolContext& ctx, const osp::Event& event) {
  if (event.id == DATA_SENT) {
    printf("  [Active] DATA_SENT -> Idle\n");
    ctx.data_sent_count++;
    return g_sm->RequestTransition(s_idle);
  }
  if (event.id == ERROR) {
    printf("  [Active] ERROR -> Idle (recovery)\n");
    ctx.error_count++;
    return g_sm->RequestTransition(s_idle);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterActive(ProtocolContext& ctx) {
  printf("  [Active] Entry: processing data\n");
  ctx.SetAction("active");
}

// --- Disconnecting ---

osp::TransitionResult OnDisconnecting(ProtocolContext& ctx,
                                       const osp::Event& event) {
  if (event.id == FIN_ACK) {
    printf("  [Disconnecting] FIN_ACK received -> Disconnected\n");
    return g_sm->RequestTransition(s_disconnected);
  }
  if (event.id == TIMEOUT) {
    printf("  [Disconnecting] TIMEOUT -> Disconnected (force close)\n");
    return g_sm->RequestTransition(s_disconnected);
  }
  return osp::TransitionResult::kUnhandled;
}

void OnEnterDisconnecting(ProtocolContext& ctx) {
  printf("  [Disconnecting] Entry: sending FIN...\n");
  ctx.SetAction("disconnecting");
}

// ============================================================================
// State Machine Setup
// ============================================================================

void SetupStateMachine(osp::StateMachine<ProtocolContext, 16>& sm) {
  // Root: Operational
  s_operational = sm.AddState({
      "Operational",
      -1,  // no parent (root)
      nullptr,
      nullptr,
      nullptr,
      nullptr
  });

  // Disconnected
  s_disconnected = sm.AddState({
      "Disconnected",
      s_operational,
      OnDisconnected,
      OnEnterDisconnected,
      nullptr,
      nullptr
  });

  // Connecting
  s_connecting = sm.AddState({
      "Connecting",
      s_operational,
      OnConnecting,
      OnEnterConnecting,
      nullptr,
      nullptr
  });

  // Connected (parent state)
  s_connected = sm.AddState({
      "Connected",
      s_operational,
      OnConnected,
      OnEnterConnected,
      OnExitConnected,
      nullptr
  });

  // Idle (child of Connected)
  s_idle = sm.AddState({
      "Idle",
      s_connected,
      OnIdle,
      OnEnterIdle,
      nullptr,
      nullptr
  });

  // Active (child of Connected)
  s_active = sm.AddState({
      "Active",
      s_connected,
      OnActive,
      OnEnterActive,
      nullptr,
      nullptr
  });

  // Disconnecting
  s_disconnecting = sm.AddState({
      "Disconnecting",
      s_operational,
      OnDisconnecting,
      OnEnterDisconnecting,
      nullptr,
      nullptr
  });

  sm.SetInitialState(s_disconnected);
}

// ============================================================================
// Main Simulation
// ============================================================================

int main() {
  printf("=== HSM Protocol Demo ===\n\n");

  ProtocolContext ctx;
  osp::StateMachine<ProtocolContext, 16> sm(ctx);
  g_sm = &sm;

  SetupStateMachine(sm);

  printf("Starting state machine...\n");
  sm.Start();
  printf("Current state: %s\n\n", sm.CurrentStateName());

  // Simulation sequence
  printf("Step 1: CONNECT\n");
  sm.Dispatch({CONNECT, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  printf("Step 2: SYN_ACK\n");
  sm.Dispatch({SYN_ACK, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  // Data transfer cycles
  for (int i = 1; i <= 3; ++i) {
    printf("Step %d: DATA_READY\n", 2 + i * 2 - 1);
    sm.Dispatch({DATA_READY, nullptr});
    printf("Current state: %s\n\n", sm.CurrentStateName());

    printf("Step %d: DATA_SENT\n", 2 + i * 2);
    sm.Dispatch({DATA_SENT, nullptr});
    printf("Current state: %s\n\n", sm.CurrentStateName());
  }

  printf("Step 9: ERROR (recovery test)\n");
  sm.Dispatch({DATA_READY, nullptr});
  sm.Dispatch({ERROR, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  printf("Step 10: DATA_READY (after recovery)\n");
  sm.Dispatch({DATA_READY, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  printf("Step 11: DATA_SENT\n");
  sm.Dispatch({DATA_SENT, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  printf("Step 12: DISCONNECT\n");
  sm.Dispatch({DISCONNECT, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  printf("Step 13: FIN_ACK\n");
  sm.Dispatch({FIN_ACK, nullptr});
  printf("Current state: %s\n\n", sm.CurrentStateName());

  // Print statistics
  printf("=== Final Statistics ===\n");
  printf("SYN count:       %d\n", ctx.syn_count);
  printf("ACK count:       %d\n", ctx.ack_count);
  printf("Data sent count: %d\n", ctx.data_sent_count);
  printf("Error count:     %d\n", ctx.error_count);
  printf("Connected:       %s\n", ctx.connected ? "true" : "false");
  printf("Last action:     %s\n", ctx.last_action);
  printf("Final state:     %s\n", sm.CurrentStateName());

  printf("\n=== Demo Complete ===\n");
  printf("Key takeaway: The Connected parent state handled DISCONNECT\n");
  printf("for both Idle and Active children, demonstrating hierarchical\n");
  printf("event handling without code duplication.\n");

  return 0;
}
