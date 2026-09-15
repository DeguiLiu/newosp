/**
 * @file test_shutdown_win.cpp
 * @brief Catch2 tests for osp::shutdown.hpp on the Windows branch
 *        (CreatePipe wakeup + SetConsoleCtrlHandler).
 *
 * Register/Quit/WaitForShutdown run the LIFO callback path; InstallSignalHandlers
 * exercises SetConsoleCtrlHandler. Guarded by OSP_PLATFORM_WINDOWS.
 */

#include "osp/shutdown.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(OSP_PLATFORM_WINDOWS)

// ============================================================================
// Quit triggers callbacks in LIFO order
// ============================================================================

TEST_CASE("shutdown_win - Quit then WaitForShutdown runs callbacks LIFO", "[shutdown][windows]") {
  static int order[3] = {0, 0, 0};
  static int idx = 0;
  idx = 0;

  osp::ShutdownManager mgr;
  REQUIRE(mgr.IsValid());
  REQUIRE(mgr.Register([](int) { order[idx++] = 1; }).has_value());
  REQUIRE(mgr.Register([](int) { order[idx++] = 2; }).has_value());
  REQUIRE(mgr.Register([](int) { order[idx++] = 3; }).has_value());

  REQUIRE_FALSE(mgr.IsShutdownRequested());
  mgr.Quit(42);
  REQUIRE(mgr.IsShutdownRequested());

  // Quit already set the flag, so WaitForShutdown does not block.
  mgr.WaitForShutdown();

  REQUIRE(idx == 3);
  REQUIRE(order[0] == 3);
  REQUIRE(order[1] == 2);
  REQUIRE(order[2] == 1);
}

TEST_CASE("shutdown_win - callbacks receive the Quit signal number", "[shutdown][windows]") {
  static int seen_signo = -1;
  seen_signo = -1;

  osp::ShutdownManager mgr;
  REQUIRE(mgr.Register([](int signo) { seen_signo = signo; }).has_value());
  mgr.Quit(7);
  mgr.WaitForShutdown();
  REQUIRE(seen_signo == 7);
}

// ============================================================================
// Register capacity
// ============================================================================

TEST_CASE("shutdown_win - Register rejects null and overflowing callbacks", "[shutdown][windows]") {
  osp::ShutdownManager mgr;
  REQUIRE(mgr.IsValid());

  auto null_r = mgr.Register(nullptr);
  REQUIRE_FALSE(null_r.has_value());
  REQUIRE(null_r.get_error() == osp::ShutdownError::kCallbacksFull);

  for (uint32_t i = 0; i < 16; ++i) {
    REQUIRE(mgr.Register([](int) {}).has_value());
  }
  auto full_r = mgr.Register([](int) {});
  REQUIRE_FALSE(full_r.has_value());
  REQUIRE(full_r.get_error() == osp::ShutdownError::kCallbacksFull);
}

// ============================================================================
// Single-instance constraint
// ============================================================================

TEST_CASE("shutdown_win - second manager instance is invalid", "[shutdown][windows]") {
  osp::ShutdownManager first;
  REQUIRE(first.IsValid());

  osp::ShutdownManager second;
  REQUIRE_FALSE(second.IsValid());

  auto r = second.Register([](int) {});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.get_error() == osp::ShutdownError::kAlreadyInstantiated);
}

// ============================================================================
// Signal handler installation (SetConsoleCtrlHandler)
// ============================================================================

TEST_CASE("shutdown_win - InstallSignalHandlers succeeds", "[shutdown][windows]") {
  osp::ShutdownManager mgr;
  auto result = mgr.InstallSignalHandlers();
  REQUIRE(result.has_value());
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("shutdown_win - Windows shutdown tests are Windows-only", "[shutdown][windows]") {
  SKIP("shutdown_win tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
