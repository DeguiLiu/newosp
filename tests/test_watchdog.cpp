/**
 * @file test_watchdog.cpp
 * @brief Tests for watchdog.hpp
 */

#include "osp/watchdog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// ============================================================================
// Basic API Tests
// ============================================================================

TEST_CASE("ThreadWatchdog: Register and Unregister", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  REQUIRE(wd.ActiveCount() == 0);

  auto result = wd.Register("TestThread", 1000);
  REQUIRE(result.has_value());
  REQUIRE(wd.ActiveCount() == 1);

  auto unreg = wd.Unregister(result.value().id);
  REQUIRE(unreg.has_value());
  REQUIRE(wd.ActiveCount() == 0);
}

TEST_CASE("ThreadWatchdog: Register returns unique IDs", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id1 = wd.Register("Thread1", 1000);
  auto id2 = wd.Register("Thread2", 1000);
  auto id3 = wd.Register("Thread3", 1000);

  REQUIRE(id1.has_value());
  REQUIRE(id2.has_value());
  REQUIRE(id3.has_value());

  REQUIRE(id1.value().id.value() != id2.value().id.value());
  REQUIRE(id2.value().id.value() != id3.value().id.value());
  REQUIRE(id1.value().id.value() != id3.value().id.value());

  REQUIRE(wd.ActiveCount() == 3);
}

TEST_CASE("ThreadWatchdog: Register with invalid timeout returns error", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto result = wd.Register("TestThread", 0);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::WatchdogError::kInvalidTimeout);
  REQUIRE(wd.ActiveCount() == 0);
}

TEST_CASE("ThreadWatchdog: Slots full returns kSlotsFull", "[watchdog]") {
  osp::ThreadWatchdog<4> wd;

  // Fill all slots
  auto id1 = wd.Register("Thread1", 1000);
  auto id2 = wd.Register("Thread2", 1000);
  auto id3 = wd.Register("Thread3", 1000);
  auto id4 = wd.Register("Thread4", 1000);

  REQUIRE(id1.has_value());
  REQUIRE(id2.has_value());
  REQUIRE(id3.has_value());
  REQUIRE(id4.has_value());
  REQUIRE(wd.ActiveCount() == 4);

  // Next registration should fail
  auto id5 = wd.Register("Thread5", 1000);
  REQUIRE(!id5.has_value());
  REQUIRE(id5.get_error() == osp::WatchdogError::kSlotsFull);
}

TEST_CASE("ThreadWatchdog: Unregister invalid ID returns kNotRegistered", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto result = wd.Unregister(osp::WatchdogSlotId(999));
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::WatchdogError::kNotRegistered);
}

TEST_CASE("ThreadWatchdog: Double Unregister returns kNotRegistered", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 1000);
  REQUIRE(id.has_value());

  auto unreg1 = wd.Unregister(id.value().id);
  REQUIRE(unreg1.has_value());

  auto unreg2 = wd.Unregister(id.value().id);
  REQUIRE(!unreg2.has_value());
  REQUIRE(unreg2.get_error() == osp::WatchdogError::kNotRegistered);
}

TEST_CASE("ThreadWatchdog: Feed updates last_feed timestamp", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 200);
  REQUIRE(id.has_value());

  // Feed immediately
  wd.Feed(id.value().id);

  // Sleep less than timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Check should not detect timeout
  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);
  REQUIRE(wd.TimedOutCount() == 0);
}

TEST_CASE("ThreadWatchdog: Capacity constexpr", "[watchdog]") {
  osp::ThreadWatchdog<16> wd;
  // Capacity is implicit in template parameter
  // Verify we can register up to MaxThreads
  for (uint32_t i = 0; i < 16; ++i) {
    auto result = wd.Register("Thread", 1000);
    REQUIRE(result.has_value());
  }
  REQUIRE(wd.ActiveCount() == 16);
}

// ============================================================================
// Timeout Detection Tests
// ============================================================================

TEST_CASE("ThreadWatchdog: Check detects timeout", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 50);
  REQUIRE(id.has_value());

  // Don't feed, let it timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 1);
  REQUIRE(wd.TimedOutCount() == 1);
}

