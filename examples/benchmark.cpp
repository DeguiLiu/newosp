// benchmark.cpp -- Performance benchmark for newosp components
//
// Measures throughput, latency, and overhead for:
// - AsyncBus (publish/subscribe)
// - StaticNode (direct dispatch vs callback dispatch)
// - ShmRingBuffer (shared memory SPSC)
// - Transport (frame encode/decode)
// - MemPool (allocation/deallocation)
// - Timer (scheduling overhead)
// - WorkerPool (task dispatch)

#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"
#include "osp/static_node.hpp"
#include "osp/shm_transport.hpp"
#include "osp/transport.hpp"
#include "osp/mem_pool.hpp"
#include "osp/timer.hpp"
#include "osp/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

// -- Test message definitions ------------------------------------------------

struct SmallMsg {  // 24 bytes
  uint64_t id;
  uint64_t ts;
  uint64_t value;
};
static_assert(sizeof(SmallMsg) == 24, "SmallMsg must be 24 bytes");

struct MediumMsg {  // 64 bytes
  uint64_t id;
  uint64_t ts;
  uint64_t value;
  uint8_t padding[40];
};
static_assert(sizeof(MediumMsg) == 64, "MediumMsg must be 64 bytes");

struct LargeMsg {  // 256 bytes
  uint64_t id;
  uint64_t ts;
  uint64_t value;
  uint8_t payload[232];
};
static_assert(sizeof(LargeMsg) == 256, "LargeMsg must be 256 bytes");

using BenchPayload = std::variant<SmallMsg, MediumMsg, LargeMsg>;
using Bus = osp::AsyncBus<BenchPayload>;

// -- Timing helpers ----------------------------------------------------------

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline uint64_t ElapsedNs(TimePoint start, TimePoint end) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

static uint64_t Percentile(std::vector<uint64_t>& samples, double pct) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  size_t idx = static_cast<size_t>(pct / 100.0 * (samples.size() - 1));
  return samples[idx];
}

// -- Throughput test ---------------------------------------------------------

template <typename Msg>
static void RunThroughput(Bus& bus, const char* label, uint32_t count) {
  uint32_t received = 0;
  auto handle = bus.template Subscribe<Msg>(
      [&received](const osp::MessageEnvelope<BenchPayload>&) { ++received; });

  Msg msg{};
  msg.ts = 0;
  msg.value = 42;

  auto t0 = Clock::now();
  for (uint32_t i = 0; i < count; ++i) {
    msg.id = i;
    bus.Publish(BenchPayload{msg}, 1);
  }
  // Drain all published messages
  for (uint32_t round = 0; round < 1000; ++round) {
    if (bus.ProcessBatch() == 0) break;
  }
  auto t1 = Clock::now();

  uint64_t ns = ElapsedNs(t0, t1);
  double secs = static_cast<double>(ns) / 1e9;
  double mps = (secs > 0) ? (static_cast<double>(received) / secs / 1e6) : 0;
  printf("  %-12s  %8u msgs  %8.2f ms  %8.3f M msgs/s  (rx %u)\n", label,
         count, secs * 1e3, mps, received);

  bus.Unsubscribe(handle);
}

static void BenchThroughput(Bus& bus) {
  printf("\n=== Throughput Test ===\n");
  printf("  %-12s  %8s        %8s      %8s\n", "Payload", "Count", "Time",
         "Rate");
  printf("  -----------------------------------------------------------\n");

  static constexpr uint32_t kCounts[] = {10000, 100000};
  for (uint32_t n : kCounts) {
    bus.Reset();
    RunThroughput<SmallMsg>(bus, "Small(24B)", n);
    bus.Reset();
    RunThroughput<MediumMsg>(bus, "Medium(64B)", n);
    bus.Reset();
    RunThroughput<LargeMsg>(bus, "Large(256B)", n);
    printf("\n");
  }
}

// -- End-to-end latency test -------------------------------------------------

