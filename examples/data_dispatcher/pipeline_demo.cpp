// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_demo -- Intra-process DataDispatcher pipeline demo.
//
// Demonstrates: DataDispatcher<InProcStore>, Pipeline DAG (serial + fan-out),
//               FaultCollector, Timer, Watchdog, Shutdown, backpressure,
//               ScanTimeout, ForceCleanup.
//
// Pipeline topology (4-stage DAG):
//   entry -> preprocess -> logging
//                       -> fusion
//
// Usage: ./osp_dd_pipeline [num_frames]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/fault_collector.hpp"
#include "osp/data_dispatcher.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"
#include "osp/watchdog.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Preprocess stage: count low-intensity points
// ---------------------------------------------------------------------------
struct PreprocessCtx {
  uint32_t frames_processed = 0;
  uint32_t points_filtered = 0;
};

static void PreprocessHandler(const uint8_t* data, uint32_t len,
                              uint32_t /*block_id*/, void* ctx) {
  auto* pp = static_cast<PreprocessCtx*>(ctx);
  if (!ValidateLidarFrame(data, len)) return;
  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  auto* pts = reinterpret_cast<const LidarPoint*>(data + sizeof(LidarFrame));
  uint32_t filtered = 0;
  for (uint32_t i = 0; i < hdr->point_count; ++i) {
    if (pts[i].intensity < 10) ++filtered;
  }
  pp->points_filtered += filtered;
  ++pp->frames_processed;
}

// ---------------------------------------------------------------------------
// Logging stage: sequence check + periodic stats
// ---------------------------------------------------------------------------
struct LogStageCtx {
  uint32_t frames_received = 0;
  uint32_t last_seq = UINT32_MAX;
  uint32_t seq_gaps = 0;
};

static void LogStageHandler(const uint8_t* data, uint32_t len,
                            uint32_t /*block_id*/, void* ctx) {
  auto* log = static_cast<LogStageCtx*>(ctx);
  if (!ValidateLidarFrame(data, len)) return;
  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  ++log->frames_received;
  if (log->last_seq != UINT32_MAX && hdr->seq_num != log->last_seq + 1) {
    ++log->seq_gaps;
  }
  log->last_seq = hdr->seq_num;
  if (log->frames_received % 10 == 0) {
    OSP_LOG_INFO("Logging", "frame #%u seq=%u pts=%u ts=%ums",
                 log->frames_received, hdr->seq_num,
                 hdr->point_count, hdr->timestamp_ms);
  }
}

// ---------------------------------------------------------------------------
// Fusion stage: bounding box
// ---------------------------------------------------------------------------
struct FusionStageCtx {
  uint32_t frames_processed = 0;
  float min_x = 1e9f, max_x = -1e9f;
  float min_y = 1e9f, max_y = -1e9f;
  float min_z = 1e9f, max_z = -1e9f;
};

