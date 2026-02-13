// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// shm_producer -- HSM-driven video frame producer over ShmChannel.
//
// Demonstrates: HSM (hierarchical states), ShmChannel, Timer, MemPool,
//               Shutdown, Log -- all newosp components.
//
// HSM hierarchy:
//   Operational (root)
//   ├── Init          -- create channel + allocate frame pool
//   ├── Running       -- parent state (handles SHUTDOWN for children)
//   │   ├── Streaming -- normal frame production
//   │   ├── Paused    -- back-pressure (ring full)
//   │   └── Throttled -- rate-limited after repeated pauses
//   ├── Error         -- recoverable error, retry init
//   └── Done          -- cleanup and exit
//
// Usage: ./osp_shm_producer [channel_name] [num_frames]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/mem_pool.hpp"
#include "osp/platform.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"

// ---------------------------------------------------------------------------
// Frame format
// ---------------------------------------------------------------------------
struct FrameHeader {
  uint32_t magic;
  uint32_t seq_num;
  uint32_t width;
  uint32_t height;
};
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

static constexpr uint32_t kMagic      = 0x4652414Du;  // 'FRAM'
static constexpr uint32_t kWidth      = 320;
static constexpr uint32_t kHeight     = 240;
static constexpr uint32_t kPixelBytes = kWidth * kHeight;
static constexpr uint32_t kFrameSize  = sizeof(FrameHeader) + kPixelBytes;

static constexpr uint32_t kSlotSize   = 81920;  // > kFrameSize (76816)
static constexpr uint32_t kSlotCount  = 16;

using Channel = osp::ShmChannel<kSlotSize, kSlotCount>;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
enum ProdEvt : uint32_t {
  kEvtInitDone = 1,
  kEvtInitFail,
  kEvtFrameSent,
  kEvtRingFull,
  kEvtRingAvail,
  kEvtThrottle,
  kEvtThrottleEnd,
  kEvtLimitReached,
  kEvtRetry,
  kEvtShutdown,
  kEvtTimerTick,
};

// ---------------------------------------------------------------------------
// Context -- shared state across all HSM handlers
// ---------------------------------------------------------------------------
struct ProdCtx {
  // SHM channel
  Channel channel;
  const char* channel_name = "frame_ch";
  uint32_t max_frames = 1000;

  // Frame pool (MemPool for frame buffers -- demonstrates mem_pool.hpp)
  osp::FixedPool<kSlotSize, 4> frame_pool;
  void* current_frame = nullptr;

  // Stats
  uint32_t seq = 0;
  uint32_t dropped = 0;
  uint32_t pause_count = 0;
  uint32_t throttle_count = 0;
  uint32_t error_count = 0;
  uint32_t consecutive_pauses = 0;

  // Timing
  uint64_t t0_us = 0;
  uint64_t pause_start_us = 0;
  double last_fps = 0.0;
  uint32_t throttle_delay_us = 5000;  // 5ms when throttled

  // Timer
  osp::TimerScheduler<4> timer;
  osp::TimerTaskId stats_timer_id{0};

  // Shutdown
  osp::ShutdownManager shutdown;

  // HSM pointer (for RequestTransition in handlers)
  osp::StateMachine<ProdCtx, 8>* sm = nullptr;

  // State indices
  int32_t s_operational = -1;
  int32_t s_init = -1;
  int32_t s_running = -1;
  int32_t s_streaming = -1;
  int32_t s_paused = -1;
  int32_t s_throttled = -1;
  int32_t s_error = -1;
  int32_t s_done = -1;

  bool finished = false;

  static constexpr uint32_t kReportInterval = 100;
  static constexpr uint32_t kThrottleThreshold = 3;  // consecutive pauses
};

// ---------------------------------------------------------------------------
// Fill frame buffer
// ---------------------------------------------------------------------------
static void FillFrame(uint8_t* buf, uint32_t seq) {
  auto* hdr    = reinterpret_cast<FrameHeader*>(buf);
  hdr->magic   = kMagic;
  hdr->seq_num = seq;
  hdr->width   = kWidth;
  hdr->height  = kHeight;

  uint8_t* pixels = buf + sizeof(FrameHeader);
  for (uint32_t i = 0; i < kPixelBytes; ++i) {
    pixels[i] = static_cast<uint8_t>((seq + i) & 0xFFu);
  }
}

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------

