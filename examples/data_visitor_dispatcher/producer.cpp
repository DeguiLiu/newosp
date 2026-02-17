// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// producer -- HSM-driven LiDAR data producer over ShmSpmcByteChannel.
//
// Demonstrates: HSM, ShmSpmcByteChannel (SPMC), Timer, Shutdown, Log,
//               Watchdog, FaultCollector.
//
// HSM hierarchy:
//   Operational (root)
//   +-- Init       -- create SPMC channel
//   +-- Running    -- parent state (handles SHUTDOWN/LIMIT for children)
//   |   +-- Streaming -- normal frame production at 10 Hz
//   |   +-- Paused    -- back-pressure (ring full)
//   +-- Error      -- recoverable error, retry init
//   +-- Done       -- cleanup and exit
//
// Usage: ./osp_dvd_producer [channel_name] [num_frames]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/fault_collector.hpp"
#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"
#include "osp/watchdog.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
enum ProdEvt : uint32_t {
  kEvtInitDone = 1,
  kEvtInitFail,
  kEvtRingFull,
  kEvtRingAvail,
  kEvtLimitReached,
  kEvtRetry,
  kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
struct ProdCtx {
  osp::ShmSpmcByteChannel channel;
  const char* channel_name = kDefaultChannelName;
  uint32_t max_frames = 500;

  uint32_t seq = 0;
  uint32_t frames_sent = 0;
  uint32_t frames_dropped = 0;
  uint32_t pause_count = 0;
  uint32_t error_count = 0;
  uint64_t t0_us = 0;
  float last_fps = 0.0f;

  osp::TimerScheduler<4> timer;
  osp::ShutdownManager shutdown;
  osp::ThreadWatchdog<4> watchdog;
  osp::WatchdogSlotId wd_main_id{0};
  osp::ThreadHeartbeat* wd_main_hb = nullptr;
  osp::FaultCollector<4, 16> fault_collector;

  alignas(16) uint8_t frame_buf[kFrameDataSize] = {};

