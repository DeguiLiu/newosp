/**
 * @file file_transfer.hpp
 * @brief File transfer with simulated packet loss and HSM-driven retransmission.
 *
 * Runs on a dedicated thread, independent of the echo channel.
 * Uses osp::StateMachine for transfer state management with retry logic.
 *
 * HSM States:
 *   Root
 *   +-- Idle (initial)
 *   +-- Transferring
 *   |   +-- Sending     -- send next chunk
 *   |   +-- WaitingAck  -- wait for server response
 *   |   +-- Retrying    -- resend after simulated loss
 *   +-- Complete
 *   +-- Failed
 *
 * Simulated packet loss:
 *   A configurable drop_rate (0.0-1.0) causes random chunks to be "lost"
 *   (server receives but client ignores the response). The HSM detects
 *   the timeout and retransmits.
 *
 * newosp components:
 *   - osp::StateMachine   -- transfer HSM
 *   - osp::FixedVector    -- file data buffer (stack-allocated)
 *   - osp::expected       -- error handling
 *   - osp::log            -- structured logging
 */

#ifndef NET_STRESS_FILE_TRANSFER_HPP_
#define NET_STRESS_FILE_TRANSFER_HPP_

#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/service.hpp"
#include "osp/vocabulary.hpp"
#include "protocol.hpp"

#include <atomic>
#include <cstring>
#include <random>
#include <thread>

namespace net_stress {

// ============================================================================
// Transfer HSM Events
// ============================================================================

enum FtEvtId : uint32_t {
  kFtStart = 100,
  kFtChunkSent,
  kFtAckOk,
  kFtAckFail,
  kFtTimeout,
  kFtRetry,
  kFtDone,
  kFtAbort,
};

// ============================================================================
// Transfer Context
// ============================================================================

static constexpr uint32_t kFtSmMaxStates = 8;
static constexpr uint32_t kMaxRetries    = 3;
static constexpr uint32_t kChunkSize     = 2048U;  // bytes per chunk

struct FtCtx;
using FtSm = osp::StateMachine<FtCtx, kFtSmMaxStates>;

struct FtCtx {
  FtSm* sm;

  // Connection
  uint32_t client_id;
  osp::FixedString<63> server_host;
  uint16_t file_port;
  osp::Client<FileTransferReq, FileTransferResp>* file_cli;

  // File data
  const uint8_t* file_data;
  uint32_t file_size;
  uint32_t total_chunks;
  uint32_t current_chunk;
  uint32_t bytes_sent;

  // Retry state
  uint32_t retry_count;
  float    drop_rate;       // simulated loss probability (0.0 - 1.0)

  // Statistics
  std::atomic<uint32_t> chunks_ok;
  std::atomic<uint32_t> chunks_retried;
  std::atomic<bool>     complete;
  std::atomic<bool>     success;

