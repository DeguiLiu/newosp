/**
 * @file main.cpp
 * @brief Serial OTA demo -- showcasing newosp component integration.
 *
 * Architecture (loopback for demo):
 *   Host sends raw bytes -> Device parser -> DeviceHandler -> response bytes
 *   Device response bytes -> Host parser -> OtaHost::OnResponse
 *
 * newosp components used:
 *   - osp::StateMachine     -- Device OTA state machine + frame parser HSM
 *   - osp::BehaviorTree     -- Host upgrade flow (Sequence of actions)
 *   - osp::DebugShell       -- Telnet debug commands (OSP_SHELL_CMD)
 *   - osp::TimerScheduler   -- Periodic OTA tick + timeout monitoring
 *   - osp::AsyncBus         -- Message bus for OTA event notifications
 *   - osp::FixedString      -- Stack-allocated status strings
 *   - osp::FixedVector      -- Stack-allocated firmware buffer
 *   - osp::expected         -- Error handling without exceptions
 *   - osp::ScopeGuard       -- RAII cleanup for resources
 *   - osp::log              -- Structured logging
 */

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"

#include "protocol.hpp"
#include "parser.hpp"
#include "device.hpp"
#include "host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <variant>

// ============================================================================
// Configuration
// ============================================================================

static constexpr uint32_t kFirmwareSize   = 4096U;
static constexpr uint32_t kChunkSize      = 128U;
static constexpr uint32_t kFlashStartAddr = 0x0000U;
static constexpr uint16_t kShellPort      = 5090U;
static constexpr uint32_t kOtaTickMs      = 5U;     // BT tick interval
static constexpr uint32_t kTimeoutCheckMs = 500U;    // Timeout monitor interval
static constexpr uint32_t kMaxOtaTimeMs   = 30000U;  // Max OTA duration

// ============================================================================
// Bus Message Types (osp::AsyncBus)
// ============================================================================

/// OTA progress notification (published by host tick timer).
struct OtaProgressMsg {
  uint32_t bytes_sent;
  uint32_t total_size;
  osp::NodeStatus status;
};

/// OTA state change notification (published on device state transitions).
struct OtaStateChangeMsg {
  osp::FixedString<32> old_state;
  osp::FixedString<32> new_state;
};

/// OTA completion notification.
struct OtaCompleteMsg {
  bool success;
  uint16_t fw_crc;
  uint16_t flash_crc;
  uint32_t elapsed_ms;
  uint32_t tick_count;
};

using OtaPayload = std::variant<OtaProgressMsg, OtaStateChangeMsg,
                                OtaCompleteMsg>;
using OtaBus = osp::AsyncBus<OtaPayload>;

static constexpr uint32_t kBusSenderId = 1U;

// ============================================================================
// Global State (for shell command access)
// ============================================================================

static ota::FrameParser       g_host_parser;
static ota::FrameParser       g_device_parser;
static ota::DeviceHandler<>   g_device;
static ota::OtaHost*          g_host = nullptr;
static std::atomic<bool>      g_ota_running{false};
static std::atomic<uint32_t>  g_tick_count{0};
static osp::FixedString<32>   g_last_device_state{"Idle"};

// ============================================================================
// Loopback: Host -> Device parser -> DeviceHandler -> Host parser
// ============================================================================

static void HostSendToDevice(const uint8_t* data, uint32_t len,
                             void* /*ctx*/) {
  g_device_parser.PutData(data, len);
}

static void DeviceSendToHost(const uint8_t* data, uint32_t len,
                             void* /*ctx*/) {
  g_host_parser.PutData(data, len);
}

// ============================================================================
// Frame Callbacks
// ============================================================================

static void DeviceFrameCallback(const ota::Frame& frame, void* /*ctx*/) {
  // Track device state changes via bus
  osp::FixedString<32> old_state(osp::TruncateToCapacity,
                                  g_device.GetOtaStateName());

  g_device.ProcessFrame(frame);

  osp::FixedString<32> new_state(osp::TruncateToCapacity,
                                  g_device.GetOtaStateName());
  if (old_state != new_state) {
    g_last_device_state = new_state;
    OtaBus::Instance().PublishTopic(
        OtaStateChangeMsg{old_state, new_state},
        kBusSenderId, "ota.state");
  }
}

