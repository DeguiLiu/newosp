/**
 * @file client_sm.hpp
 * @brief HSM-driven client connection state machine for net_stress demo.
 *
 * States (free-function handlers, no virtual dispatch):
 *   Root
 *   +-- Disconnected (initial)
 *   +-- Connecting
 *   +-- Connected
 *   |   +-- Idle       -- handshake done, waiting for test start
 *   |   +-- Running    -- periodic echo in progress
 *   +-- Error
 *
 * newosp components:
 *   - osp::StateMachine   -- HSM engine (StateConfig + free-function handlers)
 *   - osp::expected       -- error handling without exceptions
 *   - osp::FixedString    -- stack-allocated name strings
 *   - osp::log            -- structured logging
 */

#ifndef NET_STRESS_CLIENT_SM_HPP_
#define NET_STRESS_CLIENT_SM_HPP_

#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/service.hpp"
#include "osp/vocabulary.hpp"
#include "protocol.hpp"

#include <atomic>
#include <cstring>

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
// Per-Client Context (POD-like, no inheritance)
// ============================================================================

static constexpr uint32_t kSmMaxStates = 8;
struct ClientCtx;
using ClientSm = osp::StateMachine<ClientCtx, kSmMaxStates>;

struct ClientCtx {
  ClientSm* sm;

  // Identity
  uint32_t id;
  osp::FixedString<31> name;
  osp::FixedString<63> server_host;
  uint16_t hs_port;
  uint16_t echo_port;

  // Handshake result
  uint32_t slot;
  uint32_t server_id;
  bool     connected;

  // RPC client handles (non-owning pointers, lifetime managed externally)
  osp::Client<HandshakeReq, HandshakeResp>* hs_cli;
  osp::Client<EchoReq, EchoResp>*           echo_cli;

  // Test config
  uint32_t interval_ms;
  uint32_t payload_len;
  uint32_t seq;

  // Counters (atomic for cross-thread stats reading)
  std::atomic<uint32_t> n_sent;
  std::atomic<uint32_t> n_recv;
  std::atomic<uint32_t> n_err;
  std::atomic<uint64_t> sum_rtt_us;

  // State indices (populated during BuildClientSm)
  int32_t si_root;
  int32_t si_disc;
  int32_t si_conn_ing;
  int32_t si_conn_ed;
  int32_t si_idle;
  int32_t si_run;
  int32_t si_err;
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
  c.hs_cli = nullptr;
  c.echo_cli = nullptr;
  c.interval_ms = kDefaultIntervalMs;
  c.payload_len = kDefaultPayloadLen;
  c.seq = 0;
  c.n_sent.store(0, std::memory_order_relaxed);
  c.n_recv.store(0, std::memory_order_relaxed);
  c.n_err.store(0, std::memory_order_relaxed);
  c.sum_rtt_us.store(0, std::memory_order_relaxed);
  c.si_root = -1;
  c.si_disc = -1;
  c.si_conn_ing = -1;
  c.si_conn_ed = -1;
  c.si_idle = -1;
  c.si_run = -1;
  c.si_err = -1;
}

// ============================================================================
// Free-Function State Handlers
// ============================================================================

namespace hsm_handler {

using TR = osp::TransitionResult;
using Ev = osp::Event;

inline TR Root(ClientCtx& /*ctx*/, const Ev& /*evt*/) {
  return TR::kHandled;
}

inline TR Disconnected(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtConnect || evt.id == kEvtRetry) {
    return ctx.sm->RequestTransition(ctx.si_conn_ing);
  }
  return TR::kUnhandled;
}

inline TR Connecting(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtHandshakeOk) {
    return ctx.sm->RequestTransition(ctx.si_idle);
  }
  if (evt.id == kEvtError) {
    return ctx.sm->RequestTransition(ctx.si_err);
  }
  return TR::kUnhandled;
}

inline TR Connected(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtDisconnect) {
    ctx.connected = false;
    return ctx.sm->RequestTransition(ctx.si_disc);
  }
  return TR::kUnhandled;
}

inline TR Idle(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtStartTest) {
    return ctx.sm->RequestTransition(ctx.si_run);
  }
  return TR::kUnhandled;
}

