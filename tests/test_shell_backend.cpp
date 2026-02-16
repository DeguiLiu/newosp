/**
 * @file test_shell_backend.cpp
 * @brief Unit tests for ConsoleShell and UartShell backends (shell.hpp).
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/shell.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <pty.h>
#include <sys/select.h>

// ---------------------------------------------------------------------------
// Helper: non-blocking read with timeout, check if output contains needle
// ---------------------------------------------------------------------------
static bool ReadContains(int fd, const char* needle, int timeout_ms = 300) {
  char buf[512] = {};
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (select(fd + 1, &fds, nullptr, nullptr, &tv) > 0) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      return std::strstr(buf, needle) != nullptr;
    }
  }
  return false;
}

/// @brief Drain all pending data from fd (non-blocking, with timeout).
static void DrainFd(int fd, int timeout_ms = 200) {
  char tmp[256];
  fd_set fds;
  struct timeval tv;
  for (;;) {
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0) break;
  }
}

// ---------------------------------------------------------------------------
// Test command flags (static, unique per test to avoid singleton conflicts)
// ---------------------------------------------------------------------------
static bool g_pipe_cmd_1_called = false;
static int PipeCmd1(int /*argc*/, char* /*argv*/[]) {
  g_pipe_cmd_1_called = true;
  return 0;
}

static bool g_pipe_cmd_printf_called = false;
static int PipeCmdPrintf(int /*argc*/, char* /*argv*/[]) {
  g_pipe_cmd_printf_called = true;
  osp::ShellPrintf("test_output_%d", 42);
  return 0;
}

static bool g_pty_cmd_1_called = false;
static int PtyCmd1(int /*argc*/, char* /*argv*/[]) {
  g_pty_cmd_1_called = true;
  return 0;
}

// ---------------------------------------------------------------------------
// Register unique test commands at static-init time
// ---------------------------------------------------------------------------
static osp::ShellAutoReg reg_pipe_cmd_1("pipe_cmd_1", PipeCmd1,
                                        "pipe test cmd");
static osp::ShellAutoReg reg_pipe_cmd_printf("pipe_cmd_printf", PipeCmdPrintf,
                                             "pipe printf cmd");
static osp::ShellAutoReg reg_pty_cmd_1("pty_cmd_1", PtyCmd1,
                                       "pty test cmd");

// ============================================================================
// ConsoleShell tests
// ============================================================================

TEST_CASE("ConsoleShell with pipe - command execution", "[shell_backend]") {
  int pipe_in[2] = {-1, -1};   // shell reads from pipe_in[0]
  int pipe_out[2] = {-1, -1};  // shell writes to pipe_out[1]
  REQUIRE(::pipe(pipe_in) == 0);
  REQUIRE(::pipe(pipe_out) == 0);

  g_pipe_cmd_1_called = false;

  osp::ConsoleShell::Config cfg;
  cfg.read_fd = pipe_in[0];
  cfg.write_fd = pipe_out[1];
  cfg.raw_mode = false;
  osp::ConsoleShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  REQUIRE(shell.IsRunning());

  // Drain the initial prompt
  DrainFd(pipe_out[0], 200);

  // Send command
  const char* cmd = "pipe_cmd_1\n";
  (void)::write(pipe_in[1], cmd, std::strlen(cmd));

  // Wait for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read output -- prompt should reappear after command execution
  bool has_prompt = ReadContains(pipe_out[0], "osp>", 300);
  CHECK(has_prompt);

  // Close write end of input pipe to unblock the shell's read()
  ::close(pipe_in[1]);
  shell.Stop();

  ::close(pipe_in[0]);
  ::close(pipe_out[0]);
  ::close(pipe_out[1]);

  REQUIRE(g_pipe_cmd_1_called);
  REQUIRE_FALSE(shell.IsRunning());
}

