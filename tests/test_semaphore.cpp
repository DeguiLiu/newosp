/**
 * @file test_semaphore.cpp
 * @brief Catch2 tests for osp::LightSemaphore, osp::BinarySemaphore,
 *        and osp::PosixSemaphore.
 */

#include "osp/semaphore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// ============================================================================
// LightSemaphore tests
// ============================================================================

TEST_CASE("semaphore - LightSemaphore initial count zero TryWait fails",
          "[semaphore]") {
  osp::LightSemaphore sem(0);
  REQUIRE(sem.Count() == 0U);
  REQUIRE_FALSE(sem.TryWait());
  REQUIRE(sem.Count() == 0U);
}

TEST_CASE("semaphore - LightSemaphore Signal then Wait succeeds",
          "[semaphore]") {
  osp::LightSemaphore sem(0);
  sem.Signal();
  REQUIRE(sem.Count() == 1U);
  sem.Wait();
  REQUIRE(sem.Count() == 0U);
}

TEST_CASE("semaphore - LightSemaphore multiple Signal accumulates count",
          "[semaphore]") {
  osp::LightSemaphore sem(0);
  sem.Signal();
  sem.Signal();
  sem.Signal();
  REQUIRE(sem.Count() == 3U);

  REQUIRE(sem.TryWait());
  REQUIRE(sem.Count() == 2U);
  REQUIRE(sem.TryWait());
  REQUIRE(sem.Count() == 1U);
  REQUIRE(sem.TryWait());
  REQUIRE(sem.Count() == 0U);
  REQUIRE_FALSE(sem.TryWait());
}

TEST_CASE("semaphore - LightSemaphore WaitFor timeout returns false",
          "[semaphore]") {
  osp::LightSemaphore sem(0);

  auto start = std::chrono::steady_clock::now();
  bool result = sem.WaitFor(10000);  // 10ms timeout
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_FALSE(result);
  REQUIRE(sem.Count() == 0U);
  // Should have waited at least ~10ms (allow some slack)
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  REQUIRE(elapsed_ms >= 5);
}

TEST_CASE("semaphore - LightSemaphore WaitFor succeeds when signaled",
          "[semaphore]") {
  osp::LightSemaphore sem(0);
  std::atomic<bool> done{false};

  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sem.Signal();
  });

  bool result = sem.WaitFor(2000000);  // 2s timeout (generous)
  REQUIRE(result);
  REQUIRE(sem.Count() == 0U);

  producer.join();
}

TEST_CASE("semaphore - LightSemaphore producer-consumer with threads",
          "[semaphore]") {
  osp::LightSemaphore sem(0);
  constexpr int kItemCount = 100;
  std::atomic<int> consumed{0};

  // Consumer thread
  std::thread consumer([&] {
    for (int i = 0; i < kItemCount; ++i) {
      sem.Wait();
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Producer: signal kItemCount times
  for (int i = 0; i < kItemCount; ++i) {
    sem.Post();  // Post is alias for Signal
  }

  consumer.join();
  REQUIRE(consumed.load() == kItemCount);
  REQUIRE(sem.Count() == 0U);
}

// ============================================================================
// BinarySemaphore tests
// ============================================================================

TEST_CASE("semaphore - BinarySemaphore Signal clamps to 1", "[semaphore]") {
  osp::BinarySemaphore bsem(0);
  REQUIRE(bsem.Count() == 0U);

  bsem.Signal();
  REQUIRE(bsem.Count() == 1U);

  bsem.Signal();  // Should still be 1
  REQUIRE(bsem.Count() == 1U);

  bsem.Signal();  // Still 1
  REQUIRE(bsem.Count() == 1U);

  // Single Wait consumes it
  bsem.Wait();
  REQUIRE(bsem.Count() == 0U);

  // TryWait should fail now
  REQUIRE_FALSE(bsem.TryWait());
}

TEST_CASE("semaphore - BinarySemaphore basic signal/wait cycle",
          "[semaphore]") {
  osp::BinarySemaphore bsem(0);
  std::atomic<int> counter{0};

  std::thread waiter([&] {
    bsem.Wait();
    counter.fetch_add(1, std::memory_order_relaxed);
    bsem.Wait();
    counter.fetch_add(1, std::memory_order_relaxed);
  });

  // First signal
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bsem.Signal();

  // Wait for first iteration
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (counter.load() < 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(counter.load() == 1);

  // Second signal
  bsem.Signal();

  waiter.join();
  REQUIRE(counter.load() == 2);
}

// ============================================================================
// PosixSemaphore tests (Linux / macOS only)
// ============================================================================

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

TEST_CASE("semaphore - PosixSemaphore Post and Wait round-trip",
          "[semaphore]") {
  osp::PosixSemaphore psem(0);
  REQUIRE(psem.IsValid());

  // TryWait on empty should fail
  REQUIRE_FALSE(psem.TryWait());

  // Post then Wait
  psem.Post();
  psem.Wait();

  // Multiple post/wait cycles
  psem.Post();
  psem.Post();
  psem.Wait();
  psem.Wait();
  REQUIRE_FALSE(psem.TryWait());
}

TEST_CASE("semaphore - PosixSemaphore TryWait on empty returns false",
          "[semaphore]") {
  osp::PosixSemaphore psem(0);
  REQUIRE(psem.IsValid());

  REQUIRE_FALSE(psem.TryWait());
  REQUIRE_FALSE(psem.TryWait());

  // Post one, TryWait should succeed once
  psem.Post();
  REQUIRE(psem.TryWait());
  REQUIRE_FALSE(psem.TryWait());
}

TEST_CASE("semaphore - PosixSemaphore WaitFor timeout", "[semaphore]") {
  osp::PosixSemaphore psem(0);
  REQUIRE(psem.IsValid());

  auto start = std::chrono::steady_clock::now();
  bool result = psem.WaitFor(10000);  // 10ms
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_FALSE(result);
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  REQUIRE(elapsed_ms >= 5);
}

TEST_CASE("semaphore - PosixSemaphore WaitFor succeeds when posted",
          "[semaphore]") {
  osp::PosixSemaphore psem(0);
  REQUIRE(psem.IsValid());

  std::thread poster([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    psem.Post();
  });

  bool result = psem.WaitFor(2000000);  // 2s timeout
  REQUIRE(result);

  poster.join();
}

#endif  // OSP_PLATFORM_LINUX || OSP_PLATFORM_MACOS
