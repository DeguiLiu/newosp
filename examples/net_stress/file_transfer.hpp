/**
 * @file file_transfer.hpp
 * @brief File transfer with simulated packet loss and table-driven HSM.
 *
 * Runs on a dedicated thread, independent of the echo channel. The transfer
 * HSM is a static table (osp::TableHsm); RPC results are recorded by the
 * Sending action, and the driver loop translates the result into the next
 * event (kFtAckOk / kFtAckFail) so every transition decision lives in a
 * table row, never in an action.
 *
 * HSM States (7):
 *   Root
 *   +-- Idle (initial)
 *   +-- Transferring
 *   |   +-- Sending     -- send one chunk (RPC result recorded in ctx)
 *   |   +-- Retrying    -- resend after simulated loss or RPC failure
 *   +-- Complete        -- terminal success (entry flags ctx.complete/success)
 *   +-- Failed          -- terminal failure (entry flags ctx.complete)
 *
 * The former WaitingAck placeholder state was removed: in the synchronous
 * RPC mode the driver loop is the waiter; a future async mode can add the
 * state back with real wait semantics.
 *
 * Simulated packet loss: a configurable drop_rate (0.0-1.0) causes random
 * chunks to be "lost" (server receives but client ignores the response).
 * The HSM detects the timeout and retransmits.
 */

#ifndef NET_STRESS_FILE_TRANSFER_HPP_
#define NET_STRESS_FILE_TRANSFER_HPP_

#include "protocol.hpp"

#include "osp/hsm.hpp"
#include "osp/hsm_table.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/service.hpp"
#include "osp/vocabulary.hpp"

#include <cstring>

#include <atomic>
#include <random>
#include <thread>

namespace net_stress {

// ============================================================================
// Transfer HSM Events
// ============================================================================

enum FtEvtId : uint32_t {
  kFtStart = 100,
  kFtChunkSent,  // driver asks Sending to transmit the current chunk
  kFtAckOk,      // chunk acknowledged (or transfer complete)
  kFtAckFail,    // chunk lost or RPC failed -> Retrying
  kFtRetry,      // retry budget allows another attempt -> Sending
  kFtDone,       // all chunks acknowledged -> Complete
  kFtAbort,      // unrecoverable (max retries, connection lost) -> Failed
};

// ============================================================================
// State indices (fixed by table order)
// ============================================================================

enum FtSmState : int32_t {
  kFsRoot = 0,
  kFsIdle,
  kFsTransferring,
  kFsSending,
  kFsRetrying,
  kFsComplete,
  kFsFailed,
  kFsCount
};

// ============================================================================
// Transfer Context
// ============================================================================

static constexpr uint32_t kFtSmMaxStates = 8;
static constexpr uint32_t kMaxRetries = 3;
static constexpr uint32_t kChunkSize = 2048U;  // bytes per chunk

struct FtCtx;

// ============================================================================
// Row actions and guards (declarations; bodies after FtCtx is complete)
// ============================================================================

namespace ft_sm_detail {

/// Result of one Sending action, consumed by the driver loop.
enum class SendResult : uint8_t {
  kOk,       ///< chunk acknowledged
  kDone,     ///< last chunk acknowledged
  kLost,     ///< simulated drop
  kRpcFail,  ///< RPC call failed or rejected
  kNoConn,   ///< file RPC not connected
};

/// Idle + start: reset the transfer counters.
void ActStart(FtCtx& ctx, const void* data) noexcept;

/// Sending + chunk-sent: transmit the current chunk and record the result
/// (ctx.send_result). No transition decision here.
void ActSend(FtCtx& ctx, const void* data) noexcept;

/// Retrying + retry: consume one retry unit.
void ActRetry(FtCtx& ctx, const void* data) noexcept;

/// Complete entry: flag success.
void OnEnterComplete(FtCtx& ctx) noexcept;

/// Failed entry: flag failure.
void OnEnterFailed(FtCtx& ctx) noexcept;

/// Guard: retry budget still available.
bool GuardCanRetry(FtCtx& ctx, const void* data) noexcept;

}  // namespace ft_sm_detail

// ============================================================================
// Transfer Context (definition)
// ============================================================================

struct FtCtx {
  osp::TableHsm<FtCtx, kFsCount, 10>* sm;

  // Connection
  uint32_t client_id;
  osp::FixedString<63> server_host;
  uint16_t file_port;
  osp::Client<FileTransferReq, FileTransferResp> file_cli;

  // File data
  const uint8_t* file_data;
  uint32_t file_size;
  uint32_t total_chunks;
  uint32_t current_chunk;
  uint32_t bytes_sent;

  // Retry state
  uint32_t retry_count;
  float drop_rate;  // simulated loss probability (0.0 - 1.0)

  // One-shot result of the last ActSend (read by the driver loop).
  ft_sm_detail::SendResult send_result;

  // Heartbeat (optional, from ThreadWatchdog)
  osp::ThreadHeartbeat* heartbeat;

