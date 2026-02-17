// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// consumer_fusion -- DataDispatcher<ShmStore> fusion consumer.
//
// Demonstrates: DataDispatcher unified API (InterProc consumer side),
//               zero-copy GetReadable, refcount Release, HSM, Shutdown.
//
// Data path: receive NotifyMsg -> GetReadable (zero-copy) -> BBox -> Release
//
// HSM: Connecting -> Processing -> Overloaded -> Done
//
// Usage: ./osp_dd_consumer_fusion [--channel name]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <chrono>
#include <thread>

#include "osp/data_dispatcher.hpp"
#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"

#include "common.hpp"

using Store = osp::ShmStore<kFrameDataSize, kPoolMaxBlocks>;
using Disp = osp::DataDispatcher<Store>;

// ---------------------------------------------------------------------------
// BBox
// ---------------------------------------------------------------------------

struct BBox {
  float min_x = FLT_MAX, max_x = -FLT_MAX;
  float min_y = FLT_MAX, max_y = -FLT_MAX;
  float min_z = FLT_MAX, max_z = -FLT_MAX;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum FusEvt : uint32_t {
  kEvtConnected = 1, kEvtConnectFail, kEvtOverloaded,
  kEvtCaughtUp, kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct FusCtx {
  Disp disp;
  osp::ShmSpmcByteChannel notify_channel;
  void* pool_shm = nullptr;
  uint32_t pool_size = 0;

  const char* pool_name = kPoolShmName;
  const char* notify_name = kNotifyChannelName;

  uint32_t frames_processed = 0;
  uint32_t frames_skipped = 0;
  uint32_t frames_invalid = 0;

  uint64_t total_latency_us = 0;
  uint64_t max_latency_us = 0;
  uint64_t min_latency_us = UINT64_MAX;
  uint64_t t0_us = 0;
  uint32_t connect_retries = 0;

  osp::TimerScheduler<4> timer;
  osp::ShutdownManager shutdown;

  osp::StateMachine<FusCtx, 6>* sm = nullptr;
  int32_t s_connecting = -1, s_processing = -1, s_overloaded = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------

static void OnEnterConnecting(FusCtx& ctx) {
  OSP_LOG_INFO("Fusion", "connecting to pool '%s'...", ctx.pool_name);
  ctx.connect_retries = 0;
}

static osp::TransitionResult OnConnecting(FusCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtConnected) return ctx.sm->RequestTransition(ctx.s_processing);
  if (ev.id == kEvtConnectFail) return ctx.sm->RequestTransition(ctx.s_done);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterProcessing(FusCtx&) {
  OSP_LOG_INFO("Fusion", "processing frames (zero-copy via ShmStore)");
}

static osp::TransitionResult OnProcessing(FusCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtOverloaded) return ctx.sm->RequestTransition(ctx.s_overloaded);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterOverloaded(FusCtx& ctx) {
  OSP_LOG_WARN("Fusion", "overloaded: notify readable=%u bytes",
               ctx.notify_channel.ReadableBytes());
}

static osp::TransitionResult OnOverloaded(FusCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtCaughtUp) return ctx.sm->RequestTransition(ctx.s_processing);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(FusCtx& ctx) {
  uint64_t dt = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (dt > 0) ? static_cast<float>(ctx.frames_processed) * 1e6f
                         / static_cast<float>(dt) : 0.0f;
  uint64_t avg_lat = (ctx.frames_processed > 0)
      ? ctx.total_latency_us / ctx.frames_processed : 0;
  OSP_LOG_INFO("Fusion",
               "done: processed=%u skipped=%u fps=%.1f "
               "lat(avg=%lu min=%lu max=%lu us)",
               ctx.frames_processed, ctx.frames_skipped,
               static_cast<double>(fps),
               static_cast<unsigned long>(avg_lat),
               static_cast<unsigned long>(
                   ctx.min_latency_us == UINT64_MAX ? 0 : ctx.min_latency_us),
               static_cast<unsigned long>(ctx.max_latency_us));
  if (ctx.pool_shm != nullptr) {
    ClosePoolShm(ctx.pool_shm, ctx.pool_size);
    ctx.pool_shm = nullptr;
  }
  ctx.finished = true;
}

static osp::TransitionResult OnDone(FusCtx&, const osp::Event&) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// Process block (zero-copy)
// ---------------------------------------------------------------------------

static void ProcessBlock(FusCtx& ctx, uint32_t block_id) {
  const uint8_t* data = ctx.disp.GetReadable(block_id);
  uint32_t len = ctx.disp.GetPayloadSize(block_id);

  if (!ValidateLidarFrame(data, len)) {
    ++ctx.frames_invalid;
    ctx.disp.Release(block_id);
    return;
  }

  uint64_t start_us = osp::SteadyNowUs();

  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  auto* pts = reinterpret_cast<const LidarPoint*>(data + sizeof(LidarFrame));
  BBox bb;
  uint32_t count = hdr->point_count;
  for (uint32_t i = 0; i < count; ++i) {
    if (pts[i].x < bb.min_x) bb.min_x = pts[i].x;
    if (pts[i].x > bb.max_x) bb.max_x = pts[i].x;
    if (pts[i].y < bb.min_y) bb.min_y = pts[i].y;
    if (pts[i].y > bb.max_y) bb.max_y = pts[i].y;
    if (pts[i].z < bb.min_z) bb.min_z = pts[i].z;
    if (pts[i].z > bb.max_z) bb.max_z = pts[i].z;
  }

  // Simulate variable processing time
  uint32_t sim_ms = 1 + (count % 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(sim_ms));

  // Release refcount AFTER processing (zero-copy: data stays valid until Release)
  ctx.disp.Release(block_id);

  uint64_t lat = osp::SteadyNowUs() - start_us;
  ctx.total_latency_us += lat;
  if (lat > ctx.max_latency_us) ctx.max_latency_us = lat;
  if (lat < ctx.min_latency_us) ctx.min_latency_us = lat;
  ++ctx.frames_processed;

  if (ctx.frames_processed % 10 == 0) {
    OSP_LOG_INFO("Fusion",
                 "frame #%u: seq=%u bbox(x=[%.1f,%.1f] y=[%.1f,%.1f]) lat=%lu us",
                 ctx.frames_processed, hdr->seq_num,
                 static_cast<double>(bb.min_x), static_cast<double>(bb.max_x),
                 static_cast<double>(bb.min_y), static_cast<double>(bb.max_y),
                 static_cast<unsigned long>(lat));
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  FusCtx ctx;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      ctx.pool_name = argv[++i];
    }
  }

  OSP_LOG_INFO("Fusion", "starting: pool='%s'", ctx.pool_name);
  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});
  ctx.t0_us = osp::SteadyNowUs();
  ctx.pool_size = Store::RequiredShmSize();

