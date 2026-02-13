/**
 * @file monitor.cpp
 * @brief Net stress monitor -- standalone DebugShell for remote inspection.
 *
 * Connects to the server's echo service periodically to measure latency,
 * and provides a DebugShell for interactive monitoring.
 *
 * newosp components used:
 *   - osp::Service/Client  -- probe echo latency
 *   - osp::DebugShell      -- telnet monitoring interface
 *   - osp::TimerScheduler  -- periodic probe + display
 *   - osp::AsyncBus/Node   -- local event aggregation
 *   - osp::FixedVector     -- stack-allocated latency history
 *   - osp::FixedString     -- stack-allocated strings
 *   - osp::expected        -- error handling
 *   - osp::ScopeGuard      -- RAII cleanup
 *   - osp::log             -- structured logging
 */

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/service.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"
#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace net_stress;

// ============================================================================
// Probe State
// ============================================================================

static constexpr uint32_t kHistorySize = 64U;
static constexpr uint32_t kProbePayload = 64U;

struct ProbeStats {
  osp::FixedVector<uint64_t, kHistorySize> rtt_history;
  std::atomic<uint32_t> probe_sent{0};
  std::atomic<uint32_t> probe_ok{0};
  std::atomic<uint32_t> probe_fail{0};
  uint64_t min_rtt_us{UINT64_MAX};
  uint64_t max_rtt_us{0};
  uint64_t sum_rtt_us{0};
};

static ProbeStats             g_probe;
static osp::FixedString<63>  g_server_host(osp::TruncateToCapacity, "127.0.0.1");
static uint16_t               g_echo_port = kEchoPort;
static std::atomic<bool>      g_running{true};
static uint64_t               g_start_ms = 0;

// Bus for local events
struct ProbeResult {
  uint32_t seq;
  uint64_t rtt_us;
  bool     ok;
};
using MonBusPayload = std::variant<ProbeResult, StatsSnapshot>;
using MonBus  = osp::AsyncBus<MonBusPayload>;
using MonNode = osp::Node<MonBusPayload>;

// ============================================================================
// Probe Timer Callback
// ============================================================================

static void ProbeCallback(void* ctx) {
  auto* node = static_cast<MonNode*>(ctx);

  auto cli_r = osp::Client<EchoReq, EchoResp>::Connect(
      g_server_host.c_str(), g_echo_port, 2000);

  ProbeResult pr{};
  pr.seq = g_probe.probe_sent.fetch_add(1, std::memory_order_relaxed);

  if (!cli_r.has_value()) {
    pr.ok = false;
    g_probe.probe_fail.fetch_add(1, std::memory_order_relaxed);
    node->Publish(pr);
    node->SpinOnce();
    return;
  }

  auto& cli = cli_r.value();

  EchoReq req{};
  req.client_id = 0xFFFFU;  // monitor ID
  req.seq = pr.seq;
  req.payload_len = kProbePayload;
  req.send_ts_ns = NowNs();
  FillPattern(req.payload, kProbePayload, pr.seq);

  auto resp = cli.Call(req, 2000);
  if (resp.has_value() && resp.value().seq == pr.seq) {
    uint64_t rtt = (NowNs() - resp.value().client_ts_ns) / 1000ULL;
    pr.rtt_us = rtt;
    pr.ok = true;
    g_probe.probe_ok.fetch_add(1, std::memory_order_relaxed);

    // Update stats
    g_probe.sum_rtt_us += rtt;
    if (rtt < g_probe.min_rtt_us) g_probe.min_rtt_us = rtt;
    if (rtt > g_probe.max_rtt_us) g_probe.max_rtt_us = rtt;
    if (g_probe.rtt_history.size() < g_probe.rtt_history.capacity()) {
      g_probe.rtt_history.push_back(rtt);
    }
  } else {
    pr.ok = false;
    g_probe.probe_fail.fetch_add(1, std::memory_order_relaxed);
  }

  node->Publish(pr);
  node->SpinOnce();
}

// ============================================================================
// Shell Commands
// ============================================================================

static int cmd_probe(int /*argc*/, char* /*argv*/[]) {
  uint32_t sent = g_probe.probe_sent.load(std::memory_order_relaxed);
  uint32_t ok   = g_probe.probe_ok.load(std::memory_order_relaxed);
  uint32_t fail = g_probe.probe_fail.load(std::memory_order_relaxed);

  osp::DebugShell::Printf("=== Probe Statistics ===\r\n");
  osp::DebugShell::Printf("  Target:  %s:%u\r\n",
                           g_server_host.c_str(), g_echo_port);
  osp::DebugShell::Printf("  Sent:    %u\r\n", sent);
  osp::DebugShell::Printf("  OK:      %u\r\n", ok);
  osp::DebugShell::Printf("  Failed:  %u\r\n", fail);

  if (ok > 0) {
    double avg = static_cast<double>(g_probe.sum_rtt_us) /
                 static_cast<double>(ok);
    osp::DebugShell::Printf("  Min RTT: %lu us\r\n",
                             static_cast<unsigned long>(g_probe.min_rtt_us));
    osp::DebugShell::Printf("  Max RTT: %lu us\r\n",
                             static_cast<unsigned long>(g_probe.max_rtt_us));
    osp::DebugShell::Printf("  Avg RTT: %.1f us\r\n", avg);
  }

  if (sent > 0) {
    double loss = static_cast<double>(fail) * 100.0 /
                  static_cast<double>(sent);
    osp::DebugShell::Printf("  Loss:    %.1f%%\r\n", loss);
  }
  return 0;
}
OSP_SHELL_CMD(cmd_probe, "Show probe latency statistics");

