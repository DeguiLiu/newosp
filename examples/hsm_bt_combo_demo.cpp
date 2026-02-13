/**
 * @file hsm_bt_combo_demo.cpp
 * @brief Demo combining HSM and BT for an industrial device controller.
 *
 * Architecture:
 *   - HSM manages high-level device modes: Idle -> Initializing -> Running ->
 *     Error -> Shutdown
 *   - BT manages the Running mode's behavior: check sensors -> execute task ->
 *     report status
 *
 * Demonstrates:
 *   - Using BT inside HSM state handlers
 *   - State transitions triggered by events
 *   - Error handling and recovery logic
 */

#include "osp/bt.hpp"
#include "osp/hsm.hpp"

#include <cstdio>

// -- Context ----------------------------------------------------------------

struct DeviceContext {
  bool initialized = false;
  bool sensor_ok = true;
  bool task_done = false;
  int error_count = 0;
  int cycle_count = 0;
  osp::BehaviorTree<DeviceContext>* bt_ptr = nullptr;
  osp::StateMachine<DeviceContext, 8>* hsm_ptr = nullptr;
};

// -- Events -----------------------------------------------------------------

enum EventId : uint32_t {
  EVENT_START = 1,
  EVENT_INIT_DONE,
  EVENT_ERROR,
  EVENT_RESET,
  EVENT_STOP,
  EVENT_TICK
};

// -- BT Actions/Conditions --------------------------------------------------

osp::NodeStatus CheckSensors(DeviceContext& ctx) {
  if (ctx.sensor_ok) {
    std::printf("  [BT] CheckSensors: OK\n");
    return osp::NodeStatus::kSuccess;
  }
  std::printf("  [BT] CheckSensors: FAILED\n");
  return osp::NodeStatus::kFailure;
}

