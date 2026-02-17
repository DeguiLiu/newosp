/**
 * @file test_job_pool.cpp
 * @brief Catch2 tests for osp/job_pool.hpp (JobPool + Pipeline + DataDispatcher).
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/job_pool.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace osp;

// ============================================================================
// JobPool -- basic allocation
// ============================================================================

TEST_CASE("JobPool: initial state", "[job_pool]") {
  JobPool<64, 4> pool;
  REQUIRE(pool.FreeCount() == 4U);
  REQUIRE(pool.AllocCount() == 0U);
  REQUIRE(pool.Capacity() == 4U);
  REQUIRE(pool.PayloadCapacity() == 64U);
}

TEST_CASE("JobPool: alloc and release single block", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r0 = pool.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(pool.FreeCount() == 3U);
  REQUIRE(pool.AllocCount() == 1U);
  REQUIRE(pool.GetBlockState(r0.value()) == BlockState::kAllocated);

  pool.Submit(r0.value(), 32U, 1U);
  REQUIRE(pool.GetBlockState(r0.value()) == BlockState::kReady);
  REQUIRE(pool.GetPayloadSize(r0.value()) == 32U);

  bool last = pool.Release(r0.value());
  REQUIRE(last);
  REQUIRE(pool.FreeCount() == 4U);
  REQUIRE(pool.AllocCount() == 0U);
  REQUIRE(pool.GetBlockState(r0.value()) == BlockState::kFree);
}

TEST_CASE("JobPool: exhaust pool returns kPoolExhausted", "[job_pool]") {
  JobPool<32, 2> pool;

  auto r0 = pool.Alloc();
  auto r1 = pool.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(r1.has_value());
  REQUIRE(pool.FreeCount() == 0U);

  auto r2 = pool.Alloc();
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.get_error() == JobPoolError::kPoolExhausted);

  // Release one, then alloc again
  pool.Submit(r0.value(), 0U, 1U);
  pool.Release(r0.value());
  REQUIRE(pool.FreeCount() == 1U);

  auto r3 = pool.Alloc();
  REQUIRE(r3.has_value());

  pool.Submit(r1.value(), 0U, 1U);
  pool.Release(r1.value());
  pool.Submit(r3.value(), 0U, 1U);
  pool.Release(r3.value());
}

TEST_CASE("JobPool: alloc all blocks then release all", "[job_pool]") {
  constexpr uint32_t kN = 8U;
  JobPool<16, kN> pool;

  uint32_t ids[kN];
  for (uint32_t i = 0U; i < kN; ++i) {
    auto r = pool.Alloc();
    REQUIRE(r.has_value());
    ids[i] = r.value();
  }
  REQUIRE(pool.FreeCount() == 0U);
  REQUIRE(pool.AllocCount() == kN);

  for (uint32_t i = 0U; i < kN; ++i) {
    pool.Submit(ids[i], 0U, 1U);
    pool.Release(ids[i]);
  }
  REQUIRE(pool.FreeCount() == kN);
  REQUIRE(pool.AllocCount() == 0U);
}

// ============================================================================
// JobPool -- payload read/write
// ============================================================================

TEST_CASE("JobPool: payload read and write", "[job_pool]") {
  JobPool<128, 2> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  uint8_t* payload = pool.GetPayload(bid);
  REQUIRE(payload != nullptr);
  std::memset(payload, 0xAB, 128);

  pool.Submit(bid, 128U, 1U);

  const uint8_t* ro = pool.GetPayloadReadOnly(bid);
  REQUIRE(ro != nullptr);
  REQUIRE(ro[0] == 0xAB);
  REQUIRE(ro[127] == 0xAB);

  pool.Release(bid);
}

TEST_CASE("JobPool: payload pointer alignment", "[job_pool]") {
  JobPool<256, 2> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  uint8_t* payload = pool.GetPayload(r.value());
  auto addr = reinterpret_cast<uintptr_t>(payload);  // NOLINT
  REQUIRE((addr % 8U) == 0U);

  pool.Submit(r.value(), 0U, 1U);
  pool.Release(r.value());
}

// ============================================================================
// JobPool -- reference counting
// ============================================================================

TEST_CASE("JobPool: refcount fan-out (3 consumers)", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  pool.Submit(bid, 16U, 3U);

  REQUIRE_FALSE(pool.Release(bid));  // 3->2
  REQUIRE(pool.AllocCount() == 1U);

  REQUIRE_FALSE(pool.Release(bid));  // 2->1
  REQUIRE(pool.AllocCount() == 1U);

  REQUIRE(pool.Release(bid));        // 1->0, recycled
  REQUIRE(pool.AllocCount() == 0U);
  REQUIRE(pool.FreeCount() == 4U);
}

TEST_CASE("JobPool: AddRef for dynamic fan-out", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  pool.Submit(bid, 8U, 1U);
  pool.AddRef(bid, 2U);

  REQUIRE_FALSE(pool.Release(bid));
  REQUIRE_FALSE(pool.Release(bid));
  REQUIRE(pool.Release(bid));
  REQUIRE(pool.FreeCount() == 4U);
}

// ============================================================================
// JobPool -- timeout detection
// ============================================================================

TEST_CASE("JobPool: timeout detection fires", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  pool.Submit(bid, 8U, 1U, 1U);  // 1ms timeout

  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  uint32_t timeout_bid = UINT32_MAX;
  uint32_t count = pool.ScanTimeout(
      [](uint32_t block_id, void* ctx) {
        *static_cast<uint32_t*>(ctx) = block_id;
      },
      &timeout_bid);

  REQUIRE(count == 1U);
  REQUIRE(timeout_bid == bid);
  REQUIRE(pool.FreeCount() == 4U);
}

TEST_CASE("JobPool: no timeout when deadline is 0", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 8U, 1U, 0U);

  uint32_t count = pool.ScanTimeout(nullptr, nullptr);
  REQUIRE(count == 0U);

  pool.Release(r.value());
}

TEST_CASE("JobPool: no timeout before deadline", "[job_pool]") {
  JobPool<64, 4> pool;

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 8U, 1U, 5000U);  // 5s timeout

  uint32_t count = pool.ScanTimeout(nullptr, nullptr);
  REQUIRE(count == 0U);

  pool.Release(r.value());
}

// ============================================================================
// JobPool -- concurrent alloc/release
// ============================================================================

TEST_CASE("JobPool: concurrent alloc and release", "[job_pool]") {
  constexpr uint32_t kPoolSize = 32U;
  JobPool<16, kPoolSize> pool;

  std::atomic<uint32_t> success_count{0U};
  constexpr uint32_t kThreads = 4U;
  constexpr uint32_t kOpsPerThread = 100U;

  auto worker = [&]() {
    for (uint32_t i = 0U; i < kOpsPerThread; ++i) {
      auto r = pool.Alloc();
      if (r.has_value()) {
        success_count.fetch_add(1U, std::memory_order_relaxed);
        pool.Submit(r.value(), 0U, 1U);
        pool.Release(r.value());
      }
    }
  };

  std::vector<std::thread> threads;
  for (uint32_t t = 0U; t < kThreads; ++t) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(pool.FreeCount() == kPoolSize);
  REQUIRE(pool.AllocCount() == 0U);
  REQUIRE(success_count.load() > 0U);
}

TEST_CASE("JobPool: ScanTimeout vs Release concurrent safety", "[job_pool]") {
  constexpr uint32_t kPoolSize = 16U;
  JobPool<64, kPoolSize> pool;
  constexpr uint32_t kIterations = 200U;

  std::atomic<uint32_t> release_count{0U};
  std::atomic<uint32_t> timeout_count{0U};

  for (uint32_t iter = 0U; iter < kIterations; ++iter) {
    auto r = pool.Alloc();
    REQUIRE(r.has_value());
    uint32_t bid = r.value();
    pool.Submit(bid, 8U, 1U, 1U);  // 1ms timeout

    // Wait for timeout to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(3));

    // Race: one thread calls Release, another calls ScanTimeout
    std::atomic<bool> go{false};
    bool release_last = false;
    uint32_t scan_found = 0U;

    std::thread t_release([&]() {
      while (!go.load(std::memory_order_acquire)) {}
      release_last = pool.Release(bid);
    });

    std::thread t_scan([&]() {
      while (!go.load(std::memory_order_acquire)) {}
      scan_found = pool.ScanTimeout(nullptr, nullptr);
    });

    go.store(true, std::memory_order_release);
    t_release.join();
    t_scan.join();

    if (release_last) release_count.fetch_add(1U, std::memory_order_relaxed);
    if (scan_found > 0U) timeout_count.fetch_add(1U, std::memory_order_relaxed);

    // Block must be recycled exactly once (no double-free)
    // After both threads complete, the block must be free
    REQUIRE(pool.GetBlockState(bid) == BlockState::kFree);
  }

  // All blocks must be returned to the pool
  REQUIRE(pool.FreeCount() == kPoolSize);
  REQUIRE(pool.AllocCount() == 0U);
}

// ============================================================================
// Pipeline -- basic topology
// ============================================================================

TEST_CASE("Pipeline: add stages and edges", "[job_pool][pipeline]") {
  Pipeline<4, 4> pipe;
  REQUIRE(pipe.StageCount() == 0U);

  auto s0 = pipe.AddStage({"stage0", nullptr, nullptr, 0U});
  auto s1 = pipe.AddStage({"stage1", nullptr, nullptr, 0U});
  REQUIRE(s0 == 0);
  REQUIRE(s1 == 1);
  REQUIRE(pipe.StageCount() == 2U);

  REQUIRE(pipe.AddEdge(s0, s1));
  REQUIRE(pipe.SuccessorCount(s0) == 1U);
  REQUIRE(pipe.SuccessorCount(s1) == 0U);

  pipe.SetEntryStage(s0);
  REQUIRE(pipe.EntryStage() == s0);
}

TEST_CASE("Pipeline: stage limit", "[job_pool][pipeline]") {
  Pipeline<2, 2> pipe;
  REQUIRE(pipe.AddStage({"s0", nullptr, nullptr, 0U}) == 0);
  REQUIRE(pipe.AddStage({"s1", nullptr, nullptr, 0U}) == 1);
  REQUIRE(pipe.AddStage({"s2", nullptr, nullptr, 0U}) == -1);
}

TEST_CASE("Pipeline: invalid edge indices", "[job_pool][pipeline]") {
  Pipeline<4, 4> pipe;
  pipe.AddStage({"s0", nullptr, nullptr, 0U});

  REQUIRE_FALSE(pipe.AddEdge(0, 1));
  REQUIRE_FALSE(pipe.AddEdge(-1, 0));
  REQUIRE_FALSE(pipe.AddEdge(0, -1));
}

TEST_CASE("Pipeline: stage name query", "[job_pool][pipeline]") {
  Pipeline<4, 4> pipe;
  pipe.AddStage({"logging", nullptr, nullptr, 0U});
  pipe.AddStage({"fusion", nullptr, nullptr, 0U});

  REQUIRE(std::strcmp(pipe.StageName(0), "logging") == 0);
  REQUIRE(std::strcmp(pipe.StageName(1), "fusion") == 0);
  REQUIRE(std::strcmp(pipe.StageName(99), "unknown") == 0);
}

// ============================================================================
// Pipeline -- execution
// ============================================================================

TEST_CASE("Pipeline: single stage execution", "[job_pool][pipeline]") {
  std::atomic<uint32_t> call_count{0U};

  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  JobPool<64, 4> pool;
  Pipeline<4, 4> pipe;

  auto s0 = pipe.AddStage({"s0", handler, &call_count, 0U});
  pipe.SetEntryStage(s0);

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 0U, pipe.EntryFanOut());

  REQUIRE(pipe.Execute(pool, r.value()));
  REQUIRE(call_count.load() == 1U);
  REQUIRE(pool.FreeCount() == 4U);
}

TEST_CASE("Pipeline: serial chain A -> B -> C", "[job_pool][pipeline]") {
  struct Ctx {
    uint32_t* order_slot;
    std::atomic<uint32_t>* seq;
  };

  uint32_t order[3] = {0U, 0U, 0U};
  std::atomic<uint32_t> seq{1U};

  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    auto* c = static_cast<Ctx*>(ctx);
    *c->order_slot = c->seq->fetch_add(1U);
  };

  Ctx ctx_a{&order[0], &seq};
  Ctx ctx_b{&order[1], &seq};
  Ctx ctx_c{&order[2], &seq};

  JobPool<64, 4> pool;
  Pipeline<4, 4> pipe;

  auto sa = pipe.AddStage({"A", handler, &ctx_a, 0U});
  auto sb = pipe.AddStage({"B", handler, &ctx_b, 0U});
  auto sc = pipe.AddStage({"C", handler, &ctx_c, 0U});
  pipe.AddEdge(sa, sb);
  pipe.AddEdge(sb, sc);
  pipe.SetEntryStage(sa);

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 0U, pipe.EntryFanOut());
  pipe.Execute(pool, r.value());

  REQUIRE(order[0] < order[1]);
  REQUIRE(order[1] < order[2]);
  REQUIRE(pool.FreeCount() == 4U);
}

TEST_CASE("Pipeline: fan-out A -> B, A -> C", "[job_pool][pipeline]") {
  std::atomic<uint32_t> b_count{0U};
  std::atomic<uint32_t> c_count{0U};

  auto handler_b = [](const uint8_t* /*data*/, uint32_t /*len*/,
                      uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };
  auto handler_c = [](const uint8_t* /*data*/, uint32_t /*len*/,
                      uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };
  auto handler_a = [](const uint8_t* /*data*/, uint32_t /*len*/,
                      uint32_t /*block_id*/, void* /*ctx*/) {};

  JobPool<64, 4> pool;
  Pipeline<4, 4> pipe;

  auto sa = pipe.AddStage({"A", handler_a, nullptr, 0U});
  auto sb = pipe.AddStage({"B", handler_b, &b_count, 0U});
  auto sc = pipe.AddStage({"C", handler_c, &c_count, 0U});
  pipe.AddEdge(sa, sb);
  pipe.AddEdge(sa, sc);
  pipe.SetEntryStage(sa);

  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 0U, pipe.EntryFanOut());
  pipe.Execute(pool, r.value());

  REQUIRE(b_count.load() == 1U);
  REQUIRE(c_count.load() == 1U);
  REQUIRE(pool.FreeCount() == 4U);
}

