/**
 * @file test_shell_args.cpp
 * @brief Tests for shell argument parsing utilities (ShellParseInt, ShellParseUint,
 *        ShellParseBool, ShellArgCheck, ShellDispatch).
 */

#include "osp/shell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

// ============================================================================
// MockSession helper (pipe-backed output capture)
// ============================================================================

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

/// RAII guard that sets/clears CurrentSession for the scope.
struct SessionGuard {
  explicit SessionGuard(MockSession& m) {
    osp::detail::CurrentSession() = &m.session;
  }
  ~SessionGuard() { osp::detail::CurrentSession() = nullptr; }
};

}  // namespace

// ============================================================================
// ShellParseInt tests
// ============================================================================

TEST_CASE("ShellParseInt: positive number", "[shell_args]") {
  auto r = osp::ShellParseInt("42");
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 42);
}

TEST_CASE("ShellParseInt: negative number", "[shell_args]") {
  auto r = osp::ShellParseInt("-7");
  REQUIRE(r.has_value());
  REQUIRE(r.value() == -7);
}

TEST_CASE("ShellParseInt: zero", "[shell_args]") {
  auto r = osp::ShellParseInt("0");
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 0);
}

TEST_CASE("ShellParseInt: empty string", "[shell_args]") {
  auto r = osp::ShellParseInt("");
  REQUIRE_FALSE(r.has_value());
}

TEST_CASE("ShellParseInt: null", "[shell_args]") {
  auto r = osp::ShellParseInt(nullptr);
  REQUIRE_FALSE(r.has_value());
}

TEST_CASE("ShellParseInt: non-numeric", "[shell_args]") {
  auto r = osp::ShellParseInt("abc");
  REQUIRE_FALSE(r.has_value());
}

TEST_CASE("ShellParseInt: trailing garbage", "[shell_args]") {
  auto r = osp::ShellParseInt("42abc");
  REQUIRE_FALSE(r.has_value());
}

// ============================================================================
// ShellParseUint tests
// ============================================================================

TEST_CASE("ShellParseUint: valid", "[shell_args]") {
  auto r = osp::ShellParseUint("100");
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 100U);
}

TEST_CASE("ShellParseUint: zero", "[shell_args]") {
  auto r = osp::ShellParseUint("0");
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 0U);
}

TEST_CASE("ShellParseUint: negative rejected", "[shell_args]") {
  auto r = osp::ShellParseUint("-1");
  REQUIRE_FALSE(r.has_value());
}

TEST_CASE("ShellParseUint: trailing garbage rejected", "[shell_args]") {
  auto r = osp::ShellParseUint("123xyz");
  REQUIRE_FALSE(r.has_value());
}

// ============================================================================
// ShellParseBool tests
// ============================================================================

TEST_CASE("ShellParseBool: true variants", "[shell_args]") {
  REQUIRE(osp::ShellParseBool("true").value() == true);
  REQUIRE(osp::ShellParseBool("TRUE").value() == true);
  REQUIRE(osp::ShellParseBool("1").value() == true);
  REQUIRE(osp::ShellParseBool("yes").value() == true);
  REQUIRE(osp::ShellParseBool("on").value() == true);
  REQUIRE(osp::ShellParseBool("On").value() == true);
}

TEST_CASE("ShellParseBool: false variants", "[shell_args]") {
  REQUIRE(osp::ShellParseBool("false").value() == false);
  REQUIRE(osp::ShellParseBool("FALSE").value() == false);
  REQUIRE(osp::ShellParseBool("0").value() == false);
  REQUIRE(osp::ShellParseBool("no").value() == false);
  REQUIRE(osp::ShellParseBool("off").value() == false);
}

TEST_CASE("ShellParseBool: invalid", "[shell_args]") {
  REQUIRE_FALSE(osp::ShellParseBool("maybe").has_value());
}

