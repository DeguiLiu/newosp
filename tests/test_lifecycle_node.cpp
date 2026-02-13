/**
 * @file test_lifecycle_node.cpp
 * @brief Unit tests for LifecycleNode state machine.
 */

#include "osp/lifecycle_node.hpp"

#include <catch2/catch_test_macros.hpp>
#include <variant>

// Test payload types
struct SensorData {
  float temperature;
};

struct MotorCmd {
  int speed;
};

using TestPayload = std::variant<SensorData, MotorCmd>;

// Global callback counters for testing
static int g_configure_count = 0;
static int g_activate_count = 0;
static int g_deactivate_count = 0;
static int g_cleanup_count = 0;
static int g_shutdown_count = 0;

// Callback functions
static bool OnConfigure() {
  ++g_configure_count;
  return true;
}

static bool OnActivate() {
  ++g_activate_count;
  return true;
}

static bool OnDeactivate() {
  ++g_deactivate_count;
  return true;
}

static bool OnCleanup() {
  ++g_cleanup_count;
  return true;
}

static void OnShutdown() {
  ++g_shutdown_count;
}

static bool OnConfigureFail() {
  ++g_configure_count;
  return false;
}

static bool OnActivateFail() {
  ++g_activate_count;
  return false;
}

// Reset counters before each test
static void ResetCounters() {
  g_configure_count = 0;
  g_activate_count = 0;
  g_deactivate_count = 0;
  g_cleanup_count = 0;
  g_shutdown_count = 0;
}

TEST_CASE("LifecycleNode - Initial state is kUnconfigured", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
}

TEST_CASE("LifecycleNode - Full lifecycle sequence", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);
  node.SetOnDeactivate(OnDeactivate);
  node.SetOnCleanup(OnCleanup);

  // Unconfigured -> Inactive
  auto result = node.Configure();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);
  REQUIRE(g_configure_count == 1);

  // Inactive -> Active
  result = node.Activate();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);
  REQUIRE(g_activate_count == 1);

  // Active -> Inactive
  result = node.Deactivate();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);
  REQUIRE(g_deactivate_count == 1);

  // Inactive -> Unconfigured
  result = node.Cleanup();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
  REQUIRE(g_cleanup_count == 1);
}

TEST_CASE("LifecycleNode - Shutdown from Unconfigured", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnShutdown(OnShutdown);

  auto result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Shutdown from Inactive", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigure);
  node.SetOnShutdown(OnShutdown);

  node.Configure();
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  auto result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Shutdown from Active", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);
  node.SetOnShutdown(OnShutdown);

  node.Configure();
  node.Activate();
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);

  auto result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Invalid transition: Activate from Unconfigured", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  auto result = node.Activate();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kInvalidTransition);
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
}

TEST_CASE("LifecycleNode - Invalid transition: Configure from Active", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);

  node.Configure();
  node.Activate();
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);

  auto result = node.Configure();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kInvalidTransition);
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);
}

TEST_CASE("LifecycleNode - Invalid transition: Deactivate from Unconfigured", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  auto result = node.Deactivate();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kInvalidTransition);
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
}

TEST_CASE("LifecycleNode - Invalid transition: Cleanup from Unconfigured", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  auto result = node.Cleanup();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kInvalidTransition);
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
}

TEST_CASE("LifecycleNode - Invalid transition: Cleanup from Active", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);

  node.Configure();
  node.Activate();
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);

  auto result = node.Cleanup();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kInvalidTransition);
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);
}

TEST_CASE("LifecycleNode - Callback invocation", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);
  node.SetOnDeactivate(OnDeactivate);
  node.SetOnCleanup(OnCleanup);
  node.SetOnShutdown(OnShutdown);

  REQUIRE(g_configure_count == 0);
  node.Configure();
  REQUIRE(g_configure_count == 1);

  REQUIRE(g_activate_count == 0);
  node.Activate();
  REQUIRE(g_activate_count == 1);

  REQUIRE(g_deactivate_count == 0);
  node.Deactivate();
  REQUIRE(g_deactivate_count == 1);

  REQUIRE(g_cleanup_count == 0);
  node.Cleanup();
  REQUIRE(g_cleanup_count == 1);

  REQUIRE(g_shutdown_count == 0);
  node.Shutdown();
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Callback failure: Configure returns false", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigureFail);

  auto result = node.Configure();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kCallbackFailed);
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);
  REQUIRE(g_configure_count == 1);
}

TEST_CASE("LifecycleNode - Callback failure: Activate returns false", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivateFail);

  node.Configure();
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  auto result = node.Activate();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kCallbackFailed);
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);
  REQUIRE(g_activate_count == 1);
}

TEST_CASE("LifecycleNode - Double shutdown is safe", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.SetOnShutdown(OnShutdown);

  auto result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
  REQUIRE(g_shutdown_count == 1);

  // Second shutdown should succeed (idempotent)
  result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
  REQUIRE(g_shutdown_count == 2);
}

TEST_CASE("LifecycleNode - Already finalized error", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);
  node.Shutdown();
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);

  // Try to configure after shutdown
  auto result = node.Configure();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == osp::LifecycleError::kAlreadyFinalized);
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
}

TEST_CASE("LifecycleNode - Convenience methods", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);
  node.SetOnDeactivate(OnDeactivate);
  node.SetOnCleanup(OnCleanup);
  node.SetOnShutdown(OnShutdown);

  // Test convenience methods
  REQUIRE(node.Configure().has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  REQUIRE(node.Activate().has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);

  REQUIRE(node.Deactivate().has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  REQUIRE(node.Cleanup().has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);

  REQUIRE(node.Shutdown().has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
}

TEST_CASE("LifecycleNode - Destructor triggers shutdown", "[lifecycle_node]") {
  ResetCounters();
  {
    osp::LifecycleNode<TestPayload> node("test_node", 1);
    node.SetOnShutdown(OnShutdown);
    REQUIRE(g_shutdown_count == 0);
  }
  // Destructor should have called shutdown
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Destructor does not call shutdown if already finalized", "[lifecycle_node]") {
  ResetCounters();
  {
    osp::LifecycleNode<TestPayload> node("test_node", 1);
    node.SetOnShutdown(OnShutdown);
    node.Shutdown();
    REQUIRE(g_shutdown_count == 1);
  }
  // Destructor should not call shutdown again
  REQUIRE(g_shutdown_count == 1);
}

TEST_CASE("LifecycleNode - Node base class functionality", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 42);

  // Test inherited Node methods
  REQUIRE(std::strcmp(node.Name(), "test_node") == 0);
  REQUIRE(node.Id() == 42);
}

TEST_CASE("LifecycleNode - Null callbacks (no callback set)", "[lifecycle_node]") {
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  // No callbacks set, transitions should succeed
  auto result = node.Configure();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  result = node.Activate();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);

  result = node.Deactivate();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);

  result = node.Cleanup();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kUnconfigured);

  result = node.Shutdown();
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kFinalized);
}

TEST_CASE("LifecycleNode - Trigger method with explicit transitions", "[lifecycle_node]") {
  ResetCounters();
  osp::LifecycleNode<TestPayload> node("test_node", 1);

  node.SetOnConfigure(OnConfigure);
  node.SetOnActivate(OnActivate);

  // Use Trigger directly
  auto result = node.Trigger(osp::LifecycleTransition::kConfigure);
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kInactive);
  REQUIRE(g_configure_count == 1);

  result = node.Trigger(osp::LifecycleTransition::kActivate);
  REQUIRE(result.has_value());
  REQUIRE(node.GetState() == osp::LifecycleState::kActive);
  REQUIRE(g_activate_count == 1);
}