TEST_CASE("ThreadWatchdog: Check does not false-positive", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 200);
  REQUIRE(id.has_value());

  // Feed to reset timestamp
  wd.Feed(id.value().id);

  // Sleep less than timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);
  REQUIRE(wd.TimedOutCount() == 0);
}

TEST_CASE("ThreadWatchdog: timeout callback fires", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::atomic<bool> callback_fired{false};
  std::atomic<uint32_t> callback_slot_id{999};
  std::string callback_name;

  struct CallbackData {
    std::atomic<bool>* fired;
    std::atomic<uint32_t>* slot_id;
    std::string* name;
  };

  CallbackData data{&callback_fired, &callback_slot_id, &callback_name};

  wd.SetOnTimeout([](uint32_t id, const char* name, void* ctx) {
    auto* d = static_cast<CallbackData*>(ctx);
    d->fired->store(true, std::memory_order_release);
    d->slot_id->store(id, std::memory_order_release);
    *d->name = name;
  }, &data);

  auto id = wd.Register("TimeoutThread", 50);
  REQUIRE(id.has_value());

  // Don't feed, let it timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  wd.Check();

  REQUIRE(callback_fired.load(std::memory_order_acquire));
  REQUIRE(callback_slot_id.load(std::memory_order_acquire) == id.value().id.value());
  REQUIRE(callback_name == "TimeoutThread");
}

TEST_CASE("ThreadWatchdog: recovery callback fires", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::atomic<bool> timeout_fired{false};
  std::atomic<bool> recovery_fired{false};

  wd.SetOnTimeout([](uint32_t, const char*, void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
  }, &timeout_fired);

  wd.SetOnRecovered([](uint32_t, const char*, void* ctx) {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
  }, &recovery_fired);

  auto id = wd.Register("RecoverThread", 50);
  REQUIRE(id.has_value());

  // Let it timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  wd.Check();
  REQUIRE(timeout_fired.load(std::memory_order_acquire));

  // Feed to recover
  wd.Feed(id.value().id);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  wd.Check();

  REQUIRE(recovery_fired.load(std::memory_order_acquire));
}

TEST_CASE("ThreadWatchdog: TimedOutCount tracks correctly", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id1 = wd.Register("Thread1", 50);
  auto id2 = wd.Register("Thread2", 50);
  auto id3 = wd.Register("Thread3", 200);

  REQUIRE(id1.has_value());
  REQUIRE(id2.has_value());
  REQUIRE(id3.has_value());

  // Feed id3 to keep it alive
  wd.Feed(id3.value().id);

  // Let id1 and id2 timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  wd.Check();

  REQUIRE(wd.TimedOutCount() == 2);
  REQUIRE(wd.ActiveCount() == 3);
}

TEST_CASE("ThreadWatchdog: IsTimedOut per-slot query", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id1 = wd.Register("Thread1", 50);
  auto id2 = wd.Register("Thread2", 200);

  REQUIRE(id1.has_value());
  REQUIRE(id2.has_value());

  // Feed id2 to keep it alive
  wd.Feed(id2.value().id);

  // Let id1 timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  wd.Check();

  REQUIRE(wd.IsTimedOut(id1.value().id));
  REQUIRE(!wd.IsTimedOut(id2.value().id));
}

// ============================================================================
// RAII Guard Tests
// ============================================================================

TEST_CASE("WatchdogGuard: auto register and unregister", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  REQUIRE(wd.ActiveCount() == 0);

  {
    osp::WatchdogGuard<8> guard(&wd, "GuardThread", 1000);
    REQUIRE(guard.IsValid());
    REQUIRE(wd.ActiveCount() == 1);
  }

  REQUIRE(wd.ActiveCount() == 0);
}

TEST_CASE("WatchdogGuard: Feed works", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  osp::WatchdogGuard<8> guard(&wd, "GuardThread", 200);
  REQUIRE(guard.IsValid());

  guard.Feed();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);
}

