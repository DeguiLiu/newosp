/**
 * @file client.cpp
 * @brief Net stress test client -- multi-instance RPC echo with HSM + Bus.
 *
 * Architecture:
 *   - N client instances, each with its own HSM state machine
 *   - Handshake RPC to register with server
 *   - Echo RPC for periodic payload round-trip
 *   - AsyncBus for local event pub/sub (stats, connection events)
 *   - Node for typed pub/sub integration
 *   - TimerScheduler for periodic echo ticks + stats reporting
 *   - DebugShell for telnet-based runtime inspection
 *
 * Original OSP demo equivalents:
 *   CMyInstance (N instances)         -> ClientCtx[] + HSM per instance
 *   DaemonInstanceEntry (timer tick)  -> TimerScheduler callback
 *   OspConnectTcpNode                 -> osp::Client<>::Connect
 *   OspPost(COMM_TEST_EVENT)          -> echo_cli->Call()
 *   OspNodeDiscCBReg                  -> HSM disconnect handling
 *   p() / s() commands               -> DebugShell commands
 *   g_dwTotalSndMsgCount/RcvMsgCount -> atomic counters + Bus StatsSnapshot
 *
 * newosp components used:
 *   - osp::StateMachine    -- per-client connection HSM
 *   - osp::AsyncBus        -- local event pub/sub
 *   - osp::Node            -- typed pub/sub wrapper
 *   - osp::Service/Client  -- RPC (handshake + echo)
 *   - osp::TimerScheduler  -- periodic tick + stats
 *   - osp::DebugShell      -- telnet debug
 *   - osp::FixedString     -- stack-allocated strings
 *   - osp::FixedVector     -- stack-allocated client array
 *   - osp::FixedFunction   -- type-erased callbacks (bus subscribers)
 *   - osp::expected        -- error handling
 *   - osp::ScopeGuard      -- RAII cleanup
 *   - osp::log             -- structured logging
 */

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"

#include "client_sm.hpp"
#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

using namespace net_stress;

// ============================================================================
// Type Aliases
// ============================================================================

using StressBus  = osp::AsyncBus<BusPayload>;
using StressNode = osp::Node<BusPayload>;

// ============================================================================
// Per-Instance Storage
// ============================================================================

static constexpr uint32_t kMaxInstances = 64U;

// ClientCtx holds atomics -> not default-constructible in array.
// StateMachine requires Context& in ctor -> cannot default-construct.
// Use aligned storage + placement new.
struct alignas(64) ClientSlot {
  alignas(alignof(ClientCtx)) uint8_t ctx_buf[sizeof(ClientCtx)];
  alignas(alignof(ClientSm))  uint8_t sm_buf[sizeof(ClientSm)];
  bool initialized;
};

static ClientSlot g_slots[kMaxInstances];
static uint32_t   g_num_clients = 1;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_test_running{false};
static uint64_t   g_start_time_ms = 0;

static ClientCtx& Ctx(uint32_t i) noexcept {
  return *reinterpret_cast<ClientCtx*>(g_slots[i].ctx_buf);
}

static ClientSm& Sm(uint32_t i) noexcept {
  return *reinterpret_cast<ClientSm*>(g_slots[i].sm_buf);
}

// ============================================================================
// Aggregated Statistics
// ============================================================================

static uint32_t AggSent() noexcept {
  uint32_t s = 0;
  for (uint32_t i = 0; i < g_num_clients; ++i)
    s += Ctx(i).n_sent.load(std::memory_order_relaxed);
  return s;
}

static uint32_t AggRecv() noexcept {
  uint32_t s = 0;
  for (uint32_t i = 0; i < g_num_clients; ++i)
    s += Ctx(i).n_recv.load(std::memory_order_relaxed);
  return s;
}

static uint32_t AggErr() noexcept {
  uint32_t s = 0;
  for (uint32_t i = 0; i < g_num_clients; ++i)
    s += Ctx(i).n_err.load(std::memory_order_relaxed);
  return s;
}

static uint64_t AggRtt() noexcept {
  uint64_t s = 0;
  for (uint32_t i = 0; i < g_num_clients; ++i)
    s += Ctx(i).sum_rtt_us.load(std::memory_order_relaxed);
  return s;
}