static void BenchLatency(Bus& bus) {
  printf("=== End-to-End Latency Test (SmallMsg, 10000 samples) ===\n");

  static constexpr uint32_t kSamples = 10000;
  std::vector<uint64_t> latencies;
  latencies.reserve(kSamples);

  bus.Reset();
  auto handle = bus.Subscribe<SmallMsg>(
      [&latencies](const osp::MessageEnvelope<BenchPayload>& env) {
        auto now = Clock::now();
        uint64_t send_ns =
            std::get<SmallMsg>(env.payload).ts;
        uint64_t recv_ns =
            static_cast<uint64_t>(now.time_since_epoch().count());
        if (recv_ns > send_ns) {
          latencies.push_back(recv_ns - send_ns);
        }
      });

  SmallMsg msg{};
  msg.value = 0;
  for (uint32_t i = 0; i < kSamples; ++i) {
    msg.id = i;
    msg.ts =
        static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    bus.Publish(BenchPayload{msg}, 1);
    bus.ProcessBatch();
  }

  bus.Unsubscribe(handle);

  if (latencies.empty()) {
    printf("  No latency samples collected.\n");
    return;
  }

  uint64_t p50 = Percentile(latencies, 50.0);
  uint64_t p95 = Percentile(latencies, 95.0);
  uint64_t p99 = Percentile(latencies, 99.0);

  uint64_t sum = 0;
  for (uint64_t v : latencies) sum += v;
  uint64_t mean = sum / latencies.size();

  printf("  Samples: %zu\n", latencies.size());
  printf("  Mean : %8lu ns\n", static_cast<unsigned long>(mean));
  printf("  P50  : %8lu ns\n", static_cast<unsigned long>(p50));
  printf("  P95  : %8lu ns\n", static_cast<unsigned long>(p95));
  printf("  P99  : %8lu ns\n", static_cast<unsigned long>(p99));
}

// -- Backpressure / priority stress test -------------------------------------

static void BenchBackpressure(Bus& bus) {
  printf("\n=== Backpressure Stress Test ===\n");
  bus.Reset();

  uint32_t high_pub = 0, low_pub = 0;
  uint32_t high_rx = 0, low_rx = 0;

  auto handle = bus.Subscribe<SmallMsg>(
      [&high_rx, &low_rx](const osp::MessageEnvelope<BenchPayload>& env) {
        if (std::get<SmallMsg>(env.payload).value == 1)
          ++high_rx;
        else
          ++low_rx;
      });

  static constexpr uint32_t kBurst = 100000;
  SmallMsg msg{};
  msg.ts = 0;

  for (uint32_t i = 0; i < kBurst; ++i) {
    bool is_high = (i % 2 == 0);
    msg.id = i;
    msg.value = is_high ? 1 : 0;
    auto prio =
        is_high ? osp::MessagePriority::kHigh : osp::MessagePriority::kLow;
    if (bus.PublishWithPriority(BenchPayload{msg}, 1, prio)) {
      if (is_high)
        ++high_pub;
      else
        ++low_pub;
    }
  }

  // Drain
  for (uint32_t round = 0; round < 2000; ++round) {
    if (bus.ProcessBatch() == 0) break;
  }

  bus.Unsubscribe(handle);

  double high_drop =
      high_pub > 0
          ? 100.0 * (1.0 - static_cast<double>(high_rx) / high_pub)
          : 0.0;
  double low_drop =
      low_pub > 0
          ? 100.0 * (1.0 - static_cast<double>(low_rx) / low_pub)
          : 0.0;

  printf("  HIGH: published %u, received %u, drop ~%.1f%%\n", high_pub,
         high_rx, high_drop);
  printf("  LOW : published %u, received %u, drop ~%.1f%%\n", low_pub, low_rx,
         low_drop);

  if (high_drop < low_drop) {
    printf("  [PASS] HIGH messages have lower drop rate.\n");
  } else if (high_pub == low_pub && high_rx == low_rx) {
    printf("  [INFO] No drops observed -- bus capacity not reached.\n");
  } else {
    printf("  [WARN] HIGH drop >= LOW; may need tuning.\n");
  }
}

// -- ShmRingBuffer throughput test -------------------------------------------
#if defined(OSP_PLATFORM_LINUX)
static void BenchShmRingBuffer() {
  printf("\n=== ShmRingBuffer Throughput (SPSC write/read cycle) ===\n");
  static constexpr uint32_t kSlotSize = 256;
  static constexpr uint32_t kSlotCount = 1024;
  using RingBuffer = osp::ShmRingBuffer<kSlotSize, kSlotCount>;
  alignas(64) uint8_t shm_buf[RingBuffer::Size()];
  RingBuffer* rb = RingBuffer::InitAt(shm_buf);
  static constexpr uint32_t kIterations = 100000;
  uint8_t write_data[kSlotSize];
  uint8_t read_data[kSlotSize];
  std::memset(write_data, 0xAB, sizeof(write_data));
  uint32_t write_count = 0;
  uint32_t read_count = 0;
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    if (rb->TryPush(write_data, kSlotSize)) ++write_count;
    uint32_t size = 0;
    if (rb->TryPop(read_data, size)) ++read_count;
  }
  auto t1 = Clock::now();
  uint64_t ns = ElapsedNs(t0, t1);
  double secs = static_cast<double>(ns) / 1e9;
  double cycles_per_sec = (secs > 0) ? (static_cast<double>(read_count) / secs / 1e6) : 0;
  printf("  Iterations: %u\n", kIterations);
  printf("  Written   : %u\n", write_count);
  printf("  Read      : %u\n", read_count);
  printf("  Time      : %.2f ms\n", secs * 1e3);
  printf("  Throughput: %.3f M cycles/s\n", cycles_per_sec);
}
#endif

