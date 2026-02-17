// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// consumer_logging -- DataDispatcher<ShmStore> logging consumer.
//
// Demonstrates: DataDispatcher unified API (InterProc consumer side),
//               zero-copy GetReadable, refcount Release, HSM, Shutdown.
//
// Data path: receive NotifyMsg from SPMC channel
//   -> GetReadable(block_id) -- zero-copy pointer to shm block
//   -> process (validate, seq check, intensity stats)
//   -> Release(block_id) -- decrement refcount, recycle when 0
//
// HSM: Connecting -> Receiving -> Stalled -> Done
//
// Usage: ./osp_dd_consumer_logging [--channel name]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

using Store = osp::ShmStore<kFrameDataSize, kPoolMaxBlocks>;
using Disp = osp::DataDispatcher<Store>;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum LogEvt : uint32_t {
  kEvtConnected = 1, kEvtConnectFail, kEvtStall,
  kEvtDataResumed, kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct LogCtx {
  Disp disp;
  osp::ShmSpmcByteChannel notify_channel;
  void* pool_shm = nullptr;
  uint32_t pool_size = 0;

  const char* pool_name = kPoolShmName;
  const char* notify_name = kNotifyChannelName;

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

  osp::StateMachine<LogCtx, 6>* sm = nullptr;
  int32_t s_connecting = -1, s_receiving = -1, s_stalled = -1, s_done = -1;
  bool finished = false;
};

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------

static void OnEnterConnecting(LogCtx& ctx) {
  OSP_LOG_INFO("Logging", "connecting to pool '%s' + notify '%s'...",
               ctx.pool_name, ctx.notify_name);
  ctx.connect_retries = 0;
}

static osp::TransitionResult OnConnecting(LogCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtConnected) return ctx.sm->RequestTransition(ctx.s_receiving);
  if (ev.id == kEvtConnectFail) return ctx.sm->RequestTransition(ctx.s_done);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterReceiving(LogCtx&) {
  OSP_LOG_INFO("Logging", "receiving frames (zero-copy via ShmStore)");
}

static osp::TransitionResult OnReceiving(LogCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtStall) return ctx.sm->RequestTransition(ctx.s_stalled);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterStalled(LogCtx&) {
  OSP_LOG_WARN("Logging", "stalled: no data for >3s");
}

static osp::TransitionResult OnStalled(LogCtx& ctx, const osp::Event& ev) {
  if (ev.id == kEvtDataResumed) return ctx.sm->RequestTransition(ctx.s_receiving);
  if (ev.id == kEvtShutdown) return ctx.sm->RequestTransition(ctx.s_done);
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(LogCtx& ctx) {
  uint64_t dt = osp::SteadyNowUs() - ctx.t0_us;
  float fps = (dt > 0) ? static_cast<float>(ctx.frames_received) * 1e6f
                         / static_cast<float>(dt) : 0.0f;
  uint32_t avg_int = (ctx.intensity_count > 0)
      ? static_cast<uint32_t>(ctx.intensity_sum / ctx.intensity_count) : 0;
  OSP_LOG_INFO("Logging",
               "done: frames=%u invalid=%u gaps=%u fps=%.1f "
               "intensity(min=%u max=%u avg=%u)",
               ctx.frames_received, ctx.frames_invalid, ctx.seq_gaps,
               static_cast<double>(fps),
               ctx.intensity_min, ctx.intensity_max, avg_int);
  if (ctx.pool_shm != nullptr) {
    ClosePoolShm(ctx.pool_shm, ctx.pool_size);
    ctx.pool_shm = nullptr;
  }
  ctx.finished = true;
}

static osp::TransitionResult OnDone(LogCtx&, const osp::Event&) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// Process a received block (zero-copy via ShmStore)
// ---------------------------------------------------------------------------

static void ProcessBlock(LogCtx& ctx, uint32_t block_id) {
  const uint8_t* data = ctx.disp.GetReadable(block_id);
  uint32_t len = ctx.disp.GetPayloadSize(block_id);

  if (!ValidateLidarFrame(data, len)) {
    ++ctx.frames_invalid;
    ctx.disp.Release(block_id);
    return;
  }
  ++ctx.frames_received;
  ctx.last_frame_us = osp::SteadyNowUs();

  auto* hdr = reinterpret_cast<const LidarFrame*>(data);

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
  auto* pts = reinterpret_cast<const LidarPoint*>(data + sizeof(LidarFrame));
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

  // Release refcount (zero-copy: no buffer copy needed)
  ctx.disp.Release(block_id);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  LogCtx ctx;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      ctx.pool_name = argv[++i];
    }
  }

  OSP_LOG_INFO("Logging", "starting: pool='%s'", ctx.pool_name);
  ctx.shutdown.InstallSignalHandlers();
  ctx.shutdown.Register([](int) {});
  ctx.t0_us = osp::SteadyNowUs();

  ctx.pool_size = Store::RequiredShmSize();

  // Stats timer
  ctx.timer.Start();
  ctx.timer.Add(3000, [](void* arg) {
    auto* c = static_cast<LogCtx*>(arg);
    uint64_t dt = osp::SteadyNowUs() - c->t0_us;
    float fps = (dt > 0) ? static_cast<float>(c->frames_received) * 1e6f
                           / static_cast<float>(dt) : 0.0f;
    OSP_LOG_INFO("Logging", "stats: frames=%u invalid=%u gaps=%u fps=%.1f",
                 c->frames_received, c->frames_invalid, c->seq_gaps,
                 static_cast<double>(fps));
  }, &ctx);

  // Build HSM
  osp::StateMachine<LogCtx, 6> sm(ctx);
  ctx.sm = &sm;

  ctx.s_connecting = sm.AddState({"Connecting", -1, OnConnecting, OnEnterConnecting, nullptr, nullptr});
  ctx.s_receiving = sm.AddState({"Receiving", -1, OnReceiving, OnEnterReceiving, nullptr, nullptr});
  ctx.s_stalled = sm.AddState({"Stalled", -1, OnStalled, OnEnterStalled, nullptr, nullptr});
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
      // Open pool shm
      ctx.pool_shm = OpenPoolShm(ctx.pool_name, ctx.pool_size);
      if (ctx.pool_shm == nullptr) {
        ++ctx.connect_retries;
        if (ctx.connect_retries >= 50) {
          OSP_LOG_ERROR("Logging", "pool connect failed after %u retries",
                        ctx.connect_retries);
          sm.Dispatch({kEvtConnectFail});
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        continue;
      }
      // Open notify channel
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
      // Attach DataDispatcher to pool shm
      ctx.disp.Attach(ctx.pool_shm);
      ctx.last_frame_us = osp::SteadyNowUs();
      sm.Dispatch({kEvtConnected});

    } else if (cur == ctx.s_receiving) {
      auto wait = ctx.notify_channel.WaitReadable(500);
      if (wait.has_value()) {
        NotifyMsg msg;
        uint32_t len = ctx.notify_channel.Read(
            reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
        if (len == sizeof(msg)) {
          ProcessBlock(ctx, msg.block_id);
        }
      }
      if ((osp::SteadyNowUs() - ctx.last_frame_us) > 3000000) {
        sm.Dispatch({kEvtStall});
      }

    } else if (cur == ctx.s_stalled) {
      auto wait = ctx.notify_channel.WaitReadable(1000);
      if (wait.has_value()) {
        NotifyMsg msg;
        uint32_t len = ctx.notify_channel.Read(
            reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
        if (len == sizeof(msg)) {
          ProcessBlock(ctx, msg.block_id);
          sm.Dispatch({kEvtDataResumed});
        }
      }

    } else if (cur == ctx.s_done) {
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
