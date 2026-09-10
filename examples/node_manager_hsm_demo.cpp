/**
 * @file node_manager_hsm_demo.cpp
 * @brief Four heartbeat-driven node HSMs under one EventLoop timer.
 *
 * Mirrors the libev C++17 node_manager example, rebuilt on newosp primitives:
 * each node runs an independent three-state static-table HSM (Connected ->
 * Suspect -> Disconnected) driven by a heartbeat signal; a single periodic
 * EventLoop timer steps a scripted per-node heartbeat/miss sequence, one tick
 * at a time. The final states are self-checked against expected counters and
 * the process returns 0 on PASS, 1 on FAIL.
 */

#include "osp/event_loop.hpp"
#include "osp/hsm_table.hpp"

#include <cstdint>
#include <cstdio>

namespace {

enum Signal : uint32_t {
  kHeartbeatOk = 1U,
  kHeartbeatMiss = 2U,
  kDisconnect = 3U,
  kReconnect = 4U,
};

enum State : int32_t {
  kConnected = 0,
  kSuspect = 1,
  kDisconnected = 2,
  kStateCount = 3,
};

struct NodeCtx {
  uint16_t node_id = 0U;
  uint32_t missed_heartbeats = 0U;  // consecutive misses in the current run
  uint32_t total_heartbeats = 0U;   // heartbeats received
  bool connected = false;
};

static constexpr uint32_t kMissToSuspect = 2U;     // Connected -> Suspect threshold
static constexpr uint32_t kMissToDisconnect = 5U;  // Suspect -> Disconnected threshold

// --- Row actions / guards (free functions; decisions live in the table) -----

void ActHbOk(NodeCtx& ctx, const void* /*data*/) noexcept {
  ++ctx.total_heartbeats;
  ctx.missed_heartbeats = 0U;
  ctx.connected = true;
}

void ActMiss(NodeCtx& ctx, const void* /*data*/) noexcept {
  ++ctx.missed_heartbeats;
}

void ActReconnect(NodeCtx& ctx, const void* /*data*/) noexcept {
  ctx.missed_heartbeats = 0U;
  ctx.connected = true;
}

bool GuardMissToSuspect(NodeCtx& ctx, const void* /*data*/) noexcept {
  return (ctx.missed_heartbeats + 1U) >= kMissToSuspect;
}

bool GuardMissToDisconnect(NodeCtx& ctx, const void* /*data*/) noexcept {
  return (ctx.missed_heartbeats + 1U) >= kMissToDisconnect;
}

void OnEnterConnected(NodeCtx& ctx) noexcept {
  ctx.connected = true;
  std::printf("  [Node %u] -> Connected\n", ctx.node_id);
}

void OnEnterDisconnected(NodeCtx& ctx) noexcept {
  ctx.connected = false;
  std::printf("  [Node %u] -> Disconnected\n", ctx.node_id);
}

inline constexpr osp::StateDef<NodeCtx> kStates[kStateCount] = {
    {"Connected", -1, OnEnterConnected, nullptr},
    {"Suspect", -1, nullptr, nullptr},
    {"Disconnected", -1, OnEnterDisconnected, nullptr},
};

inline constexpr osp::TransitionDef<NodeCtx> kTransitions[] = {
    // Connected: heartbeat counts; a miss advances the suspected counter and,
    // once the threshold is crossed (guard reads the pre-store value), moves to
    // Suspect.
    {kConnected, kHeartbeatOk, kConnected, osp::TransitionKind::kInternal, ActHbOk, nullptr},
    {kConnected, kHeartbeatMiss, kSuspect, osp::TransitionKind::kExternal, ActMiss, GuardMissToSuspect},
    {kConnected, kHeartbeatMiss, kConnected, osp::TransitionKind::kInternal, ActMiss, nullptr},
    {kConnected, kDisconnect, kDisconnected, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Suspect: an OK recovers; a miss consumes the disconnect budget.
    {kSuspect, kHeartbeatOk, kConnected, osp::TransitionKind::kExternal, ActHbOk, nullptr},
    {kSuspect, kHeartbeatMiss, kDisconnected, osp::TransitionKind::kExternal, ActMiss, GuardMissToDisconnect},
    {kSuspect, kHeartbeatMiss, kSuspect, osp::TransitionKind::kInternal, ActMiss, nullptr},
    {kSuspect, kDisconnect, kDisconnected, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Disconnected: heartbeat events ignored; reconnect restores the link.
    {kDisconnected, kReconnect, kConnected, osp::TransitionKind::kExternal, ActReconnect, nullptr},
    {kDisconnected, kHeartbeatOk, kDisconnected, osp::TransitionKind::kInternal, nullptr, nullptr},
    {kDisconnected, kHeartbeatMiss, kDisconnected, osp::TransitionKind::kInternal, nullptr, nullptr},
};

inline constexpr uint32_t kTransCount = sizeof(kTransitions) / sizeof(kTransitions[0]);

static constexpr uint32_t kNumNodes = 4U;
static constexpr uint32_t kMaxTicks = 9U;  // length of the longest script
static constexpr uint32_t kTimerId = 1U;

// Per-node heartbeat script, one signal per tick (0 = node exhausted).
// node 0: steady heartbeats            -> stays Connected
// node 1: two misses, then recover     -> Suspect -> Connected
// node 2: five misses, then reconnect  -> Suspect -> Disconnected -> Connected
// node 3: immediate disconnect         -> Disconnected -> Connected
static constexpr uint32_t kScript[kNumNodes][kMaxTicks] = {
    {kHeartbeatOk, kHeartbeatOk, kHeartbeatOk, kHeartbeatOk, 0U, 0U, 0U, 0U, 0U},
    {kHeartbeatOk, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatOk, kHeartbeatOk, 0U, 0U, 0U},
    {kHeartbeatOk, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatMiss, kHeartbeatMiss,
     kReconnect, kHeartbeatOk},
    {kHeartbeatOk, kHeartbeatOk, kDisconnect, kReconnect, kHeartbeatOk, 0U, 0U, 0U, 0U},
};

class NodeLoop : public osp::EventLoop<NodeLoop> {
 public:
  NodeLoop() noexcept
      : nodes_{
            NodeCtx{101U, 0U, 0U, false}, NodeCtx{102U, 0U, 0U, false},
            NodeCtx{103U, 0U, 0U, false}, NodeCtx{104U, 0U, 0U, false},
        },
        hsms_{
            osp::TableHsm<NodeCtx, kStateCount, kTransCount>(nodes_[0], kStates, kStateCount, kTransitions,
                                                             kTransCount),
            osp::TableHsm<NodeCtx, kStateCount, kTransCount>(nodes_[1], kStates, kStateCount, kTransitions,
                                                             kTransCount),
            osp::TableHsm<NodeCtx, kStateCount, kTransCount>(nodes_[2], kStates, kStateCount, kTransitions,
                                                             kTransCount),
            osp::TableHsm<NodeCtx, kStateCount, kTransCount>(nodes_[3], kStates, kStateCount, kTransitions,
                                                             kTransCount),
        } {
    for (uint32_t i = 0U; i < kNumNodes; ++i) {
      hsms_[i].SetInitialState(kConnected);
      hsms_[i].Start();
    }
  }

  bool RunAndCheck() noexcept {
    (void)Schedule(1U);
    Run();
    return Check();
  }

  void OnTimer(uint32_t timer_id) noexcept {
    if (timer_id != kTimerId) {
      return;
    }
    if (tick_ >= kMaxTicks) {
      Stop();
      return;
    }
    for (uint32_t i = 0U; i < kNumNodes; ++i) {
      const uint32_t sig = kScript[i][tick_];
      if (0U != sig) {
        hsms_[i].Dispatch(osp::Event{sig, nullptr});
      }
    }
    ++tick_;
  }

 private:
  bool Check() noexcept {
    static constexpr uint32_t kExpectedHb[kNumNodes] = {4U, 3U, 2U, 3U};
    bool pass = true;
    for (uint32_t i = 0U; i < kNumNodes; ++i) {
      const NodeCtx& ctx = nodes_[i];
      std::printf("Node %u: hb=%u missed=%u [%s]\n", static_cast<unsigned>(ctx.node_id),
                  static_cast<unsigned>(ctx.total_heartbeats), static_cast<unsigned>(ctx.missed_heartbeats),
                  hsms_[i].CurrentStateName());
      if ((ctx.total_heartbeats != kExpectedHb[i]) || (0U != ctx.missed_heartbeats) ||
          (hsms_[i].CurrentState() != kConnected)) {
        pass = false;
      }
    }
    return pass;
  }

  NodeCtx nodes_[kNumNodes];
  osp::TableHsm<NodeCtx, kStateCount, kTransCount> hsms_[kNumNodes];
  uint32_t tick_ = 0U;
};

}  // namespace

int main() {
  std::printf("=== newosp node manager demo (table HSM + event loop) ===\n");

  NodeLoop loop;
  const bool pass = loop.RunAndCheck();

  std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
