/**
 * @file server.cpp
 * @brief Net stress test server -- RPC echo service with debug shell.
 *
 * Architecture:
 *   - Handshake service (port 20000): accepts client registration
 *   - Echo service (port 20001): echoes payload back with server timestamp
 *   - DebugShell (port 9600): telnet-based runtime inspection
 *   - TimerScheduler: periodic statistics reporting
 *
 * Original OSP demo equivalents:
 *   OspCreateTcpNode(0, 20000)       -> Service<HandshakeReq, HandshakeResp>
 *   TEST_REQ_EVENT / TEST_EVENT_ACK  -> Handshake RPC call
 *   COMM_TEST_EVENT echo             -> Echo RPC call
 *   p() debug command                -> DebugShell cmd_stats
 *
 * newosp components used:
 *   - osp::Service          -- RPC server (handshake + echo)
 *   - osp::DebugShell       -- telnet debug interface
 *   - osp::TimerScheduler   -- periodic stats reporting
 *   - osp::FixedString      -- stack-allocated strings
 *   - osp::FixedVector      -- stack-allocated client slot table
 *   - osp::expected         -- error handling
 *   - osp::ScopeGuard       -- RAII cleanup
 *   - osp::log              -- structured logging
 */

#include "osp/log.hpp"
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
#include <mutex>
#include <thread>

// ============================================================================
// Server State (no classes, plain data + free functions)
// ============================================================================

struct ClientSlot {
  uint32_t client_id;
  char     name[32];
  bool     active;
  uint64_t connect_time_ms;
};

static std::mutex              g_slots_mutex;
static ClientSlot              g_slots[net_stress::kMaxClients];
static std::atomic<uint32_t>   g_active_count{0};
static std::atomic<uint32_t>   g_total_echo{0};
static std::atomic<uint32_t>   g_total_handshake{0};
static std::atomic<uint64_t>   g_total_bytes{0};
static std::atomic<uint32_t>   g_total_file_chunks{0};
static std::atomic<uint64_t>   g_total_file_bytes{0};
static std::atomic<bool>       g_running{true};
static uint64_t                g_start_time_ms{0};

static void InitSlots() noexcept {
  for (uint32_t i = 0; i < net_stress::kMaxClients; ++i) {
    g_slots[i].active = false;
    g_slots[i].client_id = 0;
    g_slots[i].name[0] = '\0';
    g_slots[i].connect_time_ms = 0;
  }
}

static uint32_t AllocSlot(uint32_t client_id, const char* name) noexcept {
  std::lock_guard<std::mutex> lock(g_slots_mutex);
  for (uint32_t i = 0; i < net_stress::kMaxClients; ++i) {
    if (!g_slots[i].active) {
      g_slots[i].active = true;
      g_slots[i].client_id = client_id;
      std::strncpy(g_slots[i].name, name, sizeof(g_slots[i].name) - 1);
      g_slots[i].name[sizeof(g_slots[i].name) - 1] = '\0';
      g_slots[i].connect_time_ms = net_stress::NowMs();
      g_active_count.fetch_add(1, std::memory_order_relaxed);
      return i;
    }
  }
  return UINT32_MAX;  // full
}

// ============================================================================
// RPC Handlers (free functions with void* context)
// ============================================================================

static net_stress::HandshakeResp HandleHandshake(
    const net_stress::HandshakeReq& req, void* /*ctx*/) {
  net_stress::HandshakeResp resp{};
  resp.server_id = net_stress::kServerId;
  std::strncpy(resp.server_name, "net_stress_server",
               sizeof(resp.server_name) - 1);

  if (req.version != net_stress::kProtocolVersion) {
    resp.accepted = 0;
    OSP_LOG_WARN("SERVER", "Rejected client %u: bad version %u",
                 req.client_id, req.version);
    return resp;
  }

  uint32_t slot = AllocSlot(req.client_id, req.name);
  if (slot == UINT32_MAX) {
    resp.accepted = 0;
    OSP_LOG_WARN("SERVER", "Rejected client %u: slots full", req.client_id);
    return resp;
  }

  resp.slot = slot;
  resp.echo_port = net_stress::kEchoPort;
  resp.accepted = 1;
  g_total_handshake.fetch_add(1, std::memory_order_relaxed);

  OSP_LOG_INFO("SERVER", "Client %u (%s) -> slot %u",
               req.client_id, req.name, slot);
  return resp;
}

static net_stress::EchoResp HandleEcho(
    const net_stress::EchoReq& req, void* /*ctx*/) {
  net_stress::EchoResp resp{};
  resp.server_id = net_stress::kServerId;
  resp.seq = req.seq;
  resp.payload_len = req.payload_len;
  resp.client_ts_ns = req.send_ts_ns;
  resp.server_ts_ns = net_stress::NowNs();

  // Echo payload back
  uint32_t copy_len = (req.payload_len > net_stress::kMaxPayloadBytes)
                          ? net_stress::kMaxPayloadBytes : req.payload_len;
  std::memcpy(resp.payload, req.payload, copy_len);

  g_total_echo.fetch_add(1, std::memory_order_relaxed);
  g_total_bytes.fetch_add(copy_len, std::memory_order_relaxed);
  return resp;
}

