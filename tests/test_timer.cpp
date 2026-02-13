/**
 * @file test_timer.cpp
 * @brief Tests for timer.hpp
 */

#include "osp/timer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// ============================================================================
// Basic API Tests
// ============================================================================

TEST_CASE("TimerScheduler Add and Remove", "[timer]") {
  osp::TimerScheduler<4> sched;

  auto result = sched.Add(100, [](void*) {}, nullptr);
  REQUIRE(result.has_value());
  REQUIRE(result.value().value() > 0);

  auto rm = sched.Remove(result.value());
  REQUIRE(rm.has_value());
}

TEST_CASE("TimerScheduler invalid period", "[timer]") {
  osp::TimerScheduler<4> sched;
  auto result = sched.Add(0, [](void*) {}, nullptr);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler slots full", "[timer]") {
  osp::TimerScheduler<2> sched;
  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(r1.has_value());
  auto r2 = sched.Add(100, [](void*) {});
  REQUIRE(r2.has_value());
  auto r3 = sched.Add(100, [](void*) {});
  REQUIRE(!r3.has_value());
  REQUIRE(r3.get_error() == osp::TimerError::kSlotsFull);
}

TEST_CASE("TimerScheduler Start/Stop", "[timer]") {
  osp::TimerScheduler<4> sched;
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
  osp::TimerScheduler<4> sched;

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
  osp::TimerScheduler<4> sched;
  REQUIRE(sched.TaskCount() == 0);

  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(sched.TaskCount() == 1);

  auto r2 = sched.Add(200, [](void*) {});
  REQUIRE(sched.TaskCount() == 2);

  sched.Remove(r1.value());
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler Remove nonexistent", "[timer]") {
  osp::TimerScheduler<4> sched;
  auto result = sched.Remove(osp::TimerTaskId(999));
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kNotRunning);
}

TEST_CASE("TimerScheduler cancel non-existent timer", "[timer]") {
  osp::TimerScheduler<4> sched;
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
  osp::TimerScheduler<4> sched;
  // Zero period should be rejected
  auto result = sched.Add(0, [](void*) {});
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler multiple timers firing in order", "[timer]") {
  std::atomic<int> counter1{0};
  std::atomic<int> counter2{0};
  std::atomic<int> counter3{0};
  osp::TimerScheduler<8> sched;

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
  osp::TimerScheduler<4> sched;

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
  osp::TimerScheduler<8> sched;

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

// ============================================================================
// Template Parameterization Tests (Phase 1.5b)
// ============================================================================

TEST_CASE("TimerScheduler Capacity constexpr", "[timer]") {
  REQUIRE(osp::TimerScheduler<4>::Capacity() == 4);
  REQUIRE(osp::TimerScheduler<16>::Capacity() == 16);
  REQUIRE(osp::TimerScheduler<32>::Capacity() == 32);

  // Default template parameter
  REQUIRE(osp::TimerScheduler<>::Capacity() == 16);
}

TEST_CASE("TimerScheduler<1> single slot", "[timer]") {
  osp::TimerScheduler<1> sched;

  auto r1 = sched.Add(100, [](void*) {});
  REQUIRE(r1.has_value());
  REQUIRE(sched.TaskCount() == 1);

  // Second add should fail
  auto r2 = sched.Add(100, [](void*) {});
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::TimerError::kSlotsFull);

  // Remove and re-add
  sched.Remove(r1.value());
  REQUIRE(sched.TaskCount() == 0);

  auto r3 = sched.Add(100, [](void*) {});
  REQUIRE(r3.has_value());
}

TEST_CASE("TimerScheduler<32> large capacity", "[timer]") {
  osp::TimerScheduler<32> sched;

  // Fill all slots
  osp::TimerTaskId ids[32] = {
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
    osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0), osp::TimerTaskId(0),
  };
  for (uint32_t i = 0; i < 32; ++i) {
    auto r = sched.Add(100, [](void*) {});
    REQUIRE(r.has_value());
    ids[i] = r.value();
  }
  REQUIRE(sched.TaskCount() == 32);

  // 33rd should fail
  auto overflow = sched.Add(100, [](void*) {});
  REQUIRE(!overflow.has_value());

  // Remove all
  for (uint32_t i = 0; i < 32; ++i) {
    auto rm = sched.Remove(ids[i]);
    REQUIRE(rm.has_value());
  }
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler zero heap allocation", "[timer]") {
  // This test verifies the type is stack-allocatable with embedded array.
  // sizeof should include MaxTasks * sizeof(TaskSlot) + overhead.
  // TaskSlot: fn(8) + ctx(8) + period_ns(8) + next_fire_ns(8) + id(4) + active(1) + pad
  // Rough check: should be > MaxTasks * 32 (minimum slot size)
  constexpr size_t sz4 = sizeof(osp::TimerScheduler<4>);
  constexpr size_t sz16 = sizeof(osp::TimerScheduler<16>);
  REQUIRE(sz16 > sz4);
  // Larger capacity = larger object (proves embedded array, not pointer)
  REQUIRE(sz16 >= sz4 + 12 * 32);  // at least 12 more slots worth
}

