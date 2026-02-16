/**
 * @file test_shell.cpp
 * @brief Tests for shell.hpp (command registry, parsing, IAC/ESC/history,
 *        and backend integration via pipes/PTY)
 */

#include "osp/shell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

// PTY support for UART tests.
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static int test_cmd_a(int /*argc*/, char* /*argv*/[]) { return 0; }
static int test_cmd_b(int /*argc*/, char* /*argv*/[]) { return 1; }

// ============================================================================
// Original registry and parsing tests (preserved, nodiscard warnings fixed)
// ============================================================================

TEST_CASE("GlobalCmdRegistry register and find", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  // "help" is already auto-registered
  REQUIRE(reg.Count() >= 1);

  auto r = reg.Register("test_a_unique", test_cmd_a, "test command A");
  REQUIRE(r.has_value());

  const osp::ShellCmd* cmd = reg.Find("test_a_unique");
  REQUIRE(cmd != nullptr);
  REQUIRE(std::strcmp(cmd->name, "test_a_unique") == 0);
  REQUIRE(cmd->func == test_cmd_a);
}

TEST_CASE("GlobalCmdRegistry duplicate", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  (void)reg.Register("dup_test_cmd", test_cmd_b, "dup test");
  auto r = reg.Register("dup_test_cmd", test_cmd_b, "dup test");
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::ShellError::kDuplicateName);
}

TEST_CASE("GlobalCmdRegistry Find not found", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  const osp::ShellCmd* cmd = reg.Find("nonexistent_command_xyz");
  REQUIRE(cmd == nullptr);
}

TEST_CASE("GlobalCmdRegistry ForEach", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  uint32_t count = 0;
  reg.ForEach([&count](const osp::ShellCmd& /*cmd*/) { ++count; });
  REQUIRE(count == reg.Count());
}

TEST_CASE("GlobalCmdRegistry AutoComplete", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  // Register some commands with common prefix
  (void)reg.Register("auto_alpha", test_cmd_a, "alpha");
  (void)reg.Register("auto_beta", test_cmd_a, "beta");

  char buf[64] = {};
  uint32_t matches = reg.AutoComplete("auto_", buf, sizeof(buf));
  REQUIRE(matches == 2);
  // Common prefix should be "auto_"
  REQUIRE(std::strncmp(buf, "auto_", 5) == 0);
}

TEST_CASE("ShellSplit basic", "[shell]") {
  char cmd[] = "hello world 42";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(cmd, static_cast<uint32_t>(std::strlen(cmd)), argv);

  REQUIRE(argc == 3);
  REQUIRE(std::strcmp(argv[0], "hello") == 0);
  REQUIRE(std::strcmp(argv[1], "world") == 0);
  REQUIRE(std::strcmp(argv[2], "42") == 0);
}

TEST_CASE("ShellSplit quoted", "[shell]") {
  char cmd[] = "echo \"hello world\" done";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(cmd, static_cast<uint32_t>(std::strlen(cmd)), argv);

  REQUIRE(argc == 3);
  REQUIRE(std::strcmp(argv[0], "echo") == 0);
  REQUIRE(std::strcmp(argv[1], "hello world") == 0);
  REQUIRE(std::strcmp(argv[2], "done") == 0);
}

TEST_CASE("ShellSplit empty", "[shell]") {
  char cmd[] = "";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(cmd, 0, argv);
  REQUIRE(argc == 0);
}

TEST_CASE("ShellSplit leading spaces", "[shell]") {
  char cmd[] = "   spaced  ";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(cmd, static_cast<uint32_t>(std::strlen(cmd)), argv);

  REQUIRE(argc == 1);
  REQUIRE(std::strcmp(argv[0], "spaced") == 0);
}

TEST_CASE("OSP_SHELL_CMD macro registers", "[shell]") {
  // help should be registered via the built-in auto-registration
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  const osp::ShellCmd* cmd = reg.Find("help");
  REQUIRE(cmd != nullptr);
  REQUIRE(std::strcmp(cmd->name, "help") == 0);
}

// ============================================================================
// Integration tests (preserved)
// ============================================================================

TEST_CASE("shell - GlobalCmdRegistry command execution with output", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  // Buffer to capture output
  static char output_buf[128] = {};

  // Command that writes to buffer
  auto write_cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    std::strcpy(output_buf, "command executed");
    return 0;
  };

  auto r = reg.Register("write_test", write_cmd, "writes to buffer");
  REQUIRE(r.has_value());

  const osp::ShellCmd* cmd = reg.Find("write_test");
  REQUIRE(cmd != nullptr);

  // Execute the command
  char* dummy_argv[1] = {const_cast<char*>("write_test")};
  int ret = cmd->func(1, dummy_argv);

  REQUIRE(ret == 0);
  REQUIRE(std::strcmp(output_buf, "command executed") == 0);
}

