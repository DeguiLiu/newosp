// bus_payload_benchmark.cpp -- AsyncBus throughput with large payloads
//
// Tests: 64B, 256B, 1024B, 4096B, 8192B payloads
// QueueDepth=4096 (envelope is large), BatchSize=4096

#include "osp/bus.hpp"
#include "osp/platform.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <variant>

using Clock = std::chrono::high_resolution_clock;

struct Msg64 {
  uint8_t data[64];
};

struct Msg256 {
  uint8_t data[256];
};

struct Msg1K {
  uint8_t data[1024];
};

struct Msg4K {
  uint8_t data[4096];
};

struct Msg8K {
  uint8_t data[8192];
};

using BigPayload = std::variant<Msg64, Msg256, Msg1K, Msg4K, Msg8K>;
using Bus = osp::AsyncBus<BigPayload>;

template <typename MsgT>
static void RunBench(Bus& bus, const char* label, uint32_t count) {
  uint32_t received = 0;
  auto handle = bus.template Subscribe<MsgT>(
      [&received](const osp::MessageEnvelope<BigPayload>&) { ++received; });

  MsgT msg{};
  std::memset(&msg, 0xAB, sizeof(msg));

  auto t0 = Clock::now();

  // Publish in batches, process after each batch to avoid queue full
  static constexpr uint32_t kBatch = 256;
  uint32_t remaining = count;

  while (remaining > 0) {
    uint32_t batch = (remaining > kBatch) ? kBatch : remaining;
    for (uint32_t i = 0; i < batch; ++i) {
      bus.Publish(BigPayload{msg}, 1);
    }
    // Drain all pending
    while (bus.ProcessBatch() > 0) {}
    remaining -= batch;
  }

  auto t1 = Clock::now();

  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  double mops = (ms > 0) ? (received / (ms / 1000.0) / 1e6) : 0;
  double mbps = (ms > 0)
      ? (static_cast<double>(received) * sizeof(MsgT) / 1048576.0 / (ms / 1000.0))
      : 0;

  printf("  %-12s  %8u  %8u  %10.2f  %10.3f  %10.1f\n",
         label, count, received, ms, mops, mbps);

  bus.Unsubscribe(handle);
}

int main() {
  printf("newosp AsyncBus Large Payload Benchmark\n");
  printf("========================================\n");
  printf("QueueDepth=%u, BatchSize=%u, Envelope sizeof=%zu\n\n",
         static_cast<uint32_t>(OSP_BUS_QUEUE_DEPTH),
         static_cast<uint32_t>(OSP_BUS_BATCH_SIZE),
         sizeof(osp::MessageEnvelope<BigPayload>));

  Bus& bus = Bus::Instance();

  static constexpr uint32_t kCounts[] = {10000, 100000};

  for (uint32_t n : kCounts) {
    printf("=== %u messages per size ===\n", n);
    printf("  %-12s  %8s  %8s  %10s  %10s  %10s\n",
           "Payload", "Count", "Recv", "Time(ms)", "M msgs/s", "MB/s");
    printf("  ------------------------------------------------------------------\n");

    RunBench<Msg64>(bus, "64B", n);
    RunBench<Msg256>(bus, "256B", n);
    RunBench<Msg1K>(bus, "1024B", n);
    RunBench<Msg4K>(bus, "4096B", n);
    RunBench<Msg8K>(bus, "8192B", n);

    printf("\n");
  }

  printf("Done.\n");
  return 0;
}
