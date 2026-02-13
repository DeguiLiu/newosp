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

// ============================================================================
// Phase 3: One-shot Timer Tests
// ============================================================================

TEST_CASE("TimerScheduler AddOneShot fires once", "[timer][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.AddOneShot(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());
  REQUIRE(sched.TaskCount() == 1);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Should have fired exactly once
  REQUIRE(counter.load() == 1);
  // Slot should be auto-deactivated
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler AddOneShot invalid delay", "[timer][oneshot]") {
  osp::TimerScheduler<4> sched;
  auto r = sched.AddOneShot(0, [](void*) {});
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler AddOneShot slots full", "[timer][oneshot]") {
  osp::TimerScheduler<2> sched;
  auto r1 = sched.AddOneShot(100, [](void*) {});
  REQUIRE(r1.has_value());
  auto r2 = sched.AddOneShot(100, [](void*) {});
  REQUIRE(r2.has_value());
  auto r3 = sched.AddOneShot(100, [](void*) {});
  REQUIRE(!r3.has_value());
  REQUIRE(r3.get_error() == osp::TimerError::kSlotsFull);
}

TEST_CASE("TimerScheduler AddOneShot Remove before fire", "[timer][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.AddOneShot(500, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());

  // Remove before it fires
  auto rm = sched.Remove(r.value());
  REQUIRE(rm.has_value());
  REQUIRE(sched.TaskCount() == 0);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Should not have fired
  REQUIRE(counter.load() == 0);
}

TEST_CASE("TimerScheduler AddOneShot Remove after fire returns kNotRunning", "[timer][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.AddOneShot(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());
  osp::TimerTaskId task_id = r.value();

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  REQUIRE(counter.load() == 1);

  // Remove after fire — should return kNotRunning (already deactivated)
  auto rm = sched.Remove(task_id);
  REQUIRE(!rm.has_value());
  REQUIRE(rm.get_error() == osp::TimerError::kNotRunning);
}

TEST_CASE("TimerScheduler AddOneShot slot reuse after fire", "[timer][oneshot]") {
  osp::TimerScheduler<1> sched;

  auto r1 = sched.AddOneShot(10, [](void*) {});
  REQUIRE(r1.has_value());
  REQUIRE(sched.TaskCount() == 1);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  // Slot should be free now
  REQUIRE(sched.TaskCount() == 0);

  // Can add a new task in the freed slot
  auto r2 = sched.Add(100, [](void*) {});
  REQUIRE(r2.has_value());
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler mixed periodic and one-shot", "[timer][oneshot]") {
  std::atomic<int> periodic_count{0};
  std::atomic<int> oneshot_count{0};
  osp::TimerScheduler<4> sched;

  sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &periodic_count);

  sched.AddOneShot(20, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &oneshot_count);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Periodic should have fired multiple times
  REQUIRE(periodic_count.load() > 1);
  // One-shot should have fired exactly once
  REQUIRE(oneshot_count.load() == 1);
  // Only periodic task remains active
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler AddOneShot unique IDs", "[timer][oneshot]") {
  osp::TimerScheduler<4> sched;

  auto r1 = sched.AddOneShot(100, [](void*) {});
  auto r2 = sched.Add(100, [](void*) {});
  auto r3 = sched.AddOneShot(100, [](void*) {});

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(r3.has_value());

  // All IDs should be unique
  REQUIRE(r1.value().value() != r2.value().value());
  REQUIRE(r2.value().value() != r3.value().value());
  REQUIRE(r1.value().value() != r3.value().value());
}

// ============================================================================
// Additional Edge Case Tests
// ============================================================================

TEST_CASE("TimerScheduler Add with null fn pointer", "[timer][edge]") {
  osp::TimerScheduler<4> sched;

  // Add with nullptr callback should fail
  auto r1 = sched.Add(100, nullptr);
  REQUIRE(!r1.has_value());
  REQUIRE(r1.get_error() == osp::TimerError::kInvalidPeriod);

  // AddOneShot with nullptr callback should also fail
  auto r2 = sched.AddOneShot(100, nullptr);
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::TimerError::kInvalidPeriod);
}

