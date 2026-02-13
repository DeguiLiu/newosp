/**
 * @file test_timer.cpp
 * @brief Tests for timer.hpp
 */

#include "osp/timer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("TimerScheduler Add and Remove", "[timer]") {
  osp::TimerScheduler sched(4);

  auto result = sched.Add(100, [](void*) {}, nullptr);
  REQUIRE(result.has_value());
  REQUIRE(result.value().value() > 0);

  auto rm = sched.Remove(result.value());
  REQUIRE(rm.has_value());
}

TEST_CASE("TimerScheduler invalid period", "[timer]") {
  osp::TimerScheduler sched(4);
  auto result = sched.Add(0, [](void*) {}, nullptr);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler slots full", "[timer]") {
  osp::TimerScheduler sched(2);
  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(r1.has_value());
  auto r2 = sched.Add(100, [](void*) {});
  REQUIRE(r2.has_value());
  auto r3 = sched.Add(100, [](void*) {});
  REQUIRE(!r3.has_value());
  REQUIRE(r3.get_error() == osp::TimerError::kSlotsFull);
}

TEST_CASE("TimerScheduler Start/Stop", "[timer]") {
  osp::TimerScheduler sched(4);
  auto start_result = sched.Start();
  REQUIRE(start_result.has_value());
  REQUIRE(sched.IsRunning());

  // Double start should fail
  auto start2 = sched.Start();
  REQUIRE(!start2.has_value());
  REQUIRE(start2.get_error() == osp::TimerError::kAlreadyRunning);

  sched.Stop();
  REQUIRE(!sched.IsRunning());
}

TEST_CASE("TimerScheduler fires callback", "[timer]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler sched(4);

  sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  REQUIRE(counter.load() > 0);
}

TEST_CASE("TimerScheduler TaskCount", "[timer]") {
  osp::TimerScheduler sched(4);
  REQUIRE(sched.TaskCount() == 0);

  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(sched.TaskCount() == 1);

  auto r2 = sched.Add(200, [](void*) {});
  REQUIRE(sched.TaskCount() == 2);

  sched.Remove(r1.value());
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler Remove nonexistent", "[timer]") {
  osp::TimerScheduler sched(4);
  auto result = sched.Remove(osp::TimerTaskId(999));
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kNotRunning);
}

TEST_CASE("TimerScheduler cancel non-existent timer", "[timer]") {
  osp::TimerScheduler sched(4);
  // Try to remove a timer that was never added
  auto result = sched.Remove(osp::TimerTaskId(12345));
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kNotRunning);

  // Add and remove a timer, then try to remove it again
  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(r1.has_value());
  auto rm1 = sched.Remove(r1.value());
  REQUIRE(rm1.has_value());

  // Second remove should fail
  auto rm2 = sched.Remove(r1.value());
  REQUIRE(!rm2.has_value());
}

TEST_CASE("TimerScheduler schedule with zero delay", "[timer]") {
  osp::TimerScheduler sched(4);
  // Zero period should be rejected
  auto result = sched.Add(0, [](void*) {});
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler multiple timers firing in order", "[timer]") {
  std::atomic<int> counter1{0};
  std::atomic<int> counter2{0};
  std::atomic<int> counter3{0};
  osp::TimerScheduler sched(8);

  // Add multiple timers with different periods
  sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter1);

  sched.Add(20, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter2);

  sched.Add(30, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter3);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Faster timer should fire more times
  REQUIRE(counter1.load() > 0);
  REQUIRE(counter2.load() > 0);
  REQUIRE(counter3.load() > 0);
  REQUIRE(counter1.load() >= counter2.load());
  REQUIRE(counter2.load() >= counter3.load());
}

TEST_CASE("TimerScheduler timer reschedule", "[timer]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler sched(4);

  // Add a timer
  auto r1 = sched.Add(50, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r1.has_value());

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  // Cancel the timer
  auto rm = sched.Remove(r1.value());
  REQUIRE(rm.has_value());

  int count_after_cancel = counter.load();

  // Re-add with different period
  auto r2 = sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r2.has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  // Counter should have increased after rescheduling
  REQUIRE(counter.load() > count_after_cancel);
}

TEST_CASE("TimerScheduler rapid schedule cancel cycles", "[timer]") {
  osp::TimerScheduler sched(8);

  // Rapidly add and remove timers
  for (int i = 0; i < 20; ++i) {
    auto r = sched.Add(100, [](void*) {});
    REQUIRE(r.has_value());
    auto rm = sched.Remove(r.value());
    REQUIRE(rm.has_value());
  }

  REQUIRE(sched.TaskCount() == 0);

  // Should still be able to add timers after rapid cycles
  auto r = sched.Add(100, [](void*) {});
  REQUIRE(r.has_value());
  REQUIRE(sched.TaskCount() == 1);
}