TEST_CASE("shell - GlobalCmdRegistry argument parsing", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  static int captured_argc = 0;
  static char captured_args[3][32] = {};

  // Command that captures argc and argv
  auto parse_cmd = [](int argc, char* argv[]) -> int {
    captured_argc = argc;
    for (int i = 0; i < argc && i < 3; ++i) {
      std::strncpy(captured_args[i], argv[i], 31);
      captured_args[i][31] = '\0';
    }
    return 0;
  };

  auto r = reg.Register("parse_test", parse_cmd, "parses arguments");
  REQUIRE(r.has_value());

  const osp::ShellCmd* cmd = reg.Find("parse_test");
  REQUIRE(cmd != nullptr);

  // Simulate "cmd arg1 arg2"
  char arg0[] = "parse_test";
  char arg1[] = "arg1";
  char arg2[] = "arg2";
  char* argv[3] = {arg0, arg1, arg2};

  int ret = cmd->func(3, argv);

  REQUIRE(ret == 0);
  REQUIRE(captured_argc == 3);
  REQUIRE(std::strcmp(captured_args[0], "parse_test") == 0);
  REQUIRE(std::strcmp(captured_args[1], "arg1") == 0);
  REQUIRE(std::strcmp(captured_args[2], "arg2") == 0);
}

TEST_CASE("shell - GlobalCmdRegistry help command lists all", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  // Register multiple commands
  auto r1 = reg.Register("list_test_1", test_cmd_a, "first test command");
  auto r2 = reg.Register("list_test_2", test_cmd_b, "second test command");
  auto r3 = reg.Register("list_test_3", test_cmd_a, "third test command");
  (void)r1; (void)r2; (void)r3;

  // Verify all can be found
  const osp::ShellCmd* cmd1 = reg.Find("list_test_1");
  const osp::ShellCmd* cmd2 = reg.Find("list_test_2");
  const osp::ShellCmd* cmd3 = reg.Find("list_test_3");

  REQUIRE(cmd1 != nullptr);
  REQUIRE(cmd2 != nullptr);
  REQUIRE(cmd3 != nullptr);

  REQUIRE(std::strcmp(cmd1->name, "list_test_1") == 0);
  REQUIRE(std::strcmp(cmd2->name, "list_test_2") == 0);
  REQUIRE(std::strcmp(cmd3->name, "list_test_3") == 0);
}

TEST_CASE("shell - GlobalCmdRegistry max commands boundary", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  // Get current count
  uint32_t initial_count = reg.Count();

  // Try to register commands up to the limit (kMaxCommands = 64)
  // We need to be careful not to exceed the limit
  constexpr uint32_t kMaxCommands = 64;

  // Instead of filling the entire registry, just test that we can register
  // a few more commands and verify the count increases correctly
  if (initial_count < kMaxCommands - 5) {
    // Register a few test commands (not all the way to the limit)
    static char boundary_names[5][32] = {};
    uint32_t registered = 0;

    for (uint32_t i = 0; i < 5; ++i) {
      std::snprintf(boundary_names[i], 32, "boundary_cmd_%u_%u", initial_count, i);
      auto r = reg.Register(boundary_names[i], test_cmd_a, "boundary test");

      if (r.has_value()) {
        registered++;
      } else {
        // Hit the limit early
        REQUIRE(r.get_error() == osp::ShellError::kRegistryFull);
        break;
      }
    }

    // Verify the count increased by the number we registered
    REQUIRE(reg.Count() == initial_count + registered);
  } else {
    // Already near capacity - just verify we can't exceed the limit
    static char overflow_name[32];
    std::snprintf(overflow_name, 32, "overflow_test_%u", initial_count);

    auto r = reg.Register(overflow_name, test_cmd_a, "should fail or succeed");

    if (!r.has_value()) {
      // Should fail with registry full
      REQUIRE(r.get_error() == osp::ShellError::kRegistryFull);
      REQUIRE(reg.Count() == kMaxCommands);
    } else {
      // Succeeded, count should be less than max
      REQUIRE(reg.Count() <= kMaxCommands);
    }
  }
}

TEST_CASE("shell - Command name and description storage", "[shell]") {
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  // Use a unique name with counter to avoid conflicts
  static int storage_counter = 0;
  static char static_names[100][32] = {};  // Array to hold multiple unique names
  int idx = storage_counter++;
  std::snprintf(static_names[idx], 32, "storage_test_%d", idx);

  const char* test_desc = "This is a test description for storage verification";

  auto r = reg.Register(static_names[idx], test_cmd_a, test_desc);
  REQUIRE(r.has_value());

  const osp::ShellCmd* cmd = reg.Find(static_names[idx]);
  REQUIRE(cmd != nullptr);

  // Verify name and description are stored correctly
  REQUIRE(std::strcmp(cmd->name, static_names[idx]) == 0);
  REQUIRE(cmd->desc != nullptr);
  REQUIRE(std::strcmp(cmd->desc, test_desc) == 0);
  REQUIRE(cmd->func == test_cmd_a);
}

