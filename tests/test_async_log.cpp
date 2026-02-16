/**
 * @file test_async_log.cpp
 * @brief Unit tests for the async logging backend (async_log.hpp).
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/async_log.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

// ============================================================================
// Test sink: captures log entries for verification
// ============================================================================

struct TestSinkState {
  std::atomic<uint32_t> count{0};
  osp::log::LogEntry entries[1024];
  std::atomic<bool> ready{false};

  void Reset() {
    count.store(0, std::memory_order_relaxed);
    ready.store(false, std::memory_order_relaxed);
  }
};

static TestSinkState g_test_sink;

static void TestSink(const osp::log::LogEntry* entries, uint32_t n,
                     void* /*ctx*/) {
  uint32_t base = g_test_sink.count.fetch_add(n, std::memory_order_relaxed);
  for (uint32_t i = 0; i < n && (base + i) < 1024; ++i) {
    g_test_sink.entries[base + i] = entries[i];
  }
}

/// @brief Wait until at least `target` entries arrive, with timeout.
static bool WaitForEntries(uint32_t target, int timeout_ms = 500) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_test_sink.count.load(std::memory_order_relaxed) >= target) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_test_sink.count.load(std::memory_order_relaxed) >= target;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("LogEntry is trivially copyable", "[async_log]") {
  STATIC_REQUIRE(std::is_trivially_copyable<osp::log::LogEntry>::value);
}

TEST_CASE("LogEntry size is 320 bytes", "[async_log]") {
  STATIC_REQUIRE(sizeof(osp::log::LogEntry) == 320);
}

TEST_CASE("AsyncLog: basic async write and drain", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);
  REQUIRE(osp::log::IsAsyncEnabled());

  // Log several messages (these are DEBUG/INFO/WARN -- async path).
  OSP_LOG_INFO("Test", "hello %d", 42);
  OSP_LOG_DEBUG("Test", "debug msg");
  OSP_LOG_WARN("Test", "warn msg");

  REQUIRE(WaitForEntries(3));

  osp::log::StopAsync();
  REQUIRE_FALSE(osp::log::IsAsyncEnabled());

  auto stats = osp::log::GetAsyncStats();
  REQUIRE(stats.entries_written >= 3);
  REQUIRE(stats.entries_dropped == 0);

  // Verify content of first entry.
  CHECK(g_test_sink.entries[0].level == osp::log::Level::kInfo);
  CHECK(std::strstr(g_test_sink.entries[0].message, "hello 42") != nullptr);
  CHECK(std::strstr(g_test_sink.entries[0].category, "Test") != nullptr);
}

TEST_CASE("AsyncLog: ERROR bypasses async path", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  // ERROR goes directly to sync LogWrite (not through test sink).
  OSP_LOG_ERROR("Test", "error msg");

  // Give async writer time to process (ERROR should NOT appear in test sink).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  osp::log::StopAsync();

  // ERROR should not appear in the async sink.
  // It goes directly to stderr via sync LogWrite.
  auto stats = osp::log::GetAsyncStats();
  CHECK(stats.entries_written == 0);
}