TEST_CASE("WatchdogGuard: IsValid returns true on success", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  osp::WatchdogGuard<8> guard(&wd, "GuardThread", 1000);
  REQUIRE(guard.IsValid());
}

TEST_CASE("WatchdogGuard: invalid guard when slots full", "[watchdog]") {
  osp::ThreadWatchdog<2> wd;

  // Fill all slots
  auto id1 = wd.Register("Thread1", 1000);
  auto id2 = wd.Register("Thread2", 1000);

  REQUIRE(id1.has_value());
  REQUIRE(id2.has_value());

  // Guard should fail to register
  osp::WatchdogGuard<2> guard(&wd, "GuardThread", 1000);
  REQUIRE(!guard.IsValid());
}

// ============================================================================
// Multi-thread Tests
// ============================================================================

TEST_CASE("ThreadWatchdog: concurrent Feed from multiple threads", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::vector<osp::WatchdogSlotId> ids;
  for (int i = 0; i < 4; ++i) {
    auto id = wd.Register("WorkerThread", 200);
    REQUIRE(id.has_value());
    ids.push_back(id.value().id);
  }

  std::atomic<bool> running{true};
  std::vector<std::thread> threads;

  for (size_t i = 0; i < ids.size(); ++i) {
    threads.emplace_back([&wd, id = ids[i], &running]() {
      while (running.load(std::memory_order_acquire)) {
        wd.Feed(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }

  // Let threads run for a while
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);

  running.store(false, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }
}

TEST_CASE("ThreadWatchdog: detect hung thread", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::vector<osp::WatchdogSlotId> ids;
  for (int i = 0; i < 4; ++i) {
    auto id = wd.Register("WorkerThread", 100);
    REQUIRE(id.has_value());
    ids.push_back(id.value().id);
  }

  std::atomic<bool> running{true};
  std::vector<std::thread> threads;

  for (size_t i = 0; i < ids.size(); ++i) {
    threads.emplace_back([&wd, id = ids[i], &running, i]() {
      while (running.load(std::memory_order_acquire)) {
        if (i != 2) {  // Thread 2 stops feeding
          wd.Feed(id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }

  // Let threads run, then check
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 1);

  running.store(false, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }
}

TEST_CASE("ThreadWatchdog: concurrent Register/Unregister stress", "[watchdog]") {
  osp::ThreadWatchdog<16> wd;

  std::atomic<bool> running{true};
  std::atomic<uint32_t> error_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&wd, &running, &error_count]() {
      while (running.load(std::memory_order_acquire)) {
        auto id = wd.Register("StressThread", 1000);
        if (id.has_value()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          auto unreg = wd.Unregister(id.value().id);
          if (!unreg.has_value()) {
            error_count.fetch_add(1, std::memory_order_relaxed);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  running.store(false, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(error_count.load(std::memory_order_relaxed) == 0);
}

// ============================================================================
// TimerScheduler Integration
// ============================================================================

TEST_CASE("ThreadWatchdog: CheckTick static callback", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 50);
  REQUIRE(id.has_value());

  // Don't feed, let it timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Call CheckTick as TimerScheduler would
  osp::ThreadWatchdog<8>::CheckTick(&wd);

  REQUIRE(wd.TimedOutCount() == 1);
}

// ============================================================================
// Template Parameterization
// ============================================================================

TEST_CASE("ThreadWatchdog<1>: single slot", "[watchdog]") {
  osp::ThreadWatchdog<1> wd;

  auto id1 = wd.Register("Thread1", 1000);
  REQUIRE(id1.has_value());
  REQUIRE(wd.ActiveCount() == 1);

  auto id2 = wd.Register("Thread2", 1000);
  REQUIRE(!id2.has_value());
  REQUIRE(id2.get_error() == osp::WatchdogError::kSlotsFull);
}

TEST_CASE("ThreadWatchdog<64>: large capacity", "[watchdog]") {
  osp::ThreadWatchdog<64> wd;

  for (uint32_t i = 0; i < 64; ++i) {
    auto id = wd.Register("Thread", 1000);
    REQUIRE(id.has_value());
  }

  REQUIRE(wd.ActiveCount() == 64);

  auto overflow = wd.Register("Overflow", 1000);
  REQUIRE(!overflow.has_value());
  REQUIRE(overflow.get_error() == osp::WatchdogError::kSlotsFull);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("ThreadWatchdog: slot reuse after Unregister", "[watchdog]") {
  osp::ThreadWatchdog<2> wd;

  auto id1 = wd.Register("Thread1", 1000);
  REQUIRE(id1.has_value());

  auto id2 = wd.Register("Thread2", 1000);
  REQUIRE(id2.has_value());

  REQUIRE(wd.ActiveCount() == 2);

  // Unregister first slot
  auto unreg = wd.Unregister(id1.value().id);
  REQUIRE(unreg.has_value());
  REQUIRE(wd.ActiveCount() == 1);

  // Should be able to register again
  auto id3 = wd.Register("Thread3", 1000);
  REQUIRE(id3.has_value());
  REQUIRE(wd.ActiveCount() == 2);
}

TEST_CASE("ThreadWatchdog: Check with no registered slots returns 0", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);
  REQUIRE(wd.TimedOutCount() == 0);
  REQUIRE(wd.ActiveCount() == 0);
}

TEST_CASE("ThreadWatchdog: Feed after Unregister is safe", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  auto id = wd.Register("TestThread", 1000);
  REQUIRE(id.has_value());

  auto unreg = wd.Unregister(id.value().id);
  REQUIRE(unreg.has_value());

  // Feed with old ID should not crash
  wd.Feed(id.value().id);

  // Should still work normally
  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0);
}

// ============================================================================
// AutoCheck Tests
// ============================================================================

TEST_CASE("ThreadWatchdog: StartAutoCheck detects timeout without manual Check",
          "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::atomic<uint32_t> timeout_count{0};
  wd.SetOnTimeout([](uint32_t, const char*, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  }, &timeout_count);

  auto reg = wd.Register("test_thread", 50);  // 50ms timeout
  REQUIRE(reg.has_value());
  reg.value().heartbeat->Beat();

  // Start auto-check with 20ms interval
  wd.StartAutoCheck(20);

  // Let the thread time out (don't beat)
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  REQUIRE(timeout_count.load() >= 1U);

  wd.StopAutoCheck();
  (void)wd.Unregister(reg.value().id);
}

TEST_CASE("ThreadWatchdog: StartAutoCheck is idempotent", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  wd.StartAutoCheck(50);
  wd.StartAutoCheck(50);  // second call is no-op

  wd.StopAutoCheck();
  wd.StopAutoCheck();  // second call is no-op
}

TEST_CASE("ThreadWatchdog: StopAutoCheck without StartAutoCheck is safe",
          "[watchdog]") {
  osp::ThreadWatchdog<8> wd;
  wd.StopAutoCheck();  // no-op, should not crash
}

TEST_CASE("ThreadWatchdog: AutoCheck recovery detection", "[watchdog]") {
  osp::ThreadWatchdog<8> wd;

  std::atomic<uint32_t> timeout_count{0};
  std::atomic<uint32_t> recover_count{0};
  wd.SetOnTimeout([](uint32_t, const char*, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  }, &timeout_count);
  wd.SetOnRecovered([](uint32_t, const char*, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  }, &recover_count);

  auto reg = wd.Register("recoverable", 50);
  REQUIRE(reg.has_value());
  auto* hb = reg.value().heartbeat;
  hb->Beat();

  wd.StartAutoCheck(20);

  // Let it time out
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE(timeout_count.load() >= 1U);

  // Resume beating -> should recover
  hb->Beat();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(recover_count.load() >= 1U);

  wd.StopAutoCheck();
  (void)wd.Unregister(reg.value().id);
}

TEST_CASE("ThreadWatchdog: Destructor stops auto-check thread", "[watchdog]") {
  // Verify no leak / hang when destructor runs with active auto-check
  {
    osp::ThreadWatchdog<4> wd;
    wd.StartAutoCheck(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // ~ThreadWatchdog calls StopAutoCheck()
  }
  // If we get here, destructor didn't hang
  REQUIRE(true);
}