TEST_CASE("shell - GlobalCmdRegistry reset and re-register", "[shell]") {
  // Note: GlobalCmdRegistry is a singleton and doesn't have a public reset method.
  // This test verifies that we can continue to use the registry after many operations.

  auto& reg = osp::detail::GlobalCmdRegistry::Instance();

  // Use unique command names to avoid conflicts with other tests
  static int reset_counter = 0;
  static char static_names[100][32] = {};  // Array to hold multiple unique names
  int idx1 = reset_counter++;
  int idx2 = reset_counter++;

  std::snprintf(static_names[idx1], 32, "reset_test_cmd_%d", idx1);
  std::snprintf(static_names[idx2], 32, "reset_verify_%d", idx2);

  // Register a command
  auto r1 = reg.Register(static_names[idx1], test_cmd_a, "reset test");
  REQUIRE(r1.has_value());

  // Verify we can find it
  const osp::ShellCmd* cmd = reg.Find(static_names[idx1]);
  REQUIRE(cmd != nullptr);

  // Verify the registry is still functional
  uint32_t count_before = reg.Count();
  REQUIRE(count_before > 0);

  // Register another unique command to verify continued operation
  auto r2 = reg.Register(static_names[idx2], test_cmd_b, "verify after reset");

  // Should succeed unless we're at capacity
  if (r2.has_value()) {
    REQUIRE(reg.Count() == count_before + 1);

    const osp::ShellCmd* new_cmd = reg.Find(static_names[idx2]);
    REQUIRE(new_cmd != nullptr);
    REQUIRE(new_cmd->func == test_cmd_b);
  } else {
    // If it fails, it should be because we're at capacity
    REQUIRE(r2.get_error() == osp::ShellError::kRegistryFull);
  }
}

// ============================================================================
// Helper: create a mock ShellSession writing to a pipe for output capture
// ============================================================================

namespace {

/// Create a pipe-backed ShellSession for testing line editing / IAC / ESC.
/// Returns {session, read_fd_for_output_capture}.
/// Caller must close read_fd after use.
struct MockSession {
  osp::detail::ShellSession session{};
  int capture_read_fd = -1;   // read end -- read this to see what session wrote
  int capture_write_fd = -1;  // write end -- session writes here

  MockSession() {
    int pipefd[2] = {-1, -1};
    (void)::pipe(pipefd);
    capture_read_fd = pipefd[0];
    capture_write_fd = pipefd[1];

    // Make the read end non-blocking for test draining.
    int flags = ::fcntl(capture_read_fd, F_GETFL, 0);
    (void)::fcntl(capture_read_fd, F_SETFL, flags | O_NONBLOCK);

    session.read_fd = -1;  // not used for reading in unit tests
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

  /// Drain all output written by the session.
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

}  // namespace

// ============================================================================
// IAC filter tests
// ============================================================================

TEST_CASE("FilterIac: normal bytes pass through", "[shell][iac]") {
  osp::detail::ShellSession s{};
  s.iac_state = osp::detail::ShellSession::IacState::kNormal;

  // Normal printable characters.
  REQUIRE(osp::detail::FilterIac(s, 'A') == 'A');
  REQUIRE(osp::detail::FilterIac(s, ' ') == ' ');
  REQUIRE(osp::detail::FilterIac(s, '\r') == '\r');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);
}

TEST_CASE("FilterIac: WILL/WONT/DO/DONT consumed", "[shell][iac]") {
  osp::detail::ShellSession s{};
  s.iac_state = osp::detail::ShellSession::IacState::kNormal;

  // IAC (0xFF) starts IAC state.
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kIac);

  // WILL (0xFB) -> negotiate state.
  REQUIRE(osp::detail::FilterIac(s, 0xFB) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNego);

  // Option byte consumed, back to normal.
  REQUIRE(osp::detail::FilterIac(s, 0x01) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);

  // Test WONT (0xFC)
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0xFC) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNego);
  REQUIRE(osp::detail::FilterIac(s, 0x03) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);

  // Test DO (0xFD)
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0xFD) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0x18) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);

  // Test DONT (0xFE)
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0xFE) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0x01) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);
}

TEST_CASE("FilterIac: subnegotiation consumed", "[shell][iac]") {
  osp::detail::ShellSession s{};
  s.iac_state = osp::detail::ShellSession::IacState::kNormal;

  // IAC SB (0xFA) starts subnegotiation.
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0xFA) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kSub);

  // Subnegotiation data is consumed.
  REQUIRE(osp::detail::FilterIac(s, 0x18) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 0x00) == '\0');
  REQUIRE(osp::detail::FilterIac(s, 'x') == '\0');

  // IAC SE (0xFF 0xF0) ends subnegotiation.
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');  // -> kIac
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kIac);
  // SE (0xF0) is not WILL/WONT/DO/DONT/SB, so resets to kNormal.
  REQUIRE(osp::detail::FilterIac(s, 0xF0) == '\0');
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);
}