static uint32_t AggConnected() noexcept {
  uint32_t s = 0;
  for (uint32_t i = 0; i < g_num_clients; ++i)
    if (Ctx(i).connected) ++s;
  return s;
}

// ============================================================================
// Bus Subscribers
// ============================================================================

static void SetupBusSubscribers(StressNode& node) {
  static_cast<void>(node.Subscribe<PeerEvent>(
      [](const PeerEvent& ev, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("BUS", "Peer %u %s (slot=%u)",
                     ev.client_id,
                     ev.connected ? "connected" : "disconnected",
                     ev.slot);
      }));

  static_cast<void>(node.Subscribe<StatsSnapshot>(
      [](const StatsSnapshot& ss, const osp::MessageHeader& /*hdr*/) {
        double avg_rtt = (ss.rtt_samples > 0)
            ? static_cast<double>(ss.total_rtt_us) /
              static_cast<double>(ss.rtt_samples)
            : 0.0;
        OSP_LOG_INFO("BUS", "Stats: sent=%u recv=%u err=%u "
                     "clients=%u avg_rtt=%.1f us",
                     ss.total_sent, ss.total_recv, ss.total_errors,
                     ss.active_clients, avg_rtt);
      }));

  static_cast<void>(node.Subscribe<EchoResult>(
      [](const EchoResult& er, const osp::MessageHeader& /*hdr*/) {
        if (!er.success) {
          OSP_LOG_WARN("BUS", "Echo failed: client=%u seq=%u",
                       er.client_id, er.seq);
        }
      }));
}

// ============================================================================
// Timer Callbacks
// ============================================================================

/// Daemon tick: trigger echo on all running clients.
static void TickCallback(void* ctx) {
  auto* node = static_cast<StressNode*>(ctx);
  if (!g_test_running.load(std::memory_order_relaxed)) return;

  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (Ctx(i).connected) {
      Dispatch(Ctx(i), kEvtTick);
    }
  }

  // Process bus messages
  node->SpinOnce();
}

/// Stats reporting timer.
static void StatsCallback(void* ctx) {
  auto* node = static_cast<StressNode*>(ctx);

  StatsSnapshot ss{};
  ss.active_clients = AggConnected();
  ss.total_sent = AggSent();
  ss.total_recv = AggRecv();
  ss.total_errors = AggErr();
  ss.total_rtt_us = AggRtt();
  ss.rtt_samples = ss.total_recv;
  ss.elapsed_ms = NowMs() - g_start_time_ms;

  node->Publish(ss);
  node->SpinOnce();
}

// ============================================================================
// Shell Commands
// ============================================================================

static int cmd_p(int /*argc*/, char* /*argv*/[]) {
  uint32_t sent = AggSent();
  uint32_t recv = AggRecv();
  uint32_t err  = AggErr();
  uint64_t rtt  = AggRtt();
  uint32_t conn = AggConnected();
  uint64_t elapsed = NowMs() - g_start_time_ms;

  osp::DebugShell::Printf("=== Client Statistics ===\r\n");
  osp::DebugShell::Printf("  Elapsed:    %lu ms\r\n",
                           static_cast<unsigned long>(elapsed));
  osp::DebugShell::Printf("  Clients:    %u / %u connected\r\n",
                           conn, g_num_clients);
  osp::DebugShell::Printf("  Sent:       %u\r\n", sent);
  osp::DebugShell::Printf("  Received:   %u\r\n", recv);
  osp::DebugShell::Printf("  Errors:     %u\r\n", err);
  if (recv > 0) {
    double avg = static_cast<double>(rtt) / static_cast<double>(recv);
    osp::DebugShell::Printf("  Avg RTT:    %.1f us\r\n", avg);
  }
  osp::DebugShell::Printf("  Test:       %s\r\n",
                           g_test_running.load() ? "RUNNING" : "STOPPED");
  return 0;
}
OSP_SHELL_CMD(cmd_p, "Print aggregated statistics (like original 'p')");

static int cmd_s(int /*argc*/, char* /*argv*/[]) {
  if (g_test_running.load(std::memory_order_relaxed)) {
    osp::DebugShell::Printf("Test already running\r\n");
    return 0;
  }
  g_test_running.store(true, std::memory_order_relaxed);
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (Ctx(i).connected) {
      Dispatch(Ctx(i), kEvtStartTest);
    }
  }
  osp::DebugShell::Printf("Test started for %u clients\r\n", g_num_clients);
  return 0;
}
OSP_SHELL_CMD(cmd_s, "Start stress test (like original 's')");