static net_stress::FileTransferResp HandleFileTransfer(
    const net_stress::FileTransferReq& req, void* /*ctx*/) {
  net_stress::FileTransferResp resp{};
  resp.server_id = net_stress::kServerId;
  resp.chunk_seq = req.chunk_seq;

  uint32_t chunk_len = (req.chunk_len > net_stress::kMaxPayloadBytes)
                           ? net_stress::kMaxPayloadBytes : req.chunk_len;

  // Simulate receiving file data (verify pattern)
  bool valid = net_stress::VerifyPattern(req.data, chunk_len, req.chunk_seq);
  if (!valid) {
    OSP_LOG_WARN("FILE", "Chunk %u from client %u: pattern mismatch",
                 req.chunk_seq, req.client_id);
    resp.accepted = 0;
    return resp;
  }

  g_total_file_chunks.fetch_add(1, std::memory_order_relaxed);
  g_total_file_bytes.fetch_add(chunk_len, std::memory_order_relaxed);

  resp.received_bytes = chunk_len;
  resp.accepted = 1;
  resp.complete = (req.chunk_seq + 1 >= req.total_chunks) ? 1 : 0;

  if (resp.complete != 0) {
    OSP_LOG_INFO("FILE", "File from client %u complete: %u chunks, %u bytes",
                 req.client_id, req.total_chunks, req.file_size);
  }
  return resp;
}

// ============================================================================
// Timer Callback: periodic stats log
// ============================================================================

static void StatsTimerCb(void* /*ctx*/) {
  uint64_t elapsed = net_stress::NowMs() - g_start_time_ms;
  uint32_t echo = g_total_echo.load(std::memory_order_relaxed);
  uint64_t bytes = g_total_bytes.load(std::memory_order_relaxed);
  uint32_t active = g_active_count.load(std::memory_order_relaxed);
  uint32_t fchunks = g_total_file_chunks.load(std::memory_order_relaxed);
  uint64_t fbytes = g_total_file_bytes.load(std::memory_order_relaxed);

  double throughput_kbps = (elapsed > 0)
      ? static_cast<double>(bytes + fbytes) * 8.0 /
        static_cast<double>(elapsed)
      : 0.0;

  OSP_LOG_INFO("STATS", "elapsed=%lums clients=%u echo=%u "
               "file_chunks=%u file_bytes=%lu throughput=%.1f kbps",
               static_cast<unsigned long>(elapsed), active, echo,
               fchunks, static_cast<unsigned long>(fbytes), throughput_kbps);
}

// ============================================================================
// Shell Commands
// ============================================================================

static int cmd_stats(int /*argc*/, char* /*argv*/[]) {
  uint64_t elapsed = net_stress::NowMs() - g_start_time_ms;
  uint32_t echo = g_total_echo.load(std::memory_order_relaxed);
  uint64_t bytes = g_total_bytes.load(std::memory_order_relaxed);
  uint32_t hs = g_total_handshake.load(std::memory_order_relaxed);
  uint32_t active = g_active_count.load(std::memory_order_relaxed);
  uint32_t fchunks = g_total_file_chunks.load(std::memory_order_relaxed);
  uint64_t fbytes = g_total_file_bytes.load(std::memory_order_relaxed);

  osp::DebugShell::Printf("=== Server Statistics ===\r\n");
  osp::DebugShell::Printf("  Uptime:       %lu ms\r\n",
                           static_cast<unsigned long>(elapsed));
  osp::DebugShell::Printf("  Clients:      %u / %u\r\n",
                           active, net_stress::kMaxClients);
  osp::DebugShell::Printf("  Handshakes:   %u\r\n", hs);
  osp::DebugShell::Printf("  Echo msgs:    %u\r\n", echo);
  osp::DebugShell::Printf("  Echo bytes:   %lu\r\n",
                           static_cast<unsigned long>(bytes));
  osp::DebugShell::Printf("  File chunks:  %u\r\n", fchunks);
  osp::DebugShell::Printf("  File bytes:   %lu\r\n",
                           static_cast<unsigned long>(fbytes));

  if (elapsed > 0) {
    double kbps = static_cast<double>(bytes + fbytes) * 8.0 /
                  static_cast<double>(elapsed);
    osp::DebugShell::Printf("  Throughput:   %.1f kbps\r\n", kbps);
  }
  return 0;
}
OSP_SHELL_CMD(cmd_stats, "Show server statistics");

