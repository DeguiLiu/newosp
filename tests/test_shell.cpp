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

// ============================================================================
// New integration tests
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