// -- Transport frame encode/decode throughput --------------------------------
static void BenchTransportCodec() {
  printf("\n=== Transport Frame Encode/Decode Throughput ===\n");
  static constexpr uint32_t kIterations = 1000000;
  uint8_t buf[osp::FrameCodec::kHeaderSize];
  osp::FrameHeader hdr;
  hdr.magic = osp::kFrameMagic;
  hdr.length = 128;
  hdr.type_index = 5;
  hdr.sender_id = 42;
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    osp::FrameCodec::EncodeHeader(hdr, buf, sizeof(buf));
  }
  auto t1 = Clock::now();
  uint64_t encode_ns = ElapsedNs(t0, t1);
  double encode_secs = static_cast<double>(encode_ns) / 1e9;
  double encode_mops = (encode_secs > 0) ? (static_cast<double>(kIterations) / encode_secs / 1e6) : 0;
  osp::FrameHeader hdr_out;
  t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    osp::FrameCodec::DecodeHeader(buf, sizeof(buf), hdr_out);
  }
  t1 = Clock::now();
  uint64_t decode_ns = ElapsedNs(t0, t1);
  double decode_secs = static_cast<double>(decode_ns) / 1e9;
  double decode_mops = (decode_secs > 0) ? (static_cast<double>(kIterations) / decode_secs / 1e6) : 0;
  printf("  Iterations: %u\n", kIterations);
  printf("  Encode    : %.2f ms  (%.3f M ops/s)\n", encode_secs * 1e3, encode_mops);
  printf("  Decode    : %.2f ms  (%.3f M ops/s)\n", decode_secs * 1e3, decode_mops);
}

// -- MemPool allocation/deallocation throughput ------------------------------
static void BenchMemPool() {
  printf("\n=== MemPool Allocation/Deallocation Throughput ===\n");
  static constexpr uint32_t kBlockSize = 64;
  static constexpr uint32_t kMaxBlocks = 1024;
  osp::FixedPool<kBlockSize, kMaxBlocks> pool;
  static constexpr uint32_t kIterations = 100000;
  void* ptrs[kMaxBlocks];
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    uint32_t alloc_count = 0;
    for (uint32_t j = 0; j < kMaxBlocks; ++j) {
      ptrs[j] = pool.Allocate();
      if (ptrs[j]) ++alloc_count;
    }
    for (uint32_t j = 0; j < alloc_count; ++j) {
      pool.Free(ptrs[j]);
    }
  }
  auto t1 = Clock::now();
  uint64_t ns = ElapsedNs(t0, t1);
  double secs = static_cast<double>(ns) / 1e9;
  uint64_t total_ops = static_cast<uint64_t>(kIterations) * kMaxBlocks * 2;
  double mops = (secs > 0) ? (static_cast<double>(total_ops) / secs / 1e6) : 0;
  printf("  Iterations: %u (batch size %u)\n", kIterations, kMaxBlocks);
  printf("  Total ops : %lu (alloc + free)\n", static_cast<unsigned long>(total_ops));
  printf("  Time      : %.2f ms\n", secs * 1e3);
  printf("  Throughput: %.3f M ops/s\n", mops);
}

// -- Timer scheduling overhead -----------------------------------------------
static std::atomic<uint32_t> g_timer_fire_count{0};
static void TimerCallback(void* /*ctx*/) {
  g_timer_fire_count.fetch_add(1, std::memory_order_relaxed);
}

struct AccuracyCtx {
  std::atomic<uint32_t> fire_count{0};
  uint64_t first_fire_ns{0};
  uint64_t last_fire_ns{0};
  std::mutex mtx;
};