static int cmd_clients(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Connected Clients ===\r\n");
  std::lock_guard<std::mutex> lock(g_slots_mutex);
  uint32_t count = 0;
  for (uint32_t i = 0; i < net_stress::kMaxClients; ++i) {
    if (g_slots[i].active) {
      uint64_t age = net_stress::NowMs() - g_slots[i].connect_time_ms;
      osp::DebugShell::Printf("  [%2u] id=%u name=%-16s age=%lu ms\r\n",
                               i, g_slots[i].client_id, g_slots[i].name,
                               static_cast<unsigned long>(age));
      ++count;
    }
  }
  osp::DebugShell::Printf("  Total: %u\r\n", count);
  return 0;
}
OSP_SHELL_CMD(cmd_clients, "List connected clients");

static int cmd_quit(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("Shutting down...\r\n");
  g_running.store(false, std::memory_order_relaxed);
  return 0;
}
OSP_SHELL_CMD(cmd_quit, "Shutdown server");

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  // --- Load INI config (--config path or default net_stress.ini) ---
  net_stress::NetStressConfig cfg;
  const char* ini_path = net_stress::FindConfigArg(argc, argv);
  net_stress::LoadConfig(ini_path ? ini_path : "net_stress.ini", cfg);

  uint16_t hs_port    = cfg.server_hs_port;
  uint16_t echo_port  = cfg.server_echo_port;
  uint16_t file_port  = cfg.server_file_port;
  uint16_t shell_port = cfg.server_shell_port;

  // Command-line positional overrides (skip --config pairs)
  {
    int pos = 1;
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--config") == 0) { ++i; continue; }
      if (pos == 1) hs_port = static_cast<uint16_t>(std::atoi(argv[i]));
      else if (pos == 2) echo_port = static_cast<uint16_t>(std::atoi(argv[i]));
      else if (pos == 3) file_port = static_cast<uint16_t>(std::atoi(argv[i]));
      ++pos;
    }
  }

  OSP_LOG_INFO("SERVER", "=== Net Stress Server ===");
  OSP_LOG_INFO("SERVER", "Handshake: %u, Echo: %u, File: %u, Shell: %u",
               hs_port, echo_port, file_port, shell_port);

  InitSlots();
  g_start_time_ms = net_stress::NowMs();

  // --- Handshake Service ---
  osp::Service<net_stress::HandshakeReq, net_stress::HandshakeResp> hs_svc(
      {hs_port, 8, 4});
  hs_svc.SetHandler(HandleHandshake);
  auto hs_r = hs_svc.Start();
  if (!hs_r.has_value()) {
    OSP_LOG_ERROR("SERVER", "Failed to start handshake service on port %u",
                  hs_port);
    return 1;
  }
  OSP_LOG_INFO("SERVER", "Handshake service started on port %u", hs_port);
  OSP_SCOPE_EXIT(hs_svc.Stop());

  // --- Echo Service ---
  osp::Service<net_stress::EchoReq, net_stress::EchoResp> echo_svc(
      {echo_port, 8, net_stress::kMaxClients});
  echo_svc.SetHandler(HandleEcho);
  auto echo_r = echo_svc.Start();
  if (!echo_r.has_value()) {
    OSP_LOG_ERROR("SERVER", "Failed to start echo service on port %u",
                  echo_port);
    return 1;
  }
  OSP_LOG_INFO("SERVER", "Echo service started on port %u", echo_port);
  OSP_SCOPE_EXIT(echo_svc.Stop());

  // --- File Transfer Service ---
  osp::Service<net_stress::FileTransferReq, net_stress::FileTransferResp>
      file_svc({file_port, 8, net_stress::kMaxClients});
  file_svc.SetHandler(HandleFileTransfer);
  auto file_r = file_svc.Start();
  if (!file_r.has_value()) {
    OSP_LOG_ERROR("SERVER", "Failed to start file service on port %u",
                  file_port);
    return 1;
  }
  OSP_LOG_INFO("SERVER", "File service started on port %u", file_port);
  OSP_SCOPE_EXIT(file_svc.Stop());

  // --- Debug Shell ---
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = shell_port;
  osp::DebugShell shell(shell_cfg);
  auto shell_r = shell.Start();
  if (shell_r) {
    OSP_LOG_INFO("SERVER", "Debug shell: telnet localhost %u", shell_port);
    OSP_LOG_INFO("SERVER", "  Commands: cmd_stats, cmd_clients, cmd_quit");
  }
  OSP_SCOPE_EXIT(shell.Stop());

  // --- Timer: periodic stats ---
  osp::TimerScheduler<2> timer;
  auto timer_r = timer.Add(5000U, StatsTimerCb);
  if (timer_r) {
    timer.Start();
  }
  OSP_SCOPE_EXIT(timer.Stop());

  OSP_LOG_INFO("SERVER", "Server running. Press 'q' + Enter to quit.");

  // --- Main loop ---
  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  OSP_LOG_INFO("SERVER", "Server shutdown.");
  return 0;
}