TEST_CASE("TimerScheduler AddOneShot from within callback", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};

  struct Ctx {
    osp::TimerScheduler<4>* sched;
    std::atomic<int>* counter;
  };

  osp::TimerScheduler<4> sched;
  Ctx ctx{&sched, &counter};

  // Register a one-shot that adds another one-shot in its callback
  auto r1 = sched.AddOneShot(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    c->counter->fetch_add(1);

    // Add another one-shot from within the callback
    c->sched->AddOneShot(10, [](void* raw2) {
      auto* c2 = static_cast<Ctx*>(raw2);
      c2->counter->fetch_add(1);
    }, raw);
  }, &ctx);

  REQUIRE(r1.has_value());
  REQUIRE(sched.TaskCount() == 1);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Both one-shots should have fired
  REQUIRE(counter.load() == 2);
  // All slots should be freed
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler Remove from within callback", "[timer][edge]") {
  std::atomic<int> counter1{0};
  std::atomic<int> counter2{0};

  struct Ctx {
    osp::TimerScheduler<4>* sched;
    osp::TimerTaskId task2_id;
    std::atomic<int>* counter1;
    std::atomic<int>* counter2;
  };

  osp::TimerScheduler<4> sched;
  Ctx ctx{&sched, osp::TimerTaskId(0), &counter1, &counter2};

  // Add task 1 that removes task 2 on first fire
  auto r1 = sched.Add(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    int prev = c->counter1->fetch_add(1);
    if (prev == 0) {
      // First fire: remove task 2
      c->sched->Remove(c->task2_id);
    }
  }, &ctx);

  // Add task 2
  auto r2 = sched.Add(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    c->counter2->fetch_add(1);
  }, &ctx);

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  ctx.task2_id = r2.value();

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Task 1 should have fired multiple times
  REQUIRE(counter1.load() > 1);
  // Task 2 should have fired at most once (removed by task 1)
  REQUIRE(counter2.load() <= 1);
}

TEST_CASE("TimerScheduler Add from within callback fills slots", "[timer][edge]") {
  std::atomic<int> counter{0};
  std::atomic<bool> tried_add{false};

  struct Ctx {
    osp::TimerScheduler<2>* sched;
    std::atomic<int>* counter;
    std::atomic<bool>* tried_add;
  };

  osp::TimerScheduler<2> sched;
  Ctx ctx{&sched, &counter, &tried_add};

  // Add first task that tries to add another task
  auto r1 = sched.Add(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    c->counter->fetch_add(1);

    if (!c->tried_add->load()) {
      c->tried_add->store(true);
      // Try to add another task from callback
      c->sched->Add(50, [](void*) {});
    }
  }, &ctx);

  REQUIRE(r1.has_value());
  REQUIRE(sched.TaskCount() == 1);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  // After callback added a task, slots should be 2/2
  REQUIRE(sched.TaskCount() == 2);

  // Try to add from main thread — should fail with kSlotsFull
  auto r3 = sched.Add(100, [](void*) {});
  REQUIRE(!r3.has_value());
  REQUIRE(r3.get_error() == osp::TimerError::kSlotsFull);
}

