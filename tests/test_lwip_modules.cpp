/**
 * @file test_lwip_modules.cpp
 * @brief Compile/link gate: business modules under the lwIP backend.
 * Forces the socket-touching member bodies to compile and emit against the
 * real lwIP API. LWIP_IGMP is set by lwip_igmp_override.hpp per the design doc.
 */

// LWIP_IGMP must be on before any osp header pulls in lwip/sockets.h.
#include "lwip_igmp_override.hpp"

#include "osp/discovery.hpp"
#include "osp/node_manager.hpp"
#include "osp/service.hpp"
#include "osp/shell.hpp"
#include "osp/transport.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

// POD message types for the Service<Request, Response> templates.
struct GateRequest {
  uint32_t a;
};
struct GateResponse {
  uint32_t b;
};

// ODR-use the socket-touching methods so their bodies are emitted and linked
// against the lwIP library, not merely parsed at include time.
[[maybe_unused]] auto kServiceStart = &osp::Service<GateRequest, GateResponse>::Start;
[[maybe_unused]] auto kClientConnect = &osp::Client<GateRequest, GateResponse>::Connect;
[[maybe_unused]] auto kClientCall = &osp::Client<GateRequest, GateResponse>::Call;
[[maybe_unused]] auto kTcpConnect = &osp::TcpTransport::Connect;
[[maybe_unused]] auto kDiscoveryStart = &osp::MulticastDiscovery<32>::Start;
[[maybe_unused]] auto kShellStart = &osp::DebugShell::Start;
[[maybe_unused]] auto kNodeManagerStart = &osp::NodeManager<32>::Start;

}  // namespace

TEST_CASE("lwIP backend: business module headers compile and link", "[lwip][backend1][modules]") {
  static_assert(sizeof(osp::Service<GateRequest, GateResponse>) > 0U, "Service must be complete");
  static_assert(sizeof(osp::Client<GateRequest, GateResponse>) > 0U, "Client must be complete");
  static_assert(sizeof(osp::TcpTransport) > 0U, "TcpTransport must be complete");
  static_assert(sizeof(osp::MulticastDiscovery<32>) > 0U, "MulticastDiscovery must be complete");
  static_assert(sizeof(osp::DebugShell) > 0U, "DebugShell must be complete");
  static_assert(sizeof(osp::NodeManager<32>) > 0U, "NodeManager must be complete");
  REQUIRE(true);
}