static void HostFrameCallback(const ota::Frame& frame, void* /*ctx*/) {
  if (g_host != nullptr) {
    g_host->OnResponse(frame);
  }
}

// ============================================================================
// Timer Callbacks (osp::TimerScheduler)
// ============================================================================

/// Periodic progress reporter (timer-driven, does NOT tick the BT).
static void ProgressReportCallback(void* /*ctx*/) {
  if (g_host == nullptr || !g_ota_running.load(std::memory_order_relaxed)) {
    return;
  }
  OtaBus::Instance().PublishTopic(
      OtaProgressMsg{g_host->GetBytesSent(), kFirmwareSize,
                     g_host->GetStatus()},
      kBusSenderId, "ota.progress");
}

/// Periodic timeout monitor -- checks if OTA has exceeded max duration.
static void TimeoutCheckCallback(void* ctx) {
  auto* start_time = static_cast<
      std::chrono::steady_clock::time_point*>(ctx);
  if (!g_ota_running.load(std::memory_order_relaxed)) return;

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - *start_time);

  if (static_cast<uint32_t>(elapsed.count()) > kMaxOtaTimeMs) {
    OSP_LOG_ERROR("OTA_MAIN", "OTA timeout after %u ms",
                  static_cast<uint32_t>(elapsed.count()));
    g_ota_running.store(false, std::memory_order_relaxed);
  }
}

// ============================================================================
// Bus Subscriber -- logs OTA events
// ============================================================================

static void SetupBusSubscribers() {
  // Subscribe to state changes
  OtaBus::Instance().Subscribe<OtaStateChangeMsg>(
      [](const osp::MessageEnvelope<OtaPayload>& env) {
        const auto& msg = std::get<OtaStateChangeMsg>(env.payload);
        OSP_LOG_INFO("OTA_BUS", "State: %s -> %s",
                     msg.old_state.c_str(), msg.new_state.c_str());
      });

  // Subscribe to completion
  OtaBus::Instance().Subscribe<OtaCompleteMsg>(
      [](const osp::MessageEnvelope<OtaPayload>& env) {
        const auto& msg = std::get<OtaCompleteMsg>(env.payload);
        if (msg.success) {
          OSP_LOG_INFO("OTA_BUS", "OTA OK: %u ms, CRC=0x%04X",
                       msg.elapsed_ms, msg.fw_crc);
        } else {
          OSP_LOG_ERROR("OTA_BUS", "OTA FAILED: %u ms", msg.elapsed_ms);
        }
      });
}

// ============================================================================
// Shell Commands (osp::DebugShell)
// ============================================================================