  // Statistics
  std::atomic<uint32_t> chunks_ok;
  std::atomic<uint32_t> chunks_retried;
  std::atomic<bool> complete;
  std::atomic<bool> success;
};

using FtSm = osp::TableHsm<FtCtx, kFsCount, 10>;

inline void InitFtCtx(FtCtx& c) noexcept {
  c.sm = nullptr;
  c.client_id = 0;
  c.server_host.assign(osp::TruncateToCapacity, "127.0.0.1");
  c.file_port = kFilePort;
  c.file_data = nullptr;
  c.file_size = 0;
  c.total_chunks = 0;
  c.current_chunk = 0;
  c.bytes_sent = 0;
  c.retry_count = 0;
  c.drop_rate = 0.1f;  // 10% simulated loss
  c.send_result = ft_sm_detail::SendResult::kNoConn;
  c.heartbeat = nullptr;
  c.chunks_ok.store(0, std::memory_order_relaxed);
  c.chunks_retried.store(0, std::memory_order_relaxed);
  c.complete.store(false, std::memory_order_relaxed);
  c.success.store(false, std::memory_order_relaxed);
}

// ============================================================================
// Simulated Packet Loss
// ============================================================================

inline bool SimulateDrop(float rate) noexcept {
  if (rate <= 0.0f) {
    return false;
  }
  // Thread-safe PRNG (thread_local avoids contention)
  static thread_local std::mt19937 gen(static_cast<uint32_t>(NowNs() & 0xFFFFFFFFULL));
  std::uniform_real_distribution<float> dis(0.0f, 1.0f);
  return dis(gen) < rate;
}

// ============================================================================
// Static table
// ============================================================================

namespace ft_sm_detail {

inline constexpr osp::StateDef<FtCtx> kFtStates[kFsCount] = {
    {"Root", -1, nullptr, nullptr},
    {"Idle", kFsRoot, nullptr, nullptr},
    {"Transferring", kFsRoot, nullptr, nullptr},
    {"Sending", kFsTransferring, nullptr, nullptr},
    {"Retrying", kFsTransferring, nullptr, nullptr},
    {"Complete", kFsRoot, &OnEnterComplete, nullptr},
    {"Failed", kFsRoot, &OnEnterFailed, nullptr},
};

inline constexpr osp::TransitionDef<FtCtx> kFtTrans[] = {
    // Idle: start resets counters and enters Sending.
    {kFsIdle, kFtStart, kFsSending, osp::TransitionKind::kExternal, ActStart, nullptr},

    // Transferring (composite): abort bubbles up from Sending/Retrying.
    {kFsTransferring, kFtAbort, kFsFailed, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Sending: transmit (action records the result; driver dispatches the
    // follow-up event). Chunk-sent keeps us in Sending while the driver
    // inspects ctx.send_result.
    {kFsSending, kFtChunkSent, kFsSending, osp::TransitionKind::kInternal, ActSend, nullptr},
    {kFsSending, kFtAckOk, kFsSending, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kFsSending, kFtDone, kFsComplete, osp::TransitionKind::kExternal, nullptr, nullptr},
    {kFsSending, kFtAckFail, kFsRetrying, osp::TransitionKind::kExternal, nullptr, nullptr},

    // Retrying: retry budget available returns to Sending, exhaustion fails.
    {kFsRetrying, kFtRetry, kFsSending, osp::TransitionKind::kExternal, ActRetry, GuardCanRetry},
    {kFsRetrying, kFtRetry, kFsFailed, osp::TransitionKind::kExternal, nullptr, nullptr},
};

inline constexpr uint32_t kFtTransCount = sizeof(kFtTrans) / sizeof(kFtTrans[0]);

// --- Action / guard bodies (after FtCtx is complete) -----------------------

inline void ActStart(FtCtx& ctx, const void* /*data*/) noexcept {
  ctx.current_chunk = 0;
  ctx.bytes_sent = 0;
  ctx.retry_count = 0;
}

inline void ActSend(FtCtx& ctx, const void* /*data*/) noexcept {
  if (!ctx.file_cli.IsConnected()) {
    ctx.send_result = SendResult::kNoConn;
    return;
  }

  FileTransferReq req{};
  req.client_id = ctx.client_id;
  req.chunk_seq = ctx.current_chunk;
  req.total_chunks = ctx.total_chunks;
  req.file_size = ctx.file_size;

  const uint32_t offset = ctx.current_chunk * kChunkSize;
  const uint32_t remaining = ctx.file_size - offset;
  req.chunk_len = (remaining > kChunkSize) ? kChunkSize : remaining;
  if (req.chunk_len > kMaxPayloadBytes) {
    req.chunk_len = kMaxPayloadBytes;
  }

  FillPattern(req.data, req.chunk_len, ctx.current_chunk);

  if (SimulateDrop(ctx.drop_rate)) {
    OSP_LOG_WARN("FILE_TX", "[%u] Simulated drop: chunk %u/%u", ctx.client_id, ctx.current_chunk, ctx.total_chunks);
    ctx.chunks_retried.fetch_add(1, std::memory_order_relaxed);
    ctx.send_result = SendResult::kLost;
    return;
  }

  auto resp = ctx.file_cli.Call(req, 3000);
  if (!resp.has_value() || resp.value().accepted == 0) {
    OSP_LOG_WARN("FILE_TX", "[%u] Chunk %u failed", ctx.client_id, ctx.current_chunk);
    ctx.send_result = SendResult::kRpcFail;
    return;
  }

  ctx.bytes_sent += req.chunk_len;
  ctx.chunks_ok.fetch_add(1, std::memory_order_relaxed);
  ctx.retry_count = 0;
  ctx.current_chunk++;
  ctx.send_result = (ctx.current_chunk >= ctx.total_chunks) ? SendResult::kDone : SendResult::kOk;
}

inline void ActRetry(FtCtx& ctx, const void* /*data*/) noexcept {
  ++ctx.retry_count;
}

inline void OnEnterComplete(FtCtx& ctx) noexcept {
  ctx.complete.store(true, std::memory_order_relaxed);
  ctx.success.store(true, std::memory_order_relaxed);
}

inline void OnEnterFailed(FtCtx& ctx) noexcept {
  ctx.complete.store(true, std::memory_order_relaxed);
  ctx.success.store(false, std::memory_order_relaxed);
}

inline bool GuardCanRetry(FtCtx& ctx, const void* /*data*/) noexcept {
  return ctx.retry_count < kMaxRetries;
}

}  // namespace ft_sm_detail

// ============================================================================
// Build File Transfer HSM (API-compatible with the previous form)
// ============================================================================

inline void BuildFtSm(FtSm& sm, FtCtx& ctx) noexcept {
  ctx.sm = &sm;
  sm.SetInitialState(kFsIdle);
  sm.Start();
}

// ============================================================================
// Run File Transfer (blocking, intended for dedicated thread)
// ============================================================================

inline bool RunFileTransfer(FtCtx& ctx) noexcept {
  ctx.file_cli.Close();
  ctx.complete.store(false, std::memory_order_relaxed);
  ctx.success.store(false, std::memory_order_relaxed);
  ctx.current_chunk = 0;
  ctx.bytes_sent = 0;
  ctx.retry_count = 0;
  ctx.total_chunks = 0;
  ctx.chunks_ok.store(0, std::memory_order_relaxed);
  ctx.chunks_retried.store(0, std::memory_order_relaxed);

  // Connect to file service
  auto cli_r = osp::Client<FileTransferReq, FileTransferResp>::Connect(ctx.server_host.c_str(), ctx.file_port, 3000);
  if (!cli_r.has_value()) {
    OSP_LOG_ERROR("FILE_TX", "[%u] Connect to file service failed", ctx.client_id);
    return false;
  }
  ctx.file_cli = std::move(cli_r.value());

  // Calculate chunks
  ctx.total_chunks = (ctx.file_size + kChunkSize - 1) / kChunkSize;

  OSP_LOG_INFO("FILE_TX",
               "[%u] Starting: %u bytes, %u chunks, "
               "drop_rate=%.0f%%",
               ctx.client_id, ctx.file_size, ctx.total_chunks, static_cast<double>(ctx.drop_rate) * 100.0);

  // Start HSM (reset a reused terminal HSM to Idle so kFtStart matches).
  ctx.sm->ForceTransition(kFsIdle);
  ctx.sm->Dispatch(osp::Event{kFtStart, nullptr});

  // Driver loop: translate the Sending action result into the next event.
  // All transition decisions live in the table; this loop only observes.
  while (!ctx.complete.load(std::memory_order_relaxed)) {
    if (ctx.heartbeat != nullptr) {
      ctx.heartbeat->Beat();
    }

    if (ctx.sm->CurrentState() != kFsSending) {
      // Retrying: brief delay, then let the table decide retry vs fail.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      ctx.sm->Dispatch(osp::Event{kFtRetry, nullptr});
      continue;
    }

    ctx.sm->Dispatch(osp::Event{kFtChunkSent, nullptr});

    switch (ctx.send_result) {
      case ft_sm_detail::SendResult::kDone:
        ctx.sm->Dispatch(osp::Event{kFtDone, nullptr});
        break;
      case ft_sm_detail::SendResult::kOk:
        ctx.sm->Dispatch(osp::Event{kFtAckOk, nullptr});
        break;
      case ft_sm_detail::SendResult::kLost:
      case ft_sm_detail::SendResult::kRpcFail:
      case ft_sm_detail::SendResult::kNoConn:
      default:
        ctx.sm->Dispatch(osp::Event{kFtAckFail, nullptr});
        break;
    }
  }

  const bool ok = ctx.success.load(std::memory_order_relaxed);
  OSP_LOG_INFO("FILE_TX", "[%u] %s: sent=%u/%u retries=%u", ctx.client_id, ok ? "Complete" : "Failed",
               ctx.chunks_ok.load(std::memory_order_relaxed), ctx.total_chunks,
               ctx.chunks_retried.load(std::memory_order_relaxed));

  ctx.file_cli.Close();
  return ok;
}

}  // namespace net_stress

#endif  // NET_STRESS_FILE_TRANSFER_HPP_