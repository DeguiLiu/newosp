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
#include "osp/fault_collector.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"
#include "osp/watchdog.hpp"

#include "client_sm.hpp"
#include "file_transfer.hpp"
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
// File Transfer Thread State
// ============================================================================

struct FileThreadState {
  std::thread              thread;
  alignas(alignof(FtCtx))  uint8_t ctx_buf[sizeof(FtCtx)];
  alignas(alignof(FtSm))   uint8_t sm_buf[sizeof(FtSm)];
  std::atomic<bool>        alive{false};
  std::atomic<bool>        started{false};
  bool                     initialized{false};
};

static FileThreadState g_file_threads[kMaxInstances];

static FtCtx& FtCtxOf(uint32_t i) noexcept {
  return *reinterpret_cast<FtCtx*>(g_file_threads[i].ctx_buf);
}

static FtSm& FtSmOf(uint32_t i) noexcept {
  return *reinterpret_cast<FtSm*>(g_file_threads[i].sm_buf);
}

// Simulated file data (shared across all clients)
static constexpr uint32_t kFileSize = 32768U;  // 32KB test file
static uint8_t g_file_data[kFileSize];

static void GenerateFileData() noexcept {
  for (uint32_t i = 0; i < kFileSize; ++i) {
    g_file_data[i] = static_cast<uint8_t>(i & 0xFFU);
  }
}

// ============================================================================
// Watchdog + FaultCollector (replaces hand-written WatchdogCallback)
// ============================================================================

static constexpr uint32_t kWatchdogTimeoutMs = 10000U;  // 10s no heartbeat

// Fault codes
static constexpr uint32_t kFaultThreadTimeout  = 0x02010001U;
static constexpr uint32_t kFaultHighErrorRate  = 0x02020001U;
static constexpr uint32_t kFaultFileFail       = 0x02030001U;

// Fault indices
static constexpr uint16_t kFiThreadTimeout = 0U;
static constexpr uint16_t kFiHighErrorRate = 1U;
static constexpr uint16_t kFiFileFail      = 2U;

static osp::ThreadWatchdog<kMaxInstances + 1U>  g_watchdog;  // +1 for FCCU
static osp::FaultCollector<8U, 64U>             g_faults;
static uint32_t                                 g_wd_slot_ids[kMaxInstances];
static bool                                      g_wd_registered[kMaxInstances];

// File transfer thread function (watchdog heartbeat via FtCtx)
static void FileTransferThread(uint32_t idx) noexcept {
  auto& ft = g_file_threads[idx];
  ft.alive.store(true, std::memory_order_relaxed);

  auto& ctx = FtCtxOf(idx);
  bool ok = RunFileTransfer(ctx);

  ft.alive.store(false, std::memory_order_relaxed);

  if (ok) {
    OSP_LOG_INFO("FILE", "[%u] Transfer thread completed successfully",
                 ctx.client_id);
  } else {
    OSP_LOG_ERROR("FILE", "[%u] Transfer thread failed", ctx.client_id);
    static_cast<void>(g_faults.ReportFault(
        kFiFileFail, ctx.client_id, osp::FaultPriority::kHigh));
  }
}

// Watchdog timeout -> report fault
static void OnWatchdogTimeout(uint32_t slot_id, const char* name,
                              void* /*ctx*/) {
  OSP_LOG_ERROR("WATCHDOG", "Thread '%s' (slot %u) timed out", name, slot_id);
  static_cast<void>(g_faults.ReportFault(
      kFiThreadTimeout, slot_id, osp::FaultPriority::kHigh));
}

// Watchdog recovery -> log
static void OnWatchdogRecovered(uint32_t slot_id, const char* name,
                                void* /*ctx*/) {
  OSP_LOG_INFO("WATCHDOG", "Thread '%s' (slot %u) recovered", name, slot_id);
  g_faults.ClearFault(kFiThreadTimeout);
}