static int cmd_stop(int /*argc*/, char* /*argv*/[]) {
  g_test_running.store(false, std::memory_order_relaxed);
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    Dispatch(Ctx(i), kEvtStopTest);
  }
  osp::DebugShell::Printf("Test stopped\r\n");
  return 0;
}
OSP_SHELL_CMD(cmd_stop, "Stop stress test");

static int cmd_detail(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Per-Client Detail ===\r\n");
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto& c = Ctx(i);
    osp::DebugShell::Printf(
        "  [%2u] id=%u state=%-13s sent=%u recv=%u err=%u\r\n",
        i, c.id, c.sm->CurrentStateName(),
        c.n_sent.load(std::memory_order_relaxed),
        c.n_recv.load(std::memory_order_relaxed),
        c.n_err.load(std::memory_order_relaxed));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_detail, "Show per-client HSM state and counters");

static int cmd_bus(int /*argc*/, char* /*argv*/[]) {
  auto stats = StressBus::Instance().GetStatistics();
  osp::DebugShell::Printf("=== Bus Statistics ===\r\n");
  osp::DebugShell::Printf("  Published: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_published));
  osp::DebugShell::Printf("  Processed: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_processed));
  osp::DebugShell::Printf("  Dropped:   %lu\r\n",
                           static_cast<unsigned long>(stats.messages_dropped));
  osp::DebugShell::Printf("  Depth:     %u / %u\r\n",
                           StressBus::Instance().Depth(),
                           StressBus::kQueueDepth);
  return 0;
}
OSP_SHELL_CMD(cmd_bus, "Show AsyncBus statistics");

static int cmd_quit(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("Shutting down...\r\n");
  g_running.store(false, std::memory_order_relaxed);
  return 0;
}
OSP_SHELL_CMD(cmd_quit, "Quit client");

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::printf("Usage: %s <server_ip> [num_clients] [interval_ms] "
                "[payload_kb]\n", argv[0]);
    std::printf("  server_ip:   Server IP address\n");
    std::printf("  num_clients: Number of client instances (default 4)\n");
    std::printf("  interval_ms: Echo interval in ms (default 1000)\n");
    std::printf("  payload_kb:  Payload size in KB (default 1)\n");
    return 1;
  }

  const char* server_ip = argv[1];
  if (argc >= 3) g_num_clients = static_cast<uint32_t>(std::atoi(argv[2]));
  uint32_t interval_ms = kDefaultIntervalMs;
  if (argc >= 4) interval_ms = static_cast<uint32_t>(std::atoi(argv[3]));
  uint32_t payload_len = kDefaultPayloadLen;
  if (argc >= 5) payload_len = static_cast<uint32_t>(std::atoi(argv[4])) * 1024U;

  if (g_num_clients > kMaxInstances) g_num_clients = kMaxInstances;
  if (payload_len > kMaxPayloadBytes) payload_len = kMaxPayloadBytes;

  OSP_LOG_INFO("CLIENT", "=== Net Stress Client ===");
  OSP_LOG_INFO("CLIENT", "Server: %s, Clients: %u, Interval: %u ms, "
               "Payload: %u bytes", server_ip, g_num_clients,
               interval_ms, payload_len);

  g_start_time_ms = NowMs();

  // --- Setup Bus + Node ---
  StressBus::Instance().Reset();
  StressNode node("stress_client", 1);
  static_cast<void>(node.Start());
  SetupBusSubscribers(node);

  // --- Initialize all client instances (placement new) ---
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto* ctx = new (g_slots[i].ctx_buf) ClientCtx();  // NOLINT
    InitClientCtx(*ctx);
    ctx->id = i + 1;
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "client_%u", ctx->id);
    ctx->name.assign(osp::TruncateToCapacity, name_buf);
    ctx->server_host.assign(osp::TruncateToCapacity, server_ip);
    ctx->interval_ms = interval_ms;
    ctx->payload_len = payload_len;

    auto* sm = new (g_slots[i].sm_buf) ClientSm(*ctx);  // NOLINT
    BuildClientSm(*sm, *ctx);
    g_slots[i].initialized = true;
  }

  // RAII cleanup for placement-new objects
  OSP_SCOPE_EXIT(
    for (uint32_t i = 0; i < g_num_clients; ++i) {
      if (g_slots[i].initialized) {
        CleanupClient(Ctx(i));
        Sm(i).~StateMachine();
        Ctx(i).~ClientCtx();
        g_slots[i].initialized = false;
      }
    }
  );

  // --- Phase 1: Connect all clients ---
  OSP_LOG_INFO("CLIENT", "Connecting %u clients...", g_num_clients);
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto& c = Ctx(i);
    Dispatch(c, kEvtConnect);  // Disconnected -> Connecting

    if (DoHandshake(c)) {
      Dispatch(c, kEvtHandshakeOk);  // Connecting -> Idle
      PeerEvent ev{};
      ev.client_id = c.id;
      ev.slot = c.slot;
      ev.connected = 1;
      node.Publish(ev);
    } else {
      Dispatch(c, kEvtError);  // Connecting -> Error
    }
  }
  node.SpinOnce();

  uint32_t connected = AggConnected();
  OSP_LOG_INFO("CLIENT", "Connected: %u / %u", connected, g_num_clients);

  if (connected == 0) {
    OSP_LOG_ERROR("CLIENT", "No clients connected, exiting");
    return 1;
  }

  // --- Phase 2: Start test ---
  OSP_LOG_INFO("CLIENT", "Starting stress test...");
  g_test_running.store(true, std::memory_order_relaxed);
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (Ctx(i).connected) {
      Dispatch(Ctx(i), kEvtStartTest);  // Idle -> Running
    }
  }

  StartTestCmd start_cmd{};
  start_cmd.interval_ms = interval_ms;
  start_cmd.payload_len = payload_len;
  node.Publish(start_cmd);
  node.SpinOnce();

  // --- Debug Shell ---
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = kClientShellPort;
  osp::DebugShell shell(shell_cfg);
  auto shell_r = shell.Start();
  if (shell_r) {
    OSP_LOG_INFO("CLIENT", "Debug shell: telnet localhost %u",
                 kClientShellPort);
    OSP_LOG_INFO("CLIENT", "  Commands: cmd_p, cmd_s, cmd_stop, "
                 "cmd_detail, cmd_bus, cmd_quit");
  }
  OSP_SCOPE_EXIT(shell.Stop());

  // --- Timer: periodic echo tick + stats ---
  osp::TimerScheduler<4> timer;
  static_cast<void>(timer.Add(interval_ms, TickCallback, &node));
  static_cast<void>(timer.Add(5000U, StatsCallback, &node));
  static_cast<void>(timer.Start());
  OSP_SCOPE_EXIT(timer.Stop());

  OSP_LOG_INFO("CLIENT", "Running. Press 'q' + Enter to quit.");

  // --- Main loop ---
  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // --- Final stats ---
  timer.Stop();
  g_test_running.store(false, std::memory_order_relaxed);

  OSP_LOG_INFO("CLIENT", "");
  OSP_LOG_INFO("CLIENT", "========== Final Results ==========");
  OSP_LOG_INFO("CLIENT", "Elapsed:   %lu ms",
               static_cast<unsigned long>(NowMs() - g_start_time_ms));
  OSP_LOG_INFO("CLIENT", "Sent:      %u", AggSent());
  OSP_LOG_INFO("CLIENT", "Received:  %u", AggRecv());
  OSP_LOG_INFO("CLIENT", "Errors:    %u", AggErr());
  uint32_t total_recv = AggRecv();
  if (total_recv > 0) {
    double avg_rtt = static_cast<double>(AggRtt()) /
                     static_cast<double>(total_recv);
    OSP_LOG_INFO("CLIENT", "Avg RTT:   %.1f us", avg_rtt);
  }

  auto bus_stats = StressBus::Instance().GetStatistics();
  OSP_LOG_INFO("CLIENT", "Bus:       pub=%lu proc=%lu drop=%lu",
               static_cast<unsigned long>(bus_stats.messages_published),
               static_cast<unsigned long>(bus_stats.messages_processed),
               static_cast<unsigned long>(bus_stats.messages_dropped));
  OSP_LOG_INFO("CLIENT", "===================================");

  node.Stop();
  shell.Stop();
  OSP_LOG_INFO("CLIENT", "Client shutdown.");
  return 0;
}