static int cmd_ota_status(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== OTA Status ===\r\n");
  osp::DebugShell::Printf("Device SM:  %s\r\n", g_device.GetOtaStateName());

  const auto& dctx = g_device.GetContext();
  osp::DebugShell::Printf("  total:    %u bytes\r\n", dctx.total_size);
  osp::DebugShell::Printf("  addr:     0x%X\r\n", dctx.start_addr);
  osp::DebugShell::Printf("  received: %u bytes\r\n", dctx.received_size);
  osp::DebugShell::Printf("  exp_crc:  0x%04X\r\n", dctx.expected_crc);
  osp::DebugShell::Printf("  calc_crc: 0x%04X\r\n", dctx.calc_crc);

  if (g_host != nullptr) {
    osp::DebugShell::Printf("\r\nHost BT:    %s\r\n",
                             osp::NodeStatusToString(g_host->GetStatus()));
    osp::DebugShell::Printf("  progress: %.1f%%\r\n",
                             static_cast<double>(g_host->GetProgress()) * 100.0);
    osp::DebugShell::Printf("  sent:     %u bytes\r\n", g_host->GetBytesSent());
    osp::DebugShell::Printf("  ticks:    %u\r\n",
                             g_tick_count.load(std::memory_order_relaxed));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_ota_status, "Show OTA state machine and transfer progress");

static int cmd_serial_stats(int /*argc*/, char* /*argv*/[]) {
  const auto& hs = g_host_parser.GetStats();
  const auto& ds = g_device_parser.GetStats();

  osp::DebugShell::Printf("=== Host Parser ===\r\n");
  osp::DebugShell::Printf("  state:    %s\r\n", g_host_parser.CurrentStateName());
  osp::DebugShell::Printf("  bytes_rx: %u\r\n", hs.bytes_received);
  osp::DebugShell::Printf("  frames:   %u\r\n", hs.frames_received);
  osp::DebugShell::Printf("  sync_err: %u  crc_err: %u  tail_err: %u\r\n",
                           hs.sync_errors, hs.crc_errors, hs.tail_errors);

  osp::DebugShell::Printf("\r\n=== Device Parser ===\r\n");
  osp::DebugShell::Printf("  state:    %s\r\n", g_device_parser.CurrentStateName());
  osp::DebugShell::Printf("  bytes_rx: %u\r\n", ds.bytes_received);
  osp::DebugShell::Printf("  frames:   %u\r\n", ds.frames_received);
  osp::DebugShell::Printf("  sync_err: %u  crc_err: %u  tail_err: %u\r\n",
                           ds.sync_errors, ds.crc_errors, ds.tail_errors);
  return 0;
}
OSP_SHELL_CMD(cmd_serial_stats, "Show host/device serial parser statistics");

static int cmd_bus_stats(int /*argc*/, char* /*argv*/[]) {
  auto stats = OtaBus::Instance().GetStatistics();
  osp::DebugShell::Printf("=== Bus Statistics ===\r\n");
  osp::DebugShell::Printf("  published: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_published));
  osp::DebugShell::Printf("  processed: %lu\r\n",
                           static_cast<unsigned long>(stats.messages_processed));
  osp::DebugShell::Printf("  dropped:   %lu\r\n",
                           static_cast<unsigned long>(stats.messages_dropped));
  osp::DebugShell::Printf("  depth:     %u / %u (%u%%)\r\n",
                           OtaBus::Instance().Depth(),
                           OtaBus::kQueueDepth,
                           OtaBus::Instance().UtilizationPercent());
  return 0;
}
OSP_SHELL_CMD(cmd_bus_stats, "Show message bus statistics");

static int cmd_flash_dump(int argc, char* argv[]) {
  if (argc < 2) {
    osp::DebugShell::Printf("Usage: cmd_flash_dump <addr> [len]\r\n");
    return -1;
  }
  uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0));
  uint32_t len  = (argc >= 3)
                      ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0))
                      : 64U;

  const auto& flash = g_device.GetFlash();
  if (addr + len > flash.Size()) {
    osp::DebugShell::Printf("Error: out of range (flash=%u)\r\n", flash.Size());
    return -1;
  }

  uint8_t buf[256];
  uint32_t chunk = (len > sizeof(buf)) ? static_cast<uint32_t>(sizeof(buf)) : len;
  if (!const_cast<ota::FlashSim<>&>(flash).Read(addr, buf, chunk)) {
    osp::DebugShell::Printf("Error: read failed\r\n");
    return -1;
  }

  for (uint32_t i = 0; i < chunk; ++i) {
    if (i % 16U == 0) {
      osp::DebugShell::Printf("  %08X: ", addr + i);
    }
    osp::DebugShell::Printf("%02X ", buf[i]);
    if ((i + 1U) % 16U == 0 || i + 1U == chunk) {
      osp::DebugShell::Printf("\r\n");
    }
  }
  return 0;
}
OSP_SHELL_CMD(cmd_flash_dump, "Hex dump flash: cmd_flash_dump <addr> [len]");

static int cmd_flash_crc(int argc, char* argv[]) {
  if (argc < 3) {
    osp::DebugShell::Printf("Usage: cmd_flash_crc <addr> <len>\r\n");
    return -1;
  }
  uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0));
  uint32_t len  = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));

  uint16_t crc = g_device.GetFlash().CalcCrc(addr, len);
  osp::DebugShell::Printf("CRC16(0x%X, %u) = 0x%04X\r\n", addr, len, crc);
  return 0;
}
OSP_SHELL_CMD(cmd_flash_crc, "Calc flash CRC: cmd_flash_crc <addr> <len>");

static int cmd_timer_info(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Timer Info ===\r\n");
  osp::DebugShell::Printf("  OTA tick interval:  %u ms\r\n", kOtaTickMs);
  osp::DebugShell::Printf("  Timeout check:      %u ms\r\n", kTimeoutCheckMs);
  osp::DebugShell::Printf("  Max OTA duration:   %u ms\r\n", kMaxOtaTimeMs);
  osp::DebugShell::Printf("  OTA running:        %s\r\n",
                           g_ota_running.load() ? "yes" : "no");
  return 0;
}
OSP_SHELL_CMD(cmd_timer_info, "Show timer scheduler configuration");