osp::NodeStatus ExecuteTask(DeviceContext& ctx) {
  ctx.task_done = true;
  ++ctx.cycle_count;
  std::printf("  [BT] ExecuteTask: cycle %d completed\n", ctx.cycle_count);
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus ReportStatus(DeviceContext& ctx) {
  std::printf("  [BT] ReportStatus: cycle=%d, task_done=%d\n",
              ctx.cycle_count, ctx.task_done);
  return osp::NodeStatus::kSuccess;
}

// -- HSM State Handlers -----------------------------------------------------

// State indices (forward declarations)
static int32_t s_idle = -1;
static int32_t s_initializing = -1;
static int32_t s_running = -1;
static int32_t s_error = -1;
static int32_t s_shutdown = -1;

// Idle state: wait for START event
osp::TransitionResult IdleHandler(DeviceContext& ctx, const osp::Event& event) {
  if (event.id == EVENT_START) {
    std::printf("[HSM] Idle: received START -> Initializing\n");
    return ctx.hsm_ptr->RequestTransition(s_initializing);
  }
  return osp::TransitionResult::kUnhandled;
}

// Initializing state: on entry, set initialized flag
void InitializingEntry(DeviceContext& ctx) {
  std::printf("[HSM] Initializing: entry (setting initialized=true)\n");
  ctx.initialized = true;
}

osp::TransitionResult InitializingHandler(DeviceContext& ctx,
                                          const osp::Event& event) {
  if (event.id == EVENT_INIT_DONE) {
    std::printf("[HSM] Initializing: received INIT_DONE -> Running\n");
    return ctx.hsm_ptr->RequestTransition(s_running);
  }
  return osp::TransitionResult::kUnhandled;
}

// Running state: tick BT each cycle
osp::TransitionResult RunningHandler(DeviceContext& ctx,
                                     const osp::Event& event) {
  if (event.id == EVENT_TICK) {
    std::printf("[HSM] Running: ticking BT...\n");
    osp::NodeStatus status = ctx.bt_ptr->Tick();
    std::printf("[HSM] Running: BT status = %s\n",
                osp::NodeStatusToString(status));
    return osp::TransitionResult::kHandled;
  }
  if (event.id == EVENT_ERROR) {
    std::printf("[HSM] Running: received ERROR -> Error\n");
    return ctx.hsm_ptr->RequestTransition(s_error);
  }
  if (event.id == EVENT_STOP) {
    std::printf("[HSM] Running: received STOP -> Shutdown\n");
    return ctx.hsm_ptr->RequestTransition(s_shutdown);
  }
  return osp::TransitionResult::kUnhandled;
}

// Error state: increment error_count, allow RESET
void ErrorEntry(DeviceContext& ctx) {
  ++ctx.error_count;
  std::printf("[HSM] Error: entry (error_count=%d)\n", ctx.error_count);
}

osp::TransitionResult ErrorHandler(DeviceContext& ctx,
                                   const osp::Event& event) {
  if (event.id == EVENT_RESET) {
    if (ctx.error_count < 3) {
      std::printf("[HSM] Error: received RESET (count < 3) -> Idle\n");
      return ctx.hsm_ptr->RequestTransition(s_idle);
    } else {
      std::printf("[HSM] Error: received RESET (count >= 3) -> Shutdown\n");
      return ctx.hsm_ptr->RequestTransition(s_shutdown);
    }
  }
  return osp::TransitionResult::kUnhandled;
}

// Shutdown state: final state
void ShutdownEntry(DeviceContext& ctx) {
  std::printf("[HSM] Shutdown: entry (device shutting down)\n");
}

osp::TransitionResult ShutdownHandler(DeviceContext&, const osp::Event&) {
  return osp::TransitionResult::kHandled;
}

// -- Main -------------------------------------------------------------------

int main() {
  std::printf("=== HSM + BT Combo Demo ===\n\n");

  DeviceContext ctx;

  // Build BT (used inside Running state)
  osp::BehaviorTree<DeviceContext> bt(ctx, "device_bt");
  ctx.bt_ptr = &bt;

  int32_t root = bt.AddSequence("root");
  bt.AddCondition("CheckSensors", CheckSensors, root);
  bt.AddAction("ExecuteTask", ExecuteTask, root);
  bt.AddAction("ReportStatus", ReportStatus, root);
  bt.SetRoot(root);

  std::printf("BT built: %u nodes\n\n", bt.NodeCount());

  // Build HSM
  osp::StateMachine<DeviceContext, 8> hsm(ctx);
  ctx.hsm_ptr = &hsm;

  s_idle = hsm.AddState({
      "Idle",
      -1,  // no parent
      IdleHandler,
      nullptr,  // no entry
      nullptr,  // no exit
      nullptr   // no guard
  });

  s_initializing = hsm.AddState({
      "Initializing",
      -1,
      InitializingHandler,
      InitializingEntry,
      nullptr,
      nullptr
  });

  s_running = hsm.AddState({
      "Running",
      -1,
      RunningHandler,
      nullptr,
      nullptr,
      nullptr
  });

  s_error = hsm.AddState({
      "Error",
      -1,
      ErrorHandler,
      ErrorEntry,
      nullptr,
      nullptr
  });

  s_shutdown = hsm.AddState({
      "Shutdown",
      -1,
      ShutdownHandler,
      ShutdownEntry,
      nullptr,
      nullptr
  });

  hsm.SetInitialState(s_idle);
  hsm.Start();

  std::printf("HSM started in state: %s\n\n", hsm.CurrentStateName());

  // Scenario: START -> INIT_DONE -> tick 5 times -> ERROR -> RESET ->
  //           START -> INIT_DONE -> tick 3 times -> STOP

  std::printf("--- Step 1: Send START ---\n");
  hsm.Dispatch({EVENT_START, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 2: Send INIT_DONE ---\n");
  hsm.Dispatch({EVENT_INIT_DONE, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 3: Tick Running 5 times ---\n");
  for (int i = 0; i < 5; ++i) {
    std::printf("Tick %d:\n", i + 1);
    hsm.Dispatch({EVENT_TICK, nullptr});
    std::printf("\n");
  }

  std::printf("--- Step 4: Send ERROR ---\n");
  hsm.Dispatch({EVENT_ERROR, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 5: Send RESET ---\n");
  hsm.Dispatch({EVENT_RESET, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 6: Send START again ---\n");
  hsm.Dispatch({EVENT_START, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 7: Send INIT_DONE again ---\n");
  hsm.Dispatch({EVENT_INIT_DONE, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("--- Step 8: Tick Running 3 more times ---\n");
  for (int i = 0; i < 3; ++i) {
    std::printf("Tick %d:\n", i + 1);
    hsm.Dispatch({EVENT_TICK, nullptr});
    std::printf("\n");
  }

  std::printf("--- Step 9: Send STOP ---\n");
  hsm.Dispatch({EVENT_STOP, nullptr});
  std::printf("Current state: %s\n\n", hsm.CurrentStateName());

  std::printf("=== Demo Complete ===\n");
  std::printf("Final stats: initialized=%d, error_count=%d, cycle_count=%d\n",
              ctx.initialized, ctx.error_count, ctx.cycle_count);

  return 0;
}
