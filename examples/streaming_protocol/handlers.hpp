// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// handlers.hpp -- Protocol handlers for streaming protocol demo.
//
// Uses ospgen-generated message types (protocol_messages.hpp) and
// topology constants (topology.hpp) instead of hand-written structs.
//
// Server-side handlers use StaticNode (compile-time dispatch, inlinable).
// Client uses regular Node (dynamic callback, also timer context pointer).

#ifndef OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_
#define OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_

#include "osp/protocol_messages.hpp"
#include "osp/topology.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"
#include "osp/static_node.hpp"

#include <cstdint>
#include <cstring>

// Payload type alias from ospgen-generated variant
using Payload = protocol::ProtocolPayload;

// Session ID for this demo
static constexpr uint32_t kDemoSessionId = 0x1001;

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
// StaticNode Handler structs (compile-time dispatch, zero overhead)
//
// Each handler defines operator() for message types it cares about,
// plus a template catch-all for unrelated types (no-op, optimized away).
// Since the handler type is a template parameter of StaticNode, the
// compiler resolves all dispatch at compile time and can inline.
// ============================================================================

// -- Registrar: handles RegisterRequest, publishes RegisterResponse ---------

struct RegistrarHandler {
  ProtocolState* state;
  osp::AsyncBus<Payload>* bus;

  void operator()(const protocol::RegisterRequest& req,
                  const osp::MessageHeader& /*h*/) {
    // Validate incoming request (ospgen-generated range constraints)
    if (!req.Validate()) {
      OSP_LOG_WARN("Registrar", "rejected: port out of range [1,65535]");
      ++state->error_count;
      return;
    }

    // Debug dump (ospgen-generated snprintf)
    char dump[256];
    req.Dump(dump, sizeof(dump));
    OSP_LOG_INFO("Registrar", "recv: %s", dump);

    protocol::RegisterResponse resp{};
    std::strncpy(resp.device_id, req.device_id,
                 sizeof(resp.device_id) - 1);
    resp.result = 0;
    resp.session_id = kDemoSessionId;
    bus->Publish(Payload(resp), kNodeId_registrar);
    ++state->registered_count;
  }

  // Catch-all: ignore unrelated message types (optimized away by compiler)
  template <typename T>
  void operator()(const T& /*unused*/, const osp::MessageHeader& /*h*/) {}
};

// -- HeartbeatMonitor: handles HeartbeatMsg, checks latency -----------------

struct HeartbeatHandler {
  ProtocolState* state;

  void operator()(const protocol::HeartbeatMsg& hb,
                  const osp::MessageHeader& /*h*/) {
    uint64_t age_us = osp::SteadyNowUs() - hb.timestamp_us;
    if (age_us > 500000) {
      OSP_LOG_WARN("Heartbeat", "session 0x%X late by %lu us",
                   hb.session_id, static_cast<unsigned long>(age_us));
      ++state->error_count;
    } else {
      OSP_LOG_DEBUG("Heartbeat", "session 0x%X ok (%lu us age)",
                    hb.session_id, static_cast<unsigned long>(age_us));
    }
    ++state->heartbeat_count;
  }

  template <typename T>
  void operator()(const T& /*unused*/, const osp::MessageHeader& /*h*/) {}
};

// -- StreamController: handles StreamCommand + StreamData (hot path) --------

struct StreamHandler {
  ProtocolState* state;

  void operator()(const protocol::StreamCommand& cmd,
                  const osp::MessageHeader& /*h*/) {
    // Validate action range (ospgen-generated: [0,1])
    if (!cmd.Validate()) {
      OSP_LOG_WARN("StreamCtrl", "rejected: action out of range");
      ++state->error_count;
      return;
    }

    // Type-safe comparison via ospgen-generated standalone enums
    const char* action =
        (cmd.action ==
         static_cast<uint8_t>(protocol::StreamAction::kStart))
            ? "START"
            : "STOP";
    const char* media =
        (cmd.media_type ==
         static_cast<uint8_t>(protocol::MediaType::kVideo))
            ? "video"
        : (cmd.media_type ==
           static_cast<uint8_t>(protocol::MediaType::kAudio))
            ? "audio"
        : (cmd.media_type ==
           static_cast<uint8_t>(protocol::MediaType::kAv))
            ? "A/V"
            : "none";
    OSP_LOG_INFO("StreamCtrl", "session 0x%X %s %s",
                 cmd.session_id, action, media);
    ++state->stream_count;
  }

  void operator()(const protocol::StreamData& sd,
                  const osp::MessageHeader& /*h*/) {
    // Debug dump (ospgen-generated)
    char dump[256];
    sd.Dump(dump, sizeof(dump));
    OSP_LOG_DEBUG("StreamCtrl", "%s", dump);
    ++state->stream_count;
  }

  template <typename T>
  void operator()(const T& /*unused*/, const osp::MessageHeader& /*h*/) {}
};

// ============================================================================
// Type aliases for StaticNode instantiations
// ============================================================================

using RegistrarNode = osp::StaticNode<Payload, RegistrarHandler>;
using HeartbeatNode = osp::StaticNode<Payload, HeartbeatHandler>;
using StreamCtrlNode = osp::StaticNode<Payload, StreamHandler>;

// ============================================================================
// Heartbeat timer callback (publishes via client Node)
// ============================================================================

inline void HeartbeatTimerCb(void* ctx) {
  auto* node = static_cast<osp::Node<Payload>*>(ctx);
  protocol::HeartbeatMsg hb{};
  hb.session_id = kDemoSessionId;
  hb.timestamp_us = osp::SteadyNowUs();
  node->PublishWithPriority(hb, osp::MessagePriority::kMedium);
}

// ============================================================================
// Client setup (regular Node -- used as timer context pointer)
// ============================================================================

inline void SetupClient(osp::Node<Payload>& client) {
  client.Subscribe<protocol::RegisterResponse>(
      [](const protocol::RegisterResponse& resp, const osp::MessageHeader&) {
        // Use Dump() for structured debug output
        char dump[256];
        resp.Dump(dump, sizeof(dump));
        OSP_LOG_INFO("Client", "registered: %s", dump);
      });
}

#endif  // OSP_EXAMPLES_STREAMING_PROTOCOL_HANDLERS_HPP_