// ============================================================================
// DataDispatcher -- integration
// ============================================================================

TEST_CASE("DataDispatcher: basic alloc-submit-release", "[job_pool][dispatcher]") {
  DataDispatcher<1024, 8> disp;
  DataDispatcher<1024, 8>::Config cfg;
  cfg.name = "test";
  disp.Init(cfg);

  std::atomic<uint32_t> processed{0U};
  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  auto s0 = disp.AddStage({"proc", handler, &processed, 0U});
  disp.SetEntryStage(s0);

  auto bid = disp.AllocBlock();
  REQUIRE(bid.has_value());

  uint8_t* payload = disp.GetBlockPayload(bid.value());
  std::memset(payload, 0x42, 64);

  disp.SubmitBlock(bid.value(), 64U);

  REQUIRE(processed.load() == 1U);
  REQUIRE(disp.FreeBlocks() == 8U);
  REQUIRE(disp.AllocBlocks() == 0U);
}

TEST_CASE("DataDispatcher: backpressure callback", "[job_pool][dispatcher]") {
  DataDispatcher<32, 4> disp;
  DataDispatcher<32, 4>::Config cfg;
  cfg.name = "bp_test";
  cfg.backpressure_threshold = 3U;
  disp.Init(cfg);

  auto s0 = disp.AddStage({"noop", nullptr, nullptr, 0U});
  disp.SetEntryStage(s0);

  std::atomic<uint32_t> bp_triggered{0U};
  disp.SetBackpressureCallback(
      [](uint32_t /*free_count*/, void* ctx) {
        static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
      },
      &bp_triggered);

  // free=4, threshold=3: 4 <= 3 is false, no trigger
  auto r0 = disp.AllocBlock();
  REQUIRE(r0.has_value());
  REQUIRE(bp_triggered.load() == 0U);

  // free=3, threshold=3: 3 <= 3 is true, trigger
  auto r1 = disp.AllocBlock();
  REQUIRE(r1.has_value());
  REQUIRE(bp_triggered.load() == 1U);

  disp.SubmitBlock(r0.value(), 0U);
  disp.SubmitBlock(r1.value(), 0U);
}