TEST_CASE("TimerScheduler multiple one-shots same delay", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<8> sched;

  // Add 4 one-shots all with same delay
  for (int i = 0; i < 4; ++i) {
    auto r = sched.AddOneShot(10, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      c->fetch_add(1);
    }, &counter);
    REQUIRE(r.has_value());
  }

  REQUIRE(sched.TaskCount() == 4);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // All 4 should fire exactly once each
  REQUIRE(counter.load() == 4);
  // All slots should be freed
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler one-shot during periodic execution", "[timer][edge][oneshot]") {
  std::atomic<int> periodic_count{0};
  std::atomic<int> oneshot_count{0};
  osp::TimerScheduler<4> sched;

  // Add periodic task (10ms)
  auto r1 = sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &periodic_count);

  // Add one-shot (30ms)
  auto r2 = sched.AddOneShot(30, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &oneshot_count);

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(sched.TaskCount() == 2);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Periodic should fire multiple times
  REQUIRE(periodic_count.load() > 1);
  // One-shot should fire exactly once
  REQUIRE(oneshot_count.load() == 1);
  // Only periodic remains
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler rapid AddOneShot and Remove cycles", "[timer][edge][oneshot]") {
  osp::TimerScheduler<8> sched;

  // Rapidly add and remove one-shot timers without starting
  for (int i = 0; i < 50; ++i) {
    auto r = sched.AddOneShot(100, [](void*) {});
    REQUIRE(r.has_value());
    auto rm = sched.Remove(r.value());
    REQUIRE(rm.has_value());
  }

  REQUIRE(sched.TaskCount() == 0);

  // Add one and verify it fires
  std::atomic<int> counter{0};
  auto r = sched.AddOneShot(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  REQUIRE(counter.load() == 1);
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler Start Stop Start with one-shot", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  // Add a one-shot with 500ms delay
  auto r = sched.AddOneShot(500, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());

  // Start, wait 20ms (not enough to fire), Stop
  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sched.Stop();

  REQUIRE(counter.load() == 0);

  // Start again, wait 600ms, Stop
  auto r2 = sched.Start();
  REQUIRE(r2.has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  sched.Stop();

  // The one-shot should have fired (absolute time tracking)
  REQUIRE(counter.load() == 1);
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler concurrent AddOneShot stress", "[timer][edge][oneshot]") {
  osp::TimerScheduler<64> sched;
  std::atomic<int> counter{0};

  sched.Start();

  // Spawn 4 threads, each adds 10 one-shots
  std::thread threads[4];
  for (int t = 0; t < 4; ++t) {
    threads[t] = std::thread([&sched, &counter]() {
      for (int i = 0; i < 10; ++i) {
        sched.AddOneShot(10, [](void* ctx) {
          auto* c = static_cast<std::atomic<int>*>(ctx);
          c->fetch_add(1);
        }, &counter);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Wait for all to fire
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sched.Stop();

  // Total fire count should be 40
  REQUIRE(counter.load() == 40);
  // All slots should be freed
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler periodic timer accuracy", "[timer][edge]") {
  std::atomic<int> counter{0};
  std::atomic<uint64_t> timestamps[20];
  for (int i = 0; i < 20; ++i) {
    timestamps[i].store(0);
  }

  struct Ctx {
    std::atomic<int>* counter;
    std::atomic<uint64_t>* timestamps;
  };

  osp::TimerScheduler<4> sched;
  Ctx ctx{&counter, timestamps};

  // Add a 50ms periodic timer
  auto r = sched.Add(50, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    int idx = c->counter->fetch_add(1);
    if (idx < 20) {
      auto now = std::chrono::steady_clock::now().time_since_epoch();
      c->timestamps[idx].store(
          std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }
  }, &ctx);

  REQUIRE(r.has_value());

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  sched.Stop();

  int fires = counter.load();
  // Should fire approximately 10 times (allow 7-13 for CI tolerance)
  REQUIRE(fires >= 7);
  REQUIRE(fires <= 13);

  // Check intervals between fires (should be roughly 50ms, allow 30-80ms)
  for (int i = 1; i < fires && i < 20; ++i) {
    uint64_t t1 = timestamps[i - 1].load();
    uint64_t t2 = timestamps[i].load();
    if (t1 > 0 && t2 > 0) {
      uint64_t interval = t2 - t1;
      REQUIRE(interval >= 30);
      REQUIRE(interval <= 80);
    }
  }
}

// ============================================================================
// Additional Edge Case Tests (New)
// ============================================================================

TEST_CASE("TimerScheduler: period_ms=1 minimum legal period fires rapidly", "[timer][edge]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.Add(1, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  // Should fire many times (>10) in 50ms with 1ms period
  REQUIRE(counter.load() > 10);
}

TEST_CASE("TimerScheduler: UINT32_MAX period_ms does not overflow", "[timer][edge]") {
  osp::TimerScheduler<4> sched;

  // Add timer with maximum period
  auto r = sched.Add(UINT32_MAX, [](void*) {});
  REQUIRE(r.has_value());
  REQUIRE(sched.TaskCount() == 1);

  // Don't start the scheduler - just verify Add succeeded without overflow
  // Starting would cause the test to wait too long
  // The key test is that Add() doesn't reject UINT32_MAX and doesn't overflow internally

  // Clean up
  auto rm = sched.Remove(r.value());
  REQUIRE(rm.has_value());
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler: all one-shots same delay fire exactly once each", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<16> sched;

  // Add MaxTasks one-shots all with same delay (30ms)
  for (uint32_t i = 0; i < 16; ++i) {
    auto r = sched.AddOneShot(30, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      c->fetch_add(1);
    }, &counter);
    REQUIRE(r.has_value());
  }

  REQUIRE(sched.TaskCount() == 16);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Each should fire exactly once
  REQUIRE(counter.load() == 16);
  // All slots should be freed
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler: one-shot TaskCount decrements after fire", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.AddOneShot(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());
  REQUIRE(sched.TaskCount() == 1);

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  // Should have fired
  REQUIRE(counter.load() == 1);
  // TaskCount should be 0 now
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler: Remove already-fired one-shot returns error", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<4> sched;

  auto r = sched.AddOneShot(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r.has_value());
  osp::TimerTaskId task_id = r.value();

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  REQUIRE(counter.load() == 1);

  // Try to remove after it already fired
  auto rm = sched.Remove(task_id);
  REQUIRE(!rm.has_value());
  REQUIRE(rm.get_error() == osp::TimerError::kNotRunning);
}

TEST_CASE("TimerScheduler: Stop waits for executing callback to finish", "[timer][edge]") {
  // Use a contiguous array so flags[0] and flags[1] are adjacent without
  // ASan red-zones between them (two separate stack variables would have
  // red-zones, causing a false stack-buffer-overflow).
  std::atomic<bool> flags[2];
  flags[0].store(false, std::memory_order_relaxed);  // callback_started
  flags[1].store(false, std::memory_order_relaxed);  // callback_finished
  osp::TimerScheduler<4> sched;

  auto r = sched.Add(1, [](void* ctx) {
    auto* f = static_cast<std::atomic<bool>*>(ctx);
    f[0].store(true, std::memory_order_release);
    // Sleep for 100ms in callback
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f[1].store(true, std::memory_order_release);
  }, &flags[0]);
  REQUIRE(r.has_value());

  sched.Start();

  // Wait for callback to start
  while (!flags[0].load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Call Stop() while callback is executing
  sched.Stop();

  // Stop() should have waited for callback to finish
  REQUIRE(flags[1].load(std::memory_order_acquire));
}

TEST_CASE("TimerScheduler: rapid Start-Stop cycles are safe", "[timer][edge]") {
  osp::TimerScheduler<4> sched;

  // Add a timer
  auto r = sched.Add(10, [](void*) {});
  REQUIRE(r.has_value());

  // Rapid Start-Stop cycles
  for (int i = 0; i < 20; ++i) {
    auto start_result = sched.Start();
    REQUIRE(start_result.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sched.Stop();
  }

  // No crash, no leak
  REQUIRE(!sched.IsRunning());
  REQUIRE(sched.TaskCount() == 1);
}

TEST_CASE("TimerScheduler: Add many one-shots before Start all fire", "[timer][edge][oneshot]") {
  std::atomic<int> counter{0};
  osp::TimerScheduler<16> sched;

  // Add 16 one-shots with delay=10ms BEFORE Start
  for (int i = 0; i < 16; ++i) {
    auto r = sched.AddOneShot(10, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      c->fetch_add(1);
    }, &counter);
    REQUIRE(r.has_value());
  }

  REQUIRE(sched.TaskCount() == 16);

  // Now start
  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // All 16 should have fired
  REQUIRE(counter.load() == 16);
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler: concurrent AddOneShot stress from 8 threads", "[timer][edge][oneshot]") {
  osp::TimerScheduler<16> sched;
  std::atomic<int> counter{0};
  std::atomic<int> add_success{0};

  sched.Start();

  // 8 threads each try to AddOneShot
  std::thread threads[8];
  for (int t = 0; t < 8; ++t) {
    threads[t] = std::thread([&sched, &counter, &add_success]() {
      for (int i = 0; i < 5; ++i) {
        auto r = sched.AddOneShot(10, [](void* ctx) {
          auto* c = static_cast<std::atomic<int>*>(ctx);
          c->fetch_add(1);
        }, &counter);
        if (r.has_value()) {
          add_success.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Wait for all to fire
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // Some will fail (slots full with MaxTasks=16)
  // Total successful adds should be <= 16
  REQUIRE(add_success.load() <= 16);
  // Total fired should equal successful adds
  REQUIRE(counter.load() == add_success.load());
  // All slots should be freed
  REQUIRE(sched.TaskCount() == 0);
}

TEST_CASE("TimerScheduler: Remove from callback then re-Add same slot", "[timer][edge]") {
  std::atomic<int> counter1{0};
  std::atomic<int> counter2{0};

  struct Ctx {
    osp::TimerScheduler<4>* sched;
    osp::TimerTaskId self_id;
    std::atomic<int>* counter1;
    std::atomic<int>* counter2;
  };

  osp::TimerScheduler<4> sched;
  Ctx ctx{&sched, osp::TimerTaskId(0), &counter1, &counter2};

  // Add a one-shot that removes itself and adds a new timer
  auto r = sched.AddOneShot(10, [](void* raw) {
    auto* c = static_cast<Ctx*>(raw);
    c->counter1->fetch_add(1);

    // Remove self
    c->sched->Remove(c->self_id);

    // Add a new timer in the same slot
    c->sched->Add(10, [](void* raw2) {
      auto* c2 = static_cast<Ctx*>(raw2);
      c2->counter2->fetch_add(1);
    }, raw);
  }, &ctx);

  REQUIRE(r.has_value());
  ctx.self_id = r.value();

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sched.Stop();

  // First callback should have fired once
  REQUIRE(counter1.load() == 1);
  // New timer should have fired multiple times
  REQUIRE(counter2.load() > 0);
}

TEST_CASE("TimerScheduler: double Remove same ID returns kNotRunning second time", "[timer][edge]") {
  osp::TimerScheduler<4> sched;

  auto r = sched.Add(100, [](void*) {});
  REQUIRE(r.has_value());
  osp::TimerTaskId task_id = r.value();

  // First Remove should succeed
  auto rm1 = sched.Remove(task_id);
  REQUIRE(rm1.has_value());

  // Second Remove should fail with kNotRunning
  auto rm2 = sched.Remove(task_id);
  REQUIRE(!rm2.has_value());
  REQUIRE(rm2.get_error() == osp::TimerError::kNotRunning);
}

TEST_CASE("TimerScheduler: Add with nullptr fn returns error or fires safely", "[timer][edge]") {
  osp::TimerScheduler<4> sched;

  // Add with nullptr callback should fail (checked in timer.hpp line 133-136)
  auto r1 = sched.Add(100, nullptr);
  REQUIRE(!r1.has_value());
  REQUIRE(r1.get_error() == osp::TimerError::kInvalidPeriod);

  // AddOneShot with nullptr callback should also fail
  auto r2 = sched.AddOneShot(100, nullptr);
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::TimerError::kInvalidPeriod);

  // Verify scheduler still works after nullptr attempts
  std::atomic<int> counter{0};
  auto r3 = sched.Add(10, [](void* ctx) {
    auto* c = static_cast<std::atomic<int>*>(ctx);
    c->fetch_add(1);
  }, &counter);
  REQUIRE(r3.has_value());

  sched.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched.Stop();

  REQUIRE(counter.load() > 0);
}
