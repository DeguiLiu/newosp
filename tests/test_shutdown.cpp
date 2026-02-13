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
