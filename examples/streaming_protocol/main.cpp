// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// Streaming protocol demo: GB28181/RTSP-style pipeline simulation.
//
// Server-side nodes use StaticNode (compile-time handler dispatch).
// Client node uses regular Node (dynamic callback + timer context).
// Shell support: --console for stdin/stdout, default TCP on port 5093.

#include "handlers.hpp"
#include "messages.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

// ============================================================================
// Static pointers for shell command access
// ============================================================================

static ProtocolState* g_state = nullptr;
static osp::AsyncBus<Payload>* g_bus = nullptr;

// ============================================================================
// Shell commands
// ============================================================================

static int cmd_proto_stats(int /*argc*/, char* /*argv*/[]) {
  if (g_state == nullptr) {
    osp::DebugShell::Printf("  protocol state not available\r\n");
    return -1;
  }
  osp::DebugShell::Printf("  registered : %u\r\n", g_state->registered_count);
  osp::DebugShell::Printf("  heartbeats : %u\r\n", g_state->heartbeat_count);
  osp::DebugShell::Printf("  streams    : %u\r\n", g_state->stream_count);
  osp::DebugShell::Printf("  errors     : %u\r\n", g_state->error_count);
  return 0;
}
OSP_SHELL_CMD(cmd_proto_stats, "Show protocol statistics");

static int cmd_bus(int /*argc*/, char* /*argv*/[]) {
  if (g_bus == nullptr) {
    osp::DebugShell::Printf("  bus not available\r\n");
    return -1;
  }
  auto stats = g_bus->GetStatistics();
  osp::DebugShell::Printf("  published  : %lu\r\n",
                           static_cast<unsigned long>(stats.messages_published));
  osp::DebugShell::Printf("  processed  : %lu\r\n",
                           static_cast<unsigned long>(stats.messages_processed));
  osp::DebugShell::Printf("  dropped    : %lu\r\n",
                           static_cast<unsigned long>(stats.messages_dropped));
  return 0;
}
OSP_SHELL_CMD(cmd_bus, "Show bus statistics");

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("Proto", "=== streaming protocol demo start ===");

  // -- Parse CLI: --console selects ConsoleShell, else DebugShell -----------
  bool use_console = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0) {
      use_console = true;
    }
  }

  // -- Shell setup ----------------------------------------------------------
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = 5093;
  osp::DebugShell tcp_shell(tcp_cfg);

  osp::ConsoleShell::Config con_cfg;
  osp::ConsoleShell console_shell(con_cfg);

  if (use_console) {
    auto res = console_shell.Start();
    if (!res) {
      OSP_LOG_WARN("Proto", "console shell start failed");
    } else {
      OSP_LOG_INFO("Proto", "console shell started (stdin/stdout)");
    }
  } else {
    auto res = tcp_shell.Start();
    if (!res) {
      OSP_LOG_WARN("Proto", "debug shell start failed on port %u",
                   static_cast<unsigned>(tcp_cfg.port));
    } else {
      OSP_LOG_INFO("Proto", "debug shell started on port %u",
                   static_cast<unsigned>(tcp_cfg.port));
    }
  }

  auto& bus = osp::AsyncBus<Payload>::Instance();
  bus.Reset();

  ProtocolState state;

  // Expose to shell commands
  g_state = &state;
  g_bus = &bus;

  // -- Server-side: StaticNode (direct dispatch, zero overhead, inlinable) --

  RegistrarNode registrar(
      "registrar", kRegistrarId,
      RegistrarHandler{&state, &bus});

  HeartbeatNode heartbeat_monitor(
      "heartbeat_monitor", kHeartbeatId,
      HeartbeatHandler{&state});

  StreamCtrlNode stream_controller(
      "stream_controller", kStreamCtrlId,
      StreamHandler{&state});

  // -- Client-side: regular Node (dynamic callback, timer ctx pointer) ------

  osp::Node<Payload> client("client", kClientId);
  SetupClient(client);
  client.Start();

  // Step 1: device registration (HIGH priority)
  OSP_LOG_INFO("Proto", "--- step 1: device registration ---");
  RegisterRequest req{};
  std::strncpy(req.device_id, "CAM-310200001", sizeof(req.device_id) - 1);
  std::strncpy(req.ip, "192.168.1.100", sizeof(req.ip) - 1);
  req.port = 5060;
  client.PublishWithPriority(req, osp::MessagePriority::kHigh);
  registrar.SpinOnce();
  registrar.SpinOnce();

  // Step 2: periodic heartbeats via TimerScheduler
  OSP_LOG_INFO("Proto", "--- step 2: heartbeat monitoring ---");
  osp::TimerScheduler<4> timer;
  timer.Add(50, HeartbeatTimerCb, &client);
  timer.Start();

  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    heartbeat_monitor.SpinOnce();
  }
  timer.Stop();

  // Step 3: stream start / data / stop
  OSP_LOG_INFO("Proto", "--- step 3: stream control ---");
  StreamCommand start_cmd{};
  start_cmd.session_id = kDemoSessionId;
  start_cmd.action = 1;
  start_cmd.media_type = 2;
  client.PublishWithPriority(start_cmd, osp::MessagePriority::kHigh);
  stream_controller.SpinOnce();

  for (uint32_t seq = 0; seq < 5; ++seq) {
    StreamData sd{};
    sd.session_id = kDemoSessionId;
    sd.seq = seq;
    sd.payload_size = 128;
    std::memset(sd.data, static_cast<int>(seq & 0xFF), sd.payload_size);
    client.PublishWithPriority(sd, osp::MessagePriority::kLow);
  }
  stream_controller.SpinOnce();

  StreamCommand stop_cmd{};
  stop_cmd.session_id = kDemoSessionId;
  stop_cmd.action = 0;
  stop_cmd.media_type = 2;
  client.PublishWithPriority(stop_cmd, osp::MessagePriority::kHigh);
  stream_controller.SpinOnce();

  // Statistics
  auto bus_stats = bus.GetStatistics();
  OSP_LOG_INFO("Proto", "=== statistics ===");
  OSP_LOG_INFO("Proto", "bus: published=%lu processed=%lu dropped=%lu",
               static_cast<unsigned long>(bus_stats.messages_published),
               static_cast<unsigned long>(bus_stats.messages_processed),
               static_cast<unsigned long>(bus_stats.messages_dropped));
  OSP_LOG_INFO("Proto", "app: registered=%u heartbeats=%u streams=%u errors=%u",
               state.registered_count, state.heartbeat_count,
               state.stream_count, state.error_count);
  OSP_LOG_INFO("Proto", "=== streaming protocol demo end ===");

  // -- Shell teardown -------------------------------------------------------
  g_state = nullptr;
  g_bus = nullptr;

  if (use_console) {
    console_shell.Stop();
  } else {
    tcp_shell.Stop();
  }

  osp::log::Shutdown();
  return 0;
}
