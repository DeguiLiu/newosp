// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// visitor_fusion -- Data fusion subscriber for SPMC LiDAR data.
//
// Demonstrates: ShmSpmcByteChannel (reader), HSM, Shutdown, Timer.
// Simulates obstacle detection by computing point cloud bounding box.
//
// HSM states:
//   Connecting  -- retry OpenReader every 200ms
//   Processing  -- read frames, compute bounding box
//   Overloaded  -- skip frames when processing falls behind
//   Done        -- print final stats
//
// Usage: ./osp_dvd_visitor_fusion [channel_name]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
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
enum FusEvt : uint32_t {
  kEvtConnected = 1,
  kEvtConnectFail,
  kEvtOverloaded,
  kEvtCaughtUp,
  kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Bounding box
// ---------------------------------------------------------------------------
struct BBox {
  float min_x = FLT_MAX, max_x = -FLT_MAX;
  float min_y = FLT_MAX, max_y = -FLT_MAX;
  float min_z = FLT_MAX, max_z = -FLT_MAX;
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
struct FusCtx {
  osp::ShmSpmcByteChannel channel;
  const char* channel_name = kDefaultChannelName;

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

  alignas(16) uint8_t recv_buf[kFrameDataSize + 256] = {};

  osp::StateMachine<FusCtx, 6>* sm = nullptr;
  int32_t s_connecting = -1, s_processing = -1, s_overloaded = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// Compute bounding box
// ---------------------------------------------------------------------------
static BBox ComputeBBox(const uint8_t* buf, uint32_t len) {
  BBox bb;
  auto* hdr = reinterpret_cast<const LidarFrame*>(buf);
  auto* pts = reinterpret_cast<const LidarPoint*>(buf + sizeof(LidarFrame));
  uint32_t count = hdr->point_count;
  if (sizeof(LidarFrame) + count * sizeof(LidarPoint) > len) {
    count = static_cast<uint32_t>((len - sizeof(LidarFrame)) / sizeof(LidarPoint));
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (pts[i].x < bb.min_x) bb.min_x = pts[i].x;
    if (pts[i].x > bb.max_x) bb.max_x = pts[i].x;
    if (pts[i].y < bb.min_y) bb.min_y = pts[i].y;
    if (pts[i].y > bb.max_y) bb.max_y = pts[i].y;
    if (pts[i].z < bb.min_z) bb.min_z = pts[i].z;
    if (pts[i].z > bb.max_z) bb.max_z = pts[i].z;
  }
  return bb;
}

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------
static void OnEnterConnecting(FusCtx& ctx) {
  OSP_LOG_INFO("Fusion", "connecting to '%s'...", ctx.channel_name);
  ctx.connect_retries = 0;
}

static osp::TransitionResult OnConnecting(FusCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtConnected)
    return ctx.sm->RequestTransition(ctx.s_processing);
  if (event.id == kEvtConnectFail)
    return ctx.sm->RequestTransition(ctx.s_done);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterProcessing(FusCtx& /*ctx*/) {
  OSP_LOG_INFO("Fusion", "processing frames");
}

static osp::TransitionResult OnProcessing(FusCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtOverloaded)
    return ctx.sm->RequestTransition(ctx.s_overloaded);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterOverloaded(FusCtx& ctx) {
  OSP_LOG_WARN("Fusion", "overloaded: readable=%u bytes",
               ctx.channel.ReadableBytes());
}

static osp::TransitionResult OnOverloaded(FusCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtCaughtUp)
    return ctx.sm->RequestTransition(ctx.s_processing);
  if (event.id == kEvtShutdown)
    return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(FusCtx& ctx) {
  uint64_t elapsed_us = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (elapsed_us > 0)
      ? static_cast<float>(ctx.frames_processed) * 1e6f
        / static_cast<float>(elapsed_us) : 0.0f;
  uint64_t avg_lat = (ctx.frames_processed > 0)
      ? ctx.total_latency_us / ctx.frames_processed : 0;
  float skip_rate = (ctx.frames_processed + ctx.frames_skipped > 0)
      ? static_cast<float>(ctx.frames_skipped) * 100.0f
        / static_cast<float>(ctx.frames_processed + ctx.frames_skipped) : 0.0f;
  OSP_LOG_INFO("Fusion",
               "done: processed=%u skipped=%u fps=%.1f "
               "lat(avg=%lu min=%lu max=%lu us) skip=%.1f%%",
               ctx.frames_processed, ctx.frames_skipped,
               static_cast<double>(fps),
               static_cast<unsigned long>(avg_lat),
               static_cast<unsigned long>(
                   ctx.min_latency_us == UINT64_MAX ? 0 : ctx.min_latency_us),
               static_cast<unsigned long>(ctx.max_latency_us),
               static_cast<double>(skip_rate));
  ctx.finished = true;
}

static osp::TransitionResult OnDone(FusCtx& /*ctx*/,
                                     const osp::Event& /*event*/) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// Process one frame
// ---------------------------------------------------------------------------
static void ProcessFrame(FusCtx& ctx, uint32_t len) {
  if (!ValidateLidarFrame(ctx.recv_buf, len)) {
    ++ctx.frames_invalid;
    return;
  }
  uint64_t start_us = osp::SteadyNowUs();
  BBox bb = ComputeBBox(ctx.recv_buf, len);

  // Simulate variable processing time (1-3ms)
  auto* hdr = reinterpret_cast<const LidarFrame*>(ctx.recv_buf);
  uint32_t sim_ms = 1 + (hdr->point_count % 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(sim_ms));

  uint64_t lat = osp::SteadyNowUs() - start_us;
  ctx.total_latency_us += lat;
  if (lat > ctx.max_latency_us) ctx.max_latency_us = lat;
  if (lat < ctx.min_latency_us) ctx.min_latency_us = lat;
  ++ctx.frames_processed;

  if (ctx.frames_processed % 10 == 0) {
    OSP_LOG_INFO("Fusion",
                 "frame #%u: seq=%u bbox(x=[%.1f,%.1f] y=[%.1f,%.1f] "
                 "z=[%.1f,%.1f]) lat=%lu us",
                 ctx.frames_processed, hdr->seq_num,
                 static_cast<double>(bb.min_x), static_cast<double>(bb.max_x),
                 static_cast<double>(bb.min_y), static_cast<double>(bb.max_y),
                 static_cast<double>(bb.min_z), static_cast<double>(bb.max_z),
                 static_cast<unsigned long>(lat));
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  FusCtx ctx;
  if (argc > 1) ctx.channel_name = argv[1];

  OSP_LOG_INFO("Fusion", "starting: channel='%s'", ctx.channel_name);
  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});
  ctx.t0_us = osp::SteadyNowUs();