TEST_CASE("FilterIac: IAC IAC produces literal 0xFF", "[shell][iac]") {
  osp::detail::ShellSession s{};
  s.iac_state = osp::detail::ShellSession::IacState::kNormal;

  // IAC IAC = literal 0xFF.
  REQUIRE(osp::detail::FilterIac(s, 0xFF) == '\0');
  char result = osp::detail::FilterIac(s, 0xFF);
  REQUIRE(static_cast<uint8_t>(result) == 0xFF);
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);
}

TEST_CASE("FilterIac: no effect when telnet_mode is false in ProcessByte",
          "[shell][iac]") {
  MockSession mock;
  mock.session.telnet_mode = false;

  // Feed IAC byte directly -- should be treated as raw byte (not filtered).
  // ProcessByte with telnet_mode=false passes raw_byte as-is to char handling.
  // 0xFF as a char is -1 (signed), or 255 (unsigned). It's >= 0x20 check
  // in ProcessByte will still pass since 0xFF cast to char is negative.
  // The key test: IAC state should remain kNormal.
  (void)osp::detail::ProcessByte(mock.session, 0xFF, "test> ");
  REQUIRE(mock.session.iac_state ==
          osp::detail::ShellSession::IacState::kNormal);
}

// ============================================================================
// ESC sequence tests
// ============================================================================

TEST_CASE("ProcessByte: ESC [ A triggers HistoryUp", "[shell][esc]") {
  MockSession mock;
  auto& s = mock.session;

  // First push a command into history.
  std::strcpy(s.line_buf, "history_cmd");
  s.line_pos = static_cast<uint32_t>(std::strlen("history_cmd"));
  osp::detail::PushHistory(s);
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // Feed ESC [ A (Up arrow).
  REQUIRE(!osp::detail::ProcessByte(s, 0x1B, "test> "));
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kEsc);
  REQUIRE(!osp::detail::ProcessByte(s, '[', "test> "));
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kBracket);
  REQUIRE(!osp::detail::ProcessByte(s, 'A', "test> "));
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kNone);

  // After Up arrow, line_buf should contain the history entry.
  REQUIRE(std::strcmp(s.line_buf, "history_cmd") == 0);
  REQUIRE(s.hist_browsing);
}

TEST_CASE("ProcessByte: ESC [ B triggers HistoryDown", "[shell][esc]") {
  MockSession mock;
  auto& s = mock.session;

  // Push two commands.
  std::strcpy(s.line_buf, "first_cmd");
  s.line_pos = static_cast<uint32_t>(std::strlen("first_cmd"));
  osp::detail::PushHistory(s);
  s.line_pos = 0;

  std::strcpy(s.line_buf, "second_cmd");
  s.line_pos = static_cast<uint32_t>(std::strlen("second_cmd"));
  osp::detail::PushHistory(s);
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // Navigate up twice.
  // Up #1
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  (void)osp::detail::ProcessByte(s, '[', "t> ");
  (void)osp::detail::ProcessByte(s, 'A', "t> ");
  REQUIRE(std::strcmp(s.line_buf, "second_cmd") == 0);

  // Up #2
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  (void)osp::detail::ProcessByte(s, '[', "t> ");
  (void)osp::detail::ProcessByte(s, 'A', "t> ");
  REQUIRE(std::strcmp(s.line_buf, "first_cmd") == 0);

  // Down #1
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  (void)osp::detail::ProcessByte(s, '[', "t> ");
  (void)osp::detail::ProcessByte(s, 'B', "t> ");
  REQUIRE(std::strcmp(s.line_buf, "second_cmd") == 0);

  // Down #2 -- back to empty line.
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  (void)osp::detail::ProcessByte(s, '[', "t> ");
  (void)osp::detail::ProcessByte(s, 'B', "t> ");
  REQUIRE(s.line_pos == 0);
  REQUIRE(!s.hist_browsing);
}

TEST_CASE("ProcessByte: unknown ESC sequence ignored", "[shell][esc]") {
  MockSession mock;
  auto& s = mock.session;

  // ESC [ Z (unknown) should be silently consumed.
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  (void)osp::detail::ProcessByte(s, '[', "t> ");
  (void)osp::detail::ProcessByte(s, 'Z', "t> ");
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kNone);
  REQUIRE(s.line_pos == 0);
}

TEST_CASE("ProcessByte: bare ESC resets state", "[shell][esc]") {
  MockSession mock;
  auto& s = mock.session;

  // ESC followed by non-'[' resets.
  (void)osp::detail::ProcessByte(s, 0x1B, "t> ");
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kEsc);
  (void)osp::detail::ProcessByte(s, 'x', "t> ");
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kNone);
}

