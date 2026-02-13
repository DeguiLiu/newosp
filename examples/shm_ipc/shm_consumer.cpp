// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// shm_consumer -- HSM-driven frame consumer from ShmChannel.
//
// Demonstrates: HSM, ShmChannel, Timer, MemPool, Shutdown, Log,
//               Watchdog, FaultCollector, SpscRingbuffer.
//
// HSM hierarchy:
//   Operational (root)
//   +-- Connecting     -- wait for producer to create channel
//   +-- Running        -- parent state (handles SHUTDOWN for children)
//   |   +-- Receiving  -- normal frame consumption
//   |   +-- Validating -- frame integrity check
//   |   +-- Stalled    -- no data timeout
//   +-- Error          -- recoverable error, retry connect
//   +-- Done           -- final stats and exit
//
// New integrations vs original:
//   - ThreadWatchdog: monitors main loop liveness
//   - FaultCollector: reports stall / validation-fail / thread-death faults
//   - SpscRingbuffer<ShmStats>: pushes stats snapshots for monitoring
//
// Usage: ./osp_shm_consumer [channel_name]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/fault_collector.hpp"
#include "osp/hsm.hpp"
#include "osp/log.hpp"
#include "osp/mem_pool.hpp"
#include "osp/platform.hpp"
#include "osp/shm_transport.hpp"
#include "osp/shutdown.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/timer.hpp"
#include "osp/watchdog.hpp"

#include "shm_common.hpp"

using Channel = osp::ShmChannel<kSlotSize, kSlotCount>;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
enum ConsEvt : uint32_t {
  kEvtConnected = 1,
  kEvtConnectFail,
  kEvtFrameReady,
  kEvtFrameValid,
  kEvtFrameInvalid,
  kEvtTimeout,
  kEvtDataResumed,
  kEvtRetry,
  kEvtShutdown,
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------
struct ConsCtx {
  // SHM channel
  Channel channel;
  const char* channel_name = "frame_ch";

  // Frame receive buffer
  osp::FixedPool<kSlotSize, 4> frame_pool;
  void* recv_buf = nullptr;
  uint32_t recv_size = 0;

  // Stats
  uint32_t frames_ok = 0;
  uint32_t frames_bad = 0;
  uint32_t last_seq = 0;
  uint32_t gaps = 0;
  uint32_t stall_count = 0;
  uint32_t error_count = 0;
  uint32_t connect_retries = 0;

  // Timing
  uint64_t t0_us = 0;
  uint64_t last_frame_us = 0;
  float last_fps = 0.0f;
  float last_mbps = 0.0f;

  // Timer
  osp::TimerScheduler<4> timer;
  osp::TimerTaskId stats_timer_id{0};

  // Shutdown
  osp::ShutdownManager shutdown;

  // Watchdog
  osp::ThreadWatchdog<4> watchdog;
  osp::WatchdogSlotId wd_main_id{0};
  osp::ThreadHeartbeat* wd_main_hb = nullptr;

  // FaultCollector
  osp::FaultCollector<8, 32> fault_collector;

  // Stats ring (consumer -> monitor)
  osp::SpscRingbuffer<ShmStats, kStatsRingSize> stats_ring;

  // HSM
  osp::StateMachine<ConsCtx, 8>* sm = nullptr;
  int32_t s_operational = -1;
  int32_t s_connecting = -1;
  int32_t s_running = -1;
  int32_t s_receiving = -1;
  int32_t s_validating = -1;
  int32_t s_stalled = -1;
  int32_t s_error = -1;
  int32_t s_done = -1;

  bool finished = false;

  static constexpr uint32_t kReportInterval = 100;
  static constexpr uint32_t kStallTimeoutMs = 3000;
  static constexpr uint32_t kMaxConnectRetries = 50;