// ============================================================================
// Collect-Release-Execute Pattern Tests (Phase 1.5a)
// ============================================================================

TEST_CASE("TimerScheduler callback executes outside mutex", "[timer]") {
  // Verify that Add() is not blocked while a callback is executing.
  // If callbacks ran inside the mutex, this would deadlock or timeout.
  osp::TimerScheduler<4> sched;
  std::atomic<bool> callback_running{false};
  std::atomic<bool> add_succeeded{false};

  // Register a callback that holds for a while
  sched.Add(1, [](void* ctx) {
    auto* running = static_cast<std::atomic<bool>*>(ctx);
    running->store(true, std::memory_order_release);
    // Sleep to give the main thread time to call Add()
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }, &callback_running);

  sched.Start();

  // Wait for callback to start executing
  while (!callback_running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Now try to Add() while callback is running.
  // If mutex is held during callback, this would block for 50ms+.
  auto start = std::chrono::steady_clock::now();
  auto r = sched.Add(1000, [](void*) {});
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  // Add() should complete quickly (< 20ms), not wait for callback
  REQUIRE(r.has_value());
  REQUIRE(elapsed.count() < 20);

  sched.Stop();
}

TEST_CASE("TimerScheduler callback can safely acquire external mutex", "[timer]") {
  // This is the key deadlock scenario from the design doc.
  // Callback acquires an external mutex — should not deadlock.
  std::mutex external_mutex;
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  struct Ctx {
    std::mutex* mtx;
    std::atomic<int>* counter;
  };
  Ctx ctx{&external_mutex, &counter};

  sched.Add(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    std::lock_guard<std::mutex> lock(*c->mtx);
    c->counter->fetch_add(1);
  }, &ctx);

  sched.Start();

  // Main thread also acquires external_mutex while scheduler is running.
  // If callbacks ran inside TimerScheduler::mutex_, and main thread held
  // external_mutex while calling Add(), we'd have a lock-order inversion.
  for (int i = 0; i < 10; ++i) {
    {
      std::lock_guard<std::mutex> lock(external_mutex);
      // Simulate work under external lock
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Also call Add/Remove while scheduler is running
    auto r = sched.Add(1000, [](void*) {});
    if (r.has_value()) {
      sched.Remove(r.value());
    }
  }

  sched.Stop();
  REQUIRE(counter.load() > 0);
}

TEST_CASE("TimerScheduler Remove during callback execution", "[timer]") {
  // Remove a task while its callback is executing.
  // With collect-release-execute, the callback snapshot is already taken,
  // so Remove() should succeed immediately.
  osp::TimerScheduler<4> sched;
  std::atomic<bool> callback_started{false};
  std::atomic<bool> remove_done{false};
  std::atomic<int> fire_count{0};

  osp::TimerTaskId task_id(0);

  auto r = sched.Add(1, [](void* ctx) {
    auto* started = static_cast<std::atomic<bool>*>(ctx);
    started->store(true, std::memory_order_release);
    // Hold callback for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }, &callback_started);
  REQUIRE(r.has_value());
  task_id = r.value();

  sched.Start();

  // Wait for callback to start
  while (!callback_started.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Remove while callback is running — should not block
  auto start = std::chrono::steady_clock::now();
  auto rm = sched.Remove(task_id);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  REQUIRE(rm.has_value());
  REQUIRE(elapsed.count() < 20);

  sched.Stop();
}

TEST_CASE("TimerScheduler Add during callback execution", "[timer]") {
  // Add a new task while a callback is executing.
  osp::TimerScheduler<4> sched;
  std::atomic<bool> callback_started{false};
  std::atomic<int> new_task_fires{0};

  sched.Add(1, [](void* ctx) {
    auto* started = static_cast<std::atomic<bool>*>(ctx);
    started->store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }, &callback_started);

  sched.Start();

  // Wait for callback to start
  while (!callback_started.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Add new task while callback is running
  auto r = sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &new_task_fires);
  REQUIRE(r.has_value());

  // Wait for new task to fire
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  REQUIRE(new_task_fires.load() > 0);
}

// ============================================================================
// Concurrent Add/Remove While Running
// ============================================================================

TEST_CASE("TimerScheduler concurrent Add/Remove stress", "[timer]") {
  osp::TimerScheduler<16> sched;
  std::atomic<int> total_fires{0};

  sched.Start();

  // Spawn multiple threads that add and remove timers concurrently
  std::thread threads[4];
  for (int t = 0; t < 4; ++t) {
    threads[t] = std::thread([&sched, &total_fires]() {
      for (int i = 0; i < 20; ++i) {
        auto r = sched.Add(1, [](void* ctx) {
          auto* c = static_cast<std::atomic<int>*>(ctx);
          c->fetch_add(1);
        }, &total_fires);
        if (r.has_value()) {
          // Sleep long enough for at least one callback to fire
          std::this_thread::sleep_for(std::chrono::milliseconds(15));
          sched.Remove(r.value());
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  sched.Stop();

  // Some callbacks should have fired
  REQUIRE(total_fires.load() > 0);
  // All tasks should be cleaned up
  REQUIRE(sched.TaskCount() == 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("TimerScheduler Stop without Start", "[timer]") {
  osp::TimerScheduler<4> sched;
  // Should not crash or hang
  sched.Stop();
  REQUIRE(!sched.IsRunning());
}

TEST_CASE("TimerScheduler double Stop", "[timer]") {
  osp::TimerScheduler<4> sched;
  sched.Start();
  sched.Stop();
  sched.Stop();  // Second stop should be safe
  REQUIRE(!sched.IsRunning());
}

TEST_CASE("TimerScheduler destructor stops thread", "[timer]") {
  std::atomic<int> counter{0};
  {
    osp::TimerScheduler<4> sched;
    sched.Add(10, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      c->fetch_add(1);
    }, &counter);
    sched.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Destructor should call Stop() and join the thread
  }
  // If we get here without hanging, the destructor worked
  REQUIRE(counter.load() > 0);
}

TEST_CASE("TimerScheduler slot reuse after remove", "[timer]") {
  osp::TimerScheduler<2> sched;

  // Fill both slots
  auto r1 = sched.Add(100, [](void*) {});
  auto r2 = sched.Add(100, [](void*) {});
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(sched.TaskCount() == 2);

  // Remove first, add new — should reuse the slot
  sched.Remove(r1.value());
  REQUIRE(sched.TaskCount() == 1);

  auto r3 = sched.Add(100, [](void*) {});
  REQUIRE(r3.has_value());
  REQUIRE(sched.TaskCount() == 2);

  // IDs should be unique
  REQUIRE(r3.value().value() != r1.value().value());
  REQUIRE(r3.value().value() != r2.value().value());
}

TEST_CASE("TimerScheduler unique IDs across add/remove cycles", "[timer]") {
  osp::TimerScheduler<4> sched;
  uint32_t prev_id = 0;

  for (int i = 0; i < 100; ++i) {
    auto r = sched.Add(100, [](void*) {});
    REQUIRE(r.has_value());
    REQUIRE(r.value().value() > prev_id);
    prev_id = r.value().value();
    sched.Remove(r.value());
  }
}

TEST_CASE("TimerScheduler callback with nullptr context", "[timer]") {
  std::atomic<int> fired{0};
  osp::TimerScheduler<4> sched;

  // Use a static variable since ctx is nullptr
  static std::atomic<int>* s_fired = &fired;

  sched.Add(10, [](void*) {
    s_fired->fetch_add(1);
  }, nullptr);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  REQUIRE(fired.load() > 0);
}

TEST_CASE("TimerScheduler Start after Stop can restart", "[timer]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);

  // First run
  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();
  int count1 = counter.load();
  REQUIRE(count1 > 0);

  // Second run — should work
  auto r = sched.Start();
  REQUIRE(r.has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();
  REQUIRE(counter.load() > count1);
}
