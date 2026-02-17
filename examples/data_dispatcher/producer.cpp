// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// producer -- DataDispatcher<ShmStore, ShmNotify> LiDAR frame producer.
//
// Demonstrates: DataDispatcher unified API (InterProc mode), HSM, ShmNotify,
//               ShmSpmcByteChannel (for notification), Timer, Shutdown.
//
// Data path: Alloc -> GetWritable -> FillLidarFrame -> Submit
//   Submit sets refcount = num_consumers, then ShmNotify pushes
//   {block_id, payload_size} to the notification SPMC channel.
//   Consumers receive NotifyMsg, access data via GetReadable (zero-copy),
//   and call Release to decrement refcount. Block is recycled when
//   refcount reaches 0.
//
// HSM:
//   Operational -> Init -> Running(Streaming/Paused) -> Done
//              -> Error (retry init)
//
// Usage: ./osp_dd_producer [--channel name] [--frames N]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/data_dispatcher.hpp"
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
// Type aliases
// ---------------------------------------------------------------------------

using Store = osp::ShmStore<kFrameDataSize, kPoolMaxBlocks>;
using Disp = osp::DataDispatcher<Store, osp::ShmNotify>;

// ---------------------------------------------------------------------------
// ShmNotify callback: push NotifyMsg to SPMC channel
// ---------------------------------------------------------------------------