  // State indices
  int32_t si_root, si_idle, si_xfer, si_send, si_wait, si_retry;
  int32_t si_done, si_fail;
};

inline void InitFtCtx(FtCtx& c) noexcept {
  c.sm = nullptr;
  c.client_id = 0;
  c.server_host.assign(osp::TruncateToCapacity, "127.0.0.1");
  c.file_port = kFilePort;
  c.file_cli = nullptr;
  c.file_data = nullptr;
  c.file_size = 0;
  c.total_chunks = 0;
  c.current_chunk = 0;
  c.bytes_sent = 0;
  c.retry_count = 0;
  c.drop_rate = 0.1f;  // 10% simulated loss
  c.chunks_ok.store(0, std::memory_order_relaxed);
  c.chunks_retried.store(0, std::memory_order_relaxed);
  c.complete.store(false, std::memory_order_relaxed);
  c.success.store(false, std::memory_order_relaxed);
  c.si_root = c.si_idle = c.si_xfer = c.si_send = -1;
  c.si_wait = c.si_retry = c.si_done = c.si_fail = -1;
}

// ============================================================================
// Simulated Packet Loss
// ============================================================================

inline bool SimulateDrop(float rate) noexcept {
  if (rate <= 0.0f) return false;
  // Thread-safe PRNG (thread_local avoids contention)
  static thread_local std::mt19937 gen(
      static_cast<uint32_t>(NowNs() & 0xFFFFFFFFULL));
  std::uniform_real_distribution<float> dis(0.0f, 1.0f);
  return dis(gen) < rate;
}

// ============================================================================
// Free-Function State Handlers
// ============================================================================

namespace ft_hsm {

using TR = osp::TransitionResult;
using Ev = osp::Event;

inline TR Root(FtCtx& /*ctx*/, const Ev& /*evt*/) {
  return TR::kHandled;
}

inline TR Idle(FtCtx& ctx, const Ev& evt) {
  if (evt.id == kFtStart) {
    ctx.current_chunk = 0;
    ctx.bytes_sent = 0;
    ctx.retry_count = 0;
    return ctx.sm->RequestTransition(ctx.si_send);
  }
  return TR::kUnhandled;
}

inline TR Transferring(FtCtx& ctx, const Ev& evt) {
  if (evt.id == kFtAbort) {
    return ctx.sm->RequestTransition(ctx.si_fail);
  }
  return TR::kUnhandled;
}

inline TR Sending(FtCtx& ctx, const Ev& evt) {
  if (evt.id != kFtChunkSent) return TR::kUnhandled;

  if (ctx.file_cli == nullptr) {
    return ctx.sm->RequestTransition(ctx.si_fail);
  }

  // Build chunk request
  FileTransferReq req{};
  req.client_id = ctx.client_id;
  req.chunk_seq = ctx.current_chunk;
  req.total_chunks = ctx.total_chunks;
  req.file_size = ctx.file_size;

  uint32_t offset = ctx.current_chunk * kChunkSize;
  uint32_t remaining = ctx.file_size - offset;
  req.chunk_len = (remaining > kChunkSize) ? kChunkSize : remaining;
  if (req.chunk_len > kMaxPayloadBytes) req.chunk_len = kMaxPayloadBytes;

  // Fill data with pattern (verifiable on server)
  FillPattern(req.data, req.chunk_len, ctx.current_chunk);

  // Simulate packet loss: pretend we didn't get the response
  if (SimulateDrop(ctx.drop_rate)) {
    OSP_LOG_WARN("FILE_TX", "[%u] Simulated drop: chunk %u/%u",
                 ctx.client_id, ctx.current_chunk, ctx.total_chunks);
    ctx.chunks_retried.fetch_add(1, std::memory_order_relaxed);
    return ctx.sm->RequestTransition(ctx.si_retry);
  }

  // Actually send
  auto resp = ctx.file_cli->Call(req, 3000);
  if (!resp.has_value() || resp.value().accepted == 0) {
    OSP_LOG_WARN("FILE_TX", "[%u] Chunk %u failed",
                 ctx.client_id, ctx.current_chunk);
    return ctx.sm->RequestTransition(ctx.si_retry);
  }

  // Success
  ctx.bytes_sent += req.chunk_len;
  ctx.chunks_ok.fetch_add(1, std::memory_order_relaxed);
  ctx.retry_count = 0;
  ctx.current_chunk++;

  if (ctx.current_chunk >= ctx.total_chunks) {
    return ctx.sm->RequestTransition(ctx.si_done);
  }

  // Stay in Sending for next chunk (self-transition via parent)
  return TR::kHandled;
}

inline TR WaitingAck(FtCtx& /*ctx*/, const Ev& /*evt*/) {
  // Reserved for future async mode
  return TR::kUnhandled;
}

inline TR Retrying(FtCtx& ctx, const Ev& evt) {
  if (evt.id == kFtRetry) {
    ctx.retry_count++;
    if (ctx.retry_count > kMaxRetries) {
      OSP_LOG_ERROR("FILE_TX", "[%u] Max retries exceeded at chunk %u",
                    ctx.client_id, ctx.current_chunk);
      return ctx.sm->RequestTransition(ctx.si_fail);
    }
    OSP_LOG_INFO("FILE_TX", "[%u] Retry %u/%u for chunk %u",
                 ctx.client_id, ctx.retry_count, kMaxRetries,
                 ctx.current_chunk);
    return ctx.sm->RequestTransition(ctx.si_send);
  }
  return TR::kUnhandled;
}

inline TR Complete(FtCtx& /*ctx*/, const Ev& /*evt*/) {
  return TR::kHandled;
}

inline TR Failed(FtCtx& /*ctx*/, const Ev& /*evt*/) {
  return TR::kHandled;
}

// Entry actions for terminal states
inline void OnEnterComplete(FtCtx& ctx) {
  ctx.complete.store(true, std::memory_order_relaxed);
  ctx.success.store(true, std::memory_order_relaxed);
}

inline void OnEnterFailed(FtCtx& ctx) {
  ctx.complete.store(true, std::memory_order_relaxed);
  ctx.success.store(false, std::memory_order_relaxed);
}

}  // namespace ft_hsm

// ============================================================================
// Build File Transfer HSM
// ============================================================================

inline void BuildFtSm(FtSm& sm, FtCtx& ctx) noexcept {
  ctx.sm = &sm;
  using Cfg = osp::StateConfig<FtCtx>;

  ctx.si_root  = sm.AddState(Cfg{"Root",         -1,           ft_hsm::Root,
                                  nullptr, nullptr});
  ctx.si_idle  = sm.AddState(Cfg{"Idle",         ctx.si_root,  ft_hsm::Idle,
                                  nullptr, nullptr});
  ctx.si_xfer  = sm.AddState(Cfg{"Transferring", ctx.si_root,
                                  ft_hsm::Transferring, nullptr, nullptr});
  ctx.si_send  = sm.AddState(Cfg{"Sending",      ctx.si_xfer,  ft_hsm::Sending,
                                  nullptr, nullptr});
  ctx.si_wait  = sm.AddState(Cfg{"WaitingAck",   ctx.si_xfer,
                                  ft_hsm::WaitingAck, nullptr, nullptr});
  ctx.si_retry = sm.AddState(Cfg{"Retrying",     ctx.si_xfer,
                                  ft_hsm::Retrying, nullptr, nullptr});
  ctx.si_done  = sm.AddState(Cfg{"Complete",     ctx.si_root,
                                  ft_hsm::Complete, ft_hsm::OnEnterComplete,
                                  nullptr});
  ctx.si_fail  = sm.AddState(Cfg{"Failed",       ctx.si_root,  ft_hsm::Failed,
                                  ft_hsm::OnEnterFailed, nullptr});

  sm.SetInitialState(ctx.si_idle);
  sm.Start();
}

// ============================================================================
// Run File Transfer (blocking, intended for dedicated thread)
// ============================================================================

inline bool RunFileTransfer(FtCtx& ctx) noexcept {
  // Connect to file service
  auto cli_r = osp::Client<FileTransferReq, FileTransferResp>::Connect(
      ctx.server_host.c_str(), ctx.file_port, 3000);
  if (!cli_r.has_value()) {
    OSP_LOG_ERROR("FILE_TX", "[%u] Connect to file service failed",
                  ctx.client_id);
    return false;
  }
  auto cli = new osp::Client<FileTransferReq, FileTransferResp>(
      std::move(cli_r.value()));
  ctx.file_cli = cli;

  // Calculate chunks
  ctx.total_chunks = (ctx.file_size + kChunkSize - 1) / kChunkSize;

  OSP_LOG_INFO("FILE_TX", "[%u] Starting: %u bytes, %u chunks, "
               "drop_rate=%.0f%%",
               ctx.client_id, ctx.file_size, ctx.total_chunks,
               static_cast<double>(ctx.drop_rate) * 100.0);

  // Start HSM
  osp::Event start_evt{kFtStart, nullptr};
  ctx.sm->Dispatch(start_evt);

  // Drive the transfer loop
  while (!ctx.complete.load(std::memory_order_relaxed)) {
    int32_t cur = ctx.sm->CurrentState();

    if (cur == ctx.si_send) {
      osp::Event evt{kFtChunkSent, nullptr};
      ctx.sm->Dispatch(evt);
    } else if (cur == ctx.si_retry) {
      // Brief delay before retry
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      osp::Event evt{kFtRetry, nullptr};
      ctx.sm->Dispatch(evt);
    } else {
      // Complete or Failed -- exit loop
      break;
    }
  }

  bool ok = ctx.success.load(std::memory_order_relaxed);
  OSP_LOG_INFO("FILE_TX", "[%u] %s: sent=%u/%u retries=%u",
               ctx.client_id, ok ? "Complete" : "Failed",
               ctx.chunks_ok.load(std::memory_order_relaxed),
               ctx.total_chunks,
               ctx.chunks_retried.load(std::memory_order_relaxed));

  delete cli;
  ctx.file_cli = nullptr;
  return ok;
}

}  // namespace net_stress

#endif  // NET_STRESS_FILE_TRANSFER_HPP_
