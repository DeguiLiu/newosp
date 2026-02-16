// Copyright (c) 2024 liudegui. MIT License.
//
// Client gateway demo: WorkerPool + multi-node patterns.
// Simulates a gateway, a parallel data processor, and a heartbeat monitor.
// Supports --console flag for stdin/stdout shell, otherwise TCP on port 5092.

#include "handlers.hpp"
#include "messages.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shell.hpp"
#include "osp/shutdown.hpp"
#include "osp/worker_pool.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

static constexpr uint32_t kNumClients = 4;
static constexpr uint32_t kMsgsPerClient = 8;

// ============================================================================
// Static pointers for shell command access to stack-local objects
// ============================================================================

static GatewayStats* g_stats = nullptr;
static osp::WorkerPool<Payload>* g_pool = nullptr;

// ============================================================================
// Shell commands
// ============================================================================

/// @brief Show gateway statistics (connected, disconnected, data, bytes, hb).
static int cmd_stats(int /*argc*/, char* /*argv*/[]) {
  if (g_stats == nullptr) {
    osp::DebugShell::Printf("stats not available\r\n");
    return -1;
  }
  osp::DebugShell::Printf("=== Gateway Stats ===\r\n");
  osp::DebugShell::Printf("  connected      : %u\r\n",
                           g_stats->connected.load(std::memory_order_relaxed));
  osp::DebugShell::Printf("  disconnected   : %u\r\n",
                           g_stats->disconnected.load(std::memory_order_relaxed));
  osp::DebugShell::Printf("  data_processed : %u\r\n",
                           g_stats->data_processed.load(std::memory_order_relaxed));
  osp::DebugShell::Printf("  bytes_processed: %lu\r\n",
                           static_cast<unsigned long>(
                               g_stats->bytes_processed.load(std::memory_order_relaxed)));
  osp::DebugShell::Printf("  heartbeats     : %u\r\n",
                           g_stats->heartbeats.load(std::memory_order_relaxed));
  return 0;
}
OSP_SHELL_CMD(cmd_stats, "Show gateway statistics");

/// @brief Show async bus statistics (published, dropped).
static int cmd_bus(int /*argc*/, char* /*argv*/[]) {
  auto bs = osp::AsyncBus<Payload>::Instance().GetStatistics();
  osp::DebugShell::Printf("=== Bus Stats ===\r\n");
  osp::DebugShell::Printf("  published: %lu\r\n",
                           static_cast<unsigned long>(bs.messages_published));
  osp::DebugShell::Printf("  dropped  : %lu\r\n",
                           static_cast<unsigned long>(bs.messages_dropped));
  return 0;
}
OSP_SHELL_CMD(cmd_bus, "Show bus statistics");

/// @brief Show worker pool statistics (dispatched, processed, queue_full).
static int cmd_pool(int /*argc*/, char* /*argv*/[]) {
  if (g_pool == nullptr) {
    osp::DebugShell::Printf("pool not available\r\n");
    return -1;
  }
  auto ps = g_pool->GetStats();
  osp::DebugShell::Printf("=== Worker Pool Stats ===\r\n");
  osp::DebugShell::Printf("  dispatched : %lu\r\n",
                           static_cast<unsigned long>(ps.dispatched));
  osp::DebugShell::Printf("  processed  : %lu\r\n",
                           static_cast<unsigned long>(ps.processed));
  osp::DebugShell::Printf("  queue_full : %lu\r\n",
                           static_cast<unsigned long>(ps.worker_queue_full));
  return 0;
}
OSP_SHELL_CMD(cmd_pool, "Show worker pool statistics");

// ============================================================================
// Helper: check if argv contains a flag
// ============================================================================

