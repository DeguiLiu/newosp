/**
 * @file test_observability.cpp
 * @brief Catch2 tests for osp::observability (queue/thread/frame/latency
 *        counters) and the hcs_* diagnostic shell command registration.
 */

#include "osp/observability.hpp"
#include "osp/shell_commands.hpp"
#include "osp/worker_pool.hpp"

#include <cstdint>
#include <cstring>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <variant>

// ============================================================================
// QueueCounters
// ============================================================================

TEST_CASE("QueueCounters tracks submitted/coalesced/rejected/full", "[observability]") {
  osp::QueueCounters qc;

  qc.OnSubmitted();
  qc.OnSubmitted();
  qc.OnCoalesced();
  qc.OnRejected();
  qc.OnFull();

  auto s = qc.GetSnapshot();
  REQUIRE(s.submitted == 2U);
  REQUIRE(s.coalesced == 1U);
  REQUIRE(s.rejected == 1U);
  REQUIRE(s.full == 1U);

  qc.Reset();
  auto s2 = qc.GetSnapshot();
  REQUIRE(s2.submitted == 0U);
  REQUIRE(s2.full == 0U);
}

TEST_CASE("QueueCounters tracks depth and peak", "[observability]") {
  osp::QueueCounters qc;

  qc.OnDepth(1U);
  qc.OnDepth(5U);
  qc.OnDepth(3U);
  qc.OnDepth(9U);

  auto s = qc.GetSnapshot();
  REQUIRE(s.depth == 9U);
  REQUIRE(s.peak_depth == 9U);

  // Peak must not regress on lower depth.
  qc.OnDepth(2U);
  REQUIRE(qc.GetSnapshot().peak_depth == 9U);
}

// ============================================================================
// QueueMonitor
// ============================================================================

TEST_CASE("QueueMonitor names queues and reports strategy", "[observability]") {
  osp::QueueMonitor<4> mon;

  int32_t a = mon.AddQueue("capture");
  int32_t b = mon.AddQueue("command", osp::QueueStrategy::kCoalesceLatest);
  REQUIRE(a == 0);
  REQUIRE(b == 1);
  REQUIRE(mon.Count() == 2U);

  mon.Counter(static_cast<uint32_t>(a)).OnSubmitted();
  mon.Counter(static_cast<uint32_t>(b)).OnCoalesced();

  uint32_t visited = 0U;
  mon.ForEach([&](const osp::QueueMonitor<4>::Snapshot& s) {
    if (s.name == "capture") {
      REQUIRE(s.strategy == osp::QueueStrategy::kFifo);
      REQUIRE(s.counters.submitted == 1U);
      ++visited;
    } else if (s.name == "command") {
      REQUIRE(s.strategy == osp::QueueStrategy::kCoalesceLatest);
      REQUIRE(s.counters.coalesced == 1U);
      ++visited;
    }
  });
  REQUIRE(visited == 2U);
}

// ============================================================================
// ThreadRegistry
// ============================================================================

TEST_CASE("ThreadRegistry lists threads with heartbeat age", "[observability]") {
  osp::ThreadRegistry<4> reg;
  osp::ThreadHeartbeat hb;

  REQUIRE(reg.Register("capture", &hb, 1, 1000) == 0);
  REQUIRE(reg.Register("recorder", nullptr, -1) == 1);
  REQUIRE(reg.ActiveCount() == 2U);

  hb.Beat();

  uint32_t with_hb = 0U;
  uint32_t without_hb = 0U;
  reg.ForEach([&](const osp::ThreadRegistry<4>::Snapshot& s) {
    if (s.name == "capture") {
      REQUIRE(s.has_heartbeat);
      REQUIRE(s.priority == 1);
      REQUIRE_FALSE(s.timed_out);  // just beat
      ++with_hb;
    } else if (s.name == "recorder") {
      REQUIRE_FALSE(s.has_heartbeat);
      REQUIRE(s.priority == -1);
      REQUIRE(s.age_us == 0U);
      ++without_hb;
    }
  });
  REQUIRE(with_hb == 1U);
  REQUIRE(without_hb == 1U);

  reg.Unregister(0);
  REQUIRE(reg.ActiveCount() == 1U);
}

// ============================================================================
// FrameStageSet
// ============================================================================

TEST_CASE("FrameStageSet records frames/drops and snapshots", "[observability]") {
  osp::FrameStageSet<4> fs;

  REQUIRE(fs.AddStage("capture") == 0);
  REQUIRE(fs.AddStage("display") == 1);

  fs.MarkFrame(0U, 1U);
  fs.MarkFrame(0U, 2U);
  fs.MarkDrop(0U);
  fs.MarkFrame(1U, 7U);

  uint32_t visited = 0U;
  fs.ForEachSnapshot([&](const osp::FrameStageSet<4>::StageSnapshot& s) {
    if (s.name == "capture") {
      REQUIRE(s.total_frames == 2U);
      REQUIRE(s.total_dropped == 1U);
      REQUIRE(s.last_frame_id == 2U);
      ++visited;
    } else if (s.name == "display") {
      REQUIRE(s.total_frames == 1U);
      REQUIRE(s.last_frame_id == 7U);
      ++visited;
    }
  });
  REQUIRE(visited == 2U);
}