  // Stats timer
  ctx.timer.Start();
  ctx.timer.Add(3000, [](void* arg) {
    auto* c = static_cast<FusCtx*>(arg);
    uint64_t elapsed_us = osp::SteadyNowUs() - c->t0_us;
    float fps = (elapsed_us > 0)
        ? static_cast<float>(c->frames_processed) * 1e6f
          / static_cast<float>(elapsed_us) : 0.0f;
    uint64_t avg_lat = (c->frames_processed > 0)
        ? c->total_latency_us / c->frames_processed : 0;
    OSP_LOG_INFO("Fusion", "stats: processed=%u skipped=%u fps=%.1f avg_lat=%lu us",
                 c->frames_processed, c->frames_skipped,
                 static_cast<double>(fps), static_cast<unsigned long>(avg_lat));
  }, &ctx);

  // Build HSM
  osp::StateMachine<FusCtx, 6> sm(ctx);
  ctx.sm = &sm;

  ctx.s_connecting = sm.AddState({
      "Connecting", -1, OnConnecting, OnEnterConnecting, nullptr, nullptr});
  ctx.s_processing = sm.AddState({
      "Processing", -1, OnProcessing, OnEnterProcessing, nullptr, nullptr});
  ctx.s_overloaded = sm.AddState({
      "Overloaded", -1, OnOverloaded, OnEnterOverloaded, nullptr, nullptr});
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
        sm.Dispatch({kEvtConnected});
      } else {
        ++ctx.connect_retries;
        if (ctx.connect_retries >= 50) {
          OSP_LOG_ERROR("Fusion", "connect failed after %u retries",
                        ctx.connect_retries);
          sm.Dispatch({kEvtConnectFail});
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      }

    } else if (current == ctx.s_processing) {
      auto wait = ctx.channel.WaitReadable(500);
      if (wait.has_value()) {
        uint32_t len = ctx.channel.Read(ctx.recv_buf, sizeof(ctx.recv_buf));
        if (len > 0) ProcessFrame(ctx, len);
      }
      if (ctx.channel.ReadableBytes() > 5 * (kFrameDataSize + 4)) {
        sm.Dispatch({kEvtOverloaded});
      }

    } else if (current == ctx.s_overloaded) {
      uint32_t skipped = 0;
      while (ctx.channel.ReadableBytes() > kFrameDataSize + 4) {
        uint32_t len = ctx.channel.Read(ctx.recv_buf, sizeof(ctx.recv_buf));
        if (len == 0) break;
        ++skipped;
      }
      ctx.frames_skipped += skipped;
      if (skipped > 0) {
        OSP_LOG_WARN("Fusion", "skipped %u frames to catch up", skipped);
      }
      sm.Dispatch({kEvtCaughtUp});

    } else if (current == ctx.s_done) {
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
