/**
 * @file client_sm.hpp
 * @brief HSM-driven client connection state machine for net_stress demo.
 *
 * Table-driven form (osp::TableHsm): every (state, event) pair is one
 * constexpr table row; RPC side effects live in row actions; the hierarchy
 * (Root -> {Disconnected, Connecting, Connected -> {Idle, Running}, Error})
 * is expressed by StateDef parents.
 *
 * States (7):
 *   Root
 *   +-- Disconnected (initial)
 *   +-- Connecting
 *   +-- Connected
 *   |   +-- Idle       -- handshake done, waiting for test start
 *   |   +-- Running    -- periodic echo in progress
 *   +-- Error
 *
 * newosp components:
 *   - osp::TableHsm     -- static transition table HSM engine
 *   - osp::expected     -- error handling without exceptions
 *   - osp::FixedString  -- stack-allocated name strings
 *   - osp::log          -- structured logging
 */

#ifndef NET_STRESS_CLIENT_SM_HPP_
#define NET_STRESS_CLIENT_SM_HPP_

#include "protocol.hpp"

#include "osp/hsm.hpp"
#include "osp/hsm_table.hpp"
#include "osp/log.hpp"
#include "osp/service.hpp"
#include "osp/vocabulary.hpp"

#include <cstring>

#include <atomic>

namespace net_stress {

// ============================================================================
// HSM Event IDs
// ============================================================================

enum CEvtId : uint32_t {
  kEvtConnect = 1,
  kEvtHandshakeOk,
  kEvtStartTest,
  kEvtStopTest,
  kEvtTick,
  kEvtDisconnect,
  kEvtError,
  kEvtRetry,
};

// ============================================================================
// State indices (fixed by table order; constexpr table rows reference them)
// ============================================================================

enum ClientSmState : int32_t {
  kCsRoot = 0,
  kCsDisconnected,
  kCsConnecting,
  kCsConnected,
  kCsIdle,
  kCsRunning,
  kCsError,
  kCsCount
};

// ============================================================================
// Per-Client Context (POD-like, no inheritance)
// ============================================================================

struct ClientCtx;

// ============================================================================
// Row actions (free functions; side effects only, no transition decisions)
// ============================================================================

namespace client_sm_detail {

// Declarations only: the table below takes addresses; bodies come after
// ClientCtx is complete.

/// Disconnected/Connecting -> Connecting: nothing to do (handshake is driven
/// externally by DoHandshake, not by the HSM).
void ActNone(ClientCtx& ctx, const void* data) noexcept;

/// Connected + disconnect: release RPC handles.
void ActCleanup(ClientCtx& ctx, const void* data) noexcept;

/// Error + retry: release RPC handles before returning to Disconnected.
void ActCleanupRetry(ClientCtx& ctx, const void* data) noexcept;

/// Running + tick: one echo RPC round trip (recorded in ctx atomics; no
/// transition decision -- the external error path dispatches kEvtError).
void ActEchoTick(ClientCtx& ctx, const void* data) noexcept;

/// Cleanup RPC clients.
void CleanupClient(ClientCtx& ctx) noexcept;

}  // namespace client_sm_detail

// ============================================================================
// Per-Client Context (definition after actions need the complete type)
// ============================================================================

static constexpr uint32_t kSmMaxStates = 8;

struct ClientCtx {
  osp::TableHsm<ClientCtx, kCsCount, 12>* sm;

  // Identity
  uint32_t id;
  osp::FixedString<31> name;
  osp::FixedString<63> server_host;
  uint16_t hs_port;
  uint16_t echo_port;

  // Handshake result
  uint32_t slot;
  uint32_t server_id;
  bool connected;

  // RPC client handles (owned in-place by context, no heap allocation)
  osp::Client<HandshakeReq, HandshakeResp> hs_cli;
  osp::Client<EchoReq, EchoResp> echo_cli;

  // Test config
  uint32_t interval_ms;
  uint32_t payload_len;
  uint32_t seq;