  ctx.timer.Start();
  ctx.timer.Add(3000, [](void* arg) {
    auto* c = static_cast<FusCtx*>(arg);
    uint64_t dt = osp::SteadyNowUs() - c->t0_us;
    float fps = (dt > 0) ? static_cast<float>(c->frames_processed) * 1e6f
                           / static_cast<float>(dt) : 0.0f;
    uint64_t avg = (c->frames_processed > 0)
        ? c->total_latency_us / c->frames_processed : 0;
    OSP_LOG_INFO("Fusion", "stats: processed=%u skipped=%u fps=%.1f avg_lat=%lu us",
                 c->frames_processed, c->frames_skipped,
                 static_cast<double>(fps), static_cast<unsigned long>(avg));
  }, &ctx);

  osp::StateMachine<FusCtx, 6> sm(ctx);
  ctx.sm = &sm;

  ctx.s_connecting = sm.AddState({"Connecting", -1, OnConnecting, OnEnterConnecting, nullptr, nullptr});
  ctx.s_processing = sm.AddState({"Processing", -1, OnProcessing, OnEnterProcessing, nullptr, nullptr});
  ctx.s_overloaded = sm.AddState({"Overloaded", -1, OnOverloaded, OnEnterOverloaded, nullptr, nullptr});
  ctx.s_done = sm.AddState({"Done", -1, OnDone, OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_connecting);
  sm.Start();

  while (!ctx.finished) {
    if (ctx.shutdown.IsShutdownRequested()) {
      sm.Dispatch({kEvtShutdown});
      break;
    }

    int32_t cur = sm.CurrentState();

    if (cur == ctx.s_connecting) {
      ctx.pool_shm = OpenPoolShm(ctx.pool_name, ctx.pool_size);
      if (ctx.pool_shm == nullptr) {
        ++ctx.connect_retries;
        if (ctx.connect_retries >= 50) {
          sm.Dispatch({kEvtConnectFail});
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        continue;
      }
      auto ch = osp::ShmSpmcByteChannel::OpenReader(ctx.notify_name);
      if (!ch.has_value()) {
        ClosePoolShm(ctx.pool_shm, ctx.pool_size);
        ctx.pool_shm = nullptr;
        ++ctx.connect_retries;
        if (ctx.connect_retries >= 50) {
          sm.Dispatch({kEvtConnectFail});
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        continue;
      }
      ctx.notify_channel = static_cast<osp::ShmSpmcByteChannel&&>(ch.value());
      ctx.disp.Attach(ctx.pool_shm);
      sm.Dispatch({kEvtConnected});

    } else if (cur == ctx.s_processing) {
      auto wait = ctx.notify_channel.WaitReadable(500);
      if (wait.has_value()) {
        NotifyMsg msg;
        uint32_t len = ctx.notify_channel.Read(
            reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
        if (len == sizeof(msg)) {
          ProcessBlock(ctx, msg.block_id);
        }
      }
      // Overload detection: if notify channel is backing up
      if (ctx.notify_channel.ReadableBytes() > 5 * sizeof(NotifyMsg)) {
        sm.Dispatch({kEvtOverloaded});
      }

    } else if (cur == ctx.s_overloaded) {
      // Skip old notifications, only process the latest
      uint32_t skipped = 0;
      while (ctx.notify_channel.ReadableBytes() > sizeof(NotifyMsg)) {
        NotifyMsg msg;
        uint32_t len = ctx.notify_channel.Read(
            reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
        if (len == sizeof(msg)) {
          // Release without processing (skip)
          ctx.disp.Release(msg.block_id);
          ++skipped;
        } else {
          break;
        }
      }
      ctx.frames_skipped += skipped;
      if (skipped > 0)
        OSP_LOG_WARN("Fusion", "skipped %u frames to catch up", skipped);
      sm.Dispatch({kEvtCaughtUp});

    } else if (cur == ctx.s_done) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ctx.timer.Stop();
  OSP_LOG_INFO("Fusion", "exiting: processed=%u skipped=%u",
               ctx.frames_processed, ctx.frames_skipped);
  return 0;
}
