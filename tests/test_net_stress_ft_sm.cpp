/**
 * @file test_net_stress_ft_sm.cpp
 * @brief Tests for net_stress FtSm static-table migration.
 *
 * The network RPC path needs a live server; these tests exercise the table
 * semantics: start reset, retry guard budget, terminal entries, and the
 * driver-loop result mapping (send_result -> next event).
 */

#include "net_stress/file_transfer.hpp"
#include "net_stress/protocol.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace {

struct Fixture {
  net_stress::FtCtx ctx;
  net_stress::FtSm sm;

  Fixture()
      : ctx(),
        sm(ctx, net_stress::ft_sm_detail::kFtStates, net_stress::kFsCount, net_stress::ft_sm_detail::kFtTrans,
           net_stress::ft_sm_detail::kFtTransCount) {
    net_stress::InitFtCtx(ctx);
    net_stress::BuildFtSm(sm, ctx);
    ctx.client_id = 1;
    ctx.file_size = 4096;
    ctx.total_chunks = 2;
    ctx.drop_rate = 0.0f;  // deterministic: no simulated loss
  }
};

void Dispatch(net_stress::FtCtx& ctx, uint32_t evt) {
  ctx.sm->Dispatch(osp::Event{evt, nullptr});
}

}  // namespace

TEST_CASE("ft_sm - start from Idle resets counters and enters Sending", "[ft_sm]") {
  Fixture f;

  f.ctx.current_chunk = 7;
  f.ctx.retry_count = 2;
  Dispatch(f.ctx, net_stress::kFtStart);

  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);
  REQUIRE(f.sm.IsInState(net_stress::kFsTransferring));
  REQUIRE(f.ctx.current_chunk == 0);
  REQUIRE(f.ctx.retry_count == 0);
}

TEST_CASE("ft_sm - send with no connection fails over Retrying into Failed", "[ft_sm]") {
  Fixture f;

  Dispatch(f.ctx, net_stress::kFtStart);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);

  // ActSend records kNoConn; the driver translates it to kFtAckFail.
  Dispatch(f.ctx, net_stress::kFtChunkSent);
  REQUIRE(f.ctx.send_result == net_stress::ft_sm_detail::SendResult::kNoConn);

  Dispatch(f.ctx, net_stress::kFtAckFail);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsRetrying);

  // Retry guard: budget 3 (kMaxRetries), each kFtRetry consumes one.
  Dispatch(f.ctx, net_stress::kFtRetry);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);  // retry_count 0 -> 1
  Dispatch(f.ctx, net_stress::kFtChunkSent);
  Dispatch(f.ctx, net_stress::kFtAckFail);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsRetrying);

  Dispatch(f.ctx, net_stress::kFtRetry);
  Dispatch(f.ctx, net_stress::kFtChunkSent);
  Dispatch(f.ctx, net_stress::kFtAckFail);
  Dispatch(f.ctx, net_stress::kFtRetry);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);  // retry_count 2 -> 3? guard is < kMaxRetries

  // Fourth failure: retry_count == kMaxRetries exhausts the guard -> Failed.
  Dispatch(f.ctx, net_stress::kFtChunkSent);
  Dispatch(f.ctx, net_stress::kFtAckFail);
  Dispatch(f.ctx, net_stress::kFtRetry);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsFailed);
  REQUIRE(f.ctx.complete.load());
  REQUIRE_FALSE(f.ctx.success.load());
}

TEST_CASE("ft_sm - Complete entry flags success", "[ft_sm]") {
  Fixture f;

  Dispatch(f.ctx, net_stress::kFtStart);
  // Simulate a finished transfer: jump straight to the Done event.
  Dispatch(f.ctx, net_stress::kFtDone);

  REQUIRE(f.sm.CurrentState() == net_stress::kFsComplete);
  REQUIRE(f.ctx.complete.load());
  REQUIRE(f.ctx.success.load());
}

TEST_CASE("ft_sm - abort from Sending bubbles to Transferring row", "[ft_sm]") {
  Fixture f;

  Dispatch(f.ctx, net_stress::kFtStart);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);

  Dispatch(f.ctx, net_stress::kFtAbort);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsFailed);
  REQUIRE(f.ctx.complete.load());
  REQUIRE_FALSE(f.ctx.success.load());
}

TEST_CASE("ft_sm - reused terminal HSM needs reset before re-start", "[ft_sm]") {
  Fixture f;

  Dispatch(f.ctx, net_stress::kFtStart);
  Dispatch(f.ctx, net_stress::kFtDone);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsComplete);

  // Without reset, kFtStart hits the reject arc and stays in the terminal state.
  Dispatch(f.ctx, net_stress::kFtStart);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsComplete);

  // ForceTransition(kFsIdle) recovers re-entry (RunFileTransfer does this).
  REQUIRE(f.sm.ForceTransition(net_stress::kFsIdle));
  Dispatch(f.ctx, net_stress::kFtStart);
  REQUIRE(f.sm.CurrentState() == net_stress::kFsSending);
}