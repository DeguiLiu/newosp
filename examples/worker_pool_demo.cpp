// Copyright (c) 2024 liudegui. MIT License.
//
// worker_pool_demo.cpp -- WorkerPool comprehensive demo.
//
// Demonstrates:
//   1. Multi-type task dispatch (variant-based routing)
//   2. FlushAndPause / Resume (graceful draining)
//   3. SubmitSync (synchronous bypass)
//   4. Worker scaling comparison (1/2/4 workers)
//   5. Priority-based admission control
//   6. Statistics and monitoring
//
// NOTE: This demo also exposes a known architectural limitation --
// DispatchToWorker silently drops messages when all worker queues are full.
// See Demo 4 output for evidence.

#include "osp/bus.hpp"
#include "osp/platform.hpp"
#include "osp/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <variant>

// ============================================================================
// Timing Helpers
// ============================================================================

using Clock = std::chrono::high_resolution_clock;

static inline uint64_t ElapsedUs(Clock::time_point t0, Clock::time_point t1) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
}

// ============================================================================
// Demo 1: Multi-Type Task Dispatch
// ============================================================================

struct ComputeTask { uint32_t id; uint32_t iterations; };
struct IoTask { uint32_t id; };
struct LogTask { uint32_t id; };
using MultiPayload = std::variant<ComputeTask, IoTask, LogTask>;

static std::atomic<uint32_t> g_compute_n{0};
static std::atomic<uint32_t> g_io_n{0};
static std::atomic<uint32_t> g_log_n{0};

static void DemoMultiType() {
  printf("\n=== Demo 1: Multi-Type Task Dispatch ===\n");
  g_compute_n.store(0); g_io_n.store(0); g_log_n.store(0);
  osp::AsyncBus<MultiPayload>::Instance().Reset();

  osp::WorkerPoolConfig cfg;
  cfg.name = "multi";
  cfg.worker_num = 2;

  osp::WorkerPool<MultiPayload> pool(cfg);
  pool.RegisterHandler<ComputeTask>(
      +[](const ComputeTask& t, const osp::MessageHeader&) {
        volatile uint32_t sum = 0;
        for (uint32_t i = 0; i < t.iterations; ++i) sum += i;
        (void)sum;
        g_compute_n.fetch_add(1, std::memory_order_relaxed);
      });
  pool.RegisterHandler<IoTask>(
      +[](const IoTask&, const osp::MessageHeader&) {
        g_io_n.fetch_add(1, std::memory_order_relaxed);
      });
  pool.RegisterHandler<LogTask>(
      +[](const LogTask&, const osp::MessageHeader&) {
        g_log_n.fetch_add(1, std::memory_order_relaxed);
      });
  pool.Start();

  for (uint32_t i = 0; i < 100; ++i) {
    pool.Submit(ComputeTask{i, 100});
    pool.Submit(IoTask{i});
    pool.Submit(LogTask{i});
  }

  // Wait for all tasks
  while (g_compute_n.load() + g_io_n.load() + g_log_n.load() < 300) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  pool.Shutdown();

  printf("  ComputeTask: %u processed\n", g_compute_n.load());
  printf("  IoTask     : %u processed\n", g_io_n.load());
  printf("  LogTask    : %u processed\n", g_log_n.load());
  printf("  Total      : %u tasks (expected 300)\n",
         g_compute_n.load() + g_io_n.load() + g_log_n.load());
}

// ============================================================================
// Demo 2: FlushAndPause / Resume
// ============================================================================

struct FlushTask { uint32_t id; };
using FlushPayload = std::variant<FlushTask>;
static std::atomic<uint32_t> g_flush_n{0};

static void DemoFlushAndPause() {
  printf("\n=== Demo 2: FlushAndPause / Resume ===\n");
  g_flush_n.store(0);
  osp::AsyncBus<FlushPayload>::Instance().Reset();

  osp::WorkerPoolConfig cfg;
  cfg.name = "flush";
  cfg.worker_num = 2;

  osp::WorkerPool<FlushPayload> pool(cfg);
  pool.RegisterHandler<FlushTask>(
      +[](const FlushTask&, const osp::MessageHeader&) {
        g_flush_n.fetch_add(1, std::memory_order_relaxed);
      });
  pool.Start();

  // Phase 1: Submit batch
  for (uint32_t i = 0; i < 200; ++i) {
    while (!pool.Submit(FlushTask{i})) {}
  }

  auto t0 = Clock::now();
  pool.FlushAndPause();
  auto t1 = Clock::now();

  uint32_t phase1 = g_flush_n.load();
  printf("  Phase 1: %u tasks flushed in %lu us\n", phase1,
         static_cast<unsigned long>(ElapsedUs(t0, t1)));

  // While paused, Submit should return false
  bool rejected = !pool.Submit(FlushTask{999});
  printf("  Paused : Submit rejected = %s\n", rejected ? "yes" : "no");

  // Resume and submit more
  pool.Resume();
  for (uint32_t i = 0; i < 100; ++i) {
    while (!pool.Submit(FlushTask{200 + i})) {}
  }

  pool.FlushAndPause();
  printf("  Phase 2: %u total tasks (added 100 after resume)\n",
         g_flush_n.load());

  pool.Shutdown();
}