// ============================================================================
// LatencyTracker
// ============================================================================

TEST_CASE("LatencyTracker reports min/max/percentiles and window", "[observability]") {
  osp::LatencyTracker<64> lt;

  REQUIRE(lt.GetSnapshot().count == 0U);

  // Spread samples across several log2 buckets.
  for (uint32_t i = 0U; i < 100U; ++i) {
    lt.Record(static_cast<uint64_t>(i) * 10U);  // 0..990 us
  }

  auto s = lt.GetSnapshot();
  REQUIRE(s.count == 100U);
  REQUIRE(s.min_us == 0U);
  REQUIRE(s.max_us == 990U);
  REQUIRE(s.avg_us > 0U);
  // Percentiles must be monotonic and never exceed max.
  REQUIRE(s.p50_us <= s.p95_us);
  REQUIRE(s.p95_us <= s.p99_us);
  REQUIRE(s.p99_us <= s.max_us);
  REQUIRE(s.p50_us >= s.min_us);

  // All samples are within the last 10 s, so the window sees them all.
  REQUIRE(s.window_count == 100U);
  REQUIRE(s.window_min_us == 0U);
  REQUIRE(s.window_max_us == 990U);

  lt.Reset();
  REQUIRE(lt.GetSnapshot().count == 0U);
}

// ============================================================================
// WorkerPool extended queue statistics
// ============================================================================

struct TaskX {
  uint32_t id;
  uint32_t reserved = 0;
};
using XPayload = std::variant<TaskX>;

static std::atomic<uint32_t> g_x_done{0};
static void HandleTaskX(const TaskX&, const osp::MessageHeader&) {
  g_x_done.fetch_add(1, std::memory_order_relaxed);
}

TEST_CASE("WorkerPool exposes submitted/rejected/depth stats", "[observability][worker_pool]") {
  osp::AsyncBus<XPayload>::Instance().Reset();
  g_x_done.store(0, std::memory_order_relaxed);

  osp::WorkerPoolConfig cfg;
  cfg.name = "obs";
  cfg.worker_num = 1U;
  osp::WorkerPool<XPayload> pool(cfg);
  pool.RegisterHandler<TaskX>(&HandleTaskX);

  // Rejected before start: paused is false, but SubmitSync is unrelated; Submit
  // without Start is still accepted (bus is running). Use pause to force reject.
  pool.Start();

  constexpr int kCount = 20;
  for (int i = 0; i < kCount; ++i) {
    REQUIRE(pool.Submit(TaskX{static_cast<uint32_t>(i)}));
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (g_x_done.load() < kCount && std::chrono::steady_clock::now() < deadline) {
    osp::ThreadSleepUs(static_cast<uint64_t>(1) * 1000u);
  }

  auto stats = pool.GetStats();
  REQUIRE(stats.submitted == static_cast<uint64_t>(kCount));
  REQUIRE(stats.dispatched == static_cast<uint64_t>(kCount));
  REQUIRE(stats.processed == static_cast<uint64_t>(kCount));
  REQUIRE(stats.worker_queue_full == 0U);
  REQUIRE(stats.depth == 0U);
  REQUIRE(stats.rejected == 0U);

  // Pause rejects new submissions.
  (void)pool.FlushAndPause();
  REQUIRE_FALSE(pool.Submit(TaskX{999U}));
  auto stats2 = pool.GetStats();
  REQUIRE(stats2.rejected == 1U);

  pool.Shutdown();
}

// ============================================================================
// hcs_* shell command registration + execution (no output session)
// ============================================================================

namespace {

template <typename Fn>
bool FindAndRun(const char* name, Fn&& setup) {
  setup();
  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach([&](const osp::ShellCmd& cmd) {
    if (std::strcmp(cmd.name, name) == 0) {
      found = true;
      // No thread-local session: ShellPrintf is a no-op, the command must still
      // return 0.
      REQUIRE(cmd.func(0, nullptr) == 0);
    }
  });
  return found;
}

}  // namespace

TEST_CASE("shell_cmd RegisterThreads/Queues/Frames/Latency register and execute", "[observability][shell_commands]") {
  static osp::ThreadRegistry<4> reg;
  static osp::QueueMonitor<4> qmon;
  static osp::FrameStageSet<4> fset;
  static osp::LatencyTracker<64> lat;

  CHECK(FindAndRun("hcs_threads", [] { osp::shell_cmd::RegisterThreads(reg); }));
  CHECK(FindAndRun("hcs_queues", [] { osp::shell_cmd::RegisterQueues(qmon); }));
  CHECK(FindAndRun("hcs_frames", [] { osp::shell_cmd::RegisterFrames(fset); }));
  CHECK(FindAndRun("hcs_latency", [] { osp::shell_cmd::RegisterLatency(lat); }));
}