// ============================================================================
// Main
// ============================================================================

int main() {
  OSP_LOG_INFO("OTA_MAIN", "=== Serial OTA Demo ===");
  OSP_LOG_INFO("OTA_MAIN", "Components: StateMachine + BehaviorTree + "
               "DebugShell + TimerScheduler + AsyncBus + vocabulary");
  OSP_LOG_INFO("OTA_MAIN", "Firmware: %u bytes, chunk: %u, addr: 0x%X",
               kFirmwareSize, kChunkSize, kFlashStartAddr);

  // --- Generate fake firmware using FixedVector ------------------------------
  osp::FixedVector<uint8_t, kFirmwareSize> firmware;
  for (uint32_t i = 0; i < kFirmwareSize; ++i) {
    firmware.push_back(static_cast<uint8_t>(i & 0xFFU));
  }
  OSP_LOG_INFO("OTA_MAIN", "Firmware generated: %u bytes (FixedVector cap=%u)",
               firmware.size(), firmware.capacity());

  // --- Setup bus subscribers -------------------------------------------------
  OtaBus::Instance().Reset();
  SetupBusSubscribers();

  // --- Setup parsers (HSM-based) ---------------------------------------------
  g_device_parser.SetCallback(DeviceFrameCallback);
  g_device_parser.Start();

  g_host_parser.SetCallback(HostFrameCallback);
  g_host_parser.Start();

  // --- Setup device (StateMachine-based) -------------------------------------
  g_device.SetResponseCallback(DeviceSendToHost, nullptr);

  // --- Setup host (BehaviorTree-based) ---------------------------------------
  ota::OtaHost host(firmware.data(), firmware.size(),
                    kFlashStartAddr, kChunkSize);
  host.SetSendCallback(HostSendToDevice, nullptr);
  g_host = &host;

  // RAII cleanup for g_host pointer
  OSP_SCOPE_EXIT(g_host = nullptr);

  // --- Start shell (osp::DebugShell) -----------------------------------------
  osp::DebugShell::Config shell_cfg;
  shell_cfg.port = kShellPort;
  osp::DebugShell shell(shell_cfg);

  auto shell_result = shell.Start();
  if (shell_result) {
    OSP_LOG_INFO("OTA_MAIN", "Debug shell: telnet localhost %u", kShellPort);
    OSP_LOG_INFO("OTA_MAIN", "  Commands: cmd_ota_status, cmd_serial_stats, "
                 "cmd_bus_stats, cmd_flash_dump, cmd_flash_crc, cmd_timer_info");
  }

  // --- Start OTA upgrade (BehaviorTree) --------------------------------------
  OSP_LOG_INFO("OTA_MAIN", "Starting OTA upgrade...");
  if (!host.Start()) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to start OTA");
    return 1;
  }

  g_ota_running.store(true, std::memory_order_relaxed);
  auto ota_start_time = std::chrono::steady_clock::now();

  // --- Setup timer scheduler (osp::TimerScheduler) ---------------------------
  // Timer handles progress reporting + timeout monitoring (background thread).
  // BT ticking is done in the main loop for deterministic loopback execution.
  osp::TimerScheduler<4> timer;

  auto progress_result = timer.Add(kOtaTickMs * 50U, ProgressReportCallback);
  if (!progress_result) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to add progress timer");
    return 1;
  }
  OSP_LOG_INFO("OTA_MAIN", "Timer: progress report every %u ms",
               kOtaTickMs * 50U);

  auto timeout_result = timer.Add(kTimeoutCheckMs, TimeoutCheckCallback,
                                   &ota_start_time);
  if (!timeout_result) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to add timeout timer");
    return 1;
  }
  OSP_LOG_INFO("OTA_MAIN", "Timer: timeout check every %u ms (max %u ms)",
               kTimeoutCheckMs, kMaxOtaTimeMs);

  auto timer_start = timer.Start();
  if (!timer_start) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to start timer scheduler");
    return 1;
  }

  // RAII cleanup for timer
  OSP_SCOPE_EXIT(timer.Stop());

  // --- Main loop: tick BT + process bus messages -----------------------------
  OSP_LOG_INFO("OTA_MAIN", "OTA running...");
  constexpr uint32_t kMaxTicks = 50000U;

  while (g_ota_running.load(std::memory_order_relaxed) &&
         g_tick_count.load(std::memory_order_relaxed) < kMaxTicks) {
    auto status = host.Tick();
    g_tick_count.fetch_add(1, std::memory_order_relaxed);

    // Process bus messages (single consumer)
    OtaBus::Instance().ProcessBatch();

    if (status == osp::NodeStatus::kSuccess ||
        status == osp::NodeStatus::kFailure) {
      g_ota_running.store(false, std::memory_order_relaxed);
    }
  }

  // Stop timer before reading final state
  timer.Stop();

  // Drain remaining bus messages
  OtaBus::Instance().ProcessBatch();

  // --- Results ---------------------------------------------------------------
  auto ota_end_time = std::chrono::steady_clock::now();
  uint32_t elapsed_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          ota_end_time - ota_start_time).count());
  uint32_t ticks = g_tick_count.load(std::memory_order_relaxed);

  OSP_LOG_INFO("OTA_MAIN", "");
  OSP_LOG_INFO("OTA_MAIN", "========== Results ==========");
  OSP_LOG_INFO("OTA_MAIN", "Elapsed:      %u ms", elapsed_ms);
  OSP_LOG_INFO("OTA_MAIN", "BT ticks:     %u", ticks);
  OSP_LOG_INFO("OTA_MAIN", "Host status:  %s",
               osp::NodeStatusToString(host.GetStatus()));
  OSP_LOG_INFO("OTA_MAIN", "Bytes sent:   %u / %u",
               host.GetBytesSent(), kFirmwareSize);
  OSP_LOG_INFO("OTA_MAIN", "Device state: %s", g_device.GetOtaStateName());

  const auto& hs = g_host_parser.GetStats();
  const auto& ds = g_device_parser.GetStats();
  OSP_LOG_INFO("OTA_MAIN", "Host parser:  %u frames, %u bytes",
               hs.frames_received, hs.bytes_received);
  OSP_LOG_INFO("OTA_MAIN", "Dev parser:   %u frames, %u bytes",
               ds.frames_received, ds.bytes_received);

  // Bus statistics
  auto bus_stats = OtaBus::Instance().GetStatistics();
  OSP_LOG_INFO("OTA_MAIN", "Bus msgs:     pub=%lu proc=%lu drop=%lu",
               static_cast<unsigned long>(bus_stats.messages_published),
               static_cast<unsigned long>(bus_stats.messages_processed),
               static_cast<unsigned long>(bus_stats.messages_dropped));

  // CRC verification
  bool ota_success = host.IsComplete();
  uint16_t fw_crc = 0;
  uint16_t flash_crc = 0;

  if (ota_success) {
    fw_crc = ota::CalcCrc16(firmware.data(), firmware.size());
    flash_crc = g_device.GetFlash().CalcCrc(kFlashStartAddr, kFirmwareSize);
    ota_success = (fw_crc == flash_crc);
    OSP_LOG_INFO("OTA_MAIN", "FW CRC=0x%04X  Flash CRC=0x%04X  %s",
                 fw_crc, flash_crc,
                 ota_success ? "MATCH" : "MISMATCH");
    OSP_LOG_INFO("OTA_MAIN", "OTA upgrade completed successfully!");
  } else if (host.IsFailed()) {
    OSP_LOG_ERROR("OTA_MAIN", "OTA upgrade FAILED");
  } else {
    OSP_LOG_WARN("OTA_MAIN", "OTA timed out after %u ms", elapsed_ms);
  }

  // Publish completion event via bus
  OtaBus::Instance().PublishTopic(
      OtaCompleteMsg{ota_success, fw_crc, flash_crc, elapsed_ms, ticks},
      kBusSenderId, "ota.complete");
  OtaBus::Instance().ProcessBatch();

  OSP_LOG_INFO("OTA_MAIN", "=============================");

  // --- Shell interaction (keep alive briefly) --------------------------------
  if (shell_result) {
    OSP_LOG_INFO("OTA_MAIN", "Shell on port %u -- waiting 3s for inspection",
                 kShellPort);
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  shell.Stop();
  OSP_LOG_INFO("OTA_MAIN", "Demo finished.");
  return 0;
}