// ============================================================================
// Demo 3: SubmitSync (Synchronous Bypass)
// ============================================================================

struct SyncTask { uint32_t id; };
struct SyncOther { uint32_t id; };
using SyncPayload = std::variant<SyncTask, SyncOther>;
static std::atomic<uint32_t> g_sync_n{0};

static void DemoSubmitSync() {
  printf("\n=== Demo 3: SubmitSync (Synchronous Bypass) ===\n");
  g_sync_n.store(0);

  osp::WorkerPoolConfig cfg;
  cfg.name = "sync";
  cfg.worker_num = 1;

  osp::WorkerPool<SyncPayload> pool(cfg);
  pool.RegisterHandler<SyncTask>(
      +[](const SyncTask&, const osp::MessageHeader&) {
        g_sync_n.fetch_add(1, std::memory_order_relaxed);
      });
  // Note: NOT starting the pool -- SubmitSync doesn't need worker threads

  static constexpr uint32_t kCount = 10000;
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kCount; ++i) {
    pool.SubmitSync(SyncTask{i});
  }
  auto t1 = Clock::now();

  uint64_t us = ElapsedUs(t0, t1);
  printf("  SubmitSync: %u tasks in %lu us (%.1f us/task)\n", kCount,
         static_cast<unsigned long>(us),
         static_cast<double>(us) / kCount);
  printf("  Processed : %u (all in caller thread)\n", g_sync_n.load());

  // Unregistered type returns false
  bool ok = pool.SubmitSync(SyncOther{0});
  printf("  Unregistered type: %s\n", ok ? "true" : "false (expected)");
}

// ============================================================================
// Demo 4: Worker Scaling Comparison
//
// This demo intentionally uses a high task count to expose the
// DispatchToWorker drop behavior when worker queues are full.
// ============================================================================

struct ScaleTask { uint32_t id; uint32_t iters; };
using ScalePayload = std::variant<ScaleTask>;
static std::atomic<uint32_t> g_scale_done{0};

static void RunScaling(uint32_t worker_num, uint32_t task_count,
                       uint32_t iters) {
  g_scale_done.store(0, std::memory_order_relaxed);
  osp::AsyncBus<ScalePayload>::Instance().Reset();

  osp::WorkerPoolConfig cfg;
  cfg.name = "scale";
  cfg.worker_num = worker_num;

  osp::WorkerPool<ScalePayload> pool(cfg);
  pool.RegisterHandler<ScaleTask>(
      +[](const ScaleTask& t, const osp::MessageHeader&) {
        volatile uint32_t sum = 0;
        for (uint32_t i = 0; i < t.iters; ++i) sum += i;
        (void)sum;
        g_scale_done.fetch_add(1, std::memory_order_relaxed);
      });
  pool.Start();

  auto t0 = Clock::now();
  for (uint32_t i = 0; i < task_count; ++i) {
    while (!pool.Submit(ScaleTask{i, iters})) {}
  }

  // Wait until dispatcher drains Bus and workers finish processing.
  // Use a stable snapshot: if dispatched == processed for 3 consecutive
  // checks with Bus empty, we're done.
  uint32_t stable = 0;
  while (stable < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto s = pool.GetStats();
    if (s.dispatched == s.processed &&
        osp::AsyncBus<ScalePayload>::Instance().Depth() == 0) {
      ++stable;
    } else {
      stable = 0;
    }
  }
  auto t1 = Clock::now();
  pool.Shutdown();

  auto stats = pool.GetStats();
  uint64_t us = ElapsedUs(t0, t1);
  double secs = static_cast<double>(us) / 1e6;
  double mops = (secs > 0)
                    ? (static_cast<double>(g_scale_done.load()) / secs / 1e6)
                    : 0;

  printf("  %u workers: %8.2f ms  %6.3f M tasks/s  "
         "processed=%u/%u  dropped=%lu\n",
         worker_num, secs * 1e3, mops,
         g_scale_done.load(), task_count,
         static_cast<unsigned long>(stats.worker_queue_full));
}