static void AccuracyCallback(void* ctx) {
  auto* c = static_cast<AccuracyCtx*>(ctx);
  uint64_t now = osp::SteadyNowNs();
  std::lock_guard<std::mutex> lock(c->mtx);
  if (c->fire_count.load(std::memory_order_relaxed) == 0) {
    c->first_fire_ns = now;
  }
  c->last_fire_ns = now;
  c->fire_count.fetch_add(1, std::memory_order_relaxed);
}

static void OneShotCallback(void* ctx) {
  auto* c = static_cast<AccuracyCtx*>(ctx);
  uint64_t now = osp::SteadyNowNs();
  std::lock_guard<std::mutex> lock(c->mtx);
  c->first_fire_ns = now;
  c->fire_count.fetch_add(1, std::memory_order_relaxed);
}

// Context for periodic load testing
struct PeriodicCtx {
  std::atomic<uint32_t> fire_count{0};
  std::vector<uint64_t> fire_times;
  std::mutex mtx;
  uint32_t period_ms;
};

static void PeriodicLoadCallback(void* ctx) {
  auto* c = static_cast<PeriodicCtx*>(ctx);
  uint64_t now = osp::SteadyNowNs();
  std::lock_guard<std::mutex> lock(c->mtx);
  c->fire_times.push_back(now);
  c->fire_count.fetch_add(1, std::memory_order_relaxed);
}

static void CycleCallback(void* ctx) {
  auto* count = static_cast<std::atomic<uint32_t>*>(ctx);
  count->fetch_add(1, std::memory_order_relaxed);
}

