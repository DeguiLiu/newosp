/**
 * @file test_shell.cpp
 * @brief Tests for shell.hpp (command registry and parsing only, no telnet)
 */

#include "osp/shell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

static int test_cmd_a(int /*argc*/, char* /*argv*/[]) { return 0; }
static int test_cmd_b(int /*argc*/, char* /*argv*/[]) { return 1; }

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
  reg.Register("dup_test_cmd", test_cmd_b, "dup test");
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
  reg.Register("auto_alpha", test_cmd_a, "alpha");
  reg.Register("auto_beta", test_cmd_a, "beta");

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