inline TR Running(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtTick) {
    if (ctx.echo_cli == nullptr) {
      ctx.n_err.fetch_add(1, std::memory_order_relaxed);
      return TR::kHandled;
    }

    // Build echo request
    EchoReq req{};
    req.client_id = ctx.id;
    req.seq = ctx.seq++;
    req.payload_len = (ctx.payload_len > kMaxPayloadBytes)
                          ? kMaxPayloadBytes : ctx.payload_len;
    req.send_ts_ns = NowNs();
    FillPattern(req.payload, req.payload_len, req.seq);

    ctx.n_sent.fetch_add(1, std::memory_order_relaxed);

    auto resp = ctx.echo_cli->Call(req, 2000);
    if (resp.has_value() && resp.value().seq == req.seq) {
      ctx.n_recv.fetch_add(1, std::memory_order_relaxed);
      uint64_t rtt = (NowNs() - resp.value().client_ts_ns) / 1000ULL;
      ctx.sum_rtt_us.fetch_add(rtt, std::memory_order_relaxed);
    } else {
      ctx.n_err.fetch_add(1, std::memory_order_relaxed);
    }
    return TR::kHandled;
  }
  if (evt.id == kEvtStopTest) {
    return ctx.sm->RequestTransition(ctx.si_idle);
  }
  return TR::kUnhandled;
}

inline TR Error(ClientCtx& ctx, const Ev& evt) {
  if (evt.id == kEvtRetry) {
    ctx.connected = false;
    return ctx.sm->RequestTransition(ctx.si_disc);
  }
  return TR::kUnhandled;
}

}  // namespace hsm_handler

// ============================================================================
// Build HSM
// ============================================================================

inline void BuildClientSm(ClientSm& sm, ClientCtx& ctx) noexcept {
  ctx.sm = &sm;

  using Cfg = osp::StateConfig<ClientCtx>;

  ctx.si_root = sm.AddState(
      Cfg{"Root", -1, hsm_handler::Root, nullptr, nullptr});
  ctx.si_disc = sm.AddState(
      Cfg{"Disconnected", ctx.si_root, hsm_handler::Disconnected,
          nullptr, nullptr});
  ctx.si_conn_ing = sm.AddState(
      Cfg{"Connecting", ctx.si_root, hsm_handler::Connecting,
          nullptr, nullptr});
  ctx.si_conn_ed = sm.AddState(
      Cfg{"Connected", ctx.si_root, hsm_handler::Connected,
          nullptr, nullptr});
  ctx.si_idle = sm.AddState(
      Cfg{"Idle", ctx.si_conn_ed, hsm_handler::Idle, nullptr, nullptr});
  ctx.si_run = sm.AddState(
      Cfg{"Running", ctx.si_conn_ed, hsm_handler::Running,
          nullptr, nullptr});
  ctx.si_err = sm.AddState(
      Cfg{"Error", ctx.si_root, hsm_handler::Error, nullptr, nullptr});

  sm.SetInitialState(ctx.si_disc);
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
  // Connect handshake RPC
  auto hs_r = osp::Client<HandshakeReq, HandshakeResp>::Connect(
      ctx.server_host.c_str(), ctx.hs_port,
      static_cast<int32_t>(kConnectTimeoutMs));
  if (!hs_r.has_value()) {
    OSP_LOG_ERROR("CLIENT", "[%u] Handshake connect failed", ctx.id);
    return false;
  }

  auto* hs = new osp::Client<HandshakeReq, HandshakeResp>(
      std::move(hs_r.value()));
  ctx.hs_cli = hs;

  HandshakeReq req{};
  req.client_id = ctx.id;
  req.version = kProtocolVersion;
  std::strncpy(req.name, ctx.name.c_str(), sizeof(req.name) - 1);

  auto resp = hs->Call(req, static_cast<int32_t>(kConnectTimeoutMs));
  if (!resp.has_value() || resp.value().accepted == 0) {
    OSP_LOG_ERROR("CLIENT", "[%u] Handshake rejected", ctx.id);
    delete hs;
    ctx.hs_cli = nullptr;
    return false;
  }

  ctx.slot = resp.value().slot;
  ctx.server_id = resp.value().server_id;
  ctx.echo_port = resp.value().echo_port;
  ctx.connected = true;

  // Connect echo RPC
  auto echo_r = osp::Client<EchoReq, EchoResp>::Connect(
      ctx.server_host.c_str(), ctx.echo_port,
      static_cast<int32_t>(kConnectTimeoutMs));
  if (!echo_r.has_value()) {
    OSP_LOG_ERROR("CLIENT", "[%u] Echo connect failed", ctx.id);
    delete hs;
    ctx.hs_cli = nullptr;
    return false;
  }

  ctx.echo_cli = new osp::Client<EchoReq, EchoResp>(
      std::move(echo_r.value()));

  OSP_LOG_INFO("CLIENT", "[%u] Connected: slot=%u echo_port=%u",
               ctx.id, ctx.slot, ctx.echo_port);
  return true;
}

/// Cleanup RPC clients.
inline void CleanupClient(ClientCtx& ctx) noexcept {
  delete ctx.echo_cli;
  ctx.echo_cli = nullptr;
  delete ctx.hs_cli;
  ctx.hs_cli = nullptr;
  ctx.connected = false;
}

}  // namespace net_stress

#endif  // NET_STRESS_CLIENT_SM_HPP_