TEST_CASE("DataDispatcher: timeout scan with fault reporter", "[job_pool][dispatcher]") {
  DataDispatcher<64, 4> disp;
  DataDispatcher<64, 4>::Config cfg;
  cfg.name = "timeout_test";
  disp.Init(cfg);

  std::atomic<uint32_t> fault_count{0U};
  FaultReporter reporter;
  reporter.fn = [](uint16_t /*idx*/, uint32_t /*detail*/,
                   FaultPriority /*pri*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };
  reporter.ctx = &fault_count;
  disp.SetFaultReporter(reporter, 0U);

  // Use pool directly to avoid pipeline auto-recycle
  auto& pool = disp.Pool();
  auto r = pool.Alloc();
  REQUIRE(r.has_value());
  pool.Submit(r.value(), 8U, 1U, 1U);  // 1ms timeout

  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  uint32_t timeouts = disp.ScanTimeout();
  REQUIRE(timeouts == 1U);
  REQUIRE(fault_count.load() == 1U);
}

TEST_CASE("DataDispatcher: capacity and payload queries", "[job_pool][dispatcher]") {
  DataDispatcher<2048, 16> disp;
  REQUIRE(disp.Capacity() == 16U);
  REQUIRE(disp.PayloadCapacity() == 2048U);
  REQUIRE(disp.FreeBlocks() == 16U);
  REQUIRE(disp.AllocBlocks() == 0U);
}

TEST_CASE("DataDispatcher: pipeline with data verification", "[job_pool][dispatcher]") {
  DataDispatcher<128, 4> disp;
  DataDispatcher<128, 4>::Config cfg;
  cfg.name = "data_verify";
  disp.Init(cfg);

  struct Result {
    uint32_t sum;
    bool valid;
  };
  Result result{0U, false};

  auto handler = [](const uint8_t* data, uint32_t len,
                    uint32_t /*block_id*/, void* ctx) {
    auto* r = static_cast<Result*>(ctx);
    uint32_t sum = 0U;
    for (uint32_t i = 0U; i < len; ++i) {
      sum += data[i];
    }
    r->sum = sum;
    r->valid = true;
  };

  auto s0 = disp.AddStage({"verify", handler, &result, 0U});
  disp.SetEntryStage(s0);

  auto bid = disp.AllocBlock();
  REQUIRE(bid.has_value());

  uint8_t* payload = disp.GetBlockPayload(bid.value());
  for (uint32_t i = 0U; i < 100U; ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }

  disp.SubmitBlock(bid.value(), 100U);

  REQUIRE(result.valid);
  REQUIRE(result.sum == 4950U);
  REQUIRE(disp.FreeBlocks() == 4U);
}

TEST_CASE("DataDispatcher: multiple blocks through pipeline", "[job_pool][dispatcher]") {
  DataDispatcher<64, 8> disp;
  DataDispatcher<64, 8>::Config cfg;
  cfg.name = "multi";
  disp.Init(cfg);

  std::atomic<uint32_t> count{0U};
  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  auto s0 = disp.AddStage({"count", handler, &count, 0U});
  disp.SetEntryStage(s0);

  for (uint32_t i = 0U; i < 20U; ++i) {
    auto bid = disp.AllocBlock();
    REQUIRE(bid.has_value());
    disp.SubmitBlock(bid.value(), 0U);
  }

  REQUIRE(count.load() == 20U);
  REQUIRE(disp.FreeBlocks() == 8U);
}