static int cmd_history(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== RTT History (last %u) ===\r\n",
                           g_probe.rtt_history.size());
  for (uint32_t i = 0; i < g_probe.rtt_history.size(); ++i) {
    osp::DebugShell::Printf("  [%2u] %lu us\r\n", i,
                             static_cast<unsigned long>(g_probe.rtt_history[i]));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_history, "Show RTT history");

static int cmd_bus_stats(int /*argc*/, char* /*argv*/[]) {
  auto stats = MonBus::Instance().GetStatistics();
  osp::DebugShell::Printf("=== Bus Statistics ===\r\n");
  osp::DebugShell::Printf("  Published: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_published));
  osp::DebugShell::Printf("  Processed: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_processed));
  osp::DebugShell::Printf("  Dropped:   %lu\r\n",
                           static_cast<unsigned long>(stats.messages_dropped));
  return 0;
}
OSP_SHELL_CMD(cmd_bus_stats, "Show monitor bus statistics");

static int cmd_quit(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("Shutting down...\r\n");
  g_running.store(false, std::memory_order_relaxed);
  return 0;
}
OSP_SHELL_CMD(cmd_quit, "Quit monitor");

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::printf("Usage: %s <server_ip> [echo_port] [probe_interval_ms]\n",
                argv[0]);
    return 1;
  }

  g_server_host.assign(osp::TruncateToCapacity, argv[1]);
  if (argc >= 3) g_echo_port = static_cast<uint16_t>(std::atoi(argv[2]));
  uint32_t probe_interval = 2000U;
  if (argc >= 4) probe_interval = static_cast<uint32_t>(std::atoi(argv[3]));

  OSP_LOG_INFO("MONITOR", "=== Net Stress Monitor ===");
  OSP_LOG_INFO("MONITOR", "Target: %s:%u, Probe interval: %u ms",
               g_server_host.c_str(), g_echo_port, probe_interval);

  g_start_ms = NowMs();

  // --- Bus + Node ---
  MonBus::Instance().Reset();
  MonNode node("monitor", 1);
  node.Start();

  // Subscribe to probe results
  node.Subscribe<ProbeResult>(
      [](const ProbeResult& pr, const osp::MessageHeader& /*hdr*/) {
        if (pr.ok) {
          OSP_LOG_INFO("PROBE", "seq=%u rtt=%lu us", pr.seq,
                       static_cast<unsigned long>(pr.rtt_us));
        } else {
          OSP_LOG_WARN("PROBE", "seq=%u FAILED", pr.seq);
        }
      });

  // --- Debug Shell ---
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = kMonitorShellPort;
  osp::DebugShell shell(shell_cfg);
  auto shell_r = shell.Start();
  if (shell_r) {
    OSP_LOG_INFO("MONITOR", "Debug shell: telnet localhost %u",
                 kMonitorShellPort);
    OSP_LOG_INFO("MONITOR", "  Commands: cmd_probe, cmd_history, "
                 "cmd_bus_stats, cmd_quit");
  }
  OSP_SCOPE_EXIT(shell.Stop());

  // --- Timer: periodic probe ---
  osp::TimerScheduler<2> timer;
  auto probe_r = timer.Add(probe_interval, ProbeCallback, &node);
  if (!probe_r) {
    OSP_LOG_ERROR("MONITOR", "Failed to add probe timer");
    return 1;
  }
  timer.Start();
  OSP_SCOPE_EXIT(timer.Stop());

  OSP_LOG_INFO("MONITOR", "Monitoring. Press 'q' + Enter to quit.");

  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  timer.Stop();

  // Final report
  uint32_t ok = g_probe.probe_ok.load(std::memory_order_relaxed);
  OSP_LOG_INFO("MONITOR", "");
  OSP_LOG_INFO("MONITOR", "========== Final Report ==========");
  OSP_LOG_INFO("MONITOR", "Probes: %u sent, %u ok, %u failed",
               g_probe.probe_sent.load(std::memory_order_relaxed),
               ok, g_probe.probe_fail.load(std::memory_order_relaxed));
  if (ok > 0) {
    double avg = static_cast<double>(g_probe.sum_rtt_us) /
                 static_cast<double>(ok);
    OSP_LOG_INFO("MONITOR", "RTT: min=%lu avg=%.1f max=%lu us",
                 static_cast<unsigned long>(g_probe.min_rtt_us), avg,
                 static_cast<unsigned long>(g_probe.max_rtt_us));
  }
  OSP_LOG_INFO("MONITOR", "==================================");

  node.Stop();
  shell.Stop();
  return 0;
}
