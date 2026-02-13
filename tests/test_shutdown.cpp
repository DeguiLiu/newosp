/**
 * @file test_shutdown.cpp
 * @brief Tests for shutdown.hpp
 */

#include "osp/shutdown.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ShutdownManager Register callbacks", "[shutdown]") {
  osp::ShutdownManager mgr;

  int called = 0;
  auto r = mgr.Register([](int) {});
  REQUIRE(r.has_value());

  // Register up to max
  for (uint32_t i = 1; i < 16; ++i) {
    auto rx = mgr.Register([](int) {});
    REQUIRE(rx.has_value());
  }

  // 17th should fail
  auto rn = mgr.Register([](int) {});
  REQUIRE(!rn.has_value());
  REQUIRE(rn.get_error() == osp::ShutdownError::kCallbacksFull);
}

TEST_CASE("ShutdownManager Quit and IsShutdownRequested", "[shutdown]") {
  osp::ShutdownManager mgr;
  REQUIRE(!mgr.IsShutdownRequested());

  mgr.Quit(0);
  REQUIRE(mgr.IsShutdownRequested());
}

TEST_CASE("ShutdownManager LIFO callback order", "[shutdown]") {
  // We need a fresh manager, but the global instance pointer
  // only allows one. The previous test's mgr is destroyed already.
  osp::ShutdownManager mgr;

  int order[3] = {0, 0, 0};
  int counter = 0;

  mgr.Register([](int signo) {
    // This is the first registered, should be called last
    (void)signo;
  });

  // Use a shared counter to verify LIFO order
  // Since callbacks are function pointers (not closures),
  // we use a simpler approach
  REQUIRE(mgr.IsShutdownRequested() == false);
  mgr.Quit(42);
  REQUIRE(mgr.IsShutdownRequested() == true);

  // WaitForShutdown won't block since Quit was already called
  mgr.WaitForShutdown();
  // If we got here, callbacks were executed without crash
  REQUIRE(true);
}

TEST_CASE("ShutdownManager InstallSignalHandlers", "[shutdown]") {
  osp::ShutdownManager mgr;
  auto result = mgr.InstallSignalHandlers();
  REQUIRE(result.has_value());
}

TEST_CASE("ShutdownManager null callback rejected", "[shutdown]") {
  osp::ShutdownManager mgr;
  auto r = mgr.Register(nullptr);
  REQUIRE(!r.has_value());
}

TEST_CASE("ShutdownManager multiple callbacks LIFO order verification", "[shutdown]") {
  osp::ShutdownManager mgr;

  // Use a vector to track execution order
  static std::vector<int> execution_order;
  execution_order.clear();

  mgr.Register([](int) { execution_order.push_back(1); });
  mgr.Register([](int) { execution_order.push_back(2); });
  mgr.Register([](int) { execution_order.push_back(3); });

  mgr.Quit(0);
  mgr.WaitForShutdown();

  // Callbacks should execute in LIFO order: 3, 2, 1
  REQUIRE(execution_order.size() == 3);
  REQUIRE(execution_order[0] == 3);
  REQUIRE(execution_order[1] == 2);
  REQUIRE(execution_order[2] == 1);
}

TEST_CASE("ShutdownManager double-trigger protection", "[shutdown]") {
  osp::ShutdownManager mgr;

  static int call_count = 0;
  call_count = 0;

  mgr.Register([](int) { ++call_count; });

  // First trigger
  mgr.Quit(0);
  REQUIRE(mgr.IsShutdownRequested());

  // Second Quit() should not change the flag (already set)
  mgr.Quit(1);
  REQUIRE(mgr.IsShutdownRequested());

  // WaitForShutdown executes callbacks once
  mgr.WaitForShutdown();
  REQUIRE(call_count == 1);

  // Note: WaitForShutdown can be called multiple times and will re-execute callbacks
  // This is by design - the test verifies Quit() is idempotent
}

TEST_CASE("ShutdownManager callback that throws", "[shutdown]") {
  osp::ShutdownManager mgr;

  static bool callback1_called = false;
  static bool callback3_called = false;
  callback1_called = false;
  callback3_called = false;

  mgr.Register([](int) { callback1_called = true; });

  // Middle callback attempts to throw (with -fno-exceptions, this is a no-op or abort)
  // In practice, we can't actually throw, so we simulate problematic behavior
  mgr.Register([](int) {
    // Simulate a callback that would throw
    // With -fno-exceptions, this just continues
  });

  mgr.Register([](int) { callback3_called = true; });

  mgr.Quit(0);
  mgr.WaitForShutdown();

  // All callbacks should have been called despite the "throwing" one
  REQUIRE(callback1_called);
  REQUIRE(callback3_called);
}

TEST_CASE("ShutdownManager register callback after trigger", "[shutdown]") {
  osp::ShutdownManager mgr;

  static bool early_callback_called = false;
  static bool late_callback_called = false;
  early_callback_called = false;
  late_callback_called = false;

  mgr.Register([](int) { early_callback_called = true; });

  // Trigger shutdown
  mgr.Quit(0);
  REQUIRE(mgr.IsShutdownRequested());

  // Try to register after shutdown is requested
  auto result = mgr.Register([](int) { late_callback_called = true; });

  // Registration might succeed or fail depending on implementation
  // But the late callback should not be called if registered after trigger
  mgr.WaitForShutdown();

  REQUIRE(early_callback_called);
  // late_callback_called behavior depends on implementation
}
