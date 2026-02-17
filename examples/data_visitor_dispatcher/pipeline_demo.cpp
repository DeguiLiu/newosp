// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_demo -- Intra-process JobPool pipeline demo.
//
// Demonstrates: DataDispatcher, Pipeline (DAG), FaultCollector, Timer,
//               Watchdog, Shutdown, backpressure.
//
// Pipeline topology:
//   Entry(produce) -> logging (stats/seq check)
//                  -> fusion  (bounding box)
//
// All stages run synchronously in the producer thread via Pipeline::Execute().
// Data blocks are shared via refcount -- released only after both stages finish.
//
// Usage: ./osp_dvd_pipeline [num_frames]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "osp/fault_collector.hpp"
#include "osp/job_pool.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "osp/shutdown.hpp"
#include "osp/timer.hpp"
#include "osp/watchdog.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Logging stage context
// ---------------------------------------------------------------------------
struct LogStageCtx {
  uint32_t frames_received = 0;
  uint32_t last_seq = UINT32_MAX;
  uint32_t seq_gaps = 0;
};

static void LogStageHandler(const uint8_t* data, uint32_t len,
                            uint32_t /*block_id*/, void* ctx) {
  auto* log = static_cast<LogStageCtx*>(ctx);
  if (!ValidateLidarFrame(data, len)) {
    OSP_LOG_WARN("Logging", "invalid frame, len=%u", len);
    return;
  }
  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  ++log->frames_received;

  // Detect sequence gaps
  if (log->last_seq != UINT32_MAX &&
      hdr->seq_num != log->last_seq + 1) {
    ++log->seq_gaps;
    OSP_LOG_WARN("Logging", "seq gap: expected=%u got=%u",
                 log->last_seq + 1, hdr->seq_num);
  }
  log->last_seq = hdr->seq_num;

  // Log every 10th frame
  if (log->frames_received % 10 == 0) {
    OSP_LOG_INFO("Logging", "frame #%u seq=%u points=%u ts=%ums",
                 log->frames_received, hdr->seq_num,
                 hdr->point_count, hdr->timestamp_ms);
  }
}

