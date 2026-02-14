// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// Streaming protocol demo: GB28181/RTSP-style pipeline simulation.
//
// Server-side nodes use StaticNode (compile-time handler dispatch).
// Client node uses regular Node (dynamic callback + timer context).

#include "handlers.hpp"
#include "messages.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/timer.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("Proto", "=== streaming protocol demo start ===");

  auto& bus = osp::AsyncBus<Payload>::Instance();
  bus.Reset();

  ProtocolState state;

  // -- Server-side: StaticNode (compile-time dispatch, inlinable) -----------

  RegistrarNode registrar(
      "registrar", kRegistrarId,
      RegistrarHandler{&state, &bus});
  registrar.Start();

  HeartbeatNode heartbeat_monitor(
      "heartbeat_monitor", kHeartbeatId,
      HeartbeatHandler{&state});
  heartbeat_monitor.Start();

  StreamCtrlNode stream_controller(
      "stream_controller", kStreamCtrlId,
      StreamHandler{&state});
  stream_controller.Start();

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

  osp::log::Shutdown();
  return 0;
}
