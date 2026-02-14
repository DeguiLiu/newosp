// Copyright (c) 2024 liudegui. MIT License.
//
// handlers.hpp -- WorkerPool handlers and gateway/monitor node setup.

#ifndef OSP_EXAMPLES_CLIENT_GATEWAY_HANDLERS_HPP_
#define OSP_EXAMPLES_CLIENT_GATEWAY_HANDLERS_HPP_

#include "messages.hpp"

#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// ============================================================================
// Gateway Statistics (stack-allocated, passed by reference)
// ============================================================================

struct GatewayStats {
  std::atomic<uint32_t> connected{0};
  std::atomic<uint32_t> disconnected{0};
  std::atomic<uint32_t> data_processed{0};
  std::atomic<uint64_t> bytes_processed{0};
  std::atomic<uint32_t> heartbeats{0};
};

// ============================================================================
// WorkerPool handler registration (lambda captures, no global singleton)
// ============================================================================

inline void RegisterPoolHandlers(osp::WorkerPool<Payload>& pool,
                                 GatewayStats& stats) {
  pool.RegisterHandler<ClientData>(
      [&stats](const ClientData& msg, const osp::MessageHeader& hdr) {
        std::this_thread::sleep_for(std::chrono::microseconds(msg.data_len));
        stats.data_processed.fetch_add(1, std::memory_order_relaxed);
        stats.bytes_processed.fetch_add(msg.data_len, std::memory_order_relaxed);
        OSP_LOG_DEBUG("proc", "processed %u B from client %u (msg %lu)",
                      msg.data_len, msg.client_id,
                      static_cast<unsigned long>(hdr.msg_id));
      });

  pool.RegisterHandler<ProcessResult>(
      [](const ProcessResult& msg, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("proc", "result: client %u status=%u bytes=%u",
                     msg.client_id, msg.status, msg.processed_bytes);
      });
}

// ============================================================================
// Node subscription setup
// ============================================================================

inline void SetupGateway(osp::Node<Payload>& gateway, GatewayStats& stats) {
  gateway.Subscribe<ClientConnect>(
      [&stats](const ClientConnect& m, const osp::MessageHeader&) {
        stats.connected.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_INFO("gw", "client %u from %s:%u", m.client_id, m.ip, m.port);
      });
  gateway.Subscribe<ClientDisconnect>(
      [&stats](const ClientDisconnect& m, const osp::MessageHeader&) {
        stats.disconnected.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_INFO("gw", "client %u left (reason=%u)",
                     m.client_id, m.reason);
      });
}

inline void SetupMonitor(osp::Node<Payload>& monitor, GatewayStats& stats) {
  monitor.Subscribe<ClientHeartbeat>(
      [&stats](const ClientHeartbeat& m, const osp::MessageHeader&) {
        stats.heartbeats.fetch_add(1, std::memory_order_relaxed);
        OSP_LOG_DEBUG("mon", "hb client %u rtt=%u us", m.client_id, m.rtt_us);
      });
}

#endif  // OSP_EXAMPLES_CLIENT_GATEWAY_HANDLERS_HPP_