// ---------------------------------------------------------------------------
// Fusion stage context
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
  auto* points = reinterpret_cast<const LidarPoint*>(
      data + sizeof(LidarFrame));

  // Compute bounding box
  float lx = 1e9f, hx = -1e9f;
  float ly = 1e9f, hy = -1e9f;
  float lz = 1e9f, hz = -1e9f;
  for (uint32_t i = 0; i < hdr->point_count; ++i) {
    if (points[i].x < lx) lx = points[i].x;
    if (points[i].x > hx) hx = points[i].x;
    if (points[i].y < ly) ly = points[i].y;
    if (points[i].y > hy) hy = points[i].y;
    if (points[i].z < lz) lz = points[i].z;
    if (points[i].z > hz) hz = points[i].z;
  }

  // Update global bounding box
  if (lx < fus->min_x) fus->min_x = lx;
  if (hx > fus->max_x) fus->max_x = hx;
  if (ly < fus->min_y) fus->min_y = ly;
  if (hy > fus->max_y) fus->max_y = hy;
  if (lz < fus->min_z) fus->min_z = lz;
  if (hz > fus->max_z) fus->max_z = hz;

  ++fus->frames_processed;

  if (fus->frames_processed % 50 == 0) {
    OSP_LOG_INFO("Fusion", "bbox: x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.1f,%.1f]",
                 static_cast<double>(fus->min_x),
                 static_cast<double>(fus->max_x),
                 static_cast<double>(fus->min_y),
                 static_cast<double>(fus->max_y),
                 static_cast<double>(fus->min_z),
                 static_cast<double>(fus->max_z));
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  uint32_t max_frames = 200;
  if (argc > 1) max_frames = static_cast<uint32_t>(std::atoi(argv[1]));

  OSP_LOG_INFO("Pipeline", "starting: frames=%u block_size=%u pool_size=32",
               max_frames, kFrameDataSize);

  // --- Infrastructure ---
  osp::ShutdownManager shutdown;
  shutdown.InstallSignalHandlers();
  shutdown.Register([](int) {});

  osp::ThreadWatchdog<4> watchdog;
  auto wd_reg = watchdog.Register("main", 5000);
  osp::ThreadHeartbeat* wd_hb = nullptr;
  if (wd_reg.has_value()) {
    wd_hb = wd_reg.value().heartbeat;
  }
  watchdog.StartAutoCheck(1000);

  osp::FaultCollector<4, 16> fault_collector;
  fault_collector.RegisterFault(0, 0x00020001U);  // pool_exhausted
  fault_collector.RegisterFault(1, 0x00020002U);  // block_timeout
  fault_collector.Start();

  // --- DataDispatcher with JobPool ---
  using Dispatcher = osp::DataDispatcher<kFrameDataSize, 32>;
  Dispatcher disp;
  Dispatcher::Config cfg;
  cfg.name = "lidar_pipeline";
  cfg.default_timeout_ms = 500;       // 500ms per-block timeout
  cfg.backpressure_threshold = 4;     // warn when < 4 blocks free
  disp.Init(cfg);

  // Backpressure callback
  disp.SetBackpressureCallback(
      [](uint32_t free_count, void* /*ctx*/) {
        OSP_LOG_WARN("Pipeline", "backpressure: free=%u", free_count);
      },
      nullptr);

  // Fault reporter
  osp::FaultReporter reporter;
  reporter.fn = [](uint16_t idx, uint32_t detail,
                   osp::FaultPriority pri, void* ctx) {
    static_cast<osp::FaultCollector<4, 16>*>(ctx)->ReportFault(
        idx, detail, pri);
  };
  reporter.ctx = &fault_collector;
  disp.SetFaultReporter(reporter, 0);

  // --- Pipeline: entry -> (logging, fusion) fan-out ---
  LogStageCtx log_ctx;
  FusionStageCtx fus_ctx;

  auto s_entry = disp.AddStage({"entry", nullptr, nullptr, 0});
  auto s_log = disp.AddStage({"logging", LogStageHandler, &log_ctx, 0});
  auto s_fus = disp.AddStage({"fusion", FusionStageHandler, &fus_ctx, 0});

  // Fan-out: entry -> logging, entry -> fusion
  disp.AddEdge(s_entry, s_log);
  disp.AddEdge(s_entry, s_fus);
  disp.SetEntryStage(s_entry);

  // --- Timer: periodic stats ---
  osp::TimerScheduler<4> timer;
  timer.Start();

  struct StatsCtx {
    Dispatcher* disp;
    LogStageCtx* log;
    FusionStageCtx* fus;
    uint64_t t0_us;
  };
  StatsCtx stats_ctx{&disp, &log_ctx, &fus_ctx, osp::SteadyNowUs()};

  timer.Add(2000, [](void* arg) {
    auto* s = static_cast<StatsCtx*>(arg);
    uint64_t elapsed_us = osp::SteadyNowUs() - s->t0_us;
    float fps = (elapsed_us > 0)
        ? static_cast<float>(s->log->frames_received) * 1e6f
          / static_cast<float>(elapsed_us) : 0.0f;
    OSP_LOG_INFO("Pipeline",
                 "stats: logged=%u fused=%u gaps=%u free=%u alloc=%u fps=%.1f",
                 s->log->frames_received, s->fus->frames_processed,
                 s->log->seq_gaps, s->disp->FreeBlocks(),
                 s->disp->AllocBlocks(), static_cast<double>(fps));
  }, &stats_ctx);

  // --- Timeout scanner (every 1s) ---
  timer.Add(1000, [](void* arg) {
    auto* d = static_cast<Dispatcher*>(arg);
    uint32_t timeouts = d->ScanTimeout();
    if (timeouts > 0) {
      OSP_LOG_WARN("Pipeline", "timeout scan: %u blocks timed out", timeouts);
    }
  }, &disp);

  // --- Production loop ---
  uint64_t t0 = osp::SteadyNowUs();
  uint32_t seq = 0;
  uint32_t frames_sent = 0;
  uint32_t alloc_failures = 0;

  OSP_LOG_INFO("Pipeline", "producing %u frames at %u Hz...",
               max_frames, kProduceFps);

  while (frames_sent < max_frames && !shutdown.IsShutdownRequested()) {
    if (wd_hb != nullptr) wd_hb->Beat();

    auto bid = disp.AllocBlock();
    if (!bid.has_value()) {
      ++alloc_failures;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Fill LiDAR frame into the block payload
    uint8_t* payload = disp.GetBlockPayload(bid.value());
    uint32_t ts_ms = static_cast<uint32_t>(
        (osp::SteadyNowUs() - t0) / 1000);
    FillLidarFrame(payload, seq, ts_ms);

    // Submit: pipeline executes synchronously (entry -> logging + fusion)
    disp.SubmitBlock(bid.value(), kFrameDataSize);

    ++seq;
    ++frames_sent;

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kProduceIntervalMs));
  }

  // --- Summary ---
  uint64_t elapsed_us = osp::SteadyNowUs() - t0;
  float fps = (elapsed_us > 0)
      ? static_cast<float>(frames_sent) * 1e6f
        / static_cast<float>(elapsed_us) : 0.0f;

  OSP_LOG_INFO("Pipeline", "=== Summary ===");
  OSP_LOG_INFO("Pipeline", "  Frames produced:  %u", frames_sent);
  OSP_LOG_INFO("Pipeline", "  Alloc failures:   %u", alloc_failures);
  OSP_LOG_INFO("Pipeline", "  Logging received: %u (gaps=%u)",
               log_ctx.frames_received, log_ctx.seq_gaps);
  OSP_LOG_INFO("Pipeline", "  Fusion processed: %u", fus_ctx.frames_processed);
  OSP_LOG_INFO("Pipeline", "  Pool: free=%u alloc=%u",
               disp.FreeBlocks(), disp.AllocBlocks());
  OSP_LOG_INFO("Pipeline", "  Avg FPS: %.1f", static_cast<double>(fps));
  if (fus_ctx.frames_processed > 0) {
    OSP_LOG_INFO("Pipeline",
                 "  BBox: x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.1f,%.1f]",
                 static_cast<double>(fus_ctx.min_x),
                 static_cast<double>(fus_ctx.max_x),
                 static_cast<double>(fus_ctx.min_y),
                 static_cast<double>(fus_ctx.max_y),
                 static_cast<double>(fus_ctx.min_z),
                 static_cast<double>(fus_ctx.max_z));
  }

  timer.Stop();
  fault_collector.Stop();
  watchdog.StopAutoCheck();
  OSP_LOG_INFO("Pipeline", "done");
  return 0;
}
