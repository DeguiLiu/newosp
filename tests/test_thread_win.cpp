/**
 * @file test_thread_win.cpp
 * @brief Catch2 tests for osp::thread.hpp on the Windows branch
 *        (SetThreadPriority / GetCurrentThreadId, std::thread backend).
 *
 * The main coverage is the Windows-specific priority mapping: opts.priority > 0
 * maps to SetThreadPriority(THREAD_PRIORITY_HIGHEST), < 0 maps to IDLE. Guarded
 * by OSP_PLATFORM_WINDOWS.
 */

#include "osp/thread.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#if defined(OSP_PLATFORM_WINDOWS)

namespace {

template <typename Pred>
bool WaitUntil(Pred pred, uint64_t timeout_ms = 3000U) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    osp::ThreadYield();
  }
  return pred();
}

}  // namespace

// ============================================================================
// Start / Join
// ============================================================================

TEST_CASE("thread_win - Start and Join a lambda", "[thread][windows]") {
  std::atomic<bool> ran{false};
  osp::Thread t;
  REQUIRE(t.Start([&ran]() { ran.store(true, std::memory_order_release); }));
  REQUIRE(t.joinable());
  REQUIRE(WaitUntil([&] { return ran.load(std::memory_order_acquire); }));
  t.join();
  REQUIRE_FALSE(t.joinable());
}

// ============================================================================
// Priority mapping (Windows branch)
// ============================================================================

TEST_CASE("thread_win - priority > 0 runs (SetThreadPriority HIGHEST path)",
          "[thread][windows]") {
  std::atomic<bool> ran{false};
  osp::ThreadOptions opts;
  opts.name = "prio_high";
  opts.priority = 10;  // > 0 -> THREAD_PRIORITY_HIGHEST

  osp::Thread t;
  REQUIRE(t.Start(opts, [&ran]() { ran.store(true, std::memory_order_release); }));
  REQUIRE(WaitUntil([&] { return ran.load(std::memory_order_acquire); }));
  t.join();
  REQUIRE(ran.load(std::memory_order_acquire));
}

TEST_CASE("thread_win - priority < 0 runs (SetThreadPriority IDLE path)",
          "[thread][windows]") {
  std::atomic<bool> ran{false};
  osp::ThreadOptions opts;
  opts.name = "prio_idle";
  opts.priority = -5;  // < 0 -> THREAD_PRIORITY_IDLE

  osp::Thread t;
  REQUIRE(t.Start(opts, [&ran]() { ran.store(true, std::memory_order_release); }));
  REQUIRE(WaitUntil([&] { return ran.load(std::memory_order_acquire); }));
  t.join();
  REQUIRE(ran.load(std::memory_order_acquire));
}

// ============================================================================
// Thread identity
// ============================================================================

TEST_CASE("thread_win - CurrentThreadId differs from main thread", "[thread][windows]") {
  const uintptr_t main_id = osp::Thread::CurrentThreadId();
  std::atomic<uintptr_t> worker_id{0};
  std::atomic<bool> done{false};

  osp::Thread t;
  REQUIRE(t.Start([&worker_id, &done]() {
    worker_id.store(osp::Thread::CurrentThreadId(), std::memory_order_release);
    done.store(true, std::memory_order_release);
  }));
  REQUIRE(WaitUntil([&] { return done.load(std::memory_order_acquire); }));
  t.join();
  REQUIRE(worker_id.load(std::memory_order_acquire) != main_id);
}

// ============================================================================
// Start while joinable is rejected
// ============================================================================

TEST_CASE("thread_win - Start while joinable returns false", "[thread][windows]") {
  osp::Thread t;
  REQUIRE(t.Start([&]() { osp::ThreadSleepUs(static_cast<uint64_t>(30) * 1000u); }));
  REQUIRE_FALSE(t.Start([&]() {}));  // second Start rejected
  t.join();
  REQUIRE_FALSE(t.joinable());
}

#else  // OSP_PLATFORM_WINDOWS

TEST_CASE("thread_win - Windows thread tests are Windows-only", "[thread][windows]") {
  SKIP("thread_win tests only run on Windows");
}

#endif  // OSP_PLATFORM_WINDOWS