static void NotifyCallback(uint32_t block_id, uint32_t payload_size,
                           void* ctx) {
  auto* ch = static_cast<osp::ShmSpmcByteChannel*>(ctx);
  NotifyMsg msg{block_id, payload_size};
  ch->Write(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum ProdEvt : uint32_t {
  kEvtInitDone = 1, kEvtInitFail, kEvtRingFull, kEvtRingAvail,
  kEvtLimitReached, kEvtRetry, kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct ProdCtx {
  Disp disp;
  osp::ShmSpmcByteChannel notify_channel;
  void* pool_shm = nullptr;
  uint32_t pool_size = 0;

  const char* pool_name = kPoolShmName;
  const char* notify_name = kNotifyChannelName;
  uint32_t max_frames = 500;

  uint32_t seq = 0;
  uint32_t frames_sent = 0;
  uint32_t frames_dropped = 0;
  uint32_t pause_count = 0;
  uint32_t error_count = 0;
  uint64_t t0_us = 0;

  osp::TimerScheduler<4> timer;
  osp::ShutdownManager shutdown;
  osp::ThreadWatchdog<4> watchdog;
  osp::ThreadHeartbeat* wd_hb = nullptr;
  osp::FaultCollector<4, 16> fault_collector;

  osp::StateMachine<ProdCtx, 8>* sm = nullptr;
  int32_t s_op = -1, s_init = -1, s_running = -1;
  int32_t s_streaming = -1, s_paused = -1, s_error = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------

static void OnEnterInit(ProdCtx& ctx) {
  OSP_LOG_INFO("Producer", "creating pool shm '%s' + notify '%s'",
               ctx.pool_name, ctx.notify_name);

  // Create block pool shared memory
  ctx.pool_size = Store::RequiredShmSize();
  ctx.pool_shm = CreatePoolShm(ctx.pool_name, ctx.pool_size);
  if (ctx.pool_shm == nullptr) {
    OSP_LOG_ERROR("Producer", "failed to create pool shm");
    return;
  }

  // Create notification SPMC channel
  auto ch = osp::ShmSpmcByteChannel::CreateOrReplaceWriter(
      ctx.notify_name, kNotifyCapacity, kNotifyMaxConsumers);
  if (!ch.has_value()) {
    OSP_LOG_ERROR("Producer", "failed to create notify channel");
    ClosePoolShm(ctx.pool_shm, ctx.pool_size);
    ctx.pool_shm = nullptr;
    return;
  }
  ctx.notify_channel = static_cast<osp::ShmSpmcByteChannel&&>(ch.value());

  // Init DataDispatcher with ShmStore
  Disp::Config cfg;
  cfg.name = "lidar_pool";
  cfg.default_timeout_ms = 500;
  cfg.backpressure_threshold = 4;
  ctx.disp.Init(cfg, ctx.pool_shm, ctx.pool_size);

  // Wire ShmNotify callback
  auto& notify = ctx.disp.GetNotify();
  notify.fn = NotifyCallback;
  notify.ctx = &ctx.notify_channel;

  ctx.t0_us = osp::SteadyNowUs();
  OSP_LOG_INFO("Producer", "pool created: %u blocks x %u bytes = %u bytes shm",
               kPoolMaxBlocks, kFrameDataSize, ctx.pool_size);
}

static osp::TransitionResult OnInit(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtInitDone) return ctx.sm->RequestTransition(ctx.s_streaming);
  if (ev.id == kEvtInitFail) return ctx.sm->RequestTransition(ctx.s_error);
  return osp::TransitionResult::kUnhandled;
}

static osp::TransitionResult OnOp(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static osp::TransitionResult OnRunning(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtLimitReached) {
    OSP_LOG_INFO("Producer", "frame limit reached (%u)", ctx.max_frames);
    return ctx.sm->RequestTransition(ctx.s_done);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterStreaming(ProdCtx& /*ctx*/) {
  OSP_LOG_INFO("Producer", "streaming at %u Hz", kProduceFps);
}

static osp::TransitionResult OnStreaming(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtRingFull) return ctx.sm->RequestTransition(ctx.s_paused);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterPaused(ProdCtx& ctx) {
  ++ctx.pause_count;
  OSP_LOG_WARN("Producer", "pool exhausted, paused (count=%u)",
               ctx.pause_count);
}

static osp::TransitionResult OnPaused(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtRingAvail) return ctx.sm->RequestTransition(ctx.s_streaming);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterError(ProdCtx& ctx) {
  ++ctx.error_count;
  OSP_LOG_ERROR("Producer", "error, retry in 1s (count=%u)", ctx.error_count);
}

static osp::TransitionResult OnError(ProdCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtRetry) return ctx.sm->RequestTransition(ctx.s_init);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(ProdCtx& ctx) {
  uint64_t dt = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (dt > 0) ? static_cast<float>(ctx.frames_sent) * 1e6f
                         / static_cast<float>(dt) : 0.0f;
  OSP_LOG_INFO("Producer", "done: sent=%u dropped=%u paused=%u fps=%.1f "
               "pool(free=%u alloc=%u)",
               ctx.frames_sent, ctx.frames_dropped, ctx.pause_count,
               static_cast<double>(fps),
               ctx.disp.FreeBlocks(), ctx.disp.AllocBlocks());

  // Cleanup: force-release any leaked blocks
  uint32_t cleaned = ctx.disp.ForceCleanup(
      [](uint32_t, void*) -> bool { return true; }, nullptr);
  if (cleaned > 0)
    OSP_LOG_INFO("Producer", "ForceCleanup reclaimed %u blocks", cleaned);

  // Unlink shared memory
  ctx.notify_channel.Unlink();
  if (ctx.pool_shm != nullptr) {
    ClosePoolShm(ctx.pool_shm, ctx.pool_size);
    UnlinkPoolShm(ctx.pool_name);
    ctx.pool_shm = nullptr;
  }
  ctx.finished = true;
}

static osp::TransitionResult OnDone(ProdCtx&, const osp::Event&) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  ProdCtx ctx;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      ctx.pool_name = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      ctx.max_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
  }

  OSP_LOG_INFO("Producer", "starting: pool='%s' frames=%u",
               ctx.pool_name, ctx.max_frames);

  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});

  auto wd_reg = ctx.watchdog.Register("main", 5000);
  if (wd_reg.has_value()) ctx.wd_hb = wd_reg.value().heartbeat;
  ctx.watchdog.StartAutoCheck(1000);

  ctx.fault_collector.RegisterFault(0, 0x00010001U);
  ctx.fault_collector.Start();

  // Build HSM
  osp::StateMachine<ProdCtx, 8> sm(ctx);
  ctx.sm = &sm;

  ctx.s_op = sm.AddState({"Operational", -1, OnOp, nullptr, nullptr, nullptr});
  ctx.s_init = sm.AddState({"Init", ctx.s_op, OnInit, OnEnterInit, nullptr, nullptr});
  ctx.s_running = sm.AddState({"Running", ctx.s_op, OnRunning, nullptr, nullptr, nullptr});
  ctx.s_streaming = sm.AddState({"Streaming", ctx.s_running, OnStreaming, OnEnterStreaming, nullptr, nullptr});
  ctx.s_paused = sm.AddState({"Paused", ctx.s_running, OnPaused, OnEnterPaused, nullptr, nullptr});
  ctx.s_error = sm.AddState({"Error", ctx.s_op, OnError, OnEnterError, nullptr, nullptr});
  ctx.s_done = sm.AddState({"Done", ctx.s_op, OnDone, OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_init);

  // Stats timer
  ctx.timer.Start();
  ctx.timer.Add(2000, [](void* arg) {
    auto* c = static_cast<ProdCtx*>(arg);
    uint64_t dt = osp::SteadyNowUs() - c->t0_us;
    float fps = (dt > 0) ? static_cast<float>(c->frames_sent) * 1e6f
                           / static_cast<float>(dt) : 0.0f;
    OSP_LOG_INFO("Producer", "stats: sent=%u dropped=%u fps=%.1f "
                 "consumers=%u pool(free=%u alloc=%u)",
                 c->frames_sent, c->frames_dropped,
                 static_cast<double>(fps),
                 c->notify_channel.ConsumerCount(),
                 c->disp.FreeBlocks(), c->disp.AllocBlocks());
  }, &ctx);

  // ScanTimeout timer
  ctx.timer.Add(1000, [](void* arg) {
    auto* c = static_cast<ProdCtx*>(arg);
    uint32_t t = c->disp.ScanTimeout();
    if (t > 0) OSP_LOG_WARN("Producer", "ScanTimeout reclaimed %u blocks", t);
  }, &ctx);

  sm.Start();

  // Init result
  if (ctx.pool_shm != nullptr) {
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
    if (ctx.wd_hb) ctx.wd_hb->Beat();

    int32_t cur = sm.CurrentState();

    if (cur == ctx.s_streaming) {
      auto bid = ctx.disp.Alloc();
      if (!bid.has_value()) {
        ++ctx.frames_dropped;
        sm.Dispatch({kEvtRingFull});
        continue;
      }
      uint8_t* payload = ctx.disp.GetWritable(bid.value());
      uint32_t ts_ms = static_cast<uint32_t>(
          (osp::SteadyNowUs() - ctx.t0_us) / 1000);
      FillLidarFrame(payload, ctx.seq, ts_ms);

      // Submit with explicit consumer_count from notification channel
      uint32_t num_consumers = ctx.notify_channel.ConsumerCount();
      if (num_consumers > 0) {
        ctx.disp.Submit(bid.value(), kFrameDataSize, num_consumers);
        ++ctx.seq;
        ++ctx.frames_sent;
      } else {
        // No consumers: release block immediately
        ctx.disp.Release(bid.value());
        ++ctx.frames_dropped;
      }

      if (ctx.frames_sent >= ctx.max_frames) {
        sm.Dispatch({kEvtLimitReached});
        continue;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kProduceIntervalMs));

    } else if (cur == ctx.s_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (ctx.disp.FreeBlocks() >= 4) {
        sm.Dispatch({kEvtRingAvail});
      }

    } else if (cur == ctx.s_error) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (ctx.error_count < 3) {
        sm.Dispatch({kEvtRetry});
      } else {
        sm.Dispatch({kEvtShutdown});
      }

    } else if (cur == ctx.s_done) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ctx.timer.Stop();
  ctx.fault_collector.Stop();
  ctx.watchdog.StopAutoCheck();
  return 0;
}
