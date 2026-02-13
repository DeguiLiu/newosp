/**
 * @file codegen_demo.cpp
 * @brief Demonstrates ospgen-generated headers in a realistic scenario.
 *
 * Build: cmake -B build -DOSP_CODEGEN=ON -DOSP_BUILD_EXAMPLES=ON
 *
 * Showcases:
 *   1. Generated protocol messages (register, heartbeat, stream control)
 *   2. Generated sensor messages (temperature, alarm)
 *   3. Generated topology constants (node IDs, names)
 *   4. Event enum for dispatch/logging
 *   5. Multi-node pub/sub with generated types
 *   6. Application/Instance model with generated messages via OspPost
 */

#include "osp/protocol_messages.hpp"
#include "osp/sensor_messages.hpp"
#include "osp/topology.hpp"

#include "osp/app.hpp"
#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/post.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

// ============================================================================
// Part 1: Protocol messages + Bus/Node pub-sub
// ============================================================================

using ProtoBus = osp::AsyncBus<protocol::ProtocolPayload>;

static void RunProtocolDemo() {
  OSP_LOG_INFO("demo", "--- Part 1: Protocol pub-sub ---");
  ProtoBus::Instance().Reset();

  // Create nodes using generated topology constants
  osp::Node<protocol::ProtocolPayload> registrar(
      kNodeName_registrar, kNodeId_registrar);
  osp::Node<protocol::ProtocolPayload> hb_monitor(
      kNodeName_heartbeat_monitor, kNodeId_heartbeat_monitor);
  osp::Node<protocol::ProtocolPayload> stream_ctrl(
      kNodeName_stream_controller, kNodeId_stream_controller);
  osp::Node<protocol::ProtocolPayload> client(
      kNodeName_client, kNodeId_client);

  uint32_t reg_count = 0, hb_count = 0, stream_count = 0;

  // Registrar handles RegisterRequest, replies with RegisterResponse
  registrar.Subscribe<protocol::RegisterRequest>(
      [&](const protocol::RegisterRequest& req,
          const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("registrar", "[evt=%u] device %s from %s:%u",
                     static_cast<unsigned>(protocol::kProtocolRegister),
                     req.device_id, req.ip,
                     static_cast<unsigned>(req.port));
        protocol::RegisterResponse resp{};
        std::strncpy(resp.device_id, req.device_id,
                     sizeof(resp.device_id) - 1);
        resp.result = 0;
        resp.session_id = 0x1001;
        registrar.Publish(resp);
        ++reg_count;
      });

  // Client receives RegisterResponse
  client.Subscribe<protocol::RegisterResponse>(
      [](const protocol::RegisterResponse& resp,
         const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("client", "[evt=%u] registered %s session=0x%X",
                     static_cast<unsigned>(protocol::kProtocolRegisterAck),
                     resp.device_id, resp.session_id);
      });

  // Heartbeat monitor
  hb_monitor.Subscribe<protocol::HeartbeatMsg>(
      [&](const protocol::HeartbeatMsg& hb,
          const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_DEBUG("hb_mon", "[evt=%u] session=0x%X ts=%lu",
                      static_cast<unsigned>(protocol::kProtocolHeartbeat),
                      hb.session_id,
                      static_cast<unsigned long>(hb.timestamp_us));
        ++hb_count;
      });

  // Stream controller
  stream_ctrl.Subscribe<protocol::StreamCommand>(
      [&](const protocol::StreamCommand& cmd,
          const osp::MessageHeader& /*hdr*/) {
        const char* action = (cmd.action == 1) ? "START" : "STOP";
        OSP_LOG_INFO("stream", "[evt=%u] session=0x%X %s",
                     static_cast<unsigned>(protocol::kProtocolStreamStart),
                     cmd.session_id, action);
        ++stream_count;
      });

  // --- Simulate protocol flow ---

  // 1. Register
  protocol::RegisterRequest req{};
  std::strncpy(req.device_id, "CAM-001", sizeof(req.device_id) - 1);
  std::strncpy(req.ip, "192.168.1.100", sizeof(req.ip) - 1);
  req.port = 5060;
  client.Publish(req);

  // 2. Heartbeat
  protocol::HeartbeatMsg hb{};
  hb.session_id = 0x1001;
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  hb.timestamp_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  client.Publish(hb);

  // 3. Stream start
  protocol::StreamCommand cmd{};
  cmd.session_id = 0x1001;
  cmd.action = 1;
  cmd.media_type = 2;
  client.Publish(cmd);

  // Process all
  registrar.SpinOnce();
  hb_monitor.SpinOnce();
  stream_ctrl.SpinOnce();
  client.SpinOnce();

  OSP_LOG_INFO("demo", "protocol: reg=%u hb=%u stream=%u (nodes=%u)",
               reg_count, hb_count, stream_count, kNodeCount);
}

