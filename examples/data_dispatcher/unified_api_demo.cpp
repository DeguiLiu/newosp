// Copyright 2024 newosp contributors
// SPDX-License-Identifier: Apache-2.0
//
// unified_api_demo -- Store-agnostic DataDispatcher API demo.
//
// Demonstrates: The same template-parameterized business logic running
// on DataDispatcher<InProcStore> (and future ShmStore). Proves that
// the unified Alloc -> GetWritable -> Submit -> GetReadable -> Release
// API is Store-independent.
//
// Usage: ./osp_dd_unified [num_frames]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "osp/data_dispatcher.hpp"
#include "osp/log.hpp"
#include "osp/platform.hpp"

#include "common.hpp"

// ---------------------------------------------------------------------------
// Store-agnostic business logic (template-parameterized)
// ---------------------------------------------------------------------------

/// @brief Simple logging consumer: validate + count + detect gaps.
struct LogConsumer {
  uint32_t frames_received = 0;
  uint32_t last_seq = UINT32_MAX;
  uint32_t seq_gaps = 0;
};

static void LogHandler(const uint8_t* data, uint32_t len,
                       uint32_t /*block_id*/, void* ctx) {
  auto* log = static_cast<LogConsumer*>(ctx);
  if (!ValidateLidarFrame(data, len)) return;
  auto* hdr = reinterpret_cast<const LidarFrame*>(data);
  ++log->frames_received;
  if (log->last_seq != UINT32_MAX && hdr->seq_num != log->last_seq + 1) {
    ++log->seq_gaps;
  }
  log->last_seq = hdr->seq_num;
}

/// @brief Produce N frames through a DataDispatcher.
/// This function compiles identically for InProcStore and ShmStore.
template <typename DispatcherType>
uint32_t RunProduction(DispatcherType& disp, uint32_t num_frames) {
  uint32_t produced = 0;
  uint64_t t0 = osp::SteadyNowUs();
  for (uint32_t i = 0; i < num_frames; ++i) {
    auto bid = disp.Alloc();
    if (!bid.has_value()) {
      OSP_LOG_WARN("Unified", "pool exhausted at frame %u", i);
      break;
    }
    uint8_t* payload = disp.GetWritable(bid.value());
    uint32_t ts_ms = static_cast<uint32_t>(
        (osp::SteadyNowUs() - t0) / 1000);
    FillLidarFrame(payload, i, ts_ms);

    disp.Submit(bid.value(), kFrameDataSize);
    ++produced;
  }
  return produced;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  uint32_t num_frames = 100;
  if (argc > 1) num_frames = static_cast<uint32_t>(std::atoi(argv[1]));

  OSP_LOG_INFO("Unified", "=== Store-Agnostic API Demo ===");
  OSP_LOG_INFO("Unified", "Proving identical API for InProcStore (and ShmStore)");

  // --- Mode 1: InProcStore ---
  OSP_LOG_INFO("Unified", "--- InProcStore mode ---");
  {
    using Disp = osp::InProcDispatcher<kFrameDataSize, kPoolMaxBlocks>;
    static Disp disp;
    Disp::Config cfg;
    cfg.name = "inproc_unified";
    cfg.default_timeout_ms = 1000;
    disp.Init(cfg);

    LogConsumer consumer;
    auto s = disp.AddStage({"log", LogHandler, &consumer, 0});
    disp.SetEntryStage(s);

    uint64_t t0 = osp::SteadyNowUs();
    uint32_t produced = RunProduction(disp, num_frames);
    uint64_t elapsed_us = osp::SteadyNowUs() - t0;
    float fps = (elapsed_us > 0)
        ? static_cast<float>(produced) * 1e6f
          / static_cast<float>(elapsed_us) : 0.0f;

    OSP_LOG_INFO("Unified", "InProc: produced=%u received=%u gaps=%u "
                 "pool(free=%u alloc=%u) fps=%.0f",
                 produced, consumer.frames_received, consumer.seq_gaps,
                 disp.FreeBlocks(), disp.AllocBlocks(),
                 static_cast<double>(fps));
  }

  // --- Mode 2: ShmStore (same API, different Store) ---
  // Uncomment when running with actual shared memory:
  //
  // OSP_LOG_INFO("Unified", "--- ShmStore mode ---");
  // {
  //   using Store = osp::ShmStore<kFrameDataSize, kPoolMaxBlocks>;
  //   using Disp = osp::DataDispatcher<Store>;
  //   static Disp disp;
  //   uint32_t pool_size = Store::RequiredShmSize();
  //   void* shm = CreatePoolShm(kPoolShmName, pool_size);
  //   Disp::Config cfg;
  //   cfg.name = "shm_unified";
  //   disp.Init(cfg, shm, pool_size);
  //
  //   LogConsumer consumer;
  //   auto s = disp.AddStage({"log", LogHandler, &consumer, 0});
  //   disp.SetEntryStage(s);
  //
  //   uint32_t produced = RunProduction(disp, num_frames);  // Same function!
  //   OSP_LOG_INFO("Unified", "ShmStore: produced=%u received=%u",
  //                produced, consumer.frames_received);
  //
  //   ClosePoolShm(shm, pool_size);
  //   UnlinkPoolShm(kPoolShmName);
  // }

  OSP_LOG_INFO("Unified", "done");
  return 0;
}
