// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// Streaming protocol demo: GB28181/RTSP-style pipeline simulation.
//
// Uses ospgen-generated message types and topology constants:
//   - protocol_messages.hpp: POD structs + Validate() + Dump() + event binding
//   - topology.hpp: node IDs, names, subscription counts
//
// Server-side nodes use StaticNode (compile-time handler dispatch).
// Client node uses regular Node (dynamic callback + timer context).
// Shell support: --console for stdin/stdout, default TCP on port 5093.

#include "handlers.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shell.hpp"
#include "osp/timer.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

// ============================================================================
// Compile-time event<->message binding verification (ospgen v2)
// ============================================================================

static_assert(protocol::EventIdOf<protocol::RegisterRequest>() ==
                  protocol::kProtocolRegister,
              "RegisterRequest must bind to REGISTER event");
static_assert(protocol::EventIdOf<protocol::HeartbeatMsg>() ==
                  protocol::kProtocolHeartbeat,
              "HeartbeatMsg must bind to HEARTBEAT event");
static_assert(protocol::EventIdOf<protocol::StreamData>() ==
                  protocol::kProtocolStreamData,
              "StreamData must bind to STREAM_DATA event");

// Compile-time sizeof verification (ospgen v2)
static_assert(sizeof(protocol::RegisterRequest) == 50, "");
static_assert(sizeof(protocol::HeartbeatMsg) == 16, "");
static_assert(sizeof(protocol::StreamCommand) == 8, "");

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
  OSP_LOG_INFO("Proto", "=== streaming protocol demo (ospgen v2) ===");
  OSP_LOG_INFO("Proto", "protocol version=%u, node count=%u",
               protocol::kVersion, kNodeCount);

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

  // -- Server-side: StaticNode with ospgen topology constants ----------------

  RegistrarNode registrar(
      kNodeName_registrar, kNodeId_registrar,
      RegistrarHandler{&state, &bus});

  HeartbeatNode heartbeat_monitor(
      kNodeName_heartbeat_monitor, kNodeId_heartbeat_monitor,
      HeartbeatHandler{&state});

  StreamCtrlNode stream_controller(
      kNodeName_stream_controller, kNodeId_stream_controller,
      StreamHandler{&state});

  // -- Client-side: regular Node with ospgen topology constants --------------

  osp::Node<Payload> client(kNodeName_client, kNodeId_client);
  SetupClient(client);
  client.Start();

  // Step 1: device registration (HIGH priority)
  OSP_LOG_INFO("Proto", "--- step 1: device registration ---");
  protocol::RegisterRequest req{};
  std::strncpy(req.device_id, "CAM-310200001", sizeof(req.device_id) - 1);
  std::strncpy(req.ip, "192.168.1.100", sizeof(req.ip) - 1);
  req.port = 5060;

  // Validate before publish (ospgen-generated range check)
  if (req.Validate()) {
    client.PublishWithPriority(req, osp::MessagePriority::kHigh);
  } else {
    OSP_LOG_ERROR("Proto", "RegisterRequest validation failed");
  }
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
  protocol::StreamCommand start_cmd{};
  start_cmd.session_id = kDemoSessionId;
  start_cmd.action =
      static_cast<uint8_t>(protocol::StreamAction::kStart);
  start_cmd.media_type =
      static_cast<uint8_t>(protocol::MediaType::kAv);
  client.PublishWithPriority(start_cmd, osp::MessagePriority::kHigh);
  stream_controller.SpinOnce();

  for (uint32_t seq = 0; seq < 5; ++seq) {
    protocol::StreamData sd{};
    sd.session_id = kDemoSessionId;
    sd.seq = seq;
    sd.payload_size = 128;
    std::memset(sd.data, static_cast<int>(seq & 0xFF), sd.payload_size);
    client.PublishWithPriority(sd, osp::MessagePriority::kLow);
  }
  stream_controller.SpinOnce();

  protocol::StreamCommand stop_cmd{};
  stop_cmd.session_id = kDemoSessionId;
  stop_cmd.action =
      static_cast<uint8_t>(protocol::StreamAction::kStop);
  stop_cmd.media_type =
      static_cast<uint8_t>(protocol::MediaType::kAv);
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
  OSP_LOG_INFO("Proto",
               "topology: %s(subs=%u) %s(subs=%u) %s(subs=%u) %s(subs=%u)",
               kNodeName_registrar, kNodeSubCount_registrar,
               kNodeName_heartbeat_monitor, kNodeSubCount_heartbeat_monitor,
               kNodeName_stream_controller, kNodeSubCount_stream_controller,
               kNodeName_client, kNodeSubCount_client);
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
