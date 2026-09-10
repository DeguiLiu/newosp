/**
 * @file hsm_bt_combo_demo.cpp
 * @brief Demo combining TableHsm, BehaviorTree and EventLoop for an industrial
 *        device controller.
 *
 * Architecture:
 *   - TableHsm manages high-level device modes: Idle -> Initializing -> Running
 *     -> Error -> Shutdown, as a static StateDef/TransitionDef table.
 *   - BT manages the Running mode's behavior: check sensors -> execute task ->
 *     report status.
 *   - EventLoop steps a scripted signal sequence one event per timer tick,
 *     then self-checks the final context against expected values.
 *
 * Demonstrates:
 *   - Table-driven state transitions (no per-state handler functions)
 *   - Using BT inside a HSM transition action
 *   - Deterministic scripted behavior with RESULT PASS/FAIL + exit code
 */

#include "osp/bt.hpp"
#include "osp/event_loop.hpp"
#include "osp/hsm_table.hpp"

#include <cstdint>
#include <cstdio>

// ============================================================================
// Device Context
// ============================================================================

struct DeviceContext {
  uint32_t error_count = 0U;
  uint32_t cycle_count = 0U;
  bool initialized = false;
  bool sensor_ok = true;
  osp::BehaviorTree<DeviceContext>* bt_ptr = nullptr;
};

// ============================================================================
// State indices (fixed by table order) and events
// ============================================================================

enum DeviceState : int32_t { kIdle = 0, kInitializing, kRunning, kError, kShutdown, kStateCount };

enum DeviceEvent : uint32_t {
  kEvtStart = 1U,
  kEvtInitDone,
  kEvtError,
  kEvtReset,
  kEvtStop,
  kEvtTick,
};

// ============================================================================
// BT Actions/Conditions
// ============================================================================

osp::NodeStatus CheckSensors(DeviceContext& ctx) {
  if (ctx.sensor_ok) {
    std::printf("  [BT] CheckSensors: OK\n");
    return osp::NodeStatus::kSuccess;
  }
  std::printf("  [BT] CheckSensors: FAILED\n");
  return osp::NodeStatus::kFailure;
}

osp::NodeStatus ExecuteTask(DeviceContext& ctx) {
  ++ctx.cycle_count;
  std::printf("  [BT] ExecuteTask: cycle %u completed\n", ctx.cycle_count);
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus ReportStatus(DeviceContext& ctx) {
  std::printf("  [BT] ReportStatus: cycle=%u\n", ctx.cycle_count);
  return osp::NodeStatus::kSuccess;
}

// ============================================================================
// HSM row actions/guards/entries
// ============================================================================

namespace dev_detail {

void ActBtTick(DeviceContext& ctx, const void* /*data*/) {
  if (ctx.bt_ptr != nullptr) {
    const osp::NodeStatus status = ctx.bt_ptr->Tick();
    std::printf("  [HSM] Running: BT status = %s\n", osp::NodeStatusToString(status));
  }
}

bool GuardErrLt3(DeviceContext& ctx, const void* /*data*/) {
  return ctx.error_count < 3U;
}

void OnEnterInitializing(DeviceContext& ctx) {
  ctx.initialized = true;
  std::printf("  [HSM] Initializing: entry (initialized=true)\n");
}

void OnEnterError(DeviceContext& ctx) {
  ++ctx.error_count;
  std::printf("  [HSM] Error: entry (error_count=%u)\n", ctx.error_count);
}

void OnEnterShutdown(DeviceContext& ctx) {
  (void)ctx;
  std::printf("  [HSM] Shutdown: entry (device shutting down)\n");
}

inline constexpr osp::StateDef<DeviceContext> kStates[kStateCount] = {
    {"Idle", -1, nullptr, nullptr},
    {"Initializing", -1, OnEnterInitializing, nullptr},
    {"Running", -1, nullptr, nullptr},
    {"Error", -1, OnEnterError, nullptr},
    {"Shutdown", -1, OnEnterShutdown, nullptr},
};

inline constexpr osp::TransitionDef<DeviceContext> kTransitions[] = {
    {kIdle, kEvtStart, kInitializing, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kInitializing, kEvtInitDone, kRunning, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kRunning, kEvtTick, kRunning, osp::TransitionKind::kInternal, ActBtTick, nullptr},
    {kRunning, kEvtError, kError, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kRunning, kEvtStop, kShutdown, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kError, kEvtReset, kIdle, osp::TransitionKind::kExternal, nullptr, GuardErrLt3},
    {kError, kEvtReset, kShutdown, osp::TransitionKind::kExternal, nullptr, nullptr},
};

inline constexpr uint32_t kTransCount = sizeof(kTransitions) / sizeof(kTransitions[0]);

}  // namespace dev_detail

// ============================================================================
// Scripted EventLoop driver
// ============================================================================

class BtComboLoop : public osp::EventLoop<BtComboLoop> {
 public:
  BtComboLoop() : hsm_(ctx_, dev_detail::kStates, kStateCount, dev_detail::kTransitions, dev_detail::kTransCount) {
    hsm_.SetInitialState(kIdle);
    hsm_.Start();
    static_cast<void>(Schedule(1U));
  }

  void OnTimer(uint32_t /*timer_id*/) {
    if (idx_ >= kScriptLen) {
      Stop();
      return;
    }
    hsm_.Dispatch(osp::Event{kScript[idx_], nullptr});
    ++idx_;
  }

  DeviceContext* ctx() { return &ctx_; }
  const osp::TableHsm<DeviceContext, kStateCount, dev_detail::kTransCount>& hsm() const { return hsm_; }

 private:
  static constexpr uint32_t kScript[] = {
      kEvtStart, kEvtInitDone, kEvtTick,     kEvtTick, kEvtTick, kEvtTick, kEvtTick, kEvtError,
      kEvtReset, kEvtStart,    kEvtInitDone, kEvtTick, kEvtTick, kEvtTick, kEvtStop,
  };
  static constexpr uint32_t kScriptLen = sizeof(kScript) / sizeof(kScript[0]);

  DeviceContext ctx_;
  osp::TableHsm<DeviceContext, kStateCount, dev_detail::kTransCount> hsm_;
  uint32_t idx_ = 0U;
};

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== HSM + BT Combo Demo (TableHsm + EventLoop) ===\n\n");

  BtComboLoop loop;

  // Build BT (used inside the Running+TICK row action).
  osp::BehaviorTree<DeviceContext> bt(*loop.ctx(), "device_bt");
  loop.ctx()->bt_ptr = &bt;

  int32_t root = bt.AddSequence("root");
  bt.AddCondition("CheckSensors", CheckSensors, root);
  bt.AddAction("ExecuteTask", ExecuteTask, root);
  bt.AddAction("ReportStatus", ReportStatus, root);
  bt.SetRoot(root);

  std::printf("BT built: %u nodes\n\n", bt.NodeCount());

  loop.Run();

  // Self-check against the scripted scenario.
  const bool pass = (loop.ctx()->initialized) && (loop.ctx()->error_count == 1U) && (loop.ctx()->cycle_count == 8U) &&
                    (loop.hsm().CurrentState() == kShutdown);

  std::printf("\n=== Final Statistics ===\n");
  std::printf("initialized:  %s\n", loop.ctx()->initialized ? "true" : "false");
  std::printf("error_count:  %u\n", loop.ctx()->error_count);
  std::printf("cycle_count:  %u\n", loop.ctx()->cycle_count);
  std::printf("final state:  %s\n", loop.hsm().CurrentStateName());
  std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

  return pass ? 0 : 1;
}