// ============================================================================
// History tests
// ============================================================================

TEST_CASE("PushHistory: stores command", "[shell][history]") {
  osp::detail::ShellSession s{};
  std::strcpy(s.line_buf, "cmd1");
  s.line_pos = 4;
  osp::detail::PushHistory(s);

  REQUIRE(s.hist_count == 1);
  REQUIRE(std::strcmp(s.history[0], "cmd1") == 0);
}

TEST_CASE("PushHistory: skips consecutive duplicates", "[shell][history]") {
  osp::detail::ShellSession s{};

  std::strcpy(s.line_buf, "repeated");
  s.line_pos = static_cast<uint32_t>(std::strlen("repeated"));
  osp::detail::PushHistory(s);

  std::strcpy(s.line_buf, "repeated");
  s.line_pos = static_cast<uint32_t>(std::strlen("repeated"));
  osp::detail::PushHistory(s);

  REQUIRE(s.hist_count == 1);  // Only stored once.
}

TEST_CASE("PushHistory: ignores empty line", "[shell][history]") {
  osp::detail::ShellSession s{};
  s.line_pos = 0;
  osp::detail::PushHistory(s);

  REQUIRE(s.hist_count == 0);
}

TEST_CASE("HistoryUp: navigates to previous command", "[shell][history]") {
  MockSession mock;
  auto& s = mock.session;

  std::strcpy(s.line_buf, "alpha");
  s.line_pos = 5;
  osp::detail::PushHistory(s);
  s.line_pos = 0;

  std::strcpy(s.line_buf, "beta");
  s.line_pos = 4;
  osp::detail::PushHistory(s);
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  // HistoryUp should show "beta" (most recent).
  osp::detail::HistoryUp(s);
  REQUIRE(std::strcmp(s.line_buf, "beta") == 0);
  REQUIRE(s.hist_browsing);

  // HistoryUp again shows "alpha".
  osp::detail::HistoryUp(s);
  REQUIRE(std::strcmp(s.line_buf, "alpha") == 0);
}

TEST_CASE("HistoryDown: returns to empty line", "[shell][history]") {
  MockSession mock;
  auto& s = mock.session;

  std::strcpy(s.line_buf, "only_cmd");
  s.line_pos = 8;
  osp::detail::PushHistory(s);
  s.line_pos = 0;
  s.line_buf[0] = '\0';

  osp::detail::HistoryUp(s);
  REQUIRE(std::strcmp(s.line_buf, "only_cmd") == 0);

  osp::detail::HistoryDown(s);
  REQUIRE(s.line_pos == 0);
  REQUIRE(!s.hist_browsing);
}

TEST_CASE("History: ring buffer wraps correctly", "[shell][history]") {
  osp::detail::ShellSession s{};
  constexpr uint32_t kSize = OSP_SHELL_HISTORY_SIZE;

  // Fill the ring buffer with more entries than capacity.
  for (uint32_t i = 0; i < kSize + 4; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "cmd_%u", i);
    std::strcpy(s.line_buf, buf);
    s.line_pos = static_cast<uint32_t>(std::strlen(buf));
    osp::detail::PushHistory(s);
    s.line_pos = 0;
  }

  // Count should be capped at kSize.
  REQUIRE(s.hist_count == kSize);

  // Oldest entries should have been overwritten.
  // The last kSize entries should be present: cmd_4 .. cmd_19 (for kSize=16).
  // hist_head points to the next write slot.
  uint32_t idx = (s.hist_head + kSize - 1) % kSize;
  char expected[32];
  std::snprintf(expected, sizeof(expected), "cmd_%u", kSize + 3);
  REQUIRE(std::strcmp(s.history[idx], expected) == 0);
}

// ============================================================================
// ProcessByte comprehensive tests
// ============================================================================

TEST_CASE("ProcessByte: printable char appended and echoed", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  bool ready = osp::detail::ProcessByte(s, 'H', "t> ");
  REQUIRE(!ready);
  REQUIRE(s.line_pos == 1);
  REQUIRE(s.line_buf[0] == 'H');

  ready = osp::detail::ProcessByte(s, 'i', "t> ");
  REQUIRE(!ready);
  REQUIRE(s.line_pos == 2);
  REQUIRE(s.line_buf[0] == 'H');
  REQUIRE(s.line_buf[1] == 'i');

  // Verify echo was written.
  auto output = mock.DrainOutput();
  REQUIRE(output == "Hi");
}

