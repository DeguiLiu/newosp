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
