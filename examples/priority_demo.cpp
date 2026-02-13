// priority_demo.cpp -- Priority-based admission control demo for AsyncBus.
// Demonstrates backpressure behavior under mixed-priority load.

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <variant>

// Message definitions -------------------------------------------------------

struct CriticalAlert {
  uint32_t alert_code;
  uint16_t source_id;
};

struct TelemetryData {
  uint16_t sensor_id;
  float value;
  uint64_t timestamp_us;
};

struct DiagnosticLog {
  uint16_t module_id;
  char message[128];
};

using Payload = std::variant<CriticalAlert, TelemetryData, DiagnosticLog>;
using Bus = osp::AsyncBus<Payload>;

// Atomic counters -----------------------------------------------------------

static std::atomic<uint32_t> g_critical_pub{0};
static std::atomic<uint32_t> g_critical_recv{0};
static std::atomic<uint32_t> g_critical_drop{0};

static std::atomic<uint32_t> g_telemetry_pub{0};
static std::atomic<uint32_t> g_telemetry_recv{0};
static std::atomic<uint32_t> g_telemetry_drop{0};

static std::atomic<uint32_t> g_diagnostic_pub{0};
static std::atomic<uint32_t> g_diagnostic_recv{0};
static std::atomic<uint32_t> g_diagnostic_drop{0};

static uint64_t NowUs() {
  auto now = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch())
          .count());
}

static void PrintStats(const char* label, uint32_t pub, uint32_t recv,
                       uint32_t drop) {
  double drop_rate = (pub > 0) ? (100.0 * drop / pub) : 0.0;
  std::printf("  %-18s  pub=%-6u recv=%-6u drop=%-6u rate=%.1f%%\n", label,
              pub, recv, drop, drop_rate);
}

// Main ----------------------------------------------------------------------

int main() {
  OSP_LOG_INFO("priority", "Starting priority admission control demo");

  auto& bus = Bus::Instance();
  osp::Node<Payload> monitor("monitor", 100);

  monitor.Subscribe<CriticalAlert>(
      [](const CriticalAlert&, const osp::MessageHeader&) {
        g_critical_recv.fetch_add(1, std::memory_order_relaxed);
      });
  monitor.Subscribe<TelemetryData>(
      [](const TelemetryData&, const osp::MessageHeader&) {
        g_telemetry_recv.fetch_add(1, std::memory_order_relaxed);
      });
  monitor.Subscribe<DiagnosticLog>(
      [](const DiagnosticLog&, const osp::MessageHeader&) {
        g_diagnostic_recv.fetch_add(1, std::memory_order_relaxed);
      });
  monitor.Start();

  // Phase 1: Normal load -- 100 of each type, all should be delivered.
  OSP_LOG_INFO("priority", "Phase 1: Normal load (100 each)");
  for (uint32_t i = 0; i < 100; ++i) {
    bus.PublishWithPriority(Payload{CriticalAlert{i, 1}}, 1,
                           osp::MessagePriority::kHigh);
    g_critical_pub.fetch_add(1, std::memory_order_relaxed);

    bus.PublishWithPriority(
        Payload{TelemetryData{static_cast<uint16_t>(i), 1.0f * i, NowUs()}},
        1, osp::MessagePriority::kLow);
    g_telemetry_pub.fetch_add(1, std::memory_order_relaxed);

    DiagnosticLog dl{static_cast<uint16_t>(i), {}};
    std::snprintf(dl.message, sizeof(dl.message), "normal log %u", i);
    bus.PublishWithPriority(Payload{dl}, 1, osp::MessagePriority::kMedium);
    g_diagnostic_pub.fetch_add(1, std::memory_order_relaxed);
  }
  for (int b = 0; b < 20; ++b) {
    bus.ProcessBatch();
  }
  OSP_LOG_INFO("priority", "Phase 1 complete");

  // Phase 2: Heavy load -- flood with slow consumer.
  OSP_LOG_INFO("priority",
               "Phase 2: Heavy load (10k LOW, 5k MED, 1k HIGH)");
  for (uint32_t i = 0; i < 10000; ++i) {
    TelemetryData td{static_cast<uint16_t>(i & 0xFFFF), 0.1f * i, NowUs()};
    if (!bus.PublishWithPriority(Payload{td}, 1,
                                osp::MessagePriority::kLow)) {
      g_telemetry_drop.fetch_add(1, std::memory_order_relaxed);
    }
    g_telemetry_pub.fetch_add(1, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < 5000; ++i) {
    DiagnosticLog dl{static_cast<uint16_t>(i & 0xFFFF), {}};
    std::snprintf(dl.message, sizeof(dl.message), "heavy diag %u", i);
    if (!bus.PublishWithPriority(Payload{dl}, 1,
                                osp::MessagePriority::kMedium)) {
      g_diagnostic_drop.fetch_add(1, std::memory_order_relaxed);
    }
    g_diagnostic_pub.fetch_add(1, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < 1000; ++i) {
    CriticalAlert ca{i + 1000, 2};
    if (!bus.PublishWithPriority(Payload{ca}, 1,
                                osp::MessagePriority::kHigh)) {
      g_critical_drop.fetch_add(1, std::memory_order_relaxed);
    }
    g_critical_pub.fetch_add(1, std::memory_order_relaxed);
  }

  // Slow consumer: small batches with inter-batch delay.
  for (int b = 0; b < 200; ++b) {
    bus.ProcessBatch();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  // Phase 3: Drain remaining messages.
  OSP_LOG_INFO("priority", "Phase 3: Draining remaining messages");
  for (int b = 0; b < 500; ++b) {
    bus.ProcessBatch();
  }

  // Statistics --------------------------------------------------------------
  uint32_t cp = g_critical_pub.load();
  uint32_t cr = g_critical_recv.load();
  uint32_t cd = g_critical_drop.load();
  uint32_t tp = g_telemetry_pub.load();
  uint32_t tr = g_telemetry_recv.load();
  uint32_t tdr = g_telemetry_drop.load();
  uint32_t dp = g_diagnostic_pub.load();
  uint32_t dr = g_diagnostic_recv.load();
  uint32_t dd = g_diagnostic_drop.load();

  std::printf("\n--- Per-Type Statistics ---\n");
  PrintStats("CriticalAlert", cp, cr, cd);
  PrintStats("TelemetryData", tp, tr, tdr);
  PrintStats("DiagnosticLog", dp, dr, dd);

  auto stats = bus.GetStatistics();
  std::printf("\n--- Bus Statistics ---\n");
  std::printf("  published   %lu\n",
              static_cast<unsigned long>(stats.messages_published));
  std::printf("  processed   %lu\n",
              static_cast<unsigned long>(stats.messages_processed));
  std::printf("  dropped     %lu\n",
              static_cast<unsigned long>(stats.messages_dropped));

  // Verify: CriticalAlert drop rate should be near zero.
  double crit_drop_pct = (cp > 0) ? (100.0 * cd / cp) : 0.0;
  if (crit_drop_pct < 1.0) {
    OSP_LOG_INFO("priority", "PASS: CriticalAlert drop rate %.1f%% < 1%%",
                 crit_drop_pct);
  } else {
    OSP_LOG_WARN("priority", "FAIL: CriticalAlert drop rate %.1f%% >= 1%%",
                 crit_drop_pct);
  }

  monitor.Stop();
  bus.Reset();
  OSP_LOG_INFO("priority", "Demo complete");
  return 0;
}