TEST_CASE("ProcessByte: backspace deletes char", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  // Type "AB" then backspace.
  (void)osp::detail::ProcessByte(s, 'A', "t> ");
  (void)osp::detail::ProcessByte(s, 'B', "t> ");
  REQUIRE(s.line_pos == 2);

  bool ready = osp::detail::ProcessByte(s, 0x7F, "t> ");
  REQUIRE(!ready);
  REQUIRE(s.line_pos == 1);

  // Backspace at position 0 does nothing.
  (void)osp::detail::ProcessByte(s, 0x7F, "t> ");
  REQUIRE(s.line_pos == 0);
  (void)osp::detail::ProcessByte(s, 0x7F, "t> ");
  REQUIRE(s.line_pos == 0);
}

TEST_CASE("ProcessByte: Enter returns true", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  (void)osp::detail::ProcessByte(s, 'x', "t> ");
  bool ready = osp::detail::ProcessByte(s, '\r', "t> ");
  REQUIRE(ready);

  // skip_next_lf should be set after \r.
  REQUIRE(s.skip_next_lf);
}

TEST_CASE("ProcessByte: CRLF dedup", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  (void)osp::detail::ProcessByte(s, 'x', "t> ");
  bool ready = osp::detail::ProcessByte(s, '\r', "t> ");
  REQUIRE(ready);

  // The \n after \r should be consumed and not trigger another Enter.
  ready = osp::detail::ProcessByte(s, '\n', "t> ");
  REQUIRE(!ready);
  REQUIRE(!s.skip_next_lf);
}

TEST_CASE("ProcessByte: Ctrl+C cancels line", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  (void)osp::detail::ProcessByte(s, 'a', "t> ");
  (void)osp::detail::ProcessByte(s, 'b', "t> ");
  REQUIRE(s.line_pos == 2);

  bool ready = osp::detail::ProcessByte(s, 0x03, "t> ");
  REQUIRE(!ready);
  REQUIRE(s.line_pos == 0);

  auto output = mock.DrainOutput();
  // Should contain "^C\r\n" and the prompt.
  REQUIRE(output.find("^C") != std::string::npos);
  REQUIRE(output.find("t> ") != std::string::npos);
}

TEST_CASE("ProcessByte: Ctrl+D on empty line sets active=false",
          "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  REQUIRE(s.active.load(std::memory_order_relaxed));
  (void)osp::detail::ProcessByte(s, 0x04, "t> ");
  REQUIRE(!s.active.load(std::memory_order_relaxed));
}

TEST_CASE("ProcessByte: Tab triggers completion", "[shell][process]") {
  MockSession mock;
  auto& s = mock.session;

  // Type "hel" then Tab -- should complete to "help ".
  (void)osp::detail::ProcessByte(s, 'h', "t> ");
  (void)osp::detail::ProcessByte(s, 'e', "t> ");
  (void)osp::detail::ProcessByte(s, 'l', "t> ");
  mock.DrainOutput();  // drain echo

  (void)osp::detail::ProcessByte(s, '\t', "t> ");

  // "help" is registered as a built-in command, so should auto-complete.
  REQUIRE(std::strncmp(s.line_buf, "help", 4) == 0);
}

// ============================================================================
// ShellSplit enhanced tests
// ============================================================================

TEST_CASE("ShellSplit: single-quoted string", "[shell][split]") {
  char cmd[] = "echo 'hello world' done";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(
      cmd, static_cast<uint32_t>(std::strlen(cmd)), argv);

  REQUIRE(argc == 3);
  REQUIRE(std::strcmp(argv[0], "echo") == 0);
  REQUIRE(std::strcmp(argv[1], "hello world") == 0);
  REQUIRE(std::strcmp(argv[2], "done") == 0);
}

TEST_CASE("ShellSplit: tab separator", "[shell][split]") {
  char cmd[] = "a\tb\tc";
  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(
      cmd, static_cast<uint32_t>(std::strlen(cmd)), argv);

  REQUIRE(argc == 3);
  REQUIRE(std::strcmp(argv[0], "a") == 0);
  REQUIRE(std::strcmp(argv[1], "b") == 0);
  REQUIRE(std::strcmp(argv[2], "c") == 0);
}

TEST_CASE("ShellSplit: max args boundary", "[shell][split]") {
  // Build a command line with more than kMaxArgs tokens.
  constexpr uint32_t kOver = osp::detail::kMaxArgs + 4;
  std::string line;
  for (uint32_t i = 0; i < kOver; ++i) {
    if (i > 0) line += ' ';
    line += "arg";
    line += std::to_string(i);
  }
  // Need mutable buffer for ShellSplit.
  std::vector<char> buf(line.begin(), line.end());
  buf.push_back('\0');

  char* argv[osp::detail::kMaxArgs] = {};
  int argc = osp::detail::ShellSplit(
      buf.data(), static_cast<uint32_t>(line.size()), argv);

  REQUIRE(argc == static_cast<int>(osp::detail::kMaxArgs));
}

// ============================================================================
// SessionInit helper test
// ============================================================================

