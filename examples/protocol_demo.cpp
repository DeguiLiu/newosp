// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// protocol_demo.cpp -- Video streaming protocol handler simulation.
//
// Demonstrates how newosp modules (bus, node, timer, log) work together
// to implement a GB28181/RTSP-style streaming protocol pipeline:
//
//   1. Device registration  (RegisterRequest -> RegisterResponse)
//   2. Periodic heartbeat   (HeartbeatMsg via TimerScheduler)
//   3. Stream control        (StreamCommand, StreamData)
//
// Message priorities:
//   HIGH   - RegisterRequest  (signaling must not be dropped)
//   MEDIUM - HeartbeatMsg     (keep-alive, tolerates occasional loss)
//   LOW    - StreamData       (bulk media, shed first under pressure)

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/timer.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <variant>

// ============================================================================
// Protocol Message Definitions
// ============================================================================

struct RegisterRequest {
  char device_id[32];
  char ip[16];
  uint16_t port;
};

struct RegisterResponse {
  char device_id[32];
  uint8_t result;       // 0 = success, 1 = rejected
  uint32_t session_id;
};

struct HeartbeatMsg {
  uint32_t session_id;
  uint64_t timestamp_us;
};

struct StreamCommand {
  uint32_t session_id;
  uint8_t action;       // 0 = stop, 1 = start
  uint8_t media_type;   // 0 = video, 1 = audio, 2 = both
};

struct StreamData {
  uint32_t session_id;
  uint32_t seq;
  uint16_t payload_size;
  uint8_t data[256];
};

// Single variant carrying all protocol message types.
using Payload = std::variant<RegisterRequest, RegisterResponse,
                             HeartbeatMsg, StreamCommand, StreamData>;

// ============================================================================
// Protocol Statistics
// ============================================================================

struct ProtocolState {
  uint32_t registered_count = 0;
  uint32_t heartbeat_count = 0;
  uint32_t stream_count = 0;
  uint32_t error_count = 0;
};

static ProtocolState g_state;

// ============================================================================
// Node IDs
// ============================================================================

static constexpr uint32_t kRegistrarId = 1;
static constexpr uint32_t kHeartbeatId = 2;
static constexpr uint32_t kStreamCtrlId = 3;
static constexpr uint32_t kClientId = 10;

// Session ID assigned to the first registered device.
static constexpr uint32_t kDemoSessionId = 0x1001;

// ============================================================================
// Helper: current monotonic timestamp in microseconds.
// ============================================================================

static uint64_t NowUs() {
  auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(dur).count());
}

// ============================================================================
// Heartbeat timer callback -- publishes a HeartbeatMsg each period.
// ============================================================================

static void HeartbeatTimerCb(void* ctx) {
  auto* node = static_cast<osp::Node<Payload>*>(ctx);
  HeartbeatMsg hb{};
  hb.session_id = kDemoSessionId;
  hb.timestamp_us = NowUs();
  node->PublishWithPriority(hb, osp::MessagePriority::kMedium);
}

