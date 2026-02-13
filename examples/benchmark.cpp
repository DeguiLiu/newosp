// benchmark.cpp -- Performance benchmark for newosp AsyncBus
//
// Measures throughput, end-to-end latency, and backpressure behavior.

#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"

#include <algorithm>
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

// -- Main --------------------------------------------------------------------

int main() {
  printf("newosp AsyncBus Benchmark\n");
  printf("=========================\n");

  auto& bus = Bus::Instance();

  BenchThroughput(bus);
  BenchLatency(bus);
  BenchBackpressure(bus);

  bus.Reset();
  printf("\nDone.\n");
  return 0;
}