// ============================================================================
// Part 2: Sensor messages + Bus/Node pub-sub
// ============================================================================

using SensorBus = osp::AsyncBus<sensor::SensorPayload>;

static void RunSensorDemo() {
  OSP_LOG_INFO("demo", "--- Part 2: Sensor pub-sub ---");
  SensorBus::Instance().Reset();

  osp::Node<sensor::SensorPayload> sensor_node("sensor", 1);
  osp::Node<sensor::SensorPayload> alarm_node("alarm_handler", 2);

  uint32_t data_count = 0, alarm_count = 0;

  sensor_node.Subscribe<sensor::SensorData>(
      [&](const sensor::SensorData& d, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("sensor", "[evt=%u] temp=%.1f humidity=%.1f",
                     static_cast<unsigned>(sensor::kSensorTemperatureUpdate),
                     static_cast<double>(d.temp),
                     static_cast<double>(d.humidity));
        ++data_count;

        // Trigger alarm if temp > 40
        if (d.temp > 40.0f) {
          sensor::SensorAlarm alarm{};
          alarm.sensor_id = 1;
          alarm.code = sensor::kSensorAlarm;
          alarm.value = d.temp;
          alarm.threshold = 40.0f;
          sensor_node.Publish(alarm);
        }
      });

  alarm_node.Subscribe<sensor::SensorAlarm>(
      [&](const sensor::SensorAlarm& a, const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_WARN("alarm", "[evt=%u] sensor=%u code=%u val=%.1f thresh=%.1f",
                     static_cast<unsigned>(sensor::kSensorAlarm),
                     static_cast<unsigned>(a.sensor_id), a.code,
                     static_cast<double>(a.value),
                     static_cast<double>(a.threshold));
        ++alarm_count;
      });

  // Normal reading
  sensor::SensorData sd1{};
  sd1.temp = 23.5f;
  sd1.humidity = 61.2f;
  sensor_node.Publish(sd1);
  sensor_node.SpinOnce();

  // High temp triggers alarm
  sensor::SensorData sd2{};
  sd2.temp = 42.8f;
  sd2.humidity = 55.0f;
  sensor_node.Publish(sd2);
  sensor_node.SpinOnce();
  alarm_node.SpinOnce();

  OSP_LOG_INFO("demo", "sensor: data=%u alarms=%u", data_count, alarm_count);
}

// ============================================================================
// Part 3: Application/Instance model with generated messages via OspPost
// ============================================================================

struct HeartbeatInstance : public osp::Instance {
  uint32_t count = 0;

  void OnMessage(uint16_t event, const void* data, uint32_t len) override {
    if (event == protocol::kProtocolHeartbeat && data && len >= sizeof(protocol::HeartbeatMsg)) {
      protocol::HeartbeatMsg hb{};
      std::memcpy(&hb, data, sizeof(hb));
      OSP_LOG_DEBUG("hb_inst", "heartbeat session=0x%X", hb.session_id);
      ++count;
    }
    // Reply if sync call
    if (HasPendingReply()) {
      Reply(&count, sizeof(count));
    }
  }
};

static osp::Instance* HeartbeatFactory() { return new HeartbeatInstance(); }

static void RunAppModelDemo() {
  OSP_LOG_INFO("demo", "--- Part 3: App/Instance + OspPost ---");

  auto& reg = osp::AppRegistry::Instance();
  reg.Reset();

  osp::Application<4> hb_app(kNodeId_heartbeat_monitor, kNodeName_heartbeat_monitor);
  hb_app.SetFactory(HeartbeatFactory);
  osp::RegisterApp(hb_app);

  auto r = hb_app.CreateInstance();
  if (!r.has_value()) {
    OSP_LOG_ERROR("demo", "failed to create instance");
    osp::UnregisterApp(hb_app);
    return;
  }

  uint32_t dst = osp::MakeIID(kNodeId_heartbeat_monitor, r.value());

  // Post heartbeat via OspPost (fire-and-forget)
  protocol::HeartbeatMsg hb{};
  hb.session_id = 0x2002;
  hb.timestamp_us = 1000000;
  osp::OspPost(dst, protocol::kProtocolHeartbeat, &hb, sizeof(hb));
  hb_app.ProcessAll();

  // Post another
  hb.session_id = 0x2003;
  osp::OspPost(dst, protocol::kProtocolHeartbeat, &hb, sizeof(hb));
  hb_app.ProcessAll();

  auto* inst = static_cast<HeartbeatInstance*>(hb_app.GetInstance(r.value()));
  OSP_LOG_INFO("demo", "app model: heartbeats processed=%u", inst->count);

  osp::UnregisterApp(hb_app);
}

// ============================================================================
// main
// ============================================================================

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("demo", "=== ospgen codegen demo ===");

  RunProtocolDemo();
  RunSensorDemo();
  RunAppModelDemo();

  OSP_LOG_INFO("demo", "=== all demos complete ===");
  return 0;
}