  // Counters (atomic for cross-thread stats reading)
  std::atomic<uint32_t> n_sent;
  std::atomic<uint32_t> n_recv;
  std::atomic<uint32_t> n_err;
  std::atomic<uint64_t> sum_rtt_us;
};

// Default-initialize a ClientCtx (cannot use default member initializers
// because atomic members make the struct non-trivially-constructible).
inline void InitClientCtx(ClientCtx& c) noexcept {
  c.sm = nullptr;
  c.id = 0;
  c.name.assign(osp::TruncateToCapacity, "client");
  c.server_host.assign(osp::TruncateToCapacity, "127.0.0.1");
  c.hs_port = kHandshakePort;
  c.echo_port = kEchoPort;
  c.slot = 0;
  c.server_id = 0;
  c.connected = false;
  c.interval_ms = kDefaultIntervalMs;
  c.payload_len = kDefaultPayloadLen;
  c.seq = 0;
  c.n_sent.store(0, std::memory_order_relaxed);
  c.n_recv.store(0, std::memory_order_relaxed);
  c.n_err.store(0, std::memory_order_relaxed);
  c.sum_rtt_us.store(0, std::memory_order_relaxed);
}

using ClientSm = osp::TableHsm<ClientCtx, kCsCount, 12>;

// ============================================================================
// Static table
// ============================================================================

namespace client_sm_detail {

inline constexpr osp::StateDef<ClientCtx> kClientStates[kCsCount] = {
    {"Root", -1, nullptr, nullptr},
    {"Disconnected", kCsRoot, nullptr, nullptr},
    {"Connecting", kCsRoot, nullptr, nullptr},
    {"Connected", kCsRoot, nullptr, nullptr},
    {"Idle", kCsConnected, nullptr, nullptr},
    {"Running", kCsConnected, nullptr, nullptr},
    {"Error", kCsRoot, nullptr, nullptr},
};

inline constexpr osp::TransitionDef<ClientCtx> kClientTrans[] = {
    // Disconnected: connect or retry starts the handshake sequence.
    {kCsDisconnected, kEvtConnect, kCsConnecting, osp::TransitionKind::kExternal, ActNone, nullptr},
    {kCsDisconnected, kEvtRetry, kCsConnecting, osp::TransitionKind::kExternal, ActNone, nullptr},

    // Connecting: result comes from the external DoHandshake driver.
    {kCsConnecting, kEvtHandshakeOk, kCsIdle, osp::TransitionKind::kExternal, ActNone, nullptr},
    {kCsConnecting, kEvtError, kCsError, osp::TransitionKind::kExternal, ActNone, nullptr},

    // Connected (composite): disconnect tears the RPC handles down.
    {kCsConnected, kEvtDisconnect, kCsDisconnected, osp::TransitionKind::kExternal, ActCleanup, nullptr},

    // Idle: test start moves into Running.
    {kCsIdle, kEvtStartTest, kCsRunning, osp::TransitionKind::kExternal, ActNone, nullptr},

    // Running: each tick is one echo RPC (stays in Running); stop returns to Idle.
    {kCsRunning, kEvtTick, kCsRunning, osp::TransitionKind::kInternal, ActEchoTick, nullptr},
    {kCsRunning, kEvtStopTest, kCsIdle, osp::TransitionKind::kExternal, ActNone, nullptr},

    // Error: retry cleans up and returns to Disconnected.
    {kCsError, kEvtRetry, kCsDisconnected, osp::TransitionKind::kExternal, ActCleanupRetry, nullptr},
};

inline constexpr uint32_t kClientTransCount = sizeof(kClientTrans) / sizeof(kClientTrans[0]);

// --- Action bodies (after ClientCtx is complete) ---------------------------

inline void ActNone(ClientCtx& /*ctx*/, const void* /*data*/) noexcept {}

inline void ActCleanup(ClientCtx& ctx, const void* /*data*/) noexcept {
  CleanupClient(ctx);
}

inline void ActCleanupRetry(ClientCtx& ctx, const void* /*data*/) noexcept {
  CleanupClient(ctx);
}

inline void ActEchoTick(ClientCtx& ctx, const void* /*data*/) noexcept {
  if (!ctx.echo_cli.IsConnected()) {
    ctx.n_err.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  EchoReq req{};
  req.client_id = ctx.id;
  req.seq = ctx.seq++;
  req.payload_len = (ctx.payload_len > kMaxPayloadBytes) ? kMaxPayloadBytes : ctx.payload_len;
  req.send_ts_ns = NowNs();
  FillPattern(req.payload, req.payload_len, req.seq);

  ctx.n_sent.fetch_add(1, std::memory_order_relaxed);

  auto resp = ctx.echo_cli.Call(req, 2000);
  if (resp.has_value() && resp.value().seq == req.seq) {
    ctx.n_recv.fetch_add(1, std::memory_order_relaxed);
    const uint64_t rtt = (NowNs() - resp.value().client_ts_ns) / 1000ULL;
    ctx.sum_rtt_us.fetch_add(rtt, std::memory_order_relaxed);
  } else {
    ctx.n_err.fetch_add(1, std::memory_order_relaxed);
  }
}

inline void CleanupClient(ClientCtx& ctx) noexcept {
  ctx.echo_cli.Close();
  ctx.hs_cli.Close();
  ctx.connected = false;
}

}  // namespace client_sm_detail

// ============================================================================
// Build HSM (API-compatible with the previous StateMachine form)
// ============================================================================

inline void BuildClientSm(ClientSm& sm, ClientCtx& ctx) noexcept {
  ctx.sm = &sm;
  sm.SetInitialState(kCsDisconnected);
  sm.Start();
}

// ============================================================================
// Dispatch Helper
// ============================================================================

inline void Dispatch(ClientCtx& ctx, uint32_t evt_id) noexcept {
  osp::Event evt{evt_id, nullptr};
  ctx.sm->Dispatch(evt);
}

// ============================================================================
// Handshake Helper (called after Connecting transition)
// ============================================================================

inline bool DoHandshake(ClientCtx& ctx) noexcept {
  client_sm_detail::CleanupClient(ctx);

  // Connect handshake RPC
  auto hs_r = osp::Client<HandshakeReq, HandshakeResp>::Connect(ctx.server_host.c_str(), ctx.hs_port,
                                                                static_cast<int32_t>(kConnectTimeoutMs));
  if (!hs_r.has_value()) {
    OSP_LOG_ERROR("CLIENT", "[%u] Handshake connect failed", ctx.id);
    return false;
  }

  ctx.hs_cli = std::move(hs_r.value());

  HandshakeReq req{};
  req.client_id = ctx.id;
  req.version = kProtocolVersion;
  std::strncpy(req.name, ctx.name.c_str(), sizeof(req.name) - 1);
  req.name[sizeof(req.name) - 1] = '\0';

  auto resp = ctx.hs_cli.Call(req, static_cast<int32_t>(kConnectTimeoutMs));
  if (!resp.has_value() || resp.value().accepted == 0) {
    OSP_LOG_ERROR("CLIENT", "[%u] Handshake rejected", ctx.id);
    client_sm_detail::CleanupClient(ctx);
    return false;
  }

  ctx.slot = resp.value().slot;
  ctx.server_id = resp.value().server_id;
  ctx.echo_port = resp.value().echo_port;

  // Connect echo RPC
  auto echo_r = osp::Client<EchoReq, EchoResp>::Connect(ctx.server_host.c_str(), ctx.echo_port,
                                                        static_cast<int32_t>(kConnectTimeoutMs));
  if (!echo_r.has_value()) {
    OSP_LOG_ERROR("CLIENT", "[%u] Echo connect failed", ctx.id);
    client_sm_detail::CleanupClient(ctx);
    return false;
  }

  ctx.echo_cli = std::move(echo_r.value());
  ctx.connected = true;

  OSP_LOG_INFO("CLIENT", "[%u] Connected: slot=%u echo_port=%u", ctx.id, ctx.slot, ctx.echo_port);
  return true;
}

}  // namespace net_stress

#endif  // NET_STRESS_CLIENT_SM_HPP_