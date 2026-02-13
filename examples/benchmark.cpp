// benchmark.cpp -- Performance benchmark for newosp components
//
// Measures throughput, latency, and overhead for:
// - AsyncBus (publish/subscribe)
// - ShmRingBuffer (shared memory SPSC)
// - Transport (frame encode/decode)
// - MemPool (allocation/deallocation)
// - Timer (scheduling overhead)
// - WorkerPool (task dispatch)

#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"
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
static void BenchTimer() {
  printf("\n=== Timer Scheduling Overhead (schedule + cancel cycle) ===\n");
  osp::TimerScheduler<16> sched;
  auto start_r = sched.Start();
  if (!start_r.has_value()) {
    printf("  [ERROR] Failed to start timer scheduler\n");
    return;
  }
  static constexpr uint32_t kIterations = 10000;
  std::vector<osp::TimerTaskId> task_ids;
  task_ids.reserve(kIterations);
  auto t0 = Clock::now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    auto add_r = sched.Add(1000, TimerCallback, nullptr);
    if (add_r.has_value()) {
      task_ids.push_back(add_r.value());
    }
    if (!task_ids.empty()) {
      sched.Remove(task_ids.back());
      task_ids.pop_back();
    }
  }
  auto t1 = Clock::now();
  sched.Stop();
  uint64_t ns = ElapsedNs(t0, t1);
  double secs = static_cast<double>(ns) / 1e9;
  double ops_per_sec = (secs > 0) ? (static_cast<double>(kIterations * 2) / secs / 1e3) : 0;
  printf("  Iterations: %u (schedule + cancel)\n", kIterations);
  printf("  Time      : %.2f ms\n", secs * 1e3);
  printf("  Throughput: %.3f K ops/s\n", ops_per_sec);
  printf("  Fired     : %u (should be ~0)\n", g_timer_fire_count.load());
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
  cfg.worker_queue_depth = 2048;
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

// -- Main --------------------------------------------------------------------
int main() {
  printf("newosp Component Benchmark\n");
  printf("==========================\n");
  auto& bus = Bus::Instance();
  BenchThroughput(bus);
  BenchLatency(bus);
  BenchBackpressure(bus);
  bus.Reset();
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