// Error rate check (called by TimerScheduler)
static void ErrorRateCheckCallback(void* /*ctx*/) {
  g_watchdog.Check();

  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto& c = Ctx(i);
    if (c.connected) {
      uint32_t err = c.n_err.load(std::memory_order_relaxed);
      uint32_t sent = c.n_sent.load(std::memory_order_relaxed);
      if (sent > 10 && err > sent / 2) {
        OSP_LOG_WARN("WATCHDOG", "Client [%u] high error rate: %u/%u",
                     i, err, sent);
        static_cast<void>(g_faults.ReportFault(
            kFiHighErrorRate, c.id, osp::FaultPriority::kMedium));
      }
    }
  }
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

static int cmd_file(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== File Transfer Status ===\r\n");
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto& ft = g_file_threads[i];
    if (!ft.started.load(std::memory_order_relaxed)) {
      osp::DebugShell::Printf("  [%2u] not started\r\n", i);
      continue;
    }
    auto& fc = FtCtxOf(i);
    bool alive = ft.alive.load(std::memory_order_relaxed);
    osp::DebugShell::Printf(
        "  [%2u] state=%-12s alive=%s chunks=%u/%u retries=%u\r\n",
        i, fc.sm->CurrentStateName(),
        alive ? "yes" : "no",
        fc.chunks_ok.load(std::memory_order_relaxed),
        fc.total_chunks,
        fc.chunks_retried.load(std::memory_order_relaxed));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_file, "Show file transfer status per client");

static int cmd_watchdog(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Watchdog Status ===\r\n");
  osp::DebugShell::Printf("  Active:    %u / %u\r\n",
                           g_watchdog.ActiveCount(),
                           g_watchdog.Capacity());
  osp::DebugShell::Printf("  Timed out: %u\r\n", g_watchdog.TimedOutCount());
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (!g_file_threads[i].started.load(std::memory_order_relaxed)) continue;
    if (!g_wd_registered[i]) continue;
    bool timed_out = g_watchdog.IsTimedOut(osp::WatchdogSlotId(g_wd_slot_ids[i]));
    osp::DebugShell::Printf("  [%2u] slot=%u %s\r\n",
                             i, g_wd_slot_ids[i],
                             timed_out ? "TIMED_OUT" : "ok");
  }
  return 0;
}
OSP_SHELL_CMD(cmd_watchdog, "Show thread watchdog status");