static bool HasFlag(int argc, char* argv[], const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

int main(int argc, char* argv[]) {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("main", "=== Client Gateway Demo ===");

  constexpr uint32_t kWorkers = 2;
  constexpr uint32_t kQueueDepth = 1024;
  OSP_LOG_INFO("main", "config: workers=%u queue=%u", kWorkers, kQueueDepth);

  osp::AsyncBus<Payload>::Instance().Reset();

  // Stack-allocated stats, passed by reference (no global singleton).
  GatewayStats stats;
  g_stats = &stats;

  // -- WorkerPool for parallel data processing --
  osp::WorkerPoolConfig pool_cfg;
  pool_cfg.name = "processor";
  pool_cfg.worker_num = kWorkers;
  osp::WorkerPool<Payload> pool(pool_cfg);
  g_pool = &pool;
  RegisterPoolHandlers(pool, stats);

  // -- Gateway and monitor nodes --
  osp::Node<Payload> gateway("gateway", 1);
  osp::Node<Payload> monitor("monitor", 3);
  SetupGateway(gateway, stats);
  SetupMonitor(monitor, stats);

  gateway.Start();
  monitor.Start();
  pool.Start();
  OSP_LOG_INFO("main", "all components started");

  // -- Start debug shell --
  const bool use_console = HasFlag(argc, argv, "--console");

  osp::ConsoleShell console_shell;
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = 5092;
  osp::DebugShell tcp_shell(tcp_cfg);

  if (use_console) {
    auto res = console_shell.Start();
    if (!res) {
      OSP_LOG_WARN("main", "console shell start failed");
    } else {
      OSP_LOG_INFO("main", "console shell started (stdin/stdout)");
    }
  } else {
    auto res = tcp_shell.Start();
    if (!res) {
      OSP_LOG_WARN("main", "tcp shell start failed on port %u", tcp_cfg.port);
    } else {
      OSP_LOG_INFO("main", "debug shell started on port %u", tcp_cfg.port);
    }
  }

  // Phase 1: Connect clients.
  OSP_LOG_INFO("main", "--- connecting %u clients ---", kNumClients);
  for (uint32_t id = 1; id <= kNumClients; ++id) {
    ClientConnect msg{};
    msg.client_id = id;
    std::snprintf(msg.ip, sizeof(msg.ip), "192.168.1.%u", 100 + id);
    msg.port = static_cast<uint16_t>(9000 + id);
    gateway.Publish(msg);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Phase 2: Data messages processed by WorkerPool in parallel.
  OSP_LOG_INFO("main", "--- sending %u data msgs/client ---", kMsgsPerClient);
  for (uint32_t r = 0; r < kMsgsPerClient; ++r) {
    for (uint32_t id = 1; id <= kNumClients; ++id) {
      ClientData msg{};
      msg.client_id = id;
      msg.data_len = static_cast<uint16_t>(64 + r * 16);
      std::memset(msg.data, static_cast<int>(id & 0xFF), msg.data_len);
      pool.Submit(msg);
    }
  }

  // Phase 3: Heartbeats.
  OSP_LOG_INFO("main", "--- heartbeat monitoring ---");
  for (uint32_t id = 1; id <= kNumClients; ++id) {
    ClientHeartbeat msg{};
    msg.client_id = id;
    msg.rtt_us = 500 + id * 100;
    gateway.Publish(msg);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Phase 4: FlushAndPause -- drain all in-flight work before disconnect.
  OSP_LOG_INFO("main", "--- flush and pause ---");
  pool.FlushAndPause();
  OSP_LOG_INFO("main", "pool flushed; emitting result summaries");

  for (uint32_t id = 1; id <= kNumClients; ++id) {
    ProcessResult msg{};
    msg.client_id = id;
    msg.status = 0;
    msg.processed_bytes = static_cast<uint32_t>(
        stats.bytes_processed.load(std::memory_order_relaxed) / kNumClients);
    gateway.Publish(msg);
  }
  pool.Resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Phase 5: Disconnect.
  OSP_LOG_INFO("main", "--- disconnecting clients ---");
  for (uint32_t id = 1; id <= kNumClients; ++id) {
    ClientDisconnect msg{};
    msg.client_id = id;
    msg.reason = 0;
    gateway.Publish(msg);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Shutdown.
  OSP_LOG_INFO("main", "--- shutting down ---");

  // Stop shell before other components.
  if (use_console) {
    console_shell.Stop();
  } else {
    tcp_shell.Stop();
  }

  pool.Shutdown();
  gateway.Stop();
  monitor.Stop();

  // Clear static pointers (objects going out of scope).
  g_stats = nullptr;
  g_pool = nullptr;

  // Statistics summary.
  auto ps = pool.GetStats();
  auto bs = osp::AsyncBus<Payload>::Instance().GetStatistics();
  OSP_LOG_INFO("main", "=== Statistics ===");
  OSP_LOG_INFO("main", "  connected=%u disconnected=%u",
               stats.connected.load(), stats.disconnected.load());
  OSP_LOG_INFO("main", "  data_processed=%u bytes=%lu heartbeats=%u",
               stats.data_processed.load(),
               static_cast<unsigned long>(stats.bytes_processed.load()),
               stats.heartbeats.load());
  OSP_LOG_INFO("main", "  bus: published=%lu dropped=%lu",
               static_cast<unsigned long>(bs.messages_published),
               static_cast<unsigned long>(bs.messages_dropped));
  OSP_LOG_INFO("main", "  pool: dispatched=%lu processed=%lu qfull=%lu",
               static_cast<unsigned long>(ps.dispatched),
               static_cast<unsigned long>(ps.processed),
               static_cast<unsigned long>(ps.worker_queue_full));
  OSP_LOG_INFO("main", "=== Demo Complete ===");

  osp::log::Shutdown();
  return 0;
}