static void FusionStageHandler(const uint8_t* data, uint32_t len,
                               uint32_t /*block_id*/, void* ctx) {
  auto* fus = static_cast<FusionStageCtx*>(ctx);
  if (!ValidateLidarFrame(data, len)) return;
  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  auto* pts = reinterpret_cast<const LidarPoint*>(data + sizeof(LidarFrame));
  for (uint32_t i = 0; i < hdr->point_count; ++i) {
    if (pts[i].x < fus->min_x) fus->min_x = pts[i].x;
    if (pts[i].x > fus->max_x) fus->max_x = pts[i].x;
    if (pts[i].y < fus->min_y) fus->min_y = pts[i].y;
    if (pts[i].y > fus->max_y) fus->max_y = pts[i].y;
    if (pts[i].z < fus->min_z) fus->min_z = pts[i].z;
    if (pts[i].z > fus->max_z) fus->max_z = pts[i].z;
  }
  ++fus->frames_processed;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  uint32_t max_frames = 200;
  if (argc > 1) max_frames = static_cast<uint32_t>(std::atoi(argv[1]));

  OSP_LOG_INFO("Pipeline", "starting: frames=%u block_size=%u pool=%u",
               max_frames, kFrameDataSize, kPoolMaxBlocks);

  osp::ShutdownManager shutdown;
  shutdown.InstallSignalHandlers();
  shutdown.Register([](int) {});

  osp::ThreadWatchdog<4> watchdog;
  auto wd_reg = watchdog.Register("main", 5000);
  osp::ThreadHeartbeat* wd_hb = nullptr;
  if (wd_reg.has_value()) wd_hb = wd_reg.value().heartbeat;
  watchdog.StartAutoCheck(1000);

  osp::FaultCollector<4, 16> fault_collector;
  fault_collector.RegisterFault(0, 0x00020001U);
  fault_collector.Start();

  // --- DataDispatcher (InProc) ---
  using Dispatcher = osp::InProcDispatcher<kFrameDataSize, kPoolMaxBlocks>;
  static Dispatcher disp;
  Dispatcher::Config cfg;
  cfg.name = "lidar_pipeline";
  cfg.default_timeout_ms = 500;
  cfg.backpressure_threshold = 4;
  disp.Init(cfg);

  disp.SetBackpressureCallback(
      [](uint32_t free_count, void*) {
        OSP_LOG_WARN("Pipeline", "backpressure: free=%u", free_count);
      }, nullptr);

  osp::FaultReporter reporter;
  reporter.fn = [](uint16_t idx, uint32_t detail,
                   osp::FaultPriority pri, void* ctx) {
    static_cast<osp::FaultCollector<4, 16>*>(ctx)->ReportFault(
        idx, detail, pri);
  };
  reporter.ctx = &fault_collector;
  disp.SetFaultReporter(reporter, 0);

  // --- 4-stage DAG: entry -> preprocess -> (logging, fusion) ---
  PreprocessCtx pp_ctx;
  LogStageCtx log_ctx;
  FusionStageCtx fus_ctx;

  auto s_entry = disp.AddStage({"entry", nullptr, nullptr, 0});
  auto s_pp = disp.AddStage({"preprocess", PreprocessHandler, &pp_ctx, 0});
  auto s_log = disp.AddStage({"logging", LogStageHandler, &log_ctx, 0});
  auto s_fus = disp.AddStage({"fusion", FusionStageHandler, &fus_ctx, 0});

  disp.AddEdge(s_entry, s_pp);
  disp.AddEdge(s_pp, s_log);
  disp.AddEdge(s_pp, s_fus);
  disp.SetEntryStage(s_entry);
  OSP_LOG_INFO("Pipeline", "DAG: entry -> preprocess -> (logging, fusion)");

  // --- Timers ---
  osp::TimerScheduler<4> timer;
  timer.Start();

  struct StatsCtx {
    Dispatcher* disp; PreprocessCtx* pp;
    LogStageCtx* log; FusionStageCtx* fus; uint64_t t0_us;
  };
  StatsCtx stats{&disp, &pp_ctx, &log_ctx, &fus_ctx, osp::SteadyNowUs()};

  timer.Add(2000, [](void* arg) {
    auto* s = static_cast<StatsCtx*>(arg);
    uint64_t dt = osp::SteadyNowUs() - s->t0_us;
    float fps = (dt > 0) ? static_cast<float>(s->log->frames_received) * 1e6f
                           / static_cast<float>(dt) : 0.0f;
    OSP_LOG_INFO("Pipeline",
                 "stats: pp=%u log=%u fus=%u gaps=%u free=%u alloc=%u fps=%.1f",
                 s->pp->frames_processed, s->log->frames_received,
                 s->fus->frames_processed, s->log->seq_gaps,
                 s->disp->FreeBlocks(), s->disp->AllocBlocks(),
                 static_cast<double>(fps));
  }, &stats);

  timer.Add(1000, [](void* arg) {
    auto* d = static_cast<Dispatcher*>(arg);
    uint32_t t = d->ScanTimeout();
    if (t > 0) OSP_LOG_WARN("Pipeline", "ScanTimeout reclaimed %u blocks", t);
  }, &disp);

  // --- Production loop ---
  uint64_t t0 = osp::SteadyNowUs();
  uint32_t seq = 0, frames_sent = 0, alloc_failures = 0;

  while (frames_sent < max_frames && !shutdown.IsShutdownRequested()) {
    if (wd_hb) wd_hb->Beat();
    auto bid = disp.Alloc();
    if (!bid.has_value()) {
      ++alloc_failures;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    uint8_t* payload = disp.GetWritable(bid.value());
    uint32_t ts_ms = static_cast<uint32_t>(
        (osp::SteadyNowUs() - t0) / 1000);
    FillLidarFrame(payload, seq, ts_ms);
    disp.Submit(bid.value(), kFrameDataSize);
    ++seq; ++frames_sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(kProduceIntervalMs));
  }

  // --- ForceCleanup demo ---
  uint32_t cleaned = disp.ForceCleanup(
      [](uint32_t, void*) -> bool { return true; }, nullptr);
  if (cleaned > 0)
    OSP_LOG_INFO("Pipeline", "ForceCleanup reclaimed %u blocks", cleaned);

  // --- Summary ---
  uint64_t dt = osp::SteadyNowUs() - t0;
  float fps = (dt > 0) ? static_cast<float>(frames_sent) * 1e6f
                         / static_cast<float>(dt) : 0.0f;
  OSP_LOG_INFO("Pipeline", "=== Summary ===");
  OSP_LOG_INFO("Pipeline", "  Produced:   %u (alloc_fail=%u)", frames_sent, alloc_failures);
  OSP_LOG_INFO("Pipeline", "  Preprocess: %u (filtered_pts=%u)",
               pp_ctx.frames_processed, pp_ctx.points_filtered);
  OSP_LOG_INFO("Pipeline", "  Logging:    %u (gaps=%u)", log_ctx.frames_received, log_ctx.seq_gaps);
  OSP_LOG_INFO("Pipeline", "  Fusion:     %u", fus_ctx.frames_processed);
  OSP_LOG_INFO("Pipeline", "  Pool: free=%u alloc=%u", disp.FreeBlocks(), disp.AllocBlocks());
  OSP_LOG_INFO("Pipeline", "  Avg FPS: %.1f", static_cast<double>(fps));
  if (fus_ctx.frames_processed > 0) {
    OSP_LOG_INFO("Pipeline", "  BBox: x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.1f,%.1f]",
                 static_cast<double>(fus_ctx.min_x), static_cast<double>(fus_ctx.max_x),
                 static_cast<double>(fus_ctx.min_y), static_cast<double>(fus_ctx.max_y),
                 static_cast<double>(fus_ctx.min_z), static_cast<double>(fus_ctx.max_z));
  }

  timer.Stop();
  fault_collector.Stop();
  watchdog.StopAutoCheck();
  return 0;
}