TEST_CASE("AsyncLog: fallback to sync when not started", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  // Do NOT call StartAsync.
  REQUIRE_FALSE(osp::log::IsAsyncEnabled());

  // This should fall back to sync LogWrite (stderr).
  OSP_LOG_INFO("Test", "fallback msg");

  // No entries in test sink (it was never set as the sink).
  CHECK(g_test_sink.count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("AsyncLog: drop policy on full queue", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  // Use a sink that blocks, preventing the writer from draining the queue.
  static std::atomic<bool> sink_blocked{true};
  auto slow_sink = [](const osp::log::LogEntry* /*entries*/, uint32_t /*n*/,
                      void* /*ctx*/) {
    while (sink_blocked.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  sink_blocked.store(true, std::memory_order_relaxed);

  osp::log::AsyncLogConfig cfg;
  cfg.sink = slow_sink;
  osp::log::StartAsync(cfg);

  // Push enough entries to guarantee overflow. The writer can drain at most
  // one batch (32) before blocking, so push well beyond queue_depth + batch.
  static constexpr uint32_t kPushCount = OSP_ASYNC_LOG_QUEUE_DEPTH * 2;
  for (uint32_t i = 0; i < kPushCount; ++i) {
    OSP_LOG_INFO("Fill", "msg %u", i);
  }

  // Give time for all push attempts to complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto stats = osp::log::GetAsyncStats();
  CHECK(stats.entries_dropped > 0);

  // Unblock the sink and stop.
  sink_blocked.store(false, std::memory_order_relaxed);
  osp::log::StopAsync();
}

TEST_CASE("AsyncLog: multi-thread concurrent logging", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  static constexpr uint32_t kThreads = 4;
  static constexpr uint32_t kMsgsPerThread = 50;
  std::vector<std::thread> threads;

  for (uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      for (uint32_t i = 0; i < kMsgsPerThread; ++i) {
        OSP_LOG_INFO("MT", "thread=%u msg=%u", t, i);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  REQUIRE(WaitForEntries(kThreads * kMsgsPerThread, 2000));

  osp::log::StopAsync();

  auto stats = osp::log::GetAsyncStats();
  REQUIRE(stats.entries_written >= kThreads * kMsgsPerThread);
  REQUIRE(stats.entries_dropped == 0);
}

TEST_CASE("AsyncLog: thread registration and deregistration", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  // Spawn a thread that logs and exits.
  {
    std::thread t([]() {
      OSP_LOG_INFO("Temp", "from temporary thread");
    });
    t.join();
  }

  REQUIRE(WaitForEntries(1));

  // The thread's buffer slot should be released after thread exit.
  // Spawn another thread to verify slot reuse.
  {
    std::thread t([]() {
      OSP_LOG_INFO("Reuse", "from reused slot");
    });
    t.join();
  }

  REQUIRE(WaitForEntries(2));

  osp::log::StopAsync();

  auto stats = osp::log::GetAsyncStats();
  REQUIRE(stats.entries_written >= 2);
}

TEST_CASE("AsyncLog: custom sink receives correct data", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  OSP_LOG_INFO("MyModule", "value=%d str=%s", 99, "hello");

  REQUIRE(WaitForEntries(1));

  osp::log::StopAsync();

  const auto& e = g_test_sink.entries[0];
  CHECK(e.level == osp::log::Level::kInfo);
  CHECK(std::strcmp(e.category, "MyModule") == 0);
  CHECK(std::strstr(e.message, "value=99") != nullptr);
  CHECK(std::strstr(e.message, "str=hello") != nullptr);
  CHECK(e.line > 0);
  CHECK(e.thread_id > 0);
  CHECK(e.wallclock_sec > 0);
  CHECK(e.timestamp_ns > 0);
}

TEST_CASE("AsyncLog: graceful shutdown drains all entries", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  // Burst of entries.
  for (uint32_t i = 0; i < 100; ++i) {
    OSP_LOG_INFO("Drain", "msg %u", i);
  }

  // Immediately stop -- should drain all buffered entries.
  osp::log::StopAsync();

  auto stats = osp::log::GetAsyncStats();
  REQUIRE(stats.entries_written == 100);
  REQUIRE(stats.entries_dropped == 0);
}

TEST_CASE("AsyncLog: stats counters are accurate", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  auto stats0 = osp::log::GetAsyncStats();
  CHECK(stats0.entries_written == 0);
  CHECK(stats0.entries_dropped == 0);
  CHECK(stats0.sync_fallbacks == 0);

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;
  osp::log::StartAsync(cfg);

  for (uint32_t i = 0; i < 10; ++i) {
    OSP_LOG_INFO("Stats", "msg %u", i);
  }

  REQUIRE(WaitForEntries(10));

  osp::log::StopAsync();

  auto stats1 = osp::log::GetAsyncStats();
  CHECK(stats1.entries_written == 10);
  CHECK(stats1.entries_dropped == 0);

  // Reset and verify.
  osp::log::ResetAsyncStats();
  auto stats2 = osp::log::GetAsyncStats();
  CHECK(stats2.entries_written == 0);
}

TEST_CASE("AsyncLog: StartAsync is idempotent", "[async_log]") {
  g_test_sink.Reset();
  osp::log::ResetAsyncStats();

  osp::log::AsyncLogConfig cfg;
  cfg.sink = TestSink;

  osp::log::StartAsync(cfg);
  REQUIRE(osp::log::IsAsyncEnabled());

  // Second call should be a no-op.
  osp::log::StartAsync(cfg);
  REQUIRE(osp::log::IsAsyncEnabled());

  osp::log::StopAsync();
  REQUIRE_FALSE(osp::log::IsAsyncEnabled());

  // Stopping again should be safe.
  osp::log::StopAsync();
  REQUIRE_FALSE(osp::log::IsAsyncEnabled());
}