  void PushStats() {
    ShmStats s{};
    s.timestamp_us = osp::SteadyNowUs();
    s.frames_ok = frames_ok;
    s.frames_bad = frames_bad;
    s.gaps = gaps;
    s.stall_count = stall_count;
    s.queue_depth = channel.Depth();
    s.fps = last_fps;
    s.mbps = last_mbps;
    s.role = 1;  // consumer
    stats_ring.Push(s);
  }
};

// ---------------------------------------------------------------------------
// Verify frame data integrity
// ---------------------------------------------------------------------------
static bool VerifyFrame(const uint8_t* buf, uint32_t size) {
  if (size < sizeof(FrameHeader)) return false;
  const auto* hdr = reinterpret_cast<const FrameHeader*>(buf);
  if (hdr->magic != kMagic) return false;
  if (hdr->width != kWidth || hdr->height != kHeight) return false;
  uint32_t expected_size = sizeof(FrameHeader) + hdr->width * hdr->height;
  if (size != expected_size) return false;
  const uint8_t* pixels = buf + sizeof(FrameHeader);
  uint32_t seq = hdr->seq_num;
  for (uint32_t i = 0; i < kPixelBytes; ++i) {
    if (pixels[i] != static_cast<uint8_t>((seq + i) & 0xFFu)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// State handlers
// ---------------------------------------------------------------------------

static osp::TransitionResult OnOperational(ConsCtx& ctx,
                                            const osp::Event& event) {
  if (event.id == kEvtShutdown) {
    OSP_LOG_INFO("consumer", "shutdown signal received");
    return ctx.sm->RequestTransition(ctx.s_done);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterConnecting(ConsCtx& ctx) {
  ++ctx.connect_retries;
  OSP_LOG_INFO("consumer", "connecting to %s (attempt %u/%u) ...",
               ctx.channel_name, ctx.connect_retries, ctx.kMaxConnectRetries);
}

static osp::TransitionResult OnConnecting(ConsCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtConnected) {
    return ctx.sm->RequestTransition(ctx.s_receiving);
  }
  if (event.id == kEvtConnectFail) {
    if (ctx.connect_retries >= ctx.kMaxConnectRetries) {
      OSP_LOG_ERROR("consumer", "max retries reached");
      ctx.fault_collector.ReportFault(
          FaultCode::kConnectFail, ctx.connect_retries,
          osp::FaultPriority::kHigh);
      return ctx.sm->RequestTransition(ctx.s_error);
    }
    return osp::TransitionResult::kHandled;
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterRunning(ConsCtx& ctx) {
  ctx.t0_us = osp::SteadyNowUs();
  ctx.last_frame_us = ctx.t0_us;
  OSP_LOG_INFO("consumer", "running -- receiving frames from %s",
               ctx.channel_name);
}

static osp::TransitionResult OnRunning(ConsCtx& ctx,
                                        const osp::Event& event) {
  if (event.id == kEvtTimeout) {
    ++ctx.stall_count;
    ctx.fault_collector.ReportFault(
        FaultCode::kStall, ctx.stall_count, osp::FaultPriority::kMedium);
    return ctx.sm->RequestTransition(ctx.s_stalled);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnExitRunning(ConsCtx& ctx) {
  OSP_LOG_INFO("consumer", "leaving running (ok=%u, bad=%u, gaps=%u)",
               ctx.frames_ok, ctx.frames_bad, ctx.gaps);
}

static void OnEnterReceiving(ConsCtx& /*ctx*/) {}

static osp::TransitionResult OnReceiving(ConsCtx& ctx,
                                          const osp::Event& event) {
  if (event.id == kEvtFrameReady) {
    return ctx.sm->RequestTransition(ctx.s_validating);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterValidating(ConsCtx& /*ctx*/) {}

static osp::TransitionResult OnValidating(ConsCtx& ctx,
                                           const osp::Event& event) {
  if (event.id == kEvtFrameValid) {
    auto* buf = static_cast<uint8_t*>(ctx.recv_buf);
    const auto* hdr = reinterpret_cast<const FrameHeader*>(buf);

    if (ctx.frames_ok > 0 && hdr->seq_num != ctx.last_seq + 1) {
      ctx.gaps += hdr->seq_num - ctx.last_seq - 1;
    }
    ctx.last_seq = hdr->seq_num;
    ++ctx.frames_ok;
    ctx.last_frame_us = osp::SteadyNowUs();

    if (ctx.frames_ok == 1 || ctx.frames_ok % ctx.kReportInterval == 0) {
      uint64_t t1_us = osp::SteadyNowUs();
      double elapsed_s = static_cast<double>(t1_us - ctx.t0_us) / 1e6;
      ctx.last_fps = (elapsed_s > 0.0)
          ? static_cast<float>(ctx.kReportInterval / elapsed_s) : 0.0f;
      ctx.last_mbps = (elapsed_s > 0.0)
          ? static_cast<float>(
              static_cast<double>(ctx.kReportInterval) * ctx.recv_size
              / (elapsed_s * 1024.0 * 1024.0))
          : 0.0f;
      OSP_LOG_INFO("consumer",
                   "frame #%u OK, seq=%u, %ux%u, %.1f fps, %.1f MB/s, gaps=%u",
                   ctx.frames_ok, hdr->seq_num, hdr->width, hdr->height,
                   (ctx.frames_ok == 1) ? 0.0 : static_cast<double>(ctx.last_fps),
                   static_cast<double>(ctx.last_mbps), ctx.gaps);
      ctx.t0_us = t1_us;
    }
    return ctx.sm->RequestTransition(ctx.s_receiving);
  }
  if (event.id == kEvtFrameInvalid) {
    ++ctx.frames_bad;
    ctx.fault_collector.ReportFault(
        FaultCode::kFrameInvalid, ctx.frames_bad, osp::FaultPriority::kHigh);
    OSP_LOG_WARN("consumer", "frame verification FAILED (bad=%u)",
                 ctx.frames_bad);
    return ctx.sm->RequestTransition(ctx.s_receiving);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterStalled(ConsCtx& ctx) {
  OSP_LOG_WARN("consumer", "stalled -- no data for %u ms (stalls=%u)",
               ctx.kStallTimeoutMs, ctx.stall_count);
}

static osp::TransitionResult OnStalled(ConsCtx& ctx,
                                        const osp::Event& event) {
  if (event.id == kEvtDataResumed) {
    OSP_LOG_INFO("consumer", "data resumed after stall");
    return ctx.sm->RequestTransition(ctx.s_receiving);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterError(ConsCtx& ctx) {
  ++ctx.error_count;
  OSP_LOG_ERROR("consumer", "error state (count=%u)", ctx.error_count);
}

static osp::TransitionResult OnError(ConsCtx& ctx,
                                      const osp::Event& event) {
  if (event.id == kEvtRetry) {
    if (ctx.error_count >= 3) {
      OSP_LOG_ERROR("consumer", "too many errors, giving up");
      return ctx.sm->RequestTransition(ctx.s_done);
    }
    ctx.connect_retries = 0;
    return ctx.sm->RequestTransition(ctx.s_connecting);
  }
  return osp::TransitionResult::kUnhandled;
}

static void OnEnterDone(ConsCtx& ctx) {
  if (ctx.recv_buf != nullptr) {
    ctx.frame_pool.Free(ctx.recv_buf);
    ctx.recv_buf = nullptr;
  }
  ctx.finished = true;
  ctx.PushStats();
  OSP_LOG_INFO("consumer",
               "done. ok=%u, bad=%u, gaps=%u, stalls=%u, errors=%u, "
               "last_fps=%.1f, last_mbps=%.1f",
               ctx.frames_ok, ctx.frames_bad, ctx.gaps,
               ctx.stall_count, ctx.error_count,
               static_cast<double>(ctx.last_fps),
               static_cast<double>(ctx.last_mbps));
}

static osp::TransitionResult OnDone(ConsCtx& /*ctx*/,
                                     const osp::Event& /*event*/) {
  return osp::TransitionResult::kHandled;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  const char* channel_name = (argc > 1) ? argv[1] : "frame_ch";

  ConsCtx ctx;
  ctx.channel_name = channel_name;
  ctx.shutdown.InstallSignalHandlers();

  // Allocate receive buffer
  ctx.recv_buf = ctx.frame_pool.Allocate();
  if (ctx.recv_buf == nullptr) {
    OSP_LOG_ERROR("consumer", "frame pool allocation failed");
    return 1;
  }

  // -- Watchdog setup --
  ctx.watchdog.SetOnTimeout([](uint32_t slot_id, const char* name, void* c) {
    auto* fc = &static_cast<ConsCtx*>(c)->fault_collector;
    fc->ReportFault(FaultCode::kThreadDeath, slot_id,
                    osp::FaultPriority::kCritical);
    OSP_LOG_ERROR("consumer", "watchdog: thread '%s' (slot %u) timed out",
                  name, slot_id);
  }, &ctx);
  ctx.watchdog.SetOnRecovered([](uint32_t, const char* name, void*) {
    OSP_LOG_INFO("consumer", "watchdog: thread '%s' recovered", name);
  }, nullptr);

  auto wd_reg = ctx.watchdog.Register("main_loop", 5000);
  if (wd_reg.has_value()) {
    ctx.wd_main_id = wd_reg.value().id;
    ctx.wd_main_hb = wd_reg.value().heartbeat;
  }
  ctx.watchdog.StartAutoCheck(1000);

  // -- FaultCollector setup --
  ctx.fault_collector.RegisterFault(FaultCode::kThreadDeath,  0xFFFF0001U);
  ctx.fault_collector.RegisterFault(FaultCode::kFrameInvalid, 0x00030001U);
  ctx.fault_collector.RegisterFault(FaultCode::kStall,        0x00040001U);
  ctx.fault_collector.RegisterFault(FaultCode::kConnectFail,  0x00020001U);

  ctx.fault_collector.RegisterHook(FaultCode::kFrameInvalid,
      [](const osp::FaultEvent& e) {
        OSP_LOG_WARN("FAULT", "frame invalid (count=%u)", e.occurrence_count);
        return osp::HookAction::kHandled;
      });
  ctx.fault_collector.RegisterHook(FaultCode::kStall,
      [](const osp::FaultEvent& e) {
        OSP_LOG_WARN("FAULT", "stall detected (count=%u)", e.occurrence_count);
        return osp::HookAction::kHandled;
      });

  auto fc_reg = ctx.watchdog.Register("fault_consumer", 5000);
  if (fc_reg.has_value()) {
    ctx.fault_collector.SetConsumerHeartbeat(fc_reg.value().heartbeat);
  }
  ctx.fault_collector.Start();

  // -- Build HSM --
  osp::StateMachine<ConsCtx, 8> sm(ctx);
  ctx.sm = &sm;

  ctx.s_operational = sm.AddState({
      "Operational", -1, OnOperational, nullptr, nullptr, nullptr});
  ctx.s_connecting = sm.AddState({
      "Connecting", ctx.s_operational, OnConnecting,
      OnEnterConnecting, nullptr, nullptr});
  ctx.s_running = sm.AddState({
      "Running", ctx.s_operational, OnRunning,
      OnEnterRunning, OnExitRunning, nullptr});
  ctx.s_receiving = sm.AddState({
      "Receiving", ctx.s_running, OnReceiving,
      OnEnterReceiving, nullptr, nullptr});
  ctx.s_validating = sm.AddState({
      "Validating", ctx.s_running, OnValidating,
      OnEnterValidating, nullptr, nullptr});
  ctx.s_stalled = sm.AddState({
      "Stalled", ctx.s_running, OnStalled,
      OnEnterStalled, nullptr, nullptr});
  ctx.s_error = sm.AddState({
      "Error", ctx.s_operational, OnError,
      OnEnterError, nullptr, nullptr});
  ctx.s_done = sm.AddState({
      "Done", ctx.s_operational, OnDone,
      OnEnterDone, nullptr, nullptr});

  sm.SetInitialState(ctx.s_connecting);

  // -- Stats timer --
  ctx.timer.Start();
  auto timer_r = ctx.timer.Add(3000, [](void* arg) {
    auto* c = static_cast<ConsCtx*>(arg);
    if (c->frames_ok > 0) {
      c->PushStats();
      auto fc_stats = c->fault_collector.GetStatistics();
      OSP_LOG_INFO("consumer",
                   "[timer] ok=%u bad=%u gaps=%u stalls=%u fps=%.1f MB/s=%.1f "
                   "faults=%lu state=%s pool=%u/%u",
                   c->frames_ok, c->frames_bad, c->gaps, c->stall_count,
                   static_cast<double>(c->last_fps),
                   static_cast<double>(c->last_mbps),
                   static_cast<unsigned long>(fc_stats.total_reported),
                   c->sm->CurrentStateName(),
                   c->frame_pool.Capacity() - c->frame_pool.FreeCount(),
                   c->frame_pool.Capacity());
    }
  }, &ctx);
  if (timer_r) {
    ctx.stats_timer_id = timer_r.value();
  }

  // -- Start HSM --
  sm.Start();

  // -- Main loop --
  while (!ctx.finished) {
    if (ctx.wd_main_hb != nullptr) {
      ctx.wd_main_hb->Beat();
    }

    if (ctx.shutdown.IsShutdownRequested()) {
      sm.Dispatch({kEvtShutdown, nullptr});
      break;
    }

    int32_t state = sm.CurrentState();

    if (state == ctx.s_connecting) {
      auto result = Channel::OpenReader(channel_name);
      if (result) {
        ctx.channel = static_cast<Channel&&>(result.value());
        sm.Dispatch({kEvtConnected, nullptr});
      } else {
        sm.Dispatch({kEvtConnectFail, nullptr});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }

    } else if (state == ctx.s_receiving) {
      auto wait_r = ctx.channel.WaitReadable(500);
      if (!wait_r) {
        uint64_t now_us = osp::SteadyNowUs();
        auto since_last = (now_us - ctx.last_frame_us) / 1000;
        if (since_last > ctx.kStallTimeoutMs) {
          sm.Dispatch({kEvtTimeout, nullptr});
        }
        continue;
      }
      ctx.recv_size = kSlotSize;
      auto rd = ctx.channel.Read(ctx.recv_buf, ctx.recv_size);
      if (rd) {
        sm.Dispatch({kEvtFrameReady, nullptr});
      }

    } else if (state == ctx.s_validating) {
      auto* buf = static_cast<uint8_t*>(ctx.recv_buf);
      if (VerifyFrame(buf, ctx.recv_size)) {
        sm.Dispatch({kEvtFrameValid, nullptr});
      } else {
        sm.Dispatch({kEvtFrameInvalid, nullptr});
      }

    } else if (state == ctx.s_stalled) {
      auto wait_r = ctx.channel.WaitReadable(1000);
      if (wait_r) {
        sm.Dispatch({kEvtDataResumed, nullptr});
      }

    } else if (state == ctx.s_error) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      sm.Dispatch({kEvtRetry, nullptr});

    } else if (state == ctx.s_done) {
      break;
    }
  }

  // -- Cleanup --
  ctx.timer.Remove(ctx.stats_timer_id);
  ctx.timer.Stop();
  ctx.fault_collector.Stop();
  ctx.watchdog.StopAutoCheck();
  if (wd_reg.has_value()) {
    (void)ctx.watchdog.Unregister(ctx.wd_main_id);
  }
  if (fc_reg.has_value()) {
    (void)ctx.watchdog.Unregister(fc_reg.value().id);
  }

  auto fc_stats = ctx.fault_collector.GetStatistics();
  OSP_LOG_INFO("consumer", "fault stats: reported=%lu processed=%lu dropped=%lu",
               static_cast<unsigned long>(fc_stats.total_reported),
               static_cast<unsigned long>(fc_stats.total_processed),
               static_cast<unsigned long>(fc_stats.total_dropped));
  return 0;
}
