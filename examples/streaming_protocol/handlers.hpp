// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// handlers.hpp -- Node subscription handlers for streaming protocol demo.

#ifndef OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_
#define OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_

#include "messages.hpp"

#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"

#include <cstdint>
#include <cstring>

// ============================================================================
// Protocol Statistics
// ============================================================================

struct ProtocolState {
  uint32_t registered_count = 0;
  uint32_t heartbeat_count = 0;
  uint32_t stream_count = 0;
  uint32_t error_count = 0;
};

// ============================================================================
// Node IDs
// ============================================================================

static constexpr uint32_t kRegistrarId = 1;
static constexpr uint32_t kHeartbeatId = 2;
static constexpr uint32_t kStreamCtrlId = 3;
static constexpr uint32_t kClientId = 10;
static constexpr uint32_t kDemoSessionId = 0x1001;

// ============================================================================
// Heartbeat timer callback
// ============================================================================

inline void HeartbeatTimerCb(void* ctx) {
  auto* node = static_cast<osp::Node<Payload>*>(ctx);
  HeartbeatMsg hb{};
  hb.session_id = kDemoSessionId;
  hb.timestamp_us = osp::SteadyNowUs();
  node->PublishWithPriority(hb, osp::MessagePriority::kMedium);
}

// ============================================================================
// Node subscription setup
// ============================================================================

inline void SetupRegistrar(osp::Node<Payload>& registrar,
                           ProtocolState& state) {
  registrar.Subscribe<RegisterRequest>(
      [&registrar, &state](const RegisterRequest& req,
                           const osp::MessageHeader&) {
        OSP_LOG_INFO("Registrar", "device %s from %s:%u", req.device_id,
                     req.ip, static_cast<unsigned>(req.port));
        RegisterResponse resp{};
        std::strncpy(resp.device_id, req.device_id,
                     sizeof(resp.device_id) - 1);
        resp.result = 0;
        resp.session_id = kDemoSessionId;
        registrar.Publish(resp);
        ++state.registered_count;
      });
}

inline void SetupClient(osp::Node<Payload>& client) {
  client.Subscribe<RegisterResponse>(
      [](const RegisterResponse& resp, const osp::MessageHeader&) {
        OSP_LOG_INFO("Client",
                     "registered device %s, session 0x%X, result %u",
                     resp.device_id, resp.session_id,
                     static_cast<unsigned>(resp.result));
      });
}

inline void SetupHeartbeatMonitor(osp::Node<Payload>& monitor,
                                  ProtocolState& state) {
  monitor.Subscribe<HeartbeatMsg>(
      [&state](const HeartbeatMsg& hb, const osp::MessageHeader&) {
        uint64_t age_us = osp::SteadyNowUs() - hb.timestamp_us;
        if (age_us > 500000) {
          OSP_LOG_WARN("Heartbeat", "session 0x%X late by %lu us",
                       hb.session_id, static_cast<unsigned long>(age_us));
          ++state.error_count;
        } else {
          OSP_LOG_DEBUG("Heartbeat", "session 0x%X ok (%lu us age)",
                        hb.session_id, static_cast<unsigned long>(age_us));
        }
        ++state.heartbeat_count;
      });
}

inline void SetupStreamController(osp::Node<Payload>& controller,
                                  ProtocolState& state) {
  controller.Subscribe<StreamCommand>(
      [&state](const StreamCommand& cmd, const osp::MessageHeader&) {
        const char* action = (cmd.action == 1) ? "START" : "STOP";
        const char* media = (cmd.media_type == 0)   ? "video"
                            : (cmd.media_type == 1) ? "audio"
                                                    : "A/V";
        OSP_LOG_INFO("StreamCtrl", "session 0x%X %s %s",
                     cmd.session_id, action, media);
        ++state.stream_count;
      });

  controller.Subscribe<StreamData>(
      [&state](const StreamData& sd, const osp::MessageHeader&) {
        OSP_LOG_DEBUG("StreamCtrl", "session 0x%X seq=%u size=%u",
                      sd.session_id, sd.seq,
                      static_cast<unsigned>(sd.payload_size));
        ++state.stream_count;
      });
}

#endif  // OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_
