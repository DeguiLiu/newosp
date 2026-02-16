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
#include "osp/platform.hpp"
#if OSP_HAS_NETWORK
#include "osp/transport.hpp"
#endif
#include "osp/worker_pool.hpp"
#include "osp/serial_transport.hpp"

// Minimal payload for bus/node tests
// NOTE: structs must be >= 8 bytes to avoid GCC 14 wide-read optimization
// triggering ASan stack-buffer-overflow (8-byte memcpy on smaller struct).
struct TestMsg {
  uint32_t value;
  uint32_t reserved = 0;
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

#if OSP_HAS_NETWORK
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
#endif  // OSP_HAS_NETWORK

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

// ============================================================================
// MockSession helper (pipe-backed output capture)
// ============================================================================

#include <fcntl.h>
#include <unistd.h>
#include <string>

namespace {

struct MockSession {
  osp::detail::ShellSession session{};
  int capture_read_fd = -1;
  int capture_write_fd = -1;

  MockSession() {
    int pipefd[2] = {-1, -1};
    (void)::pipe(pipefd);
    capture_read_fd = pipefd[0];
    capture_write_fd = pipefd[1];

    int flags = ::fcntl(capture_read_fd, F_GETFL, 0);
    (void)::fcntl(capture_read_fd, F_SETFL, flags | O_NONBLOCK);

    session.read_fd = -1;
    session.write_fd = capture_write_fd;
    session.write_fn = osp::detail::ShellPosixWrite;
    session.read_fn = osp::detail::ShellPosixRead;
    session.telnet_mode = false;
    session.iac_state = osp::detail::ShellSession::IacState::kNormal;
    session.esc_state = osp::detail::ShellSession::EscState::kNone;
    session.line_pos = 0;
    session.hist_browsing = false;
    session.skip_next_lf = false;
    session.active.store(true, std::memory_order_relaxed);
  }

  ~MockSession() {
    if (capture_read_fd >= 0) ::close(capture_read_fd);
    if (capture_write_fd >= 0) ::close(capture_write_fd);
  }

  std::string DrainOutput() {
    std::string result;
    char buf[512];
    for (;;) {
      ssize_t n = ::read(capture_read_fd, buf, sizeof(buf));
      if (n <= 0) break;
      result.append(buf, static_cast<size_t>(n));
    }
    return result;
  }