static void DemoWorkerScaling() {
  printf("\n=== Demo 4: Worker Scaling Comparison ===\n");
  printf("  (CPU-bound: 500 iterations/task, 10000 tasks)\n\n");

  RunScaling(1, 10000, 500);
  RunScaling(2, 10000, 500);
  RunScaling(4, 10000, 500);

  printf("\n  NOTE: 'dropped' shows messages lost when worker queues are full.\n"
         "  This is a known limitation -- DispatchToWorker has no backpressure.\n");
}

// ============================================================================
// Demo 5: Priority-Based Admission
// ============================================================================

struct HighTask { uint32_t id; };
struct LowTask { uint32_t id; };
using PrioPayload = std::variant<HighTask, LowTask>;
static std::atomic<uint32_t> g_high_n{0};
static std::atomic<uint32_t> g_low_n{0};

static void DemoPriority() {
  printf("\n=== Demo 5: Priority-Based Admission ===\n");
  g_high_n.store(0); g_low_n.store(0);
  osp::AsyncBus<PrioPayload>::Instance().Reset();

  osp::WorkerPoolConfig cfg;
  cfg.name = "prio";
  cfg.worker_num = 1;

  osp::WorkerPool<PrioPayload> pool(cfg);
  pool.RegisterHandler<HighTask>(
      +[](const HighTask&, const osp::MessageHeader&) {
        g_high_n.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      });
  pool.RegisterHandler<LowTask>(
      +[](const LowTask&, const osp::MessageHeader&) {
        g_low_n.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      });
  pool.Start();

  uint32_t high_ok = 0, low_ok = 0, high_fail = 0, low_fail = 0;
  for (uint32_t i = 0; i < 500; ++i) {
    if (pool.Submit(HighTask{i}, osp::MessagePriority::kHigh))
      ++high_ok;
    else
      ++high_fail;
    if (pool.Submit(LowTask{i}, osp::MessagePriority::kLow))
      ++low_ok;
    else
      ++low_fail;
  }

  // Wait for processing (use stable snapshot -- some messages may be dropped)
  {
    uint32_t stable = 0;
    while (stable < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      auto s = pool.GetStats();
      if (s.dispatched == s.processed &&
          osp::AsyncBus<PrioPayload>::Instance().Depth() == 0) {
        ++stable;
      } else {
        stable = 0;
      }
    }
  }
  pool.Shutdown();

  printf("  HIGH: submitted=%u, rejected=%u, processed=%u\n",
         high_ok, high_fail, g_high_n.load());
  printf("  LOW : submitted=%u, rejected=%u, processed=%u\n",
         low_ok, low_fail, g_low_n.load());
}

// ============================================================================
// Demo 6: Statistics and Monitoring
// ============================================================================

struct StatsTask { uint32_t id; };
using StatsPayload = std::variant<StatsTask>;
static std::atomic<uint32_t> g_stats_n{0};

static void DemoStats() {
  printf("\n=== Demo 6: Statistics and Monitoring ===\n");
  g_stats_n.store(0);
  osp::AsyncBus<StatsPayload>::Instance().Reset();

  osp::WorkerPoolConfig cfg;
  cfg.name = "stats";
  cfg.worker_num = 2;

  osp::WorkerPool<StatsPayload> pool(cfg);
  pool.RegisterHandler<StatsTask>(
      +[](const StatsTask&, const osp::MessageHeader&) {
        g_stats_n.fetch_add(1, std::memory_order_relaxed);
      });
  pool.Start();

  printf("  IsRunning: %s, IsPaused: %s, Workers: %u\n",
         pool.IsRunning() ? "yes" : "no",
         pool.IsPaused() ? "yes" : "no",
         pool.WorkerCount());

  for (int wave = 1; wave <= 3; ++wave) {
    for (uint32_t i = 0; i < 1000; ++i) {
      while (!pool.Submit(StatsTask{i})) {}
    }
    pool.FlushAndPause();

    auto stats = pool.GetStats();
    printf("  Wave %d: dispatched=%lu, processed=%lu, queue_full=%lu\n",
           wave,
           static_cast<unsigned long>(stats.dispatched),
           static_cast<unsigned long>(stats.processed),
           static_cast<unsigned long>(stats.worker_queue_full));

    pool.Resume();
  }

  pool.Shutdown();
  printf("  Final: IsRunning=%s\n", pool.IsRunning() ? "yes" : "no");
}

// ============================================================================
// Main
// ============================================================================

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);

  printf("WorkerPool Comprehensive Demo\n");
  printf("==============================\n");

  DemoMultiType();
  DemoFlushAndPause();
  DemoSubmitSync();
  DemoWorkerScaling();
  DemoPriority();
  DemoStats();

  printf("\nAll demos completed.\n");
  return 0;
}