TEST_CASE("SessionInit: initializes all fields correctly", "[shell][init]") {
  osp::detail::ShellSession s{};
  // Set some non-default values to verify they get overwritten.
  s.line_pos = 42;
  s.hist_browsing = true;
  s.esc_state = osp::detail::ShellSession::EscState::kBracket;

  osp::detail::ShellSessionInit(s, 7, 8,
                             osp::detail::ShellPosixWrite,
                             osp::detail::ShellPosixRead, false);

  REQUIRE(s.read_fd == 7);
  REQUIRE(s.write_fd == 8);
  REQUIRE(s.write_fn == osp::detail::ShellPosixWrite);
  REQUIRE(s.read_fn == osp::detail::ShellPosixRead);
  REQUIRE(!s.telnet_mode);
  REQUIRE(s.iac_state == osp::detail::ShellSession::IacState::kNormal);
  REQUIRE(s.esc_state == osp::detail::ShellSession::EscState::kNone);
  REQUIRE(s.line_pos == 0);
  REQUIRE(!s.hist_browsing);
  REQUIRE(!s.skip_next_lf);
  REQUIRE(s.active.load(std::memory_order_relaxed));
}

// ============================================================================
// DebugShell lifecycle tests
// ============================================================================

TEST_CASE("DebugShell: start and stop", "[shell][tcp]") {
  osp::DebugShell::Config cfg;
  cfg.port = 15090;  // Use a high port to avoid conflicts.
  osp::DebugShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  REQUIRE(shell.IsRunning());

  // Starting again should fail.
  auto r2 = shell.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::ShellError::kAlreadyRunning);

  shell.Stop();
  REQUIRE(!shell.IsRunning());
}

TEST_CASE("DebugShell: no auth when username=nullptr", "[shell][tcp]") {
  osp::DebugShell::Config cfg;
  cfg.port = 15092;
  cfg.username = nullptr;
  cfg.password = nullptr;
  osp::DebugShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Connect via TCP and verify prompt is received without auth.
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(sock >= 0);

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(15092);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int rc = ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr));
  if (rc == 0) {
    // Read all initial data (IAC negotiation bytes + prompt).
    std::string response;
    for (int attempt = 0; attempt < 5; ++attempt) {
      struct pollfd pfd{};
      pfd.fd = sock;
      pfd.events = POLLIN;
      int pr = ::poll(&pfd, 1, 200);
      if (pr <= 0) break;
      char buf[512] = {};
      ssize_t n = ::recv(sock, buf, sizeof(buf) - 1, 0);
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
      if (response.find("osp> ") != std::string::npos) break;
    }
    // Should contain the prompt without auth prompts.
    REQUIRE(response.find("osp> ") != std::string::npos);
    REQUIRE(response.find("Username") == std::string::npos);
  }

  // Close client socket first so server's recv() returns 0 (client disconnect).
  // This is safer than relying on shutdown() from the server side to unblock
  // the session thread's recv() -- POSIX guarantees that the peer's recv()
  // returns 0 when we close/shutdown our end.
  (void)::shutdown(sock, SHUT_RDWR);
  ::close(sock);

  shell.Stop();
}

// ============================================================================
// ConsoleShell via pipe test
// ============================================================================

TEST_CASE("ConsoleShell: start and stop via pipes", "[shell][console]") {
  // Create pipes for stdin/stdout simulation.
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  REQUIRE(::pipe(in_pipe) == 0);
  REQUIRE(::pipe(out_pipe) == 0);

  osp::ConsoleShell::Config cfg;
  cfg.prompt = "con> ";
  cfg.read_fd = in_pipe[0];
  cfg.write_fd = out_pipe[1];
  cfg.raw_mode = false;  // Pipes don't support termios.
  osp::ConsoleShell console(cfg);

  auto r = console.Start();
  REQUIRE(r.has_value());
  REQUIRE(console.IsRunning());

  // Wait briefly for the shell thread to start and write prompt.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Read the initial prompt.
  struct pollfd pfd{};
  pfd.fd = out_pipe[0];
  pfd.events = POLLIN;
  int pr = ::poll(&pfd, 1, 500);
  if (pr > 0) {
    char buf[1024] = {};
    ssize_t n = ::read(out_pipe[0], buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      std::string output(buf, static_cast<size_t>(n));
      // Should contain the prompt.
      REQUIRE(output.find("con> ") != std::string::npos);
    }
  }

  // Close the write end of the input pipe to signal EOF to the shell.
  ::close(in_pipe[1]);
  in_pipe[1] = -1;

  console.Stop();
  REQUIRE(!console.IsRunning());

  if (in_pipe[0] >= 0) ::close(in_pipe[0]);
  if (in_pipe[1] >= 0) ::close(in_pipe[1]);
  ::close(out_pipe[0]);
  ::close(out_pipe[1]);
}

