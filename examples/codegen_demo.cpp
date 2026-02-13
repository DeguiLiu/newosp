/**
 * @file codegen_demo.cpp
 * @brief Demonstrates using ospgen-generated headers.
 *
 * Build with: cmake -B build -DOSP_CODEGEN=ON -DOSP_BUILD_EXAMPLES=ON
 *
 * This example uses protocol_messages.hpp and topology.hpp generated
 * from YAML definitions in defs/, showing zero-boilerplate message
 * definitions and node topology constants.
 */

#include "osp/protocol_messages.hpp"
#include "osp/topology.hpp"
#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/log.hpp"

#include <cstdio>
#include <cstring>

using Bus = osp::AsyncBus<protocol::ProtocolPayload>;

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);
  OSP_LOG_INFO("codegen", "=== codegen demo start ===");

  Bus::Instance().Reset();

  // Create nodes using generated topology constants
  osp::Node<protocol::ProtocolPayload> registrar(
      kNodeName_registrar, kNodeId_registrar);
  osp::Node<protocol::ProtocolPayload> client(
      kNodeName_client, kNodeId_client);

  // Subscribe using generated message types
  registrar.Subscribe<protocol::RegisterRequest>(
      [&](const protocol::RegisterRequest& req,
          const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("registrar", "device %s from %s:%u",
                     req.device_id, req.ip,
                     static_cast<unsigned>(req.port));

        protocol::RegisterResponse resp{};
        std::strncpy(resp.device_id, req.device_id,
                     sizeof(resp.device_id) - 1);
        resp.result = 0;
        resp.session_id = 0x1001;
        registrar.Publish(resp);
      });

  client.Subscribe<protocol::RegisterResponse>(
      [](const protocol::RegisterResponse& resp,
         const osp::MessageHeader& /*hdr*/) {
        OSP_LOG_INFO("client", "registered %s session=0x%X result=%u",
                     resp.device_id, resp.session_id,
                     static_cast<unsigned>(resp.result));
      });

  // Publish using generated event enum for logging
  OSP_LOG_INFO("codegen", "sending REGISTER (event=%u)",
               static_cast<unsigned>(protocol::kProtocolRegister));

  protocol::RegisterRequest req{};
  std::strncpy(req.device_id, "CAM-001", sizeof(req.device_id) - 1);
  std::strncpy(req.ip, "192.168.1.100", sizeof(req.ip) - 1);
  req.port = 5060;
  client.Publish(req);

  // Process
  uint32_t n = registrar.SpinOnce();
  n += client.SpinOnce();
  OSP_LOG_INFO("codegen", "processed %u messages, node_count=%u",
               n, kNodeCount);

  OSP_LOG_INFO("codegen", "=== codegen demo done ===");
  return 0;
}
