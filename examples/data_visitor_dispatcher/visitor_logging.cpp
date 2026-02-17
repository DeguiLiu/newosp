// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// visitor_logging -- Logging subscriber for SPMC LiDAR data.
//
// Demonstrates: ShmSpmcByteChannel (reader), HSM, Shutdown, Timer.
// Subscribes to the SPMC channel and logs frame statistics.
//
// HSM states:
//   Connecting -- retry OpenReader every 200ms
//   Receiving  -- read frames, log every 10th, detect seq gaps
//   Stalled    -- no data > 3s
//   Done       -- print final stats
//
// Usage: ./osp_dvd_visitor_logging [channel_name]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
enum LogEvt : uint32_t {
  kEvtConnected = 1,
  kEvtConnectFail,
  kEvtStall,
  kEvtDataResumed,
  kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
struct LogCtx {
  osp::ShmSpmcByteChannel channel;
  const char* channel_name = kDefaultChannelName;

  uint32_t frames_received = 0;
  uint32_t frames_invalid = 0;
  uint32_t seq_gaps = 0;
  uint32_t last_seq = 0;
  bool first_frame = true;

  uint64_t intensity_sum = 0;
  uint32_t intensity_count = 0;
  uint8_t intensity_min = 255;
  uint8_t intensity_max = 0;

  uint64_t t0_us = 0;
  uint64_t last_frame_us = 0;
  uint32_t connect_retries = 0;

  osp::TimerScheduler<4> timer;
  osp::ShutdownManager shutdown;

  alignas(16) uint8_t recv_buf[kFrameDataSize + 256] = {};

