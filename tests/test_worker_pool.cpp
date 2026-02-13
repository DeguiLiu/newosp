/**
 * @file test_worker_pool.cpp
 * @brief Catch2 tests for osp::WorkerPool.
 */

#include "osp/worker_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <variant>

// ============================================================================
// Test types
// ============================================================================

struct TaskA {
  int id;
};

struct TaskB {
  float value;
};

using TestPayload = std::variant<TaskA, TaskB>;
using TestBus = osp::AsyncBus<TestPayload>;

// ============================================================================
// Bus reset helper - called before each test section
// ============================================================================

static void ResetBus() { TestBus::Instance().Reset(); }

// ============================================================================
// Shared counters for handler verification
// ============================================================================

static std::atomic<int> g_task_a_count{0};
static std::atomic<int> g_task_a_last_id{0};
static std::atomic<int> g_task_b_count{0};
static std::atomic<float> g_task_b_last_value{0.0f};

static void HandleTaskA(const TaskA& task, const osp::MessageHeader& /*hdr*/) {
  g_task_a_count.fetch_add(1, std::memory_order_relaxed);
  g_task_a_last_id.store(task.id, std::memory_order_relaxed);
}

static void HandleTaskB(const TaskB& task, const osp::MessageHeader& /*hdr*/) {
  g_task_b_count.fetch_add(1, std::memory_order_relaxed);
  g_task_b_last_value.store(task.value, std::memory_order_relaxed);
}

static void ResetCounters() {
  g_task_a_count.store(0, std::memory_order_relaxed);
  g_task_a_last_id.store(0, std::memory_order_relaxed);
  g_task_b_count.store(0, std::memory_order_relaxed);
  g_task_b_last_value.store(0.0f, std::memory_order_relaxed);
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("WorkerPool construction and basic config", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  SECTION("default config creates a 1-worker pool") {
    osp::WorkerPoolConfig cfg;
    osp::WorkerPool<TestPayload> pool(cfg);

    REQUIRE(pool.WorkerCount() == 1U);
    REQUIRE_FALSE(pool.IsRunning());
    REQUIRE_FALSE(pool.IsPaused());
  }

  SECTION("custom worker count is respected") {
    osp::WorkerPoolConfig cfg;
    cfg.name = "test4";
    cfg.worker_num = 4U;
    osp::WorkerPool<TestPayload> pool(cfg);

    REQUIRE(pool.WorkerCount() == 4U);
  }

  SECTION("zero worker_num is clamped to 1") {
    osp::WorkerPoolConfig cfg;
    cfg.worker_num = 0U;
    osp::WorkerPool<TestPayload> pool(cfg);

    REQUIRE(pool.WorkerCount() == 1U);
  }
}

TEST_CASE("WorkerPool handler registration", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "reg";
  osp::WorkerPool<TestPayload> pool(cfg);

  SECTION("SubmitSync returns false for unregistered type") {
    REQUIRE_FALSE(pool.SubmitSync(TaskA{42}));
  }

  SECTION("SubmitSync returns true after registering handler") {
    pool.RegisterHandler<TaskA>(&HandleTaskA);
    REQUIRE(pool.SubmitSync(TaskA{42}));
    REQUIRE(g_task_a_count.load() == 1);
    REQUIRE(g_task_a_last_id.load() == 42);
  }

  SECTION("multiple handlers for different types") {
    pool.RegisterHandler<TaskA>(&HandleTaskA);
    pool.RegisterHandler<TaskB>(&HandleTaskB);

    REQUIRE(pool.SubmitSync(TaskA{10}));
    REQUIRE(pool.SubmitSync(TaskB{3.14f}));

    REQUIRE(g_task_a_count.load() == 1);
    REQUIRE(g_task_a_last_id.load() == 10);
    REQUIRE(g_task_b_count.load() == 1);
  }
}

TEST_CASE("WorkerPool start/shutdown lifecycle", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "life";
  cfg.worker_num = 2U;
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);

  SECTION("Start sets running flag") {
    pool.Start();
    REQUIRE(pool.IsRunning());
    pool.Shutdown();
    REQUIRE_FALSE(pool.IsRunning());
  }

  SECTION("double Start is a no-op") {
    pool.Start();
    pool.Start();  // should not crash or create extra threads
    REQUIRE(pool.IsRunning());
    pool.Shutdown();
  }

  SECTION("double Shutdown is safe") {
    pool.Start();
    pool.Shutdown();
    pool.Shutdown();  // should not crash
    REQUIRE_FALSE(pool.IsRunning());
  }

  SECTION("destructor calls Shutdown if running") {
    {
      osp::WorkerPoolConfig cfg2;
      cfg2.name = "dtor";
      osp::WorkerPool<TestPayload> pool2(cfg2);
      pool2.RegisterHandler<TaskA>(&HandleTaskA);
      pool2.Start();
      REQUIRE(pool2.IsRunning());
    }
    // pool2 destroyed here; should not hang or crash

    // Reset bus for subsequent tests
    ResetBus();
  }
}

