// Copyright (c) 2024 liudegui. MIT License.
//
// client_demo.cpp -- Client management demo: WorkerPool + multi-node patterns.
// Simulates a gateway, a parallel data processor, and a heartbeat monitor.

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/shutdown.hpp"
#include "osp/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <variant>

// ============================================================================
// Message Definitions
// ============================================================================

struct ClientConnect {
  uint32_t client_id;
  char ip[16];
  uint16_t port;
};

struct ClientDisconnect {
  uint32_t client_id;
  uint8_t reason;
};

struct ClientData {
  uint32_t client_id;
  uint16_t data_len;
  uint8_t data[512];
};

struct ClientHeartbeat {
  uint32_t client_id;
  uint32_t rtt_us;
};

struct ProcessResult {
  uint32_t client_id;
  uint8_t status;
  uint32_t processed_bytes;
};

using Payload = std::variant<ClientConnect, ClientDisconnect, ClientData,
                             ClientHeartbeat, ProcessResult>;

// ============================================================================
// Global Counters
// ============================================================================

static std::atomic<uint32_t> g_connected{0};
static std::atomic<uint32_t> g_disconnected{0};
static std::atomic<uint32_t> g_data_processed{0};
static std::atomic<uint64_t> g_bytes_processed{0};
static std::atomic<uint32_t> g_heartbeats{0};

// ============================================================================
// WorkerPool Handlers (free functions, -fno-rtti compatible)
// ============================================================================

static void HandleClientData(const ClientData& msg,
                             const osp::MessageHeader& hdr) {
  std::this_thread::sleep_for(std::chrono::microseconds(msg.data_len));
  g_data_processed.fetch_add(1, std::memory_order_relaxed);
  g_bytes_processed.fetch_add(msg.data_len, std::memory_order_relaxed);
  OSP_LOG_DEBUG("proc", "processed %u B from client %u (msg %lu)",
                msg.data_len, msg.client_id,
                static_cast<unsigned long>(hdr.msg_id));
}

static void HandleProcessResult(const ProcessResult& msg,
                                const osp::MessageHeader& /*hdr*/) {
  OSP_LOG_INFO("proc", "result: client %u status=%u bytes=%u",
               msg.client_id, msg.status, msg.processed_bytes);
}

static constexpr uint32_t kNumClients = 4;
static constexpr uint32_t kMsgsPerClient = 8;

// ============================================================================
// Main
// ============================================================================

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("main", "=== Client Management Demo ===");

  // WorkerPoolConfig: 2 workers, 1024 queue depth (simulated defaults).
  constexpr uint32_t kWorkers = 2;
  constexpr uint32_t kQueueDepth = 1024;
  OSP_LOG_INFO("main", "config: workers=%u queue=%u", kWorkers, kQueueDepth);

  osp::AsyncBus<Payload>::Instance().Reset();

  // -- WorkerPool for parallel data processing --
  osp::WorkerPoolConfig pool_cfg;
  pool_cfg.name = "processor";
  pool_cfg.worker_num = kWorkers;
  pool_cfg.worker_queue_depth = kQueueDepth;
  osp::WorkerPool<Payload> pool(pool_cfg);
  pool.RegisterHandler<ClientData>(&HandleClientData);
  pool.RegisterHandler<ProcessResult>(&HandleProcessResult);

  // -- Gateway node: connect/disconnect --
  osp::Node<Payload> gateway("gateway", 1);
  gateway.Subscribe<ClientConnect>(
      [](const ClientConnect& m, const osp::MessageHeader&) {
        g_connected.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_INFO("gw", "client %u from %s:%u", m.client_id, m.ip, m.port);
      });
  gateway.Subscribe<ClientDisconnect>(
      [](const ClientDisconnect& m, const osp::MessageHeader&) {
        g_disconnected.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_INFO("gw", "client %u left (reason=%u)", m.client_id, m.reason);
      });

  // -- Monitor node: heartbeat tracking --
  osp::Node<Payload> monitor("monitor", 3);
  monitor.Subscribe<ClientHeartbeat>(
      [](const ClientHeartbeat& m, const osp::MessageHeader&) {
        g_heartbeats.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_DEBUG("mon", "hb client %u rtt=%u us", m.client_id, m.rtt_us);
      });

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
        g_bytes_processed.load(std::memory_order_relaxed) / kNumClients);
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
               g_connected.load(), g_disconnected.load());
  OSP_LOG_INFO("main", "  data_processed=%u bytes=%lu heartbeats=%u",
               g_data_processed.load(),
               static_cast<unsigned long>(g_bytes_processed.load()),
               g_heartbeats.load());
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