// ============================================================================
// main
// ============================================================================

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("Proto", "=== streaming protocol demo start ===");

  // Reset the singleton bus so repeated runs in tests are clean.
  osp::AsyncBus<Payload>::Instance().Reset();

  // ---- Create nodes ----
  osp::Node<Payload> registrar("registrar", kRegistrarId);
  osp::Node<Payload> heartbeat_monitor("heartbeat_monitor", kHeartbeatId);
  osp::Node<Payload> stream_controller("stream_controller", kStreamCtrlId);
  osp::Node<Payload> client("client", kClientId);

  // ---- Registrar: handle RegisterRequest, reply with RegisterResponse ----
  registrar.Subscribe<RegisterRequest>(
      [&](const RegisterRequest& req, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("Registrar", "device %s from %s:%u", req.device_id,
                     req.ip, static_cast<unsigned>(req.port));
        RegisterResponse resp{};
        std::strncpy(resp.device_id, req.device_id,
                     sizeof(resp.device_id) - 1);
        resp.result = 0;  // success
        resp.session_id = kDemoSessionId;
        registrar.Publish(resp);
        ++g_state.registered_count;
      });

  // ---- Client: receive RegisterResponse ----
  client.Subscribe<RegisterResponse>(
      [](const RegisterResponse& resp, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("Client",
                     "registered device %s, session 0x%X, result %u",
                     resp.device_id, resp.session_id,
                     static_cast<unsigned>(resp.result));
      });

  // ---- Heartbeat monitor: validate heartbeat freshness ----
  heartbeat_monitor.Subscribe<HeartbeatMsg>(
      [](const HeartbeatMsg& hb, const osp::MessageHeader& /*hdr*/) {
        uint64_t age_us = NowUs() - hb.timestamp_us;
        if (age_us > 500000) {  // > 500 ms is suspicious
          OSP_LOG_WARN("Heartbeat", "session 0x%X late by %lu us",
                       hb.session_id, static_cast<unsigned long>(age_us));
          ++g_state.error_count;
        } else {
          OSP_LOG_DEBUG("Heartbeat", "session 0x%X ok (%lu us age)",
                        hb.session_id, static_cast<unsigned long>(age_us));
        }
        ++g_state.heartbeat_count;
      });

  // ---- Stream controller: handle commands and data ----
  stream_controller.Subscribe<StreamCommand>(
      [](const StreamCommand& cmd, const osp::MessageHeader& /*hdr*/) {
        const char* action = (cmd.action == 1) ? "START" : "STOP";
        const char* media = (cmd.media_type == 0)   ? "video"
                            : (cmd.media_type == 1) ? "audio"
                                                    : "A/V";
        OSP_LOG_INFO("StreamCtrl", "session 0x%X %s %s",
                     cmd.session_id, action, media);
        ++g_state.stream_count;
      });

  stream_controller.Subscribe<StreamData>(
      [](const StreamData& sd, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_DEBUG("StreamCtrl", "session 0x%X seq=%u size=%u",
                      sd.session_id, sd.seq,
                      static_cast<unsigned>(sd.payload_size));
        ++g_state.stream_count;
      });

  // ---- Start all nodes ----
  registrar.Start();
  heartbeat_monitor.Start();
  stream_controller.Start();
  client.Start();

  // ---- Step 1: device registration (HIGH priority) ----
  OSP_LOG_INFO("Proto", "--- step 1: device registration ---");
  RegisterRequest req{};
  std::strncpy(req.device_id, "CAM-310200001",
               sizeof(req.device_id) - 1);
  std::strncpy(req.ip, "192.168.1.100", sizeof(req.ip) - 1);
  req.port = 5060;
  client.PublishWithPriority(req, osp::MessagePriority::kHigh);
  registrar.SpinOnce();  // dispatch request
  registrar.SpinOnce();  // dispatch response

  // ---- Step 2: periodic heartbeats via TimerScheduler ----
  OSP_LOG_INFO("Proto", "--- step 2: heartbeat monitoring ---");
  osp::TimerScheduler<4> timer;
  timer.Add(50, HeartbeatTimerCb, &client);  // 50 ms period
  timer.Start();

  // Let heartbeats accumulate for ~200 ms, draining the bus periodically.
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    heartbeat_monitor.SpinOnce();
  }
  timer.Stop();

  // ---- Step 3: stream start / data / stop ----
  OSP_LOG_INFO("Proto", "--- step 3: stream control ---");
  StreamCommand start_cmd{};
  start_cmd.session_id = kDemoSessionId;
  start_cmd.action = 1;      // start
  start_cmd.media_type = 2;  // audio+video
  client.PublishWithPriority(start_cmd, osp::MessagePriority::kHigh);
  stream_controller.SpinOnce();

  // Send a burst of stream data (LOW priority -- first to shed).
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
  stop_cmd.action = 0;       // stop
  stop_cmd.media_type = 2;
  client.PublishWithPriority(stop_cmd, osp::MessagePriority::kHigh);
  stream_controller.SpinOnce();

  // ---- Statistics summary ----
  auto stats = osp::AsyncBus<Payload>::Instance().GetStatistics();
  OSP_LOG_INFO("Proto", "=== statistics ===");
  OSP_LOG_INFO("Proto",
               "bus: published=%lu processed=%lu dropped=%lu",
               static_cast<unsigned long>(stats.messages_published),
               static_cast<unsigned long>(stats.messages_processed),
               static_cast<unsigned long>(stats.messages_dropped));
  OSP_LOG_INFO("Proto",
               "app: registered=%u heartbeats=%u streams=%u errors=%u",
               g_state.registered_count, g_state.heartbeat_count,
               g_state.stream_count, g_state.error_count);
  OSP_LOG_INFO("Proto", "=== streaming protocol demo end ===");

  osp::log::Shutdown();
  return 0;
}