TEST_CASE("WorkerPool submit and process messages", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "proc";
  cfg.worker_num = 1U;
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);
  pool.RegisterHandler<TaskB>(&HandleTaskB);
  pool.Start();

  SECTION("single message is processed") {
    REQUIRE(pool.Submit(TaskA{100}));

    // Wait for processing with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_task_a_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(g_task_a_count.load() == 1);
    REQUIRE(g_task_a_last_id.load() == 100);
  }

  SECTION("multiple messages are all processed") {
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i) {
      REQUIRE(pool.Submit(TaskA{i}));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_task_a_count.load() < kCount && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(g_task_a_count.load() == kCount);
  }

  SECTION("both types are dispatched correctly") {
    REQUIRE(pool.Submit(TaskA{7}));
    REQUIRE(pool.Submit(TaskB{2.5f}));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((g_task_a_count.load() < 1 || g_task_b_count.load() < 1) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(g_task_a_count.load() == 1);
    REQUIRE(g_task_b_count.load() == 1);
  }

  pool.Shutdown();
}

TEST_CASE("WorkerPool SubmitSync synchronous execution", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "sync";
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);

  // SubmitSync works without Start() - it bypasses the bus entirely
  SECTION("SubmitSync processes synchronously without Start") {
    REQUIRE(pool.SubmitSync(TaskA{999}));
    // Should be immediately available (no async processing)
    REQUIRE(g_task_a_count.load() == 1);
    REQUIRE(g_task_a_last_id.load() == 999);
  }

  SECTION("SubmitSync returns false for unregistered type") {
    REQUIRE_FALSE(pool.SubmitSync(TaskB{1.0f}));
    REQUIRE(g_task_b_count.load() == 0);
  }

  SECTION("multiple SubmitSync calls accumulate") {
    for (int i = 0; i < 10; ++i) {
      REQUIRE(pool.SubmitSync(TaskA{i}));
    }
    REQUIRE(g_task_a_count.load() == 10);
    REQUIRE(g_task_a_last_id.load() == 9);
  }
}

TEST_CASE("WorkerPool stats reporting", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "stat";
  cfg.worker_num = 1U;
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);
  pool.Start();

  constexpr int kCount = 20;
  for (int i = 0; i < kCount; ++i) {
    pool.Submit(TaskA{i});
  }

  // Wait for all to be processed
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (g_task_a_count.load() < kCount && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  auto stats = pool.GetStats();
  REQUIRE(stats.dispatched == static_cast<uint64_t>(kCount));
  REQUIRE(stats.processed == static_cast<uint64_t>(kCount));
  REQUIRE(stats.worker_queue_full == 0U);
  REQUIRE(stats.bus_stats.messages_published >= static_cast<uint64_t>(kCount));

  pool.Shutdown();
}

TEST_CASE("WorkerPool FlushAndPause/Resume", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "pause";
  cfg.worker_num = 2U;
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);
  pool.Start();

  SECTION("FlushAndPause drains all pending work") {
    constexpr int kCount = 30;
    for (int i = 0; i < kCount; ++i) {
      pool.Submit(TaskA{i});
    }

    pool.FlushAndPause();

    REQUIRE(pool.IsPaused());
    REQUIRE(g_task_a_count.load() == kCount);

    // Submit should return false while paused
    REQUIRE_FALSE(pool.Submit(TaskA{9999}));
  }

  SECTION("Resume allows new submissions") {
    pool.FlushAndPause();
    REQUIRE(pool.IsPaused());

    pool.Resume();
    REQUIRE_FALSE(pool.IsPaused());

    REQUIRE(pool.Submit(TaskA{42}));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_task_a_count.load() < 1 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(g_task_a_count.load() >= 1);
  }

  pool.Shutdown();
}

TEST_CASE("WorkerPool multiple workers", "[worker_pool]") {
  ResetBus();
  ResetCounters();

  osp::WorkerPoolConfig cfg;
  cfg.name = "multi";
  cfg.worker_num = 4U;
  osp::WorkerPool<TestPayload> pool(cfg);
  pool.RegisterHandler<TaskA>(&HandleTaskA);
  pool.Start();

  constexpr int kCount = 200;
  for (int i = 0; i < kCount; ++i) {
    pool.Submit(TaskA{i});
  }

  // Wait for all to be processed
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (g_task_a_count.load() < kCount && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  REQUIRE(g_task_a_count.load() == kCount);

  auto stats = pool.GetStats();
  REQUIRE(stats.dispatched == static_cast<uint64_t>(kCount));
  REQUIRE(stats.processed == static_cast<uint64_t>(kCount));

  pool.Shutdown();
}

TEST_CASE("SpscQueue basic operations", "[worker_pool][spsc]") {
  SECTION("push and pop") {
    osp::SpscQueue<int> q(4U);
    REQUIRE(q.Capacity() == 4U);  // 4 is already power of 2
    REQUIRE(q.Empty());
    REQUIRE(q.Size() == 0U);

    REQUIRE(q.TryPush(10));
    REQUIRE(q.TryPush(20));
    REQUIRE_FALSE(q.Empty());
    REQUIRE(q.Size() == 2U);

    int val = 0;
    REQUIRE(q.TryPop(val));
    REQUIRE(val == 10);
    REQUIRE(q.TryPop(val));
    REQUIRE(val == 20);
    REQUIRE(q.Empty());
    REQUIRE_FALSE(q.TryPop(val));
  }

  SECTION("queue full rejects push") {
    osp::SpscQueue<int> q(2U);
    REQUIRE(q.Capacity() == 2U);
    REQUIRE(q.TryPush(1));
    REQUIRE(q.TryPush(2));
    REQUIRE_FALSE(q.TryPush(3));  // full
  }

  SECTION("capacity rounds up to power of 2") {
    osp::SpscQueue<int> q(3U);
    REQUIRE(q.Capacity() == 4U);

    osp::SpscQueue<int> q2(5U);
    REQUIRE(q2.Capacity() == 8U);
  }
}