TEST_CASE("ShellParseBool: null", "[shell_args]") {
  REQUIRE_FALSE(osp::ShellParseBool(nullptr).has_value());
}

// ============================================================================
// ShellArgCheck tests
// ============================================================================

TEST_CASE("ShellArgCheck: sufficient", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  REQUIRE(osp::ShellArgCheck(3, 2, "cmd <arg>") == true);
}

TEST_CASE("ShellArgCheck: insufficient prints usage", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  REQUIRE(osp::ShellArgCheck(1, 3, "cmd <a> <b>") == false);
  auto output = mock.DrainOutput();
  REQUIRE(output.find("Usage:") != std::string::npos);
  REQUIRE(output.find("cmd <a> <b>") != std::string::npos);
}

// ============================================================================
// ShellDispatch tests
// ============================================================================

namespace {

static int s_dispatch_called = 0;
static int s_dispatch_argc = 0;

int dispatch_default(int argc, char* /*argv*/[]) {
  s_dispatch_called = 1;
  s_dispatch_argc = argc;
  return 0;
}

int dispatch_sub_reset(int argc, char* /*argv*/[]) {
  s_dispatch_called = 2;
  s_dispatch_argc = argc;
  return 42;
}

int dispatch_sub_status(int argc, char* /*argv*/[]) {
  s_dispatch_called = 3;
  s_dispatch_argc = argc;
  return 0;
}

static const osp::ShellSubCmd kTestSubs[] = {
    {"status", nullptr, "Show status", dispatch_sub_status},
    {"reset", nullptr, "Reset counters", dispatch_sub_reset},
};

}  // namespace

TEST_CASE("ShellDispatch: no subcommand calls default", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  s_dispatch_called = 0;
  char arg0[] = "mycmd";
  char* argv[] = {arg0};
  int ret = osp::ShellDispatch(1, argv, kTestSubs, 2U, dispatch_default);
  REQUIRE(ret == 0);
  REQUIRE(s_dispatch_called == 1);
  REQUIRE(s_dispatch_argc == 1);
}

TEST_CASE("ShellDispatch: subcommand match", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  s_dispatch_called = 0;
  char arg0[] = "mycmd";
  char arg1[] = "reset";
  char* argv[] = {arg0, arg1};
  int ret = osp::ShellDispatch(2, argv, kTestSubs, 2U, dispatch_default);
  REQUIRE(ret == 42);
  REQUIRE(s_dispatch_called == 2);
  // argc should be shifted: 2-1 = 1
  REQUIRE(s_dispatch_argc == 1);
}

TEST_CASE("ShellDispatch: help subcommand", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  char arg0[] = "mycmd";
  char arg1[] = "help";
  char* argv[] = {arg0, arg1};
  int ret = osp::ShellDispatch(2, argv, kTestSubs, 2U, dispatch_default);
  REQUIRE(ret == 0);
  auto output = mock.DrainOutput();
  REQUIRE(output.find("status") != std::string::npos);
  REQUIRE(output.find("reset") != std::string::npos);
  REQUIRE(output.find("Reset counters") != std::string::npos);
}

TEST_CASE("ShellDispatch: unknown subcommand", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  char arg0[] = "mycmd";
  char arg1[] = "bogus";
  char* argv[] = {arg0, arg1};
  int ret = osp::ShellDispatch(2, argv, kTestSubs, 2U, dispatch_default);
  REQUIRE(ret == -1);
  auto output = mock.DrainOutput();
  REQUIRE(output.find("Unknown subcommand") != std::string::npos);
}

TEST_CASE("ShellDispatch: no default shows help", "[shell_args]") {
  MockSession mock;
  SessionGuard guard(mock);
  char arg0[] = "mycmd";
  char* argv[] = {arg0};
  int ret = osp::ShellDispatch(1, argv, kTestSubs, 2U, nullptr);
  REQUIRE(ret == 0);
  auto output = mock.DrainOutput();
  REQUIRE(output.find("Usage:") != std::string::npos);
}
