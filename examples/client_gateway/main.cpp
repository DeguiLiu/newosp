// Copyright (c) 2024 liudegui. MIT License.
//
// Client gateway demo: WorkerPool + multi-node patterns.
// Simulates a gateway, a parallel data processor, and a heartbeat monitor.

#include "handlers.hpp"
#include "messages.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shutdown.hpp"
#include "osp/worker_pool.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

static constexpr uint32_t kNumClients = 4;
static constexpr uint32_t kMsgsPerClient = 8;

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("main", "=== Client Gateway Demo ===");

  constexpr uint32_t kWorkers = 2;
  constexpr uint32_t kQueueDepth = 1024;
  OSP_LOG_INFO("main", "config: workers=%u queue=%u", kWorkers, kQueueDepth);

  osp::AsyncBus<Payload>::Instance().Reset();

  // Stack-allocated stats, passed by reference (no global singleton).
  GatewayStats stats;

  // -- WorkerPool for parallel data processing --
  osp::WorkerPoolConfig pool_cfg;
  pool_cfg.name = "processor";
  pool_cfg.worker_num = kWorkers;
  osp::WorkerPool<Payload> pool(pool_cfg);
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
  pool.Shutdown();
  gateway.Stop();
  monitor.Stop();

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