// --- Operational (root) ---
static osp::TransitionResult OnOperational(ProdCtx& ctx,
                                            const osp::Event& event) {
  if (event.id == kEvtShutdown) {
    OSP_LOG_INFO("producer", "shutdown signal received");
    return ctx.sm->RequestTransition(ctx.s_done);
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Init ---
static void OnEnterInit(ProdCtx& ctx) {
  OSP_LOG_INFO("producer", "initializing channel %s ...", ctx.channel_name);
  // Actual init is done in main loop to avoid nested Dispatch in entry action
}

static osp::TransitionResult OnInit(ProdCtx& ctx, const osp::Event& event) {
  if (event.id == kEvtInitDone) {
    return ctx.sm->RequestTransition(ctx.s_streaming);
  }
  if (event.id == kEvtInitFail) {
    return ctx.sm->RequestTransition(ctx.s_error);
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Running (parent) ---
static void OnEnterRunning(ProdCtx& ctx) {
  OSP_LOG_INFO("producer", "running (seq=%u)", ctx.seq);
}

static osp::TransitionResult OnRunning(ProdCtx& ctx,
                                        const osp::Event& event) {
  // Parent handles limit-reached for all children
  if (event.id == kEvtLimitReached) {
    OSP_LOG_INFO("producer", "frame limit reached (%u)", ctx.max_frames);
    return ctx.sm->RequestTransition(ctx.s_done);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnExitRunning(ProdCtx& ctx) {
  OSP_LOG_INFO("producer", "leaving running state (seq=%u)", ctx.seq);
}

// --- Streaming ---
static void OnEnterStreaming(ProdCtx& ctx) {
  ctx.consecutive_pauses = 0;
  OSP_LOG_INFO("producer", "streaming (seq=%u, fps=%.1f)",
               ctx.seq, ctx.last_fps);
}

static osp::TransitionResult OnStreaming(ProdCtx& ctx,
                                          const osp::Event& event) {
  if (event.id == kEvtRingFull) {
    ++ctx.pause_count;
    ++ctx.consecutive_pauses;
    if (ctx.consecutive_pauses >= ctx.kThrottleThreshold) {
      return ctx.sm->RequestTransition(ctx.s_throttled);
    }
    return ctx.sm->RequestTransition(ctx.s_paused);
  }
  if (event.id == kEvtFrameSent) {
    return osp::TransitionResult::kHandled;
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Paused ---
static void OnEnterPaused(ProdCtx& ctx) {
  ctx.pause_start_us = osp::SteadyNowUs();
}

static osp::TransitionResult OnPaused(ProdCtx& ctx,
                                       const osp::Event& event) {
  if (event.id == kEvtRingAvail) {
    return ctx.sm->RequestTransition(ctx.s_streaming);
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Throttled ---
static void OnEnterThrottled(ProdCtx& ctx) {
  ++ctx.throttle_count;
  OSP_LOG_WARN("producer", "throttled after %u consecutive pauses (delay=%u us)",
               ctx.consecutive_pauses, ctx.throttle_delay_us);
}

static osp::TransitionResult OnThrottled(ProdCtx& ctx,
                                          const osp::Event& event) {
  if (event.id == kEvtThrottleEnd) {
    ctx.consecutive_pauses = 0;
    return ctx.sm->RequestTransition(ctx.s_streaming);
  }
  if (event.id == kEvtRingAvail) {
    return osp::TransitionResult::kHandled;  // stay throttled
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Error ---
static void OnEnterError(ProdCtx& ctx) {
  ++ctx.error_count;
  OSP_LOG_ERROR("producer", "error state (count=%u), will retry...",
                ctx.error_count);
}

static osp::TransitionResult OnError(ProdCtx& ctx,
                                      const osp::Event& event) {
  if (event.id == kEvtRetry) {
    if (ctx.error_count >= 3) {
      OSP_LOG_ERROR("producer", "too many errors, giving up");
      return ctx.sm->RequestTransition(ctx.s_done);
    }
    return ctx.sm->RequestTransition(ctx.s_init);
  }
  return osp::TransitionResult::kUnhandled;
}

// --- Done ---
static void OnEnterDone(ProdCtx& ctx) {
  // Return frame buffer to pool
  if (ctx.current_frame != nullptr) {
    ctx.frame_pool.Free(ctx.current_frame);
    ctx.current_frame = nullptr;
  }
  ctx.channel.Unlink();
  ctx.finished = true;
  OSP_LOG_INFO("producer",
               "done. total=%u, dropped=%u, pauses=%u, throttles=%u, errors=%u",
               ctx.seq, ctx.dropped, ctx.pause_count,
               ctx.throttle_count, ctx.error_count);
}

static osp::TransitionResult OnDone(ProdCtx& /*ctx*/,
                                     const osp::Event& /*event*/) {
  return osp::TransitionResult::kHandled;  // absorb all events
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  const char* channel_name = (argc > 1) ? argv[1] : "frame_ch";
  const uint32_t max_frames =
      (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 1000;

  ProdCtx ctx;
  ctx.channel_name = channel_name;
  ctx.max_frames   = max_frames;
  ctx.shutdown.InstallSignalHandlers();

  // Build HSM
  osp::StateMachine<ProdCtx, 8> sm(ctx);
  ctx.sm = &sm;

  ctx.s_operational = sm.AddState({
      "Operational", -1, OnOperational, nullptr, nullptr, nullptr});
  ctx.s_init = sm.AddState({
      "Init", ctx.s_operational, OnInit, OnEnterInit, nullptr, nullptr});
  ctx.s_running = sm.AddState({
      "Running", ctx.s_operational, OnRunning,
      OnEnterRunning, OnExitRunning, nullptr});
  ctx.s_streaming = sm.AddState({
      "Streaming", ctx.s_running, OnStreaming,
      OnEnterStreaming, nullptr, nullptr});
  ctx.s_paused = sm.AddState({
      "Paused", ctx.s_running, OnPaused,
      OnEnterPaused, nullptr, nullptr});
  ctx.s_throttled = sm.AddState({
      "Throttled", ctx.s_running, OnThrottled,
      OnEnterThrottled, nullptr, nullptr});
  ctx.s_error = sm.AddState({
      "Error", ctx.s_operational, OnError,
      OnEnterError, nullptr, nullptr});
  ctx.s_done = sm.AddState({
      "Done", ctx.s_operational, OnDone,
      OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_init);

  // Start periodic stats timer (demonstrates timer.hpp)
  // TimerTaskFn is void(*)(void*), so use a static function + context pointer
  ctx.timer.Start();
  auto timer_r = ctx.timer.Add(2000, [](void* arg) {
    auto* c = static_cast<ProdCtx*>(arg);
    if (c->seq > 0) {
      OSP_LOG_INFO("producer",
                   "[timer] seq=%u fps=%.1f pauses=%u throttles=%u pool=%u/%u",
                   c->seq, c->last_fps, c->pause_count, c->throttle_count,
                   c->frame_pool.Capacity() - c->frame_pool.FreeCount(),
                   c->frame_pool.Capacity());
    }
  }, &ctx);
  if (timer_r) {
    ctx.stats_timer_id = timer_r.value();
  }

  // Start HSM
  sm.Start();

  // Main loop
  while (!ctx.finished) {
    if (ctx.shutdown.IsShutdownRequested()) {
      sm.Dispatch({kEvtShutdown, nullptr});
      break;
    }

    int32_t state = sm.CurrentState();

    if (state == ctx.s_init) {
      // Perform init in main loop (avoid nested Dispatch in entry action)
      auto result = Channel::CreateWriter(ctx.channel_name);
      if (!result) {
        OSP_LOG_ERROR("producer", "failed to create channel (err=%d)",
                      static_cast<int>(result.get_error()));
        sm.Dispatch({kEvtInitFail, nullptr});
        continue;
      }
      ctx.channel = static_cast<Channel&&>(result.value());

      ctx.current_frame = ctx.frame_pool.Allocate();
      if (ctx.current_frame == nullptr) {
        OSP_LOG_ERROR("producer", "frame pool exhausted");
        sm.Dispatch({kEvtInitFail, nullptr});
        continue;
      }

      ctx.t0_us = osp::SteadyNowUs();
      OSP_LOG_INFO("producer", "channel created (slot=%u x %u), pool=%u/%u",
                   kSlotSize, kSlotCount,
                   ctx.frame_pool.Capacity() - ctx.frame_pool.FreeCount(),
                   ctx.frame_pool.Capacity());
      sm.Dispatch({kEvtInitDone, nullptr});

    } else if (state == ctx.s_streaming) {
      if (ctx.max_frames > 0 && ctx.seq >= ctx.max_frames) {
        sm.Dispatch({kEvtLimitReached, nullptr});
        continue;
      }

      auto* buf = static_cast<uint8_t*>(ctx.current_frame);
      FillFrame(buf, ctx.seq);
      auto wr = ctx.channel.Write(buf, kFrameSize);
      if (wr) {
        ++ctx.seq;
        // Update FPS
        if (ctx.seq % ctx.kReportInterval == 0) {
          uint64_t t1_us = osp::SteadyNowUs();
          double elapsed_s = static_cast<double>(t1_us - ctx.t0_us) / 1000000.0;
          ctx.last_fps = (elapsed_s > 0.0)
              ? ctx.kReportInterval / elapsed_s : 0.0;
          OSP_LOG_INFO("producer", "frame #%u (%u bytes), %.1f fps",
                       ctx.seq, kFrameSize, ctx.last_fps);
          ctx.t0_us = t1_us;
        }
        sm.Dispatch({kEvtFrameSent, nullptr});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        ++ctx.dropped;
        sm.Dispatch({kEvtRingFull, nullptr});
      }

    } else if (state == ctx.s_paused) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      if (ctx.channel.Depth() < kSlotCount) {
        sm.Dispatch({kEvtRingAvail, nullptr});
      }

    } else if (state == ctx.s_throttled) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(ctx.throttle_delay_us));
      if (ctx.channel.Depth() < kSlotCount / 2) {
        sm.Dispatch({kEvtThrottleEnd, nullptr});
      }

    } else if (state == ctx.s_error) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      sm.Dispatch({kEvtRetry, nullptr});

    } else if (state == ctx.s_done) {
      break;
    }
  }

  ctx.timer.Remove(ctx.stats_timer_id);
  ctx.timer.Stop();
  return 0;
}