static void BenchTimer() {
  printf("\n=== Timer Benchmark ===\n");

  // --- 1. Add/Remove throughput (cold — scheduler not running) ---
  {
    printf("\n  --- Add/Remove Throughput (scheduler stopped) ---\n");
    osp::TimerScheduler<16> sched;
    static constexpr uint32_t kIterations = 10000;
    g_timer_fire_count.store(0);

    auto t0 = Clock::now();
    for (uint32_t i = 0; i < kIterations; ++i) {
      auto add_r = sched.Add(1000, TimerCallback, nullptr);
      if (add_r.has_value()) {
        sched.Remove(add_r.value());
      }
    }
    auto t1 = Clock::now();

    uint64_t ns = ElapsedNs(t0, t1);
    double secs = static_cast<double>(ns) / 1e9;
    double kops = (secs > 0) ? (static_cast<double>(kIterations * 2) / secs / 1e3) : 0;
    printf("    Iterations: %u (Add + Remove pairs)\n", kIterations);
    printf("    Time      : %.2f ms\n", secs * 1e3);
    printf("    Throughput: %.1f K ops/s\n", kops);
  }

  // --- 2. Add/Remove throughput (hot — scheduler running with active tasks) ---
  {
    printf("\n  --- Add/Remove Throughput (scheduler running, 8 active tasks) ---\n");
    osp::TimerScheduler<16> sched;
    g_timer_fire_count.store(0);

    // Pre-fill 8 active periodic tasks
    for (int i = 0; i < 8; ++i) {
      sched.Add(10, TimerCallback, nullptr);
    }
    sched.Start();

    static constexpr uint32_t kIterations = 10000;
    auto t0 = Clock::now();
    for (uint32_t i = 0; i < kIterations; ++i) {
      auto add_r = sched.Add(1000, TimerCallback, nullptr);
      if (add_r.has_value()) {
        sched.Remove(add_r.value());
      }
    }
    auto t1 = Clock::now();
    sched.Stop();

    uint64_t ns = ElapsedNs(t0, t1);
    double secs = static_cast<double>(ns) / 1e9;
    double kops = (secs > 0) ? (static_cast<double>(kIterations * 2) / secs / 1e3) : 0;
    printf("    Iterations: %u (Add + Remove pairs)\n", kIterations);
    printf("    Time      : %.2f ms\n", secs * 1e3);
    printf("    Throughput: %.1f K ops/s\n", kops);
    printf("    BG fires  : %u\n", g_timer_fire_count.load());
  }

  // --- 3. Periodic timer firing accuracy ---
  {
    printf("\n  --- Periodic Timer Accuracy (50ms period, 500ms run) ---\n");
    osp::TimerScheduler<4> sched;
    AccuracyCtx ctx;

    sched.Add(50, AccuracyCallback, &ctx);

    uint64_t start_ns = osp::SteadyNowNs();
    sched.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sched.Stop();

    uint32_t fires = ctx.fire_count.load();
    double expected = 10.0;  // 500ms / 50ms
    double error_pct = (fires > 0) ? (std::abs(static_cast<double>(fires) - expected) / expected * 100.0) : 100.0;

    double avg_interval_ms = 0;
    if (fires > 1) {
      uint64_t span = ctx.last_fire_ns - ctx.first_fire_ns;
      avg_interval_ms = static_cast<double>(span) / (fires - 1) / 1e6;
    }

    printf("    Expected  : ~%.0f fires\n", expected);
    printf("    Actual    : %u fires\n", fires);
    printf("    Error     : %.1f%%\n", error_pct);
    printf("    Avg intv  : %.2f ms (target 50.00 ms)\n", avg_interval_ms);
    printf("    %s\n", (error_pct < 30.0) ? "[PASS]" : "[WARN] accuracy outside 30%");
  }

  // --- 4. One-shot timer accuracy ---
  {
    printf("\n  --- One-shot Timer Accuracy (50ms delay, 10 samples) ---\n");
    static constexpr uint32_t kSamples = 10;
    uint64_t delays[kSamples];

    for (uint32_t s = 0; s < kSamples; ++s) {
      osp::TimerScheduler<1> sched;
      AccuracyCtx ctx;

      uint64_t before = osp::SteadyNowNs();
      sched.AddOneShot(50, OneShotCallback, &ctx);
      sched.Start();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      sched.Stop();

      if (ctx.fire_count.load() > 0) {
        delays[s] = (ctx.first_fire_ns - before) / 1000000ULL;  // ms
      } else {
        delays[s] = 0;
      }
    }

    uint64_t sum = 0, min_d = UINT64_MAX, max_d = 0;
    for (uint32_t s = 0; s < kSamples; ++s) {
      sum += delays[s];
      if (delays[s] < min_d) min_d = delays[s];
      if (delays[s] > max_d) max_d = delays[s];
    }
    double avg = static_cast<double>(sum) / kSamples;
    double error_pct = std::abs(avg - 50.0) / 50.0 * 100.0;

    printf("    Target    : 50 ms\n");
    printf("    Avg delay : %.1f ms\n", avg);
    printf("    Min/Max   : %lu / %lu ms\n", static_cast<unsigned long>(min_d), static_cast<unsigned long>(max_d));
    printf("    Error     : %.1f%%\n", error_pct);
    printf("    %s\n", (error_pct < 30.0) ? "[PASS]" : "[WARN] accuracy outside 30%");
  }

  // --- 5. One-shot firing accuracy (100 samples, 10ms delay) ---
  {
    printf("\n  --- One-shot Firing Accuracy (10ms delay, 100 samples) ---\n");
    static constexpr uint32_t kSamples = 100;
    std::vector<int64_t> jitters;
    jitters.reserve(kSamples);

    for (uint32_t s = 0; s < kSamples; ++s) {
      osp::TimerScheduler<1> sched;
      AccuracyCtx ctx;

      uint64_t before = osp::SteadyNowNs();
      sched.AddOneShot(10, OneShotCallback, &ctx);
      sched.Start();
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      sched.Stop();

      if (ctx.fire_count.load() > 0) {
        int64_t actual_us = static_cast<int64_t>((ctx.first_fire_ns - before) / 1000ULL);
        int64_t expected_us = 10000;
        jitters.push_back(actual_us - expected_us);
      }
    }

    if (!jitters.empty()) {
      int64_t sum = 0, max_jitter = INT64_MIN;
      for (int64_t j : jitters) {
        sum += j;
        if (std::abs(j) > std::abs(max_jitter)) max_jitter = j;
      }
      double mean_jitter = static_cast<double>(sum) / jitters.size();
      printf("    Samples   : %zu\n", jitters.size());
      printf("    Mean jitter: %.1f us\n", mean_jitter);
      printf("    Max jitter : %ld us\n", static_cast<long>(max_jitter));
      printf("    %s\n", (std::abs(mean_jitter) < 5000.0) ? "[PASS]" : "[WARN] mean jitter > 5ms");
    }
  }

  // --- 6. Periodic timer accuracy under load (8 timers, different periods) ---
  {
    printf("\n  --- Periodic Timer Accuracy Under Load (8 timers, 500ms run) ---\n");
    osp::TimerScheduler<16> sched;

    PeriodicCtx ctxs[8];
    uint32_t periods[] = {5, 10, 20, 50, 5, 10, 20, 50};

    for (int i = 0; i < 8; ++i) {
      ctxs[i].period_ms = periods[i];
      ctxs[i].fire_times.reserve(100);
      sched.Add(periods[i], PeriodicLoadCallback, &ctxs[i]);
    }

    sched.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sched.Stop();

    printf("    Period(ms)  Fires  Mean Jitter(us)  Max Jitter(us)\n");
    printf("    -----------------------------------------------\n");
    for (int i = 0; i < 8; ++i) {
      uint32_t fires = ctxs[i].fire_count.load();
      if (fires > 1 && ctxs[i].fire_times.size() > 1) {
        std::vector<int64_t> jitters;
        uint64_t expected_interval_ns = static_cast<uint64_t>(ctxs[i].period_ms) * 1000000ULL;
        for (size_t j = 1; j < ctxs[i].fire_times.size(); ++j) {
          int64_t actual_interval = static_cast<int64_t>(ctxs[i].fire_times[j] - ctxs[i].fire_times[j-1]);
          int64_t jitter = actual_interval - static_cast<int64_t>(expected_interval_ns);
          jitters.push_back(jitter / 1000);  // convert to us
        }
        int64_t sum = 0, max_jitter = 0;
        for (int64_t j : jitters) {
          sum += j;
          if (std::abs(j) > std::abs(max_jitter)) max_jitter = j;
        }
        double mean_jitter = static_cast<double>(sum) / jitters.size();
        printf("    %5u       %5u  %15.1f  %15ld\n",
               ctxs[i].period_ms, fires, mean_jitter, static_cast<long>(max_jitter));
      }
    }
  }

  // --- 7. Add/Remove throughput while running with callbacks firing ---
  {
    printf("\n  --- Add/Remove Throughput (separate thread, 8 active firing timers) ---\n");
    osp::TimerScheduler<32> sched;
    g_timer_fire_count.store(0);

    // Pre-fill 8 active periodic tasks with short period
    for (int i = 0; i < 8; ++i) {
      sched.Add(5, TimerCallback, nullptr);
    }
    sched.Start();

    std::atomic<bool> worker_done{false};
    std::atomic<uint32_t> ops_count{0};

    std::thread worker([&]() {
      auto t0 = Clock::now();
      while (ElapsedNs(t0, Clock::now()) < 200000000ULL) {  // 200ms
        auto add_r = sched.Add(1000, TimerCallback, nullptr);
        if (add_r.has_value()) {
          sched.Remove(add_r.value());
          ops_count.fetch_add(2, std::memory_order_relaxed);
        }
      }
      worker_done.store(true);
    });

    worker.join();
    sched.Stop();

    uint32_t total_ops = ops_count.load();
    double kops = static_cast<double>(total_ops) / 200.0;  // ops per ms
    printf("    Duration  : 200 ms\n");
    printf("    Total ops : %u (Add + Remove)\n", total_ops);
    printf("    Throughput: %.1f K ops/s\n", kops);
    printf("    BG fires  : %u\n", g_timer_fire_count.load());
  }

  // --- 8. One-shot Add throughput (add, fire, add cycle) ---
  {
    printf("\n  --- One-shot Add Throughput (add-fire-add cycle) ---\n");
    osp::TimerScheduler<4> sched;
    std::atomic<uint32_t> cycle_count{0};

    sched.Start();
    auto t0 = Clock::now();

    static constexpr uint32_t kCycles = 100;
    for (uint32_t i = 0; i < kCycles; ++i) {
      sched.AddOneShot(1, CycleCallback, &cycle_count);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto t1 = Clock::now();
    sched.Stop();

    uint64_t ns = ElapsedNs(t0, t1);
    double secs = static_cast<double>(ns) / 1e9;
    double rate = (secs > 0) ? (static_cast<double>(kCycles) / secs) : 0;
    printf("    Cycles    : %u\n", kCycles);
    printf("    Fired     : %u\n", cycle_count.load());
    printf("    Time      : %.2f ms\n", secs * 1e3);
    printf("    Rate      : %.1f cycles/s\n", rate);
  }

  // --- 9. Concurrent Add/Remove throughput (4 threads) ---
  {
    printf("\n  --- Concurrent Add/Remove Throughput (4 threads) ---\n");
    osp::TimerScheduler<64> sched;
    sched.Start();

    std::atomic<uint32_t> total_ops{0};
    std::atomic<bool> stop_flag{false};

    auto worker_fn = [&]() {
      uint32_t local_ops = 0;
      while (!stop_flag.load(std::memory_order_relaxed)) {
        auto add_r = sched.Add(1000, TimerCallback, nullptr);
        if (add_r.has_value()) {
          sched.Remove(add_r.value());
          local_ops += 2;
        }
      }
      total_ops.fetch_add(local_ops, std::memory_order_relaxed);
    };

    std::thread workers[4];
    auto t0 = Clock::now();
    for (int i = 0; i < 4; ++i) {
      workers[i] = std::thread(worker_fn);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop_flag.store(true, std::memory_order_relaxed);

    for (int i = 0; i < 4; ++i) {
      workers[i].join();
    }
    auto t1 = Clock::now();
    sched.Stop();

    uint64_t ns = ElapsedNs(t0, t1);
    double secs = static_cast<double>(ns) / 1e9;
    uint32_t ops = total_ops.load();
    double mops = (secs > 0) ? (static_cast<double>(ops) / secs / 1e6) : 0;
    printf("    Duration  : %.2f ms\n", secs * 1e3);
    printf("    Total ops : %u (4 threads)\n", ops);
    printf("    Throughput: %.3f M ops/s\n", mops);
  }
}

// -- WorkerPool task dispatch throughput -------------------------------------
struct WorkTask {
  uint32_t id;
};
using WorkPayload = std::variant<WorkTask>;
static std::atomic<uint32_t> g_work_processed{0};
static void WorkHandler(const WorkTask& /*task*/, const osp::MessageHeader& /*hdr*/) {
  g_work_processed.fetch_add(1, std::memory_order_relaxed);
}
static void BenchWorkerPool() {
  printf("\n=== WorkerPool Task Dispatch Throughput ===\n");
  osp::WorkerPoolConfig cfg;
  cfg.name = "bench";
  cfg.worker_num = 2;
  osp::WorkerPool<WorkPayload> pool(cfg);
  pool.RegisterHandler<WorkTask>(WorkHandler);
  pool.Start();
  static constexpr uint32_t kIterations = 100000;
  g_work_processed.store(0, std::memory_order_relaxed);
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    WorkTask task{i};
    while (!pool.Submit(task, osp::MessagePriority::kMedium)) {
    }
  }
  while (g_work_processed.load(std::memory_order_relaxed) < kIterations) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  auto t1 = Clock::now();
  pool.Shutdown();
  uint64_t ns = ElapsedNs(t0, t1);
  double secs = static_cast<double>(ns) / 1e9;
  double mops = (secs > 0) ? (static_cast<double>(kIterations) / secs / 1e6) : 0;
  auto stats = pool.GetStats();
  printf("  Iterations: %u\n", kIterations);
  printf("  Processed : %u\n", g_work_processed.load());
  printf("  Dispatched: %lu\n", static_cast<unsigned long>(stats.dispatched));
  printf("  Time      : %.2f ms\n", secs * 1e3);
  printf("  Throughput: %.3f M tasks/s\n", mops);
}

// -- StaticNode direct dispatch vs callback dispatch -------------------------

struct DispatchHandler {
  uint32_t count = 0;

  void operator()(const SmallMsg& /*d*/,
                  const osp::MessageHeader& /*h*/) noexcept {
    ++count;
  }
  void operator()(const MediumMsg& /*d*/,
                  const osp::MessageHeader& /*h*/) noexcept {
    ++count;
  }
  void operator()(const LargeMsg& /*d*/,
                  const osp::MessageHeader& /*h*/) noexcept {
    ++count;
  }
};

static void BenchStaticNodeDispatch() {
  printf("\n=== StaticNode Dispatch: Direct vs Callback ===\n");

  static constexpr uint32_t kRounds = 20;
  static constexpr uint32_t kMsgsPerRound = 1000;

  std::vector<uint64_t> direct_ns;
  std::vector<uint64_t> callback_ns;
  std::vector<uint64_t> node_ns;
  direct_ns.reserve(kRounds);
  callback_ns.reserve(kRounds);
  node_ns.reserve(kRounds);

  auto& bus = Bus::Instance();

  // --- Direct dispatch (ProcessBatchWith, no Start) ---
  for (uint32_t r = 0; r < kRounds; ++r) {
    bus.Reset();
    osp::StaticNode<BenchPayload, DispatchHandler> sn(
        "direct", 1, DispatchHandler{});
    // No Start() -- direct dispatch mode

    SmallMsg msg{};
    for (uint32_t i = 0; i < kMsgsPerRound; ++i) {
      msg.id = i;
      bus.Publish(BenchPayload{msg}, 1);
    }

    auto t0 = Clock::now();
    while (bus.Depth() > 0) {
      sn.SpinOnce();
    }
    auto t1 = Clock::now();
    direct_ns.push_back(ElapsedNs(t0, t1));
  }

  // --- Callback dispatch (Start + ProcessBatch) ---
  for (uint32_t r = 0; r < kRounds; ++r) {
    bus.Reset();
    osp::StaticNode<BenchPayload, DispatchHandler> sn(
        "callback", 1, DispatchHandler{});
    sn.Start();

    SmallMsg msg{};
    for (uint32_t i = 0; i < kMsgsPerRound; ++i) {
      msg.id = i;
      bus.Publish(BenchPayload{msg}, 1);
    }

    auto t0 = Clock::now();
    while (bus.Depth() > 0) {
      sn.SpinOnce();
    }
    auto t1 = Clock::now();
    callback_ns.push_back(ElapsedNs(t0, t1));
    sn.Stop();
  }

  // --- Regular Node (FixedFunction callback) ---
  for (uint32_t r = 0; r < kRounds; ++r) {
    bus.Reset();
    uint32_t count = 0;
    osp::Node<BenchPayload> n("node", 1);
    n.Subscribe<SmallMsg>(
        [&count](const SmallMsg&, const osp::MessageHeader&) {
          ++count;
        });
    n.Subscribe<MediumMsg>(
        [&count](const MediumMsg&, const osp::MessageHeader&) {
          ++count;
        });
    n.Subscribe<LargeMsg>(
        [&count](const LargeMsg&, const osp::MessageHeader&) {
          ++count;
        });

    SmallMsg msg{};
    for (uint32_t i = 0; i < kMsgsPerRound; ++i) {
      msg.id = i;
      bus.Publish(BenchPayload{msg}, 1);
    }

    auto t0 = Clock::now();
    while (bus.Depth() > 0) {
      bus.ProcessBatch();
    }
    auto t1 = Clock::now();
    node_ns.push_back(ElapsedNs(t0, t1));
  }

  // --- Results ---
  uint64_t d_p50 = Percentile(direct_ns, 50.0);
  uint64_t d_p99 = Percentile(direct_ns, 99.0);
  uint64_t c_p50 = Percentile(callback_ns, 50.0);
  uint64_t c_p99 = Percentile(callback_ns, 99.0);
  uint64_t n_p50 = Percentile(node_ns, 50.0);
  uint64_t n_p99 = Percentile(node_ns, 99.0);

  double d_per_msg = static_cast<double>(d_p50) / kMsgsPerRound;
  double c_per_msg = static_cast<double>(c_p50) / kMsgsPerRound;
  double n_per_msg = static_cast<double>(n_p50) / kMsgsPerRound;

  printf("  %-20s  %8s  %8s  %10s\n",
         "Mode", "P50(us)", "P99(us)", "ns/msg");
  printf("  --------------------------------------------------\n");
  printf("  %-20s  %8.1f  %8.1f  %10.1f\n",
         "Direct (visit)",
         static_cast<double>(d_p50) / 1e3,
         static_cast<double>(d_p99) / 1e3,
         d_per_msg);
  printf("  %-20s  %8.1f  %8.1f  %10.1f\n",
         "Callback (Start+PB)",
         static_cast<double>(c_p50) / 1e3,
         static_cast<double>(c_p99) / 1e3,
         c_per_msg);
  printf("  %-20s  %8.1f  %8.1f  %10.1f\n",
         "Node (FixedFunction)",
         static_cast<double>(n_p50) / 1e3,
         static_cast<double>(n_p99) / 1e3,
         n_per_msg);

  if (c_per_msg > 0) {
    double speedup = c_per_msg / d_per_msg;
    printf("\n  Direct vs Callback speedup: %.2fx\n", speedup);
  }
  if (n_per_msg > 0) {
    double speedup = n_per_msg / d_per_msg;
    printf("  Direct vs Node speedup:     %.2fx\n", speedup);
  }

  bus.Reset();
}

// -- Main --------------------------------------------------------------------
int main() {
  printf("newosp Component Benchmark\n");
  printf("==========================\n");
  auto& bus = Bus::Instance();
  BenchThroughput(bus);
  BenchLatency(bus);
  BenchBackpressure(bus);
  bus.Reset();
  BenchStaticNodeDispatch();
#if defined(OSP_PLATFORM_LINUX)
  BenchShmRingBuffer();
#endif
  BenchTransportCodec();
  BenchMemPool();
  BenchTimer();
  BenchWorkerPool();
  printf("\nDone.\n");
  return 0;
}
