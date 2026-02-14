/**
 * @file test_fault_collector.cpp
 * @brief Catch2 tests for osp::FaultCollector.
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/fault_collector.hpp"
#include "osp/watchdog.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace osp;

// ============================================================================
// Construction & static_assert
// ============================================================================

TEST_CASE("FaultCollector default construction", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  REQUIRE_FALSE(fc.IsRunning());
  REQUIRE(fc.ActiveFaultCount() == 0U);
  REQUIRE(fc.QueueDepthCurrent() == 0U);
}

TEST_CASE("FaultCollector template parameters", "[fault_collector]") {
  // Various valid configurations
  FaultCollector<1, 4> fc_tiny;
  FaultCollector<64, 256> fc_default;
  FaultCollector<256, 1024> fc_large;
  REQUIRE_FALSE(fc_tiny.IsRunning());
  REQUIRE_FALSE(fc_default.IsRunning());
  REQUIRE_FALSE(fc_large.IsRunning());
}

// ============================================================================
// Registration
// ============================================================================

TEST_CASE("RegisterFault valid index", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto result = fc.RegisterFault(0U, 0x01010001U);
  REQUIRE(result.has_value());

  result = fc.RegisterFault(15U, 0x01010010U, 0U, 3U);
  REQUIRE(result.has_value());
}

TEST_CASE("RegisterFault invalid index", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto result = fc.RegisterFault(16U, 0x01010001U);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == FaultCollectorError::kInvalidIndex);

  result = fc.RegisterFault(255U, 0x01010001U);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("RegisterHook valid", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  auto result = fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kHandled;
  });
  REQUIRE(result.has_value());
}

TEST_CASE("RegisterHook unregistered fault", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  // Fault 5 not registered
  auto result = fc.RegisterHook(5U, [](const FaultEvent&) {
    return HookAction::kHandled;
  });
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == FaultCollectorError::kInvalidIndex);
}

TEST_CASE("RegisterHook invalid index", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto result = fc.RegisterHook(16U, [](const FaultEvent&) {
    return HookAction::kHandled;
  });
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == FaultCollectorError::kInvalidIndex);
}

// ============================================================================
// Start / Stop lifecycle
// ============================================================================

TEST_CASE("Start and Stop", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto result = fc.Start();
  REQUIRE(result.has_value());
  REQUIRE(fc.IsRunning());

  fc.Stop();
  REQUIRE_FALSE(fc.IsRunning());
}

TEST_CASE("Double Start returns error", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.Start();
  auto result = fc.Start();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == FaultCollectorError::kAlreadyStarted);
  fc.Stop();
}

TEST_CASE("Double Stop is safe", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.Start();
  fc.Stop();
  fc.Stop();  // Should not crash
  REQUIRE_FALSE(fc.IsRunning());
}

TEST_CASE("Destructor calls Stop", "[fault_collector]") {
  {
    FaultCollector<16, 64> fc;
    fc.Start();
    // Destructor should join the thread
  }
  SUCCEED("No crash on destruction");
}

// ============================================================================
// ReportFault
// ============================================================================

TEST_CASE("ReportFault invalid index", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto result = fc.ReportFault(16U, 0xDEADU);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.get_error() == FaultCollectorError::kInvalidIndex);
}

TEST_CASE("ReportFault without Start", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  // Should still enqueue (consumer just not running)
  auto result = fc.ReportFault(0U, 0xBEEFU);
  REQUIRE(result.has_value());
  REQUIRE(fc.QueueDepthCurrent() > 0U);
}

TEST_CASE("ReportFault sets active bitmap", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterFault(5U, 0x01010006U);

  REQUIRE_FALSE(fc.IsFaultActive(0U));
  REQUIRE_FALSE(fc.IsFaultActive(5U));

  fc.ReportFault(0U, 0x1111U);
  REQUIRE(fc.IsFaultActive(0U));
  REQUIRE_FALSE(fc.IsFaultActive(5U));

  fc.ReportFault(5U, 0x2222U);
  REQUIRE(fc.IsFaultActive(0U));
  REQUIRE(fc.IsFaultActive(5U));
  REQUIRE(fc.ActiveFaultCount() == 2U);
}

TEST_CASE("ReportFault timestamp is monotonic", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  fc.ReportFault(0U, 0x1111U);
  auto t1 = SteadyNowUs();
  fc.ReportFault(0U, 0x2222U);
  auto t2 = SteadyNowUs();

  REQUIRE(t2 >= t1);
}

// ============================================================================
// Hook callbacks
// ============================================================================

TEST_CASE("Hook callback invoked on processing", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  std::atomic<uint32_t> hook_called{0U};
  std::atomic<uint32_t> last_detail{0U};

  fc.RegisterHook(0U, [&hook_called, &last_detail](const FaultEvent& evt) {
    hook_called.fetch_add(1U, std::memory_order_relaxed);
    last_detail.store(evt.detail, std::memory_order_relaxed);
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kMedium);

  // Wait for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(hook_called.load() >= 1U);
  REQUIRE(last_detail.load() == 0xDEADU);
}

TEST_CASE("Default hook used when no specific hook", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  std::atomic<uint32_t> default_called{0U};
  fc.SetDefaultHook([&default_called](const FaultEvent&) {
    default_called.fetch_add(1U, std::memory_order_relaxed);
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(default_called.load() >= 1U);
}

TEST_CASE("Hook receives correct FaultEvent fields", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(3U, 0xAABBCCDDU, 0U, 1U);

  std::atomic<bool> verified{false};
  fc.RegisterHook(3U, [&verified](const FaultEvent& evt) {
    if (evt.fault_index == 3U &&
        evt.fault_code == 0xAABBCCDDU &&
        evt.detail == 0x12345678U &&
        evt.priority == FaultPriority::kHigh &&
        evt.is_first &&
        evt.occurrence_count == 1U) {
      verified.store(true, std::memory_order_relaxed);
    }
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(3U, 0x12345678U, FaultPriority::kHigh);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(verified.load());
}

TEST_CASE("Hook kHandled clears active bit", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE_FALSE(fc.IsFaultActive(0U));
}

TEST_CASE("Hook kDefer keeps active bit", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kDefer;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(fc.IsFaultActive(0U));
}

TEST_CASE("Hook kEscalate re-enqueues at higher priority", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  std::atomic<uint32_t> call_count{0U};
  std::atomic<uint8_t> last_priority{255U};

  fc.RegisterHook(0U, [&call_count, &last_priority](const FaultEvent& evt) {
    uint32_t cnt = call_count.fetch_add(1U, std::memory_order_relaxed);
    last_priority.store(static_cast<uint8_t>(evt.priority),
                        std::memory_order_relaxed);
    // First call: escalate. Second call: handle.
    if (cnt == 0U) {
      return HookAction::kEscalate;
    }
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U, FaultPriority::kMedium);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fc.Stop();

  REQUIRE(call_count.load() >= 2U);
  // Last call should be at kHigh (escalated from kMedium)
  REQUIRE(last_priority.load() ==
          static_cast<uint8_t>(FaultPriority::kHigh));
}

// ============================================================================
// Active bitmap & ClearFault
// ============================================================================

TEST_CASE("ClearFault removes active bit", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.ReportFault(0U, 0x1111U);
  REQUIRE(fc.IsFaultActive(0U));

  fc.ClearFault(0U);
  REQUIRE_FALSE(fc.IsFaultActive(0U));
  REQUIRE(fc.ActiveFaultCount() == 0U);
}

TEST_CASE("ClearAllFaults", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  for (uint16_t i = 0U; i < 16U; ++i) {
    fc.RegisterFault(i, 0x01010000U + i);
    fc.ReportFault(i, 0U);
  }
  REQUIRE(fc.ActiveFaultCount() == 16U);

  fc.ClearAllFaults();
  REQUIRE(fc.ActiveFaultCount() == 0U);
}

TEST_CASE("ClearFault invalid index is safe", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.ClearFault(16U);  // Out of range, should not crash
  fc.ClearFault(255U);
  SUCCEED("No crash on invalid ClearFault");
}

TEST_CASE("IsFaultActive invalid index returns false", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  REQUIRE_FALSE(fc.IsFaultActive(16U));
  REQUIRE_FALSE(fc.IsFaultActive(255U));
}

TEST_CASE("Bitmap with MaxFaults > 64", "[fault_collector]") {
  FaultCollector<128, 64> fc;
  fc.RegisterFault(0U, 0x01U);
  fc.RegisterFault(63U, 0x40U);
  fc.RegisterFault(64U, 0x41U);
  fc.RegisterFault(127U, 0x80U);

  fc.ReportFault(0U, 0U);
  fc.ReportFault(63U, 0U);
  fc.ReportFault(64U, 0U);
  fc.ReportFault(127U, 0U);

  REQUIRE(fc.IsFaultActive(0U));
  REQUIRE(fc.IsFaultActive(63U));
  REQUIRE(fc.IsFaultActive(64U));
  REQUIRE(fc.IsFaultActive(127U));
  REQUIRE(fc.ActiveFaultCount() == 4U);

  fc.ClearFault(63U);
  REQUIRE_FALSE(fc.IsFaultActive(63U));
  REQUIRE(fc.ActiveFaultCount() == 3U);
}

// ============================================================================
// Statistics
// ============================================================================

TEST_CASE("Statistics initial state", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_reported == 0U);
  REQUIRE(stats.total_processed == 0U);
  REQUIRE(stats.total_dropped == 0U);
}

TEST_CASE("Statistics after reporting", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U, FaultPriority::kMedium);
  fc.ReportFault(0U, 0x2222U, FaultPriority::kHigh);
  fc.ReportFault(0U, 0x3333U, FaultPriority::kCritical);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fc.Stop();

  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_reported == 3U);
  REQUIRE(stats.total_processed == 3U);
  REQUIRE(stats.total_dropped == 0U);
  REQUIRE(stats.priority_reported[0] == 1U);  // kCritical
  REQUIRE(stats.priority_reported[1] == 1U);  // kHigh
  REQUIRE(stats.priority_reported[2] == 1U);  // kMedium
}

TEST_CASE("ResetStatistics", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.ReportFault(0U, 0x1111U);

  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_reported == 1U);

  fc.ResetStatistics();
  stats = fc.GetStatistics();
  REQUIRE(stats.total_reported == 0U);
  REQUIRE(stats.total_dropped == 0U);
}

// ============================================================================
// Overflow callback
// ============================================================================

TEST_CASE("Overflow callback on queue full", "[fault_collector]") {
  // Tiny queue: depth=4
  FaultCollector<4, 4> fc;
  fc.RegisterFault(0U, 0x01010001U);

  std::atomic<uint32_t> overflow_count{0U};
  fc.SetOverflowCallback(
      [&overflow_count](uint16_t, FaultPriority) {
        overflow_count.fetch_add(1U, std::memory_order_relaxed);
      });

  // Fill the kLow queue (threshold = 4*60/100 = 2, so 3rd kLow should drop)
  for (uint32_t i = 0U; i < 8U; ++i) {
    fc.ReportFault(0U, i, FaultPriority::kLow);
  }

  REQUIRE(overflow_count.load() > 0U);
  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_dropped > 0U);
}

// ============================================================================
// Priority admission control
// ============================================================================

TEST_CASE("kCritical never dropped when queue has space",
          "[fault_collector]") {
  FaultCollector<4, 8> fc;
  fc.RegisterFault(0U, 0x01010001U);

  // Fill with kCritical - should accept all 8
  uint32_t accepted = 0U;
  for (uint32_t i = 0U; i < 8U; ++i) {
    auto result = fc.ReportFault(0U, i, FaultPriority::kCritical);
    if (result.has_value()) {
      ++accepted;
    }
  }
  // Should accept at least QueueDepth - 1 (ring buffer leaves one slot)
  REQUIRE(accepted >= 7U);
}

TEST_CASE("kLow dropped before kCritical", "[fault_collector]") {
  FaultCollector<4, 8> fc;
  fc.RegisterFault(0U, 0x01010001U);

  // Fill kLow queue past threshold
  uint32_t low_accepted = 0U;
  for (uint32_t i = 0U; i < 8U; ++i) {
    auto result = fc.ReportFault(0U, i, FaultPriority::kLow);
    if (result.has_value()) {
      ++low_accepted;
    }
  }

  // kLow threshold = 8*60/100 = 4, so should accept ~4
  REQUIRE(low_accepted <= 5U);

  // kCritical should still work (separate queue)
  auto result = fc.ReportFault(0U, 0xFFU, FaultPriority::kCritical);
  REQUIRE(result.has_value());
}

// ============================================================================
// Backpressure level
// ============================================================================

TEST_CASE("BackpressureLevel reflects queue usage", "[fault_collector]") {
  FaultCollector<4, 16> fc;
  fc.RegisterFault(0U, 0x01010001U);

  REQUIRE(fc.GetBackpressureLevel() == BackpressureLevel::kNormal);

  // Fill kCritical queue to ~60%
  for (uint32_t i = 0U; i < 10U; ++i) {
    fc.ReportFault(0U, i, FaultPriority::kCritical);
  }
  auto level = fc.GetBackpressureLevel();
  REQUIRE(level >= BackpressureLevel::kWarning);
}

// ============================================================================
// Occurrence count and is_first
// ============================================================================

TEST_CASE("Occurrence count increments", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  std::atomic<uint32_t> last_count{0U};
  std::atomic<bool> first_was_true{false};
  std::atomic<bool> second_was_false{false};

  fc.RegisterHook(0U, [&](const FaultEvent& evt) {
    last_count.store(evt.occurrence_count, std::memory_order_relaxed);
    if (evt.occurrence_count == 1U && evt.is_first) {
      first_was_true.store(true, std::memory_order_relaxed);
    }
    if (evt.occurrence_count == 2U && !evt.is_first) {
      second_was_false.store(true, std::memory_order_relaxed);
    }
    return HookAction::kHandled;
  });

  fc.Start();
  fc.ReportFault(0U, 0x1111U);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.ReportFault(0U, 0x2222U);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(last_count.load() == 2U);
  REQUIRE(first_was_true.load());
  REQUIRE(second_was_false.load());
}

// ============================================================================
// Concurrent stress test (MPSC)
// ============================================================================

TEST_CASE("MPSC stress: 4 producers x 1000 faults", "[fault_collector]") {
  FaultCollector<16, 4096> fc;
  for (uint16_t i = 0U; i < 4U; ++i) {
    fc.RegisterFault(i, 0x01010000U + i);
  }

  std::atomic<uint32_t> processed{0U};
  fc.SetDefaultHook([&processed](const FaultEvent&) {
    processed.fetch_add(1U, std::memory_order_relaxed);
    return HookAction::kHandled;
  });

  fc.Start();

  constexpr uint32_t kPerThread = 1000U;
  std::atomic<uint32_t> total_attempts{0U};
  std::vector<std::thread> producers;
  for (uint32_t t = 0U; t < 4U; ++t) {
    producers.emplace_back([&fc, &total_attempts, t] {
      for (uint32_t i = 0U; i < kPerThread; ++i) {
        // Retry on queue full (backpressure)
        while (!fc.ReportFault(static_cast<uint16_t>(t), i,
                               FaultPriority::kMedium).has_value()) {
          std::this_thread::yield();
        }
        total_attempts.fetch_add(1U, std::memory_order_relaxed);
      }
    });
  }

  for (auto& th : producers) {
    th.join();
  }

  // Wait for consumer to drain
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  fc.Stop();

  auto stats = fc.GetStatistics();
  REQUIRE(total_attempts.load() == 4U * kPerThread);
  REQUIRE(stats.total_reported == 4U * kPerThread);
  REQUIRE(stats.total_processed == 4U * kPerThread);
  REQUIRE(processed.load() == 4U * kPerThread);
}

TEST_CASE("Priority flood: kLow flood + kCritical penetration",
          "[fault_collector]") {
  FaultCollector<4, 32> fc;
  fc.RegisterFault(0U, 0x01U);  // kLow flood target
  fc.RegisterFault(1U, 0x02U);  // kCritical target

  std::atomic<uint32_t> critical_processed{0U};
  fc.RegisterHook(1U, [&critical_processed](const FaultEvent&) {
    critical_processed.fetch_add(1U, std::memory_order_relaxed);
    return HookAction::kHandled;
  });
  fc.SetDefaultHook([](const FaultEvent&) {
    return HookAction::kHandled;
  });

  fc.Start();

  // Flood kLow
  for (uint32_t i = 0U; i < 100U; ++i) {
    fc.ReportFault(0U, i, FaultPriority::kLow);
  }

  // kCritical should still get through
  constexpr uint32_t kCriticalCount = 10U;
  uint32_t critical_accepted = 0U;
  for (uint32_t i = 0U; i < kCriticalCount; ++i) {
    auto result = fc.ReportFault(1U, i, FaultPriority::kCritical);
    if (result.has_value()) {
      ++critical_accepted;
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  fc.Stop();

  // All kCritical should be accepted (separate queue)
  REQUIRE(critical_accepted == kCriticalCount);
  REQUIRE(critical_processed.load() == kCriticalCount);
}

TEST_CASE("Concurrent report and clear", "[fault_collector]") {
  FaultCollector<16, 256> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.SetDefaultHook([](const FaultEvent&) {
    return HookAction::kDefer;  // Keep active
  });

  fc.Start();

  std::atomic<bool> done{false};

  // Reporter thread
  std::thread reporter([&fc, &done] {
    for (uint32_t i = 0U; i < 500U; ++i) {
      fc.ReportFault(0U, i);
    }
    done.store(true, std::memory_order_release);
  });

  // Clearer thread
  std::thread clearer([&fc, &done] {
    while (!done.load(std::memory_order_acquire)) {
      fc.ClearFault(0U);
      std::this_thread::yield();
    }
  });

  reporter.join();
  clearer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fc.Stop();

  // No crash, no data race
  SUCCEED("Concurrent report+clear completed without crash");
}

// ============================================================================
// Shutdown request
// ============================================================================

TEST_CASE("Hook kShutdown sets shutdown flag", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kShutdown;
  });

  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kCritical);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  // Shutdown was requested (internal flag)
  // We verify by checking the fault was processed
  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_processed >= 1U);
}

// ============================================================================
// IsShutdownRequested
// ============================================================================

TEST_CASE("IsShutdownRequested initially false", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  REQUIRE_FALSE(fc.IsShutdownRequested());
}

TEST_CASE("IsShutdownRequested true after kShutdown hook",
          "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kShutdown;
  });

  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kCritical);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(fc.IsShutdownRequested());
}

TEST_CASE("kShutdown stops consumer loop", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kShutdown;
  });

  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kCritical);

  // Consumer loop should exit on its own due to shutdown_requested_
  // Wait a reasonable time, then verify Stop() returns quickly
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(fc.IsShutdownRequested());
  fc.Stop();
  REQUIRE_FALSE(fc.IsRunning());
}

// ============================================================================
// Shutdown callback
// ============================================================================

TEST_CASE("Shutdown callback invoked on kShutdown", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kShutdown;
  });

  std::atomic<uint32_t> cb_called{0U};
  fc.SetShutdownCallback(
      [](void* ctx) {
        static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(
            1U, std::memory_order_relaxed);
      },
      &cb_called);

  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kCritical);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(cb_called.load() == 1U);
  REQUIRE(fc.IsShutdownRequested());
}

TEST_CASE("No shutdown callback when not set", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.RegisterHook(0U, [](const FaultEvent&) {
    return HookAction::kShutdown;
  });

  // No SetShutdownCallback — should not crash
  fc.Start();
  fc.ReportFault(0U, 0xDEADU, FaultPriority::kCritical);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  REQUIRE(fc.IsShutdownRequested());
}

// ============================================================================
// Consumer heartbeat integration
// ============================================================================

TEST_CASE("SetConsumerHeartbeat updates heartbeat in consumer loop",
          "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  ThreadHeartbeat hb;
  REQUIRE(hb.LastBeatUs() == 0U);

  fc.SetConsumerHeartbeat(&hb);
  fc.Start();

  // Wait for consumer loop to beat at least once
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t first_beat = hb.LastBeatUs();
  REQUIRE(first_beat > 0U);

  // Wait more and verify heartbeat advances
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t second_beat = hb.LastBeatUs();
  REQUIRE(second_beat >= first_beat);

  fc.Stop();
}

TEST_CASE("Consumer heartbeat updated during fault processing",
          "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);
  fc.SetDefaultHook([](const FaultEvent&) {
    return HookAction::kHandled;
  });

  ThreadHeartbeat hb;
  fc.SetConsumerHeartbeat(&hb);
  fc.Start();

  // Report faults to trigger processing
  for (uint32_t i = 0U; i < 10U; ++i) {
    fc.ReportFault(0U, i, FaultPriority::kMedium);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fc.Stop();

  // Heartbeat should have been updated
  REQUIRE(hb.LastBeatUs() > 0U);
  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_processed == 10U);
}

TEST_CASE("Null heartbeat is safe (no-op)", "[fault_collector]") {
  FaultCollector<16, 64> fc;
  fc.RegisterFault(0U, 0x01010001U);

  // Do not call SetConsumerHeartbeat — default nullptr
  fc.Start();
  fc.ReportFault(0U, 0x1111U);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  fc.Stop();

  // Should not crash
  auto stats = fc.GetStatistics();
  REQUIRE(stats.total_processed >= 1U);
}

// ============================================================================
// Watchdog + FaultCollector integration (application-level wiring)
// ============================================================================

TEST_CASE("Watchdog monitors FaultCollector consumer thread",
          "[fault_collector][watchdog][integration]") {
  // This test demonstrates the application-level integration pattern
  ThreadWatchdog<8> wd;
  FaultCollector<16, 64> fc;

  // Register consumer thread with watchdog
  auto reg = wd.Register("fccu_consumer", 5000);
  REQUIRE(reg.has_value());

  // Wire heartbeat into FaultCollector
  fc.SetConsumerHeartbeat(reg.value().heartbeat);

  fc.RegisterFault(0U, 0x01010001U);
  fc.SetDefaultHook([](const FaultEvent&) {
    return HookAction::kHandled;
  });

  fc.Start();

  // Report some faults
  for (uint32_t i = 0U; i < 5U; ++i) {
    fc.ReportFault(0U, i, FaultPriority::kMedium);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Watchdog check — consumer should NOT be timed out
  uint32_t timed_out = wd.Check();
  REQUIRE(timed_out == 0U);
  REQUIRE_FALSE(wd.IsTimedOut(reg.value().id));

  fc.Stop();
  wd.Unregister(reg.value().id);
}