  osp::StateMachine<LogCtx, 6>* sm = nullptr;
  int32_t s_connecting = -1, s_receiving = -1, s_stalled = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------
static void OnEnterConnecting(LogCtx& ctx) {
  OSP_LOG_INFO("Logging", "connecting to '%s'...", ctx.channel_name);
  ctx.connect_retries = 0;
}

static osp::TransitionResult OnConnecting(LogCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtConnected)
    return ctx.sm->RequestTransition(ctx.s_receiving);
  if (event.id == kEvtConnectFail)
    return ctx.sm->RequestTransition(ctx.s_done);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterReceiving(LogCtx& /*ctx*/) {
  OSP_LOG_INFO("Logging", "receiving frames");
}

static osp::TransitionResult OnReceiving(LogCtx& ctx,
                                          const osp::Event& event) {
  if (event.id == kEvtStall)
    return ctx.sm->RequestTransition(ctx.s_stalled);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterStalled(LogCtx& /*ctx*/) {
  OSP_LOG_WARN("Logging", "stalled: no data for >3s");
}

static osp::TransitionResult OnStalled(LogCtx& ctx,
                                        const osp::Event& event) {
  if (event.id == kEvtDataResumed)
    return ctx.sm->RequestTransition(ctx.s_receiving);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(LogCtx& ctx) {
  uint64_t elapsed_us = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (elapsed_us > 0)
      ? static_cast<float>(ctx.frames_received) * 1e6f
        / static_cast<float>(elapsed_us) : 0.0f;
  uint32_t avg_int = (ctx.intensity_count > 0)
      ? static_cast<uint32_t>(ctx.intensity_sum / ctx.intensity_count) : 0;
  OSP_LOG_INFO("Logging",
               "done: frames=%u invalid=%u gaps=%u fps=%.1f "
               "intensity(min=%u max=%u avg=%u)",
               ctx.frames_received, ctx.frames_invalid, ctx.seq_gaps,
               static_cast<double>(fps),
               ctx.intensity_min, ctx.intensity_max, avg_int);
  ctx.finished = true;
}

static osp::TransitionResult OnDone(LogCtx& /*ctx*/,
                                     const osp::Event& /*event*/) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// Process a received frame
// ---------------------------------------------------------------------------
static void ProcessFrame(LogCtx& ctx, uint32_t len) {
  if (!ValidateLidarFrame(ctx.recv_buf, len)) {
    ++ctx.frames_invalid;
    return;
  }
  ++ctx.frames_received;
  ctx.last_frame_us = osp::SteadyNowUs();

  auto* hdr = reinterpret_cast<const LidarFrame*>(ctx.recv_buf);

  // Sequence gap detection
  if (!ctx.first_frame && hdr->seq_num != ctx.last_seq + 1) {
    uint32_t gap = hdr->seq_num - ctx.last_seq - 1;
    ctx.seq_gaps += gap;
    OSP_LOG_WARN("Logging", "seq gap: expected=%u got=%u (gap=%u)",
                 ctx.last_seq + 1, hdr->seq_num, gap);
  }
  ctx.first_frame = false;
  ctx.last_seq = hdr->seq_num;

  // Intensity stats
  auto* pts = reinterpret_cast<const LidarPoint*>(
      ctx.recv_buf + sizeof(LidarFrame));
  for (uint32_t i = 0; i < hdr->point_count; ++i) {
    uint8_t v = pts[i].intensity;
    ctx.intensity_sum += v;
    ++ctx.intensity_count;
    if (v < ctx.intensity_min) ctx.intensity_min = v;
    if (v > ctx.intensity_max) ctx.intensity_max = v;
  }

  if (ctx.frames_received % 10 == 0) {
    uint32_t avg = (ctx.intensity_count > 0)
        ? static_cast<uint32_t>(ctx.intensity_sum / ctx.intensity_count) : 0;
    OSP_LOG_INFO("Logging",
                 "frame #%u: seq=%u pts=%u intensity(min=%u max=%u avg=%u)",
                 ctx.frames_received, hdr->seq_num, hdr->point_count,
                 ctx.intensity_min, ctx.intensity_max, avg);
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  LogCtx ctx;
  if (argc > 1) ctx.channel_name = argv[1];

  OSP_LOG_INFO("Logging", "starting: channel='%s'", ctx.channel_name);
  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});
  ctx.t0_us = osp::SteadyNowUs();

  // Stats timer
  ctx.timer.Start();
  ctx.timer.Add(3000, [](void* arg) {
    auto* c = static_cast<LogCtx*>(arg);
    uint64_t elapsed_us = osp::SteadyNowUs() - c->t0_us;
    float fps = (elapsed_us > 0)
        ? static_cast<float>(c->frames_received) * 1e6f
          / static_cast<float>(elapsed_us) : 0.0f;
    OSP_LOG_INFO("Logging", "stats: frames=%u invalid=%u gaps=%u fps=%.1f",
                 c->frames_received, c->frames_invalid, c->seq_gaps,
                 static_cast<double>(fps));
  }, &ctx);

  // Build HSM
  osp::StateMachine<LogCtx, 6> sm(ctx);
  ctx.sm = &sm;

  ctx.s_connecting = sm.AddState({
      "Connecting", -1, OnConnecting, OnEnterConnecting, nullptr, nullptr});
  ctx.s_receiving = sm.AddState({
      "Receiving", -1, OnReceiving, OnEnterReceiving, nullptr, nullptr});
  ctx.s_stalled = sm.AddState({
      "Stalled", -1, OnStalled, OnEnterStalled, nullptr, nullptr});
  ctx.s_done = sm.AddState({
      "Done", -1, OnDone, OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_connecting);
  sm.Start();

  while (!ctx.finished) {
    if (ctx.shutdown.IsShutdownRequested()) {
      sm.Dispatch({kEvtShutdown});
      break;
    }

    int32_t current = sm.CurrentState();

    if (current == ctx.s_connecting) {
      auto result = osp::ShmSpmcByteChannel::OpenReader(ctx.channel_name);
      if (result.has_value()) {
        ctx.channel = static_cast<osp::ShmSpmcByteChannel&&>(result.value());
        ctx.last_frame_us = osp::SteadyNowUs();
        sm.Dispatch({kEvtConnected});
      } else {
        ++ctx.connect_retries;
        if (ctx.connect_retries >= 50) {
          OSP_LOG_ERROR("Logging", "connect failed after %u retries",
                        ctx.connect_retries);
          sm.Dispatch({kEvtConnectFail});
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      }

    } else if (current == ctx.s_receiving) {
      auto wait = ctx.channel.WaitReadable(500);
      if (wait.has_value()) {
        uint32_t len = ctx.channel.Read(ctx.recv_buf, sizeof(ctx.recv_buf));
        if (len > 0) ProcessFrame(ctx, len);
      }
      if ((osp::SteadyNowUs() - ctx.last_frame_us) > 3000000) {
        sm.Dispatch({kEvtStall});
      }

    } else if (current == ctx.s_stalled) {
      auto wait = ctx.channel.WaitReadable(1000);
      if (wait.has_value()) {
        uint32_t len = ctx.channel.Read(ctx.recv_buf, sizeof(ctx.recv_buf));
        if (len > 0) {
          ProcessFrame(ctx, len);
          sm.Dispatch({kEvtDataResumed});
        }
      }

    } else if (current == ctx.s_done) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ctx.timer.Stop();
  OSP_LOG_INFO("Logging", "exiting: frames=%u gaps=%u",
               ctx.frames_received, ctx.seq_gaps);
  return 0;
}
