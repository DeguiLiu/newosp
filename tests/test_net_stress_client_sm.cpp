/**
 * @file test_net_stress_client_sm.cpp
 * @brief Tests for net_stress ClientSm static-table migration.
 *
 * Verifies the transition table against the original handler semantics:
 * Disconnected -> Connecting -> Idle/Running lifecycle, Connected-level
 * disconnect cleanup, Error retry path, and Running tick echo accounting.
 * The RPC echo path needs a live server, so ActEchoTick's network branch is
 * exercised indirectly via the disconnected-client error counter.
 */

#include "net_stress/client_sm.hpp"
#include "net_stress/protocol.hpp"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace {

net_stress::ClientCtx& MakeCtx(net_stress::ClientCtx& c) {
  net_stress::InitClientCtx(c);
  return c;
}

}  // namespace

TEST_CASE("client_sm - lifecycle Disconnected->Connecting->Idle->Running->Idle", "[client_sm]") {
  net_stress::ClientCtx ctx;
  MakeCtx(ctx);
  net_stress::ClientSm sm(ctx, net_stress::client_sm_detail::kClientStates, net_stress::kCsCount,
                          net_stress::client_sm_detail::kClientTrans, net_stress::client_sm_detail::kClientTransCount);
  net_stress::BuildClientSm(sm, ctx);

  REQUIRE(sm.CurrentState() == net_stress::kCsDisconnected);
  REQUIRE(std::strcmp(sm.CurrentStateName(), "Disconnected") == 0);

  // connect -> Connecting
  net_stress::Dispatch(ctx, net_stress::kEvtConnect);
  REQUIRE(sm.CurrentState() == net_stress::kCsConnecting);

  // handshake ok -> Idle (child of Connected)
  net_stress::Dispatch(ctx, net_stress::kEvtHandshakeOk);
  REQUIRE(sm.CurrentState() == net_stress::kCsIdle);
  REQUIRE(sm.IsInState(net_stress::kCsConnected));

  // start test -> Running
  net_stress::Dispatch(ctx, net_stress::kEvtStartTest);
  REQUIRE(sm.CurrentState() == net_stress::kCsRunning);

  // stop test -> Idle
  net_stress::Dispatch(ctx, net_stress::kEvtStopTest);
  REQUIRE(sm.CurrentState() == net_stress::kCsIdle);
}

TEST_CASE("client_sm - Connecting error path", "[client_sm]") {
  net_stress::ClientCtx ctx;
  MakeCtx(ctx);
  net_stress::ClientSm sm(ctx, net_stress::client_sm_detail::kClientStates, net_stress::kCsCount,
                          net_stress::client_sm_detail::kClientTrans, net_stress::client_sm_detail::kClientTransCount);
  net_stress::BuildClientSm(sm, ctx);

  net_stress::Dispatch(ctx, net_stress::kEvtConnect);
  net_stress::Dispatch(ctx, net_stress::kEvtError);
  REQUIRE(sm.CurrentState() == net_stress::kCsError);
  REQUIRE(std::strcmp(sm.CurrentStateName(), "Error") == 0);

  // retry from Error: cleanup runs (connected flag cleared) and returns to Disconnected.
  ctx.connected = true;
  net_stress::Dispatch(ctx, net_stress::kEvtRetry);
  REQUIRE(sm.CurrentState() == net_stress::kCsDisconnected);
  REQUIRE_FALSE(ctx.connected);
}

TEST_CASE("client_sm - disconnect from Idle cleans up RPC handles", "[client_sm]") {
  net_stress::ClientCtx ctx;
  MakeCtx(ctx);
  net_stress::ClientSm sm(ctx, net_stress::client_sm_detail::kClientStates, net_stress::kCsCount,
                          net_stress::client_sm_detail::kClientTrans, net_stress::client_sm_detail::kClientTransCount);
  net_stress::BuildClientSm(sm, ctx);

  net_stress::Dispatch(ctx, net_stress::kEvtConnect);
  net_stress::Dispatch(ctx, net_stress::kEvtHandshakeOk);
  ctx.connected = true;

  // Disconnect bubbles from Idle up to Connected where the row lives.
  net_stress::Dispatch(ctx, net_stress::kEvtDisconnect);
  REQUIRE(sm.CurrentState() == net_stress::kCsDisconnected);
  REQUIRE_FALSE(ctx.connected);
}

TEST_CASE("client_sm - Running tick with disconnected RPC counts error, stays Running", "[client_sm]") {
  net_stress::ClientCtx ctx;
  MakeCtx(ctx);
  net_stress::ClientSm sm(ctx, net_stress::client_sm_detail::kClientStates, net_stress::kCsCount,
                          net_stress::client_sm_detail::kClientTrans, net_stress::client_sm_detail::kClientTransCount);
  net_stress::BuildClientSm(sm, ctx);

  net_stress::Dispatch(ctx, net_stress::kEvtConnect);
  net_stress::Dispatch(ctx, net_stress::kEvtHandshakeOk);
  net_stress::Dispatch(ctx, net_stress::kEvtStartTest);
  REQUIRE(sm.CurrentState() == net_stress::kCsRunning);

  // No live echo RPC: the tick action records an error and stays in Running.
  net_stress::Dispatch(ctx, net_stress::kEvtTick);
  REQUIRE(sm.CurrentState() == net_stress::kCsRunning);
  REQUIRE(ctx.n_err.load() == 1U);
  REQUIRE(ctx.n_sent.load() == 0U);
}

TEST_CASE("client_sm - unmatched events are rejected (stay in state)", "[client_sm]") {
  net_stress::ClientCtx ctx;
  MakeCtx(ctx);
  net_stress::ClientSm sm(ctx, net_stress::client_sm_detail::kClientStates, net_stress::kCsCount,
                          net_stress::client_sm_detail::kClientTrans, net_stress::client_sm_detail::kClientTransCount);
  net_stress::BuildClientSm(sm, ctx);

  // Disconnected has no row for start-test: reject arc keeps the state.
  net_stress::Dispatch(ctx, net_stress::kEvtStartTest);
  REQUIRE(sm.CurrentState() == net_stress::kCsDisconnected);

  // Tick is not handled by Disconnected/Root: also rejected.
  net_stress::Dispatch(ctx, net_stress::kEvtTick);
  REQUIRE(sm.CurrentState() == net_stress::kCsDisconnected);
  REQUIRE(ctx.n_err.load() == 0U);  // reject arc runs no action
}