  osp::StateMachine<ProdCtx, 8>* sm = nullptr;
  int32_t s_operational = -1, s_init = -1, s_running = -1;
  int32_t s_streaming = -1, s_paused = -1, s_error = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------
static void OnEnterInit(ProdCtx& ctx) {
  OSP_LOG_INFO("Producer", "creating SPMC channel '%s'", ctx.channel_name);
  auto result = osp::ShmSpmcByteChannel::CreateOrReplaceWriter(
      ctx.channel_name, kSpmcCapacity, kSpmcMaxConsumers);
  if (!result.has_value()) {
    OSP_LOG_ERROR("Producer", "failed to create writer, err=%u",
                  static_cast<unsigned>(result.get_error()));
    return;  // handler will dispatch kEvtInitFail
  }
  ctx.channel = static_cast<osp::ShmSpmcByteChannel&&>(result.value());
  ctx.t0_us = osp::SteadyNowUs();
  OSP_LOG_INFO("Producer", "channel created, capacity=%u", ctx.channel.Capacity());
}

static osp::TransitionResult OnInit(ProdCtx& ctx, const osp::Event& event) {
  if (event.id == kEvtInitDone)
    return ctx.sm->RequestTransition(ctx.s_streaming);
  if (event.id == kEvtInitFail)
    return ctx.sm->RequestTransition(ctx.s_error);
  return osp::TransitionResult::kUnhandled;
}

static osp::TransitionResult OnOperational(ProdCtx& ctx,
                                            const osp::Event& event) {
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterRunning(ProdCtx& /*ctx*/) {}

static osp::TransitionResult OnRunning(ProdCtx& ctx,
                                        const osp::Event& event) {
  if (event.id == kEvtLimitReached) {
    OSP_LOG_INFO("Producer", "frame limit reached (%u)", ctx.max_frames);
    return ctx.sm->RequestTransition(ctx.s_done);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterStreaming(ProdCtx& /*ctx*/) {
  OSP_LOG_INFO("Producer", "streaming at %u Hz", kProduceFps);
}

static osp::TransitionResult OnStreaming(ProdCtx& ctx,
                                          const osp::Event& event) {
  if (event.id == kEvtRingFull)
    return ctx.sm->RequestTransition(ctx.s_paused);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterPaused(ProdCtx& ctx) {
  ++ctx.pause_count;
  OSP_LOG_WARN("Producer", "ring full, paused (count=%u)", ctx.pause_count);
}

static osp::TransitionResult OnPaused(ProdCtx& ctx,
                                       const osp::Event& event) {
  if (event.id == kEvtRingAvail)
    return ctx.sm->RequestTransition(ctx.s_streaming);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterError(ProdCtx& ctx) {
  ++ctx.error_count;
  OSP_LOG_ERROR("Producer", "error, retry in 1s (count=%u)", ctx.error_count);
}

static osp::TransitionResult OnError(ProdCtx& ctx,
                                      const osp::Event& event) {
  if (event.id == kEvtRetry)
    return ctx.sm->RequestTransition(ctx.s_init);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(ProdCtx& ctx) {
  uint64_t elapsed_us = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (elapsed_us > 0)
      ? static_cast<float>(ctx.frames_sent) * 1e6f
        / static_cast<float>(elapsed_us) : 0.0f;
  OSP_LOG_INFO("Producer", "done: sent=%u dropped=%u paused=%u fps=%.1f",
               ctx.frames_sent, ctx.frames_dropped, ctx.pause_count,
               static_cast<double>(fps));
  ctx.channel.Unlink();
  ctx.finished = true;
}

static osp::TransitionResult OnDone(ProdCtx& /*ctx*/,
                                     const osp::Event& /*event*/) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  ProdCtx ctx;
  if (argc > 1) ctx.channel_name = argv[1];
  if (argc > 2) ctx.max_frames = static_cast<uint32_t>(std::atoi(argv[2]));

  OSP_LOG_INFO("Producer", "starting: channel='%s' frames=%u",
               ctx.channel_name, ctx.max_frames);

  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});

  // Watchdog
  auto wd_reg = ctx.watchdog.Register("main", 5000);
  if (wd_reg.has_value()) {
    ctx.wd_main_id = wd_reg.value().id;
    ctx.wd_main_hb = wd_reg.value().heartbeat;
  }
  ctx.watchdog.StartAutoCheck(1000);

  // FaultCollector
  ctx.fault_collector.RegisterFault(0, 0x00010001U);  // ring_full
  ctx.fault_collector.Start();

  // Build HSM
  osp::StateMachine<ProdCtx, 8> sm(ctx);
  ctx.sm = &sm;

  ctx.s_operational = sm.AddState({
      "Operational", -1, OnOperational, nullptr, nullptr, nullptr});
  ctx.s_init = sm.AddState({
      "Init", ctx.s_operational, OnInit, OnEnterInit, nullptr, nullptr});
  ctx.s_running = sm.AddState({
      "Running", ctx.s_operational, OnRunning, OnEnterRunning, nullptr, nullptr});
  ctx.s_streaming = sm.AddState({
      "Streaming", ctx.s_running, OnStreaming, OnEnterStreaming, nullptr, nullptr});
  ctx.s_paused = sm.AddState({
      "Paused", ctx.s_running, OnPaused, OnEnterPaused, nullptr, nullptr});
  ctx.s_error = sm.AddState({
      "Error", ctx.s_operational, OnError, OnEnterError, nullptr, nullptr});
  ctx.s_done = sm.AddState({
      "Done", ctx.s_operational, OnDone, OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_init);

  // Stats timer (every 2s)
  ctx.timer.Start();
  ctx.timer.Add(2000, [](void* arg) {
    auto* c = static_cast<ProdCtx*>(arg);
    uint64_t elapsed_us = osp::SteadyNowUs() - c->t0_us;
    if (elapsed_us > 0) {
      c->last_fps = static_cast<float>(c->frames_sent) * 1e6f
                    / static_cast<float>(elapsed_us);
    }
    OSP_LOG_INFO("Producer", "stats: sent=%u dropped=%u fps=%.1f consumers=%u",
                 c->frames_sent, c->frames_dropped,
                 static_cast<double>(c->last_fps), c->channel.ConsumerCount());
  }, &ctx);

  sm.Start();

  // Init: dispatch result
  if (ctx.channel.Capacity() > 0) {
    sm.Dispatch({kEvtInitDone});
  } else {
    sm.Dispatch({kEvtInitFail});
  }

  // Main loop
  while (!ctx.finished) {
    if (ctx.shutdown.IsShutdownRequested()) {
      sm.Dispatch({kEvtShutdown});
      break;
    }
    if (ctx.wd_main_hb) ctx.wd_main_hb->Beat();

    int32_t current = sm.CurrentState();

    if (current == ctx.s_streaming) {
      uint32_t ts_ms = static_cast<uint32_t>(
          (osp::SteadyNowUs() - ctx.t0_us) / 1000);
      FillLidarFrame(ctx.frame_buf, ctx.seq, ts_ms);

      auto wr = ctx.channel.Write(ctx.frame_buf, kFrameDataSize);
      if (wr.has_value()) {
        ++ctx.seq;
        ++ctx.frames_sent;
        if (ctx.frames_sent >= ctx.max_frames) {
          sm.Dispatch({kEvtLimitReached});
          continue;
        }
      } else {
        ++ctx.frames_dropped;
        ctx.fault_collector.ReportFault(0, ctx.seq, osp::FaultPriority::kLow);
        sm.Dispatch({kEvtRingFull});
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kProduceIntervalMs));

    } else if (current == ctx.s_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (ctx.channel.WriteableBytes() >= kFrameDataSize + 4) {
        sm.Dispatch({kEvtRingAvail});
      }

    } else if (current == ctx.s_error) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (ctx.error_count < 3) {
        sm.Dispatch({kEvtRetry});
      } else {
        sm.Dispatch({kEvtShutdown});
      }

    } else if (current == ctx.s_done) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ctx.timer.Stop();
  ctx.fault_collector.Stop();
  ctx.watchdog.StopAutoCheck();
  OSP_LOG_INFO("Producer", "exiting: sent=%u dropped=%u",
               ctx.frames_sent, ctx.frames_dropped);
  return 0;
}
