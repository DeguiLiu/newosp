/**
 * @file hsm_protocol_demo.cpp
 * @brief Table-driven hierarchical protocol HSM driven by an EventLoop timer.
 *
 * Models a simplified connection protocol (TCP-lite / Modbus-like) with
 * hierarchical states showing the power of state inheritance:
 *
 * Operational (root)
 * +-- Disconnected (initial)
 * +-- Connecting
 * +-- Connected (parent state)
 * |   +-- Idle (initial child)
 * |   +-- Active
 * +-- Disconnecting
 *
 * Key feature: the Connected parent state handles DISCONNECT for both the
 * Idle and Active children (parent-state event inheritance). The transition
 * table is a constexpr static table; an EventLoop timer steps a scripted
 * signal sequence one signal per tick, then self-checking asserts the final
 * context counters against expected values (RESULT: PASS / FAIL).
 */

#include "osp/event_loop.hpp"
#include "osp/hsm_table.hpp"

#include <cstdio>

// ============================================================================
// Protocol Events
// ============================================================================

enum ProtocolEvent : uint32_t {
  kConnect = 1U,
  kSynAck = 2U,
  kDisconnect = 3U,
  kFinAck = 4U,
  kTimeout = 5U,
  kDataReady = 6U,
  kDataSent = 7U,
  kError = 8U,
};

// ============================================================================
// Protocol Context
// ============================================================================

struct ProtocolContext {
  uint32_t syn_count = 0U;
  uint32_t ack_count = 0U;
  uint32_t data_sent_count = 0U;
  uint32_t error_count = 0U;
  bool connected = false;
};

// ============================================================================
// State indices (fixed by table order)
// ============================================================================

enum ProtocolState : int32_t {
  kOperational = 0,
  kDisconnected,
  kConnecting,
  kConnected,
  kIdle,
  kActive,
  kDisconnecting,
  kStateCount
};

// ============================================================================
// Row actions and guards (free functions; decisions live in table rows)
// ============================================================================

namespace protocol_detail {

inline void ActAck(ProtocolContext& ctx, const void* /*data*/) noexcept { ++ctx.ack_count; }

inline void ActDataSent(ProtocolContext& ctx, const void* /*data*/) noexcept { ++ctx.data_sent_count; }

inline void ActError(ProtocolContext& ctx, const void* /*data*/) noexcept { ++ctx.error_count; }

inline void OnEnterDisconnected(ProtocolContext& ctx) noexcept { ctx.connected = false; }

inline void OnEnterConnecting(ProtocolContext& ctx) noexcept { ++ctx.syn_count; }

inline void OnEnterConnected(ProtocolContext& ctx) noexcept { ctx.connected = true; }

}  // namespace protocol_detail

// ============================================================================
// Static tables
// ============================================================================

namespace protocol_detail {

using PD = osp::TransitionDef<ProtocolContext>;

inline constexpr osp::StateDef<ProtocolContext> kStates[kStateCount] = {
    {"Operational", -1, nullptr, nullptr},
    {"Disconnected", kOperational, protocol_detail::OnEnterDisconnected, nullptr},
    {"Connecting", kOperational, protocol_detail::OnEnterConnecting, nullptr},
    {"Connected", kOperational, protocol_detail::OnEnterConnected, nullptr},
    {"Idle", kConnected, nullptr, nullptr},
    {"Active", kConnected, nullptr, nullptr},
    {"Disconnecting", kOperational, nullptr, nullptr},
};

// Field order: {from, event, to, kind, action, guard}.
inline constexpr osp::TransitionDef<ProtocolContext> kTrans[] = {
    {kDisconnected, kConnect, kConnecting, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kConnecting, kSynAck, kIdle, osp::TransitionKind::kExternal, protocol_detail::ActAck, nullptr},
    {kConnecting, kTimeout, kDisconnected, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Connected (parent) handles DISCONNECT for both Idle and Active children.
    {kConnected, kDisconnect, kDisconnecting, osp::TransitionKind::kExternal, nullptr, nullptr},

    {kIdle, kDataReady, kActive, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kActive, kDataSent, kIdle, osp::TransitionKind::kExternal, protocol_detail::ActDataSent, nullptr},
    {kActive, kError, kIdle, osp::TransitionKind::kExternal, protocol_detail::ActError, nullptr},
    {kDisconnecting, kFinAck, kDisconnected, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kDisconnecting, kTimeout, kDisconnected, osp::TransitionKind::kExternal, nullptr, nullptr},
};

inline constexpr uint32_t kTransCount = sizeof(kTrans) / sizeof(kTrans[0]);

}  // namespace protocol_detail

// Scripted signal sequence: 14 signals stepped one per timer tick.
static constexpr uint32_t kScript[] = {
    kConnect, kSynAck, kDataReady, kDataSent, kDataReady, kDataSent, kDataReady, kDataSent,
    kDataReady, kError, kDataReady, kDataSent, kDisconnect, kFinAck,
};
static constexpr uint32_t kScriptLen = sizeof(kScript) / sizeof(kScript[0]);

// ============================================================================
// EventLoop: steps the scripted signal sequence one signal per timer tick
// ============================================================================

class ProtocolLoop final : public osp::EventLoop<ProtocolLoop> {
 public:
  ProtocolLoop()
      : hsm_(ctx_, protocol_detail::kStates, kStateCount, protocol_detail::kTrans,
             protocol_detail::kTransCount) {
    hsm_.SetInitialState(kDisconnected);
  }

  ~ProtocolLoop() { Stop(); }

  int Run() noexcept {
    hsm_.Start();
    std::printf("=== HSM Protocol Demo (table-driven) ===\n");

    idx_ = 0U;
    auto timer_r = Schedule(1U);
    if (!timer_r.has_value()) {
      return 1;
    }
    timer_id_ = timer_r.value();

    ClearStop();
    EventLoop::Run();

    const bool pass = (ctx_.syn_count == 1U) && (ctx_.ack_count == 1U) && (ctx_.data_sent_count == 4U) &&
                      (ctx_.error_count == 1U) && (!ctx_.connected) &&
                      (hsm_.CurrentState() == kDisconnected);

    std::printf("\n=== final context ===\n");
    std::printf("syn_count:       %u\n", ctx_.syn_count);
    std::printf("ack_count:       %u\n", ctx_.ack_count);
    std::printf("data_sent_count: %u\n", ctx_.data_sent_count);
    std::printf("error_count:     %u\n", ctx_.error_count);
    std::printf("connected:       %s\n", ctx_.connected ? "true" : "false");
    std::printf("hsm state:       %s\n", hsm_.CurrentStateName());
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
  }

  void OnTimer(uint32_t /*timer_id*/) noexcept {
    if (idx_ >= kScriptLen) {
      Stop();
      return;
    }
    hsm_.Dispatch(osp::Event{kScript[idx_], nullptr});
    ++idx_;
  }

 private:
  ProtocolContext ctx_;
  osp::TableHsm<ProtocolContext, kStateCount, protocol_detail::kTransCount> hsm_;
  uint32_t timer_id_ = 0U;
  uint32_t idx_ = 0U;
};

int main() {
  ProtocolLoop loop;
  return loop.Run();
}