static int cmd_faults(int /*argc*/, char* /*argv*/[]) {
  auto fs = g_faults.GetStatistics();
  osp::DebugShell::Printf("=== Fault Collector ===\r\n");
  osp::DebugShell::Printf("  Reported:  %lu\r\n",
                           static_cast<unsigned long>(fs.total_reported));
  osp::DebugShell::Printf("  Processed: %lu\r\n",
                           static_cast<unsigned long>(fs.total_processed));
  osp::DebugShell::Printf("  Dropped:   %lu\r\n",
                           static_cast<unsigned long>(fs.total_dropped));
  osp::DebugShell::Printf("  Active:    %u\r\n",
                           g_faults.ActiveFaultCount());
  osp::DebugShell::Printf("  By priority:\r\n");
  static const char* kPriNames[] = {"Critical", "High", "Medium", "Low"};
  for (uint32_t i = 0; i < 4; ++i) {
    osp::DebugShell::Printf("    %-8s: reported=%lu dropped=%lu\r\n",
                             kPriNames[i],
                             static_cast<unsigned long>(fs.priority_reported[i]),
                             static_cast<unsigned long>(fs.priority_dropped[i]));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_faults, "Show fault collector statistics");

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
  // --- Load INI config (--config path or default net_stress.ini) ---
  NetStressConfig cfg;
  const char* ini_path = FindConfigArg(argc, argv);
  LoadConfig(ini_path ? ini_path : "net_stress.ini", cfg);

  // Build positional args (skip --config pairs)
  const char* pos_args[16] = {};
  int pos_count = 0;
  for (int i = 1; i < argc && pos_count < 16; ++i) {
    if (std::strcmp(argv[i], "--config") == 0) { ++i; continue; }
    pos_args[pos_count++] = argv[i];
  }

  if (pos_count < 1) {
    std::printf("Usage: %s [--config net_stress.ini] <server_ip> "
                "[num_clients] [interval_ms] [payload_kb]\n", argv[0]);
    std::printf("  server_ip:   Server IP address\n");
    std::printf("  num_clients: Number of client instances (default %u)\n",
                cfg.client_num);
    std::printf("  interval_ms: Echo interval in ms (default %u)\n",
                cfg.client_interval_ms);
    std::printf("  payload_kb:  Payload size in KB (default %u)\n",
                cfg.client_payload_len / 1024U);
    return 1;
  }

  const char* server_ip = pos_args[0];
  g_num_clients = cfg.client_num;
  if (pos_count >= 2) g_num_clients = static_cast<uint32_t>(std::atoi(pos_args[1]));
  uint32_t interval_ms = cfg.client_interval_ms;
  if (pos_count >= 3) interval_ms = static_cast<uint32_t>(std::atoi(pos_args[2]));
  uint32_t payload_len = cfg.client_payload_len;
  if (pos_count >= 4) payload_len = static_cast<uint32_t>(std::atoi(pos_args[3])) * 1024U;

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

  // --- Setup Watchdog + FaultCollector ---
  g_faults.RegisterFault(kFiThreadTimeout, kFaultThreadTimeout);
  g_faults.RegisterFault(kFiHighErrorRate, kFaultHighErrorRate);
  g_faults.RegisterFault(kFiFileFail, kFaultFileFail);
  g_faults.SetDefaultHook([](const osp::FaultEvent& e) -> osp::HookAction {
    OSP_LOG_WARN("FAULT", "code=0x%08x detail=0x%08x count=%u",
                 e.fault_code, e.detail, e.occurrence_count);
    return osp::HookAction::kHandled;
  });
  // Wire fault collector consumer thread heartbeat to watchdog
  auto fc_reg = g_watchdog.Register("fccu_consumer", kWatchdogTimeoutMs);
  osp::ThreadHeartbeat* fc_hb = nullptr;
  if (fc_reg.has_value()) {
    fc_hb = fc_reg.value().heartbeat;
    g_faults.SetConsumerHeartbeat(fc_hb);
  }
  g_watchdog.SetOnTimeout(OnWatchdogTimeout);
  g_watchdog.SetOnRecovered(OnWatchdogRecovered);
  static_cast<void>(g_faults.Start());
  OSP_SCOPE_EXIT(g_faults.Stop();
                 if (fc_reg.has_value()) {
                   static_cast<void>(g_watchdog.Unregister(fc_reg.value().id));
                 });

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

  // --- Phase 3: Launch file transfer threads (parallel to echo) ---
  GenerateFileData();
  OSP_LOG_INFO("CLIENT", "Launching file transfer threads (%u bytes, "
               "10%% simulated loss)...", kFileSize);

  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (!Ctx(i).connected) continue;

    auto& ft = g_file_threads[i];
    auto* fc = new (ft.ctx_buf) FtCtx();  // NOLINT
    InitFtCtx(*fc);
    fc->client_id = Ctx(i).id;
    fc->server_host.assign(osp::TruncateToCapacity, server_ip);
    fc->file_data = g_file_data;
    fc->file_size = kFileSize;
    fc->drop_rate = 0.1f;  // 10% simulated loss

    // Register with watchdog and pass heartbeat to FtCtx
    char wd_name[32];
    std::snprintf(wd_name, sizeof(wd_name), "file_tx_%u", Ctx(i).id);
    auto wd_reg = g_watchdog.Register(wd_name, kWatchdogTimeoutMs);
    if (wd_reg.has_value()) {
      g_wd_slot_ids[i] = wd_reg.value().id.value();
      g_wd_registered[i] = true;
      fc->heartbeat = wd_reg.value().heartbeat;
    }

    auto* fsm = new (ft.sm_buf) FtSm(*fc);  // NOLINT
    BuildFtSm(*fsm, *fc);
    ft.initialized = true;
    ft.started.store(true, std::memory_order_relaxed);

    ft.thread = std::thread(FileTransferThread, i);
  }

  // --- Debug Shell ---
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = cfg.client_shell_port;
  osp::DebugShell shell(shell_cfg);
  auto shell_r = shell.Start();
  if (shell_r) {
    OSP_LOG_INFO("CLIENT", "Debug shell: telnet localhost %u",
                 cfg.client_shell_port);
    OSP_LOG_INFO("CLIENT", "  Commands: cmd_p, cmd_s, cmd_stop, "
                 "cmd_detail, cmd_bus, cmd_file, cmd_watchdog, "
                 "cmd_faults, cmd_quit");
  }
  OSP_SCOPE_EXIT(shell.Stop());

  // --- Timer: periodic echo tick + stats + watchdog check ---
  osp::TimerScheduler<4> timer;
  static_cast<void>(timer.Add(interval_ms, TickCallback, &node));
  static_cast<void>(timer.Add(5000U, StatsCallback, &node));
  static_cast<void>(timer.Add(3000U, ErrorRateCheckCallback));
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

  // Join file transfer threads
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    auto& ft = g_file_threads[i];
    if (ft.thread.joinable()) {
      ft.thread.join();
    }
  }

  OSP_LOG_INFO("CLIENT", "");
  OSP_LOG_INFO("CLIENT", "========== Final Results ==========");
  OSP_LOG_INFO("CLIENT", "Elapsed:   %lu ms",
               static_cast<unsigned long>(NowMs() - g_start_time_ms));
  OSP_LOG_INFO("CLIENT", "--- Echo Channel ---");
  OSP_LOG_INFO("CLIENT", "Sent:      %u", AggSent());
  OSP_LOG_INFO("CLIENT", "Received:  %u", AggRecv());
  OSP_LOG_INFO("CLIENT", "Errors:    %u", AggErr());
  uint32_t total_recv = AggRecv();
  if (total_recv > 0) {
    double avg_rtt = static_cast<double>(AggRtt()) /
                     static_cast<double>(total_recv);
    OSP_LOG_INFO("CLIENT", "Avg RTT:   %.1f us", avg_rtt);
  }

  OSP_LOG_INFO("CLIENT", "--- File Channel ---");
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (!g_file_threads[i].initialized) continue;
    auto& fc = FtCtxOf(i);
    OSP_LOG_INFO("CLIENT", "  [%u] %s: chunks=%u/%u retries=%u",
                 fc.client_id,
                 fc.success.load(std::memory_order_relaxed) ? "OK" : "FAIL",
                 fc.chunks_ok.load(std::memory_order_relaxed),
                 fc.total_chunks,
                 fc.chunks_retried.load(std::memory_order_relaxed));
  }

  auto bus_stats = StressBus::Instance().GetStatistics();
  OSP_LOG_INFO("CLIENT", "--- Bus ---");
  OSP_LOG_INFO("CLIENT", "Bus:       pub=%lu proc=%lu drop=%lu",
               static_cast<unsigned long>(bus_stats.messages_published),
               static_cast<unsigned long>(bus_stats.messages_processed),
               static_cast<unsigned long>(bus_stats.messages_dropped));

  auto fault_stats = g_faults.GetStatistics();
  OSP_LOG_INFO("CLIENT", "--- Watchdog + Faults ---");
  OSP_LOG_INFO("CLIENT", "Watchdog:  active=%u timed_out=%u",
               g_watchdog.ActiveCount(), g_watchdog.TimedOutCount());
  OSP_LOG_INFO("CLIENT", "Faults:    reported=%lu processed=%lu dropped=%lu "
               "active=%u",
               static_cast<unsigned long>(fault_stats.total_reported),
               static_cast<unsigned long>(fault_stats.total_processed),
               static_cast<unsigned long>(fault_stats.total_dropped),
               g_faults.ActiveFaultCount());

  OSP_LOG_INFO("CLIENT", "===================================");

  // Cleanup RPC clients and file transfer objects
  for (uint32_t i = 0; i < g_num_clients; ++i) {
    if (g_file_threads[i].initialized) {
      // Unregister from watchdog (only if registration succeeded)
      if (g_wd_registered[i]) {
        static_cast<void>(g_watchdog.Unregister(
            osp::WatchdogSlotId(g_wd_slot_ids[i])));
      }
      FtSmOf(i).~StateMachine();
      FtCtxOf(i).~FtCtx();
      g_file_threads[i].initialized = false;
    }
  }

  node.Stop();
  shell.Stop();
  OSP_LOG_INFO("CLIENT", "Client shutdown.");
  return 0;
}