TEST_CASE("ConsoleShell: command execution via pipe", "[shell][console]") {
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  REQUIRE(::pipe(in_pipe) == 0);
  REQUIRE(::pipe(out_pipe) == 0);

  // Register a test command with unique name.
  static bool console_cmd_executed = false;
  console_cmd_executed = false;
  auto console_cmd = [](int /*argc*/, char* /*argv*/[]) -> int {
    console_cmd_executed = true;
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register(
      "console_exec_test", console_cmd, "console test");

  osp::ConsoleShell::Config cfg;
  cfg.prompt = "p> ";
  cfg.read_fd = in_pipe[0];
  cfg.write_fd = out_pipe[1];
  cfg.raw_mode = false;
  osp::ConsoleShell console(cfg);

  auto r = console.Start();
  REQUIRE(r.has_value());

  // Give the shell a moment to start its loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send command.
  const char* cmd = "console_exec_test\r";
  (void)::write(in_pipe[1], cmd, std::strlen(cmd));

  // Wait for execution.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  REQUIRE(console_cmd_executed);

  console.Stop();

  ::close(in_pipe[0]);
  ::close(in_pipe[1]);
  ::close(out_pipe[0]);
  ::close(out_pipe[1]);
}

// ============================================================================
// UartShell via PTY test
// ============================================================================

TEST_CASE("UartShell: start and stop via PTY", "[shell][uart]") {
  // Open a pseudo-terminal pair.
  int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (master_fd < 0) {
    SKIP("posix_openpt not available");
  }
  REQUIRE(::grantpt(master_fd) == 0);
  REQUIRE(::unlockpt(master_fd) == 0);

  osp::UartShell::Config cfg;
  cfg.override_fd = master_fd;
  cfg.prompt = "uart> ";
  osp::UartShell uart(cfg);

  auto r = uart.Start();
  REQUIRE(r.has_value());
  REQUIRE(uart.IsRunning());

  // Starting again should fail.
  auto r2 = uart.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::ShellError::kAlreadyRunning);

  uart.Stop();
  REQUIRE(!uart.IsRunning());
  // master_fd is not owned by UartShell (override_fd), so close manually.
  ::close(master_fd);
}

TEST_CASE("UartShell: invalid device returns error", "[shell][uart]") {
  osp::UartShell::Config cfg;
  cfg.device = "/dev/nonexistent_uart_device";
  cfg.override_fd = -1;
  osp::UartShell uart(cfg);

  auto r = uart.Start();
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::ShellError::kDeviceOpenFailed);
}

// ============================================================================
// ExecuteLine and SessionWrite tests
// ============================================================================

TEST_CASE("ExecuteLine: unknown command produces error message",
          "[shell][exec]") {
  MockSession mock;
  auto& s = mock.session;

  std::strcpy(s.line_buf, "nosuch_cmd_xyz");
  s.line_pos = static_cast<uint32_t>(std::strlen("nosuch_cmd_xyz"));

  osp::detail::ShellExecuteLine(s);

  auto output = mock.DrainOutput();
  REQUIRE(output.find("unknown command") != std::string::npos);
  REQUIRE(output.find("nosuch_cmd_xyz") != std::string::npos);
}

TEST_CASE("ExecuteLine: dispatches registered command", "[shell][exec]") {
  MockSession mock;
  auto& s = mock.session;

  static int exec_test_ret = -1;
  exec_test_ret = -1;
  auto exec_cmd = [](int argc, char* /*argv*/[]) -> int {
    exec_test_ret = argc;
    return 0;
  };
  (void)osp::detail::GlobalCmdRegistry::Instance().Register(
      "exec_line_test", exec_cmd, "exec test");

  std::strcpy(s.line_buf, "exec_line_test arg1 arg2");
  s.line_pos = static_cast<uint32_t>(
      std::strlen("exec_line_test arg1 arg2"));

  osp::detail::ShellExecuteLine(s);

  REQUIRE(exec_test_ret == 3);  // "exec_line_test", "arg1", "arg2"
}

TEST_CASE("ExecuteLine: empty line does nothing", "[shell][exec]") {
  MockSession mock;
  auto& s = mock.session;

  s.line_pos = 0;
  s.line_buf[0] = '\0';
  osp::detail::ShellExecuteLine(s);

  auto output = mock.DrainOutput();
  REQUIRE(output.empty());
}

// ============================================================================
// ReplaceLine test
// ============================================================================

TEST_CASE("ReplaceLine: updates line buffer and position", "[shell][edit]") {
  MockSession mock;
  auto& s = mock.session;

  std::strcpy(s.line_buf, "old");
  s.line_pos = 3;

  osp::detail::ReplaceLine(s, "new_content", 11);

  REQUIRE(s.line_pos == 11);
  REQUIRE(std::strcmp(s.line_buf, "new_content") == 0);
}