  MockSession(const MockSession&) = delete;
  MockSession& operator=(const MockSession&) = delete;
};

struct SessionGuard {
  explicit SessionGuard(MockSession& m) {
    osp::detail::CurrentSession() = &m.session;
  }
  ~SessionGuard() { osp::detail::CurrentSession() = nullptr; }
};

}  // namespace

// ============================================================================
// Testable ConfigStore subclass (no file parsing needed)
// ============================================================================

namespace {

class TestableConfig : public osp::ConfigStore {};

/// Shared config instance for all config tests.
/// Must call EnsureConfigRegistered() before using osp_config command.
TestableConfig& SharedTestConfig() {
  static TestableConfig cfg;
  return cfg;
}

void EnsureConfigRegistered() {
  static bool done = false;
  if (!done) {
    auto& cfg = SharedTestConfig();
    cfg.SetString("net", "port", "8080");
    cfg.SetString("net", "host", "127.0.0.1");
    cfg.SetString("log", "level", "3");
    osp::shell_cmd::RegisterConfig(cfg);
    done = true;
  }
}

/// Shared bus instance for bus control tests.
void EnsureBusRegistered() {
  static bool done = false;
  if (!done) {
    auto& bus = osp::AsyncBus<TestPayload>::Instance();
    osp::shell_cmd::RegisterBusStats(bus);
    done = true;
  }
}

/// Shared lifecycle node for lifecycle control tests.
void EnsureLifecycleRegistered() {
  static bool done = false;
  if (!done) {
    static osp::LifecycleNode<TestPayload> node("test_lc_ctrl");
    osp::shell_cmd::RegisterLifecycle(node);
    done = true;
  }
}

}  // namespace

// ============================================================================
// RegisterLog control command tests
// ============================================================================

TEST_CASE("shell_cmd RegisterLog: default shows level", "[shell_commands]") {
  osp::shell_cmd::RegisterLog();

  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd = osp::detail::GlobalCmdRegistry::Instance().Find("osp_log");
  REQUIRE(cmd != nullptr);

  // No-arg invocation should show current level.
  char arg0[] = "osp_log";
  char* argv[] = {arg0};
  int rc = cmd->func(1, argv);
  CHECK(rc == 0);
  auto output = mock.DrainOutput();
  CHECK(output.find("[osp_log]") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterLog: set level by number", "[shell_commands]") {
  osp::shell_cmd::RegisterLog();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd = osp::detail::GlobalCmdRegistry::Instance().Find("osp_log");
  REQUIRE(cmd != nullptr);

  auto prev = osp::log::GetLevel();

  char arg0[] = "osp_log";
  char arg1[] = "level";
  char arg2[] = "3";
  char* argv[] = {arg0, arg1, arg2};
  int rc = cmd->func(3, argv);
  CHECK(rc == 0);
  CHECK(osp::log::GetLevel() == osp::log::Level::kError);

  auto output = mock.DrainOutput();
  CHECK(output.find("ERROR") != std::string::npos);

  // Restore previous level.
  osp::log::SetLevel(prev);
}

TEST_CASE("shell_cmd RegisterLog: set level by name", "[shell_commands]") {
  osp::shell_cmd::RegisterLog();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd = osp::detail::GlobalCmdRegistry::Instance().Find("osp_log");
  REQUIRE(cmd != nullptr);

  auto prev = osp::log::GetLevel();

  char arg0[] = "osp_log";
  char arg1[] = "level";
  char arg2[] = "debug";
  char* argv[] = {arg0, arg1, arg2};
  int rc = cmd->func(3, argv);
  CHECK(rc == 0);
  CHECK(osp::log::GetLevel() == osp::log::Level::kDebug);

  osp::log::SetLevel(prev);
}

TEST_CASE("shell_cmd RegisterLog: invalid level", "[shell_commands]") {
  osp::shell_cmd::RegisterLog();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd = osp::detail::GlobalCmdRegistry::Instance().Find("osp_log");
  REQUIRE(cmd != nullptr);

  auto prev = osp::log::GetLevel();

  char arg0[] = "osp_log";
  char arg1[] = "level";
  char arg2[] = "99";
  char* argv[] = {arg0, arg1, arg2};
  int rc = cmd->func(3, argv);
  CHECK(rc == -1);

  // Level should be unchanged.
  CHECK(osp::log::GetLevel() == prev);
}

// ============================================================================
// RegisterConfig control command tests
// ============================================================================

TEST_CASE("shell_cmd RegisterConfig: list entries", "[shell_commands]") {
  EnsureConfigRegistered();

  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_config");
  REQUIRE(cmd != nullptr);

  // Default invocation lists all entries.
  char arg0[] = "osp_config";
  char* argv[] = {arg0};
  int rc = cmd->func(1, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("port") != std::string::npos);
  CHECK(output.find("8080") != std::string::npos);
  CHECK(output.find("level") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterConfig: get existing key", "[shell_commands]") {
  EnsureConfigRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_config");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_config";
  char arg1[] = "get";
  char arg2[] = "net";
  char arg3[] = "port";
  char* argv[] = {arg0, arg1, arg2, arg3};
  int rc = cmd->func(4, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("8080") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterConfig: get missing key", "[shell_commands]") {
  EnsureConfigRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_config");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_config";
  char arg1[] = "get";
  char arg2[] = "net";
  char arg3[] = "nonexistent";
  char* argv[] = {arg0, arg1, arg2, arg3};
  int rc = cmd->func(4, argv);
  CHECK(rc == -1);

  auto output = mock.DrainOutput();
  CHECK(output.find("not found") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterConfig: set and verify", "[shell_commands]") {
  EnsureConfigRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_config");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_config";
  char arg1[] = "set";
  char arg2[] = "net";
  char arg3[] = "port";
  char arg4[] = "9090";
  char* argv[] = {arg0, arg1, arg2, arg3, arg4};
  int rc = cmd->func(5, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("9090") != std::string::npos);
  CHECK(output.find("set") != std::string::npos);
}

// ============================================================================
// Upgraded RegisterBusStats tests
// ============================================================================

TEST_CASE("shell_cmd RegisterBusStats: no-arg backward compat",
          "[shell_commands]") {
  EnsureBusRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_bus");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_bus";
  char* argv[] = {arg0};
  int rc = cmd->func(1, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("published") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterBusStats: reset subcommand",
          "[shell_commands]") {
  EnsureBusRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_bus");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_bus";
  char arg1[] = "reset";
  char* argv[] = {arg0, arg1};
  int rc = cmd->func(2, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("reset") != std::string::npos);
}

// ============================================================================
// Upgraded RegisterLifecycle tests
// ============================================================================

TEST_CASE("shell_cmd RegisterLifecycle: status", "[shell_commands]") {
  EnsureLifecycleRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_lifecycle");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_lifecycle";
  char* argv[] = {arg0};
  int rc = cmd->func(1, argv);
  CHECK(rc == 0);

  auto output = mock.DrainOutput();
  CHECK(output.find("osp_lifecycle") != std::string::npos);
}

TEST_CASE("shell_cmd RegisterLifecycle: configure transition",
          "[shell_commands]") {
  EnsureLifecycleRegistered();
  MockSession mock;
  SessionGuard guard(mock);

  const auto* cmd =
      osp::detail::GlobalCmdRegistry::Instance().Find("osp_lifecycle");
  REQUIRE(cmd != nullptr);

  char arg0[] = "osp_lifecycle";
  char arg1[] = "configure";
  char* argv[] = {arg0, arg1};
  int rc = cmd->func(2, argv);
  // Result depends on current state of the LifecycleNode registered
  // in earlier tests. Just verify it runs without crash.
  (void)rc;

  auto output = mock.DrainOutput();
  CHECK(output.find("osp_lifecycle") != std::string::npos);
}
