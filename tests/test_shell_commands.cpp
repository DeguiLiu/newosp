/**
 * @file test_shell_commands.cpp
 * @brief Unit tests for shell_commands.hpp diagnostic command registration.
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/shell_commands.hpp"
#include "osp/bus.hpp"
#include "osp/watchdog.hpp"
#include "osp/fault_collector.hpp"
#include "osp/node_manager_hsm.hpp"
#include "osp/service_hsm.hpp"
#include "osp/discovery_hsm.hpp"
#include "osp/lifecycle_node.hpp"
#include "osp/qos.hpp"
#include "osp/mem_pool.hpp"
#include "osp/transport.hpp"
#include "osp/worker_pool.hpp"
#include "osp/serial_transport.hpp"

// Minimal payload for bus/node tests
struct TestMsg {
  uint32_t value;
};
using TestPayload = std::variant<TestMsg>;

// ============================================================================
// Registration Tests -- verify commands register without crash
// ============================================================================

TEST_CASE("shell_cmd RegisterWatchdog registers and executes",
          "[shell_commands]") {
  osp::ThreadWatchdog<8> wd;
  auto r = wd.Register("test_thread", 1000);
  REQUIRE(r.has_value());
  r.value().heartbeat->Beat();

  osp::shell_cmd::RegisterWatchdog(wd);

  // Find and execute the command
  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_watchdog") == 0) {
          found = true;
          // Execute -- output goes to thread-local session (nullptr = no-op)
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterFaults registers and executes",
          "[shell_commands]") {
  osp::FaultCollector<16, 32> fc;
  fc.RegisterFault(0, 0x01010001U);
  fc.Start();
  fc.ReportFault(0, 42, osp::FaultPriority::kHigh);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  osp::shell_cmd::RegisterFaults(fc);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_faults") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
  fc.Stop();
}

TEST_CASE("shell_cmd RegisterBusStats registers and executes",
          "[shell_commands]") {
  auto& bus = osp::AsyncBus<TestPayload>::Instance();

  osp::shell_cmd::RegisterBusStats(bus);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_bus") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterHsmNodes registers and executes",
          "[shell_commands]") {
  osp::HsmNodeManager<8> mgr;
  mgr.AddNode(1);
  mgr.AddNode(2);

  osp::shell_cmd::RegisterHsmNodes(mgr);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_nodes") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterServiceHsm registers and executes",
          "[shell_commands]") {
  osp::HsmService svc;

  osp::shell_cmd::RegisterServiceHsm(svc);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_service") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterDiscoveryHsm registers and executes",
          "[shell_commands]") {
  osp::HsmDiscovery disc;

  osp::shell_cmd::RegisterDiscoveryHsm(disc);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_discovery") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterTransport registers and executes",
          "[shell_commands]") {
  osp::SequenceTracker tracker;
  tracker.Track(0);
  tracker.Track(1);
  tracker.Track(5);  // gap: lost 3 packets

  osp::shell_cmd::RegisterTransport(tracker);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_transport") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterQos registers and executes",
          "[shell_commands]") {
  osp::shell_cmd::RegisterQos(osp::QosSensorData, "sensor");

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_qos") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterMemPool registers and executes",
          "[shell_commands]") {
  osp::FixedPool<64, 16> pool;
  (void)pool.Allocate();

  osp::shell_cmd::RegisterMemPool(pool, "test_pool");

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_mempool") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterLifecycle registers and executes",
          "[shell_commands]") {
  osp::LifecycleNode<TestPayload> node("test_lifecycle", 99);

  osp::shell_cmd::RegisterLifecycle(node);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_lifecycle") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterWorkerPool registers and executes",
          "[shell_commands]") {
  osp::WorkerPoolConfig cfg;
  cfg.name = "test_pool";
  cfg.worker_num = 1U;
  osp::WorkerPool<TestPayload> pool(cfg);

  osp::shell_cmd::RegisterWorkerPool(pool);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_pool") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}

TEST_CASE("shell_cmd RegisterSerial registers and executes",
          "[shell_commands]") {
  osp::SerialConfig scfg;
  scfg.port_name = "/dev/null";
  osp::SerialTransport serial(scfg);

  osp::shell_cmd::RegisterSerial(serial);

  bool found = false;
  osp::detail::GlobalCmdRegistry::Instance().ForEach(
      [&](const osp::ShellCmd& cmd) {
        if (std::strcmp(cmd.name, "osp_serial") == 0) {
          found = true;
          int rc = cmd.func(0, nullptr);
          CHECK(rc == 0);
        }
      });
  CHECK(found);
}