TEST_CASE("ConsoleShell with pipe - help command", "[shell_backend]") {
  int pipe_in[2] = {-1, -1};
  int pipe_out[2] = {-1, -1};
  REQUIRE(::pipe(pipe_in) == 0);
  REQUIRE(::pipe(pipe_out) == 0);

  osp::ConsoleShell::Config cfg;
  cfg.read_fd = pipe_in[0];
  cfg.write_fd = pipe_out[1];
  cfg.raw_mode = false;
  osp::ConsoleShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Drain initial prompt
  DrainFd(pipe_out[0], 200);

  // Send help command
  const char* cmd = "help\n";
  (void)::write(pipe_in[1], cmd, std::strlen(cmd));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read output -- should contain "help" (the built-in help lists itself)
  bool has_help = ReadContains(pipe_out[0], "help", 300);
  CHECK(has_help);

  ::close(pipe_in[1]);
  shell.Stop();

  ::close(pipe_in[0]);
  ::close(pipe_out[0]);
  ::close(pipe_out[1]);
}

TEST_CASE("UartShell with PTY - command execution", "[shell_backend]") {
  int master = -1;
  int slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);

  g_pty_cmd_1_called = false;

  osp::UartShell::Config cfg;
  cfg.override_fd = slave;
  osp::UartShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  REQUIRE(shell.IsRunning());

  // Drain initial prompt
  DrainFd(master, 200);

  // Send command via master side of PTY
  const char* cmd = "pty_cmd_1\n";
  (void)::write(master, cmd, std::strlen(cmd));

  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // Read output from master
  bool has_prompt = ReadContains(master, "osp>", 300);
  CHECK(has_prompt);

  // Close master to unblock shell's read on slave
  ::close(master);
  shell.Stop();

  REQUIRE(g_pty_cmd_1_called);
  REQUIRE_FALSE(shell.IsRunning());
}

TEST_CASE("UartShell with PTY - help output", "[shell_backend]") {
  int master = -1;
  int slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);

  osp::UartShell::Config cfg;
  cfg.override_fd = slave;
  osp::UartShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Drain initial prompt
  DrainFd(master, 200);

  const char* cmd = "help\n";
  (void)::write(master, cmd, std::strlen(cmd));

  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  bool has_help = ReadContains(master, "help", 300);
  CHECK(has_help);

  ::close(master);
  shell.Stop();
}

TEST_CASE("ShellPrintf works with ConsoleShell", "[shell_backend]") {
  int pipe_in[2] = {-1, -1};
  int pipe_out[2] = {-1, -1};
  REQUIRE(::pipe(pipe_in) == 0);
  REQUIRE(::pipe(pipe_out) == 0);

  g_pipe_cmd_printf_called = false;

  osp::ConsoleShell::Config cfg;
  cfg.read_fd = pipe_in[0];
  cfg.write_fd = pipe_out[1];
  cfg.raw_mode = false;
  osp::ConsoleShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());

  // Drain initial prompt
  DrainFd(pipe_out[0], 200);

  // Send the command that calls ShellPrintf("test_output_%d", 42)
  const char* cmd = "pipe_cmd_printf\n";
  (void)::write(pipe_in[1], cmd, std::strlen(cmd));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read output -- should contain "test_output_42"
  bool has_output = ReadContains(pipe_out[0], "test_output_42", 300);
  CHECK(has_output);

  ::close(pipe_in[1]);
  shell.Stop();

  ::close(pipe_in[0]);
  ::close(pipe_out[0]);
  ::close(pipe_out[1]);

  REQUIRE(g_pipe_cmd_printf_called);
}

TEST_CASE("ConsoleShell stop is clean", "[shell_backend]") {
  int pipe_in[2] = {-1, -1};
  int pipe_out[2] = {-1, -1};
  REQUIRE(::pipe(pipe_in) == 0);
  REQUIRE(::pipe(pipe_out) == 0);

  osp::ConsoleShell::Config cfg;
  cfg.read_fd = pipe_in[0];
  cfg.write_fd = pipe_out[1];
  cfg.raw_mode = false;
  osp::ConsoleShell shell(cfg);

  auto r = shell.Start();
  REQUIRE(r.has_value());
  REQUIRE(shell.IsRunning());

  // Close write end of input pipe to let shell's read() return 0
  ::close(pipe_in[1]);

  shell.Stop();
  REQUIRE_FALSE(shell.IsRunning());

  ::close(pipe_in[0]);
  ::close(pipe_out[0]);
  ::close(pipe_out[1]);
}
