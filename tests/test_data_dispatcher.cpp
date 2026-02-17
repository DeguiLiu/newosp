/**
 * @file test_data_dispatcher.cpp
 * @brief Catch2 tests for osp/data_dispatcher.hpp
 *        (InProcStore + Pipeline + DataDispatcher).
 *
 * Test structure:
 *   - Store-level: CAS alloc/release/recycle/timeout via minimal dispatcher
 *   - Pipeline-level: topology, execution order, fan-out
 *   - Dispatcher-level: integration (pipeline + backpressure + fault + notify)
 */

#include <catch2/catch_test_macros.hpp>

#include "osp/data_dispatcher.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace osp;

// Convenience alias for tests: InProcDispatcher with default notify/pipeline.
template <uint32_t BS, uint32_t MB>
using Disp = InProcDispatcher<BS, MB>;

// Helper: raw alloc + manual refcount (bypass pipeline for pool-level tests).
static void RawSubmit(DataBlock* blk, uint32_t size, uint32_t refcount,
                      uint32_t timeout_ms = 0U) {
  blk->payload_size = size;
  if (timeout_ms > 0U) {
    blk->deadline_us = SteadyNowUs() +
                        static_cast<uint64_t>(timeout_ms) * 1000U;
  } else {
    blk->deadline_us = 0U;
  }
  blk->refcount.store(refcount, std::memory_order_release);
  blk->SetState(BlockState::kReady);
}

// ============================================================================
// Pool -- basic allocation
// ============================================================================

TEST_CASE("Pool: initial state", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);
  REQUIRE(d.FreeBlocks() == 4U);
  REQUIRE(d.AllocBlocks() == 0U);
  REQUIRE(d.Capacity() == 4U);
  REQUIRE(d.PayloadCapacity() == 64U);
}

TEST_CASE("Pool: alloc and release single block", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r0 = d.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(d.FreeBlocks() == 3U);
  REQUIRE(d.AllocBlocks() == 1U);
  REQUIRE(d.GetBlockState(r0.value()) == BlockState::kAllocated);

  RawSubmit(d.GetStore().GetBlock(r0.value()), 32U, 1U);
  REQUIRE(d.GetPayloadSize(r0.value()) == 32U);

  bool last = d.Release(r0.value());
  REQUIRE(last);
  REQUIRE(d.FreeBlocks() == 4U);
  REQUIRE(d.AllocBlocks() == 0U);
  REQUIRE(d.GetBlockState(r0.value()) == BlockState::kFree);
}

TEST_CASE("Pool: exhaust pool returns kPoolExhausted", "[job_pool]") {
  Disp<32, 2> d;
  Disp<32, 2>::Config cfg;
  d.Init(cfg);

  auto r0 = d.Alloc();
  auto r1 = d.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(r1.has_value());
  REQUIRE(d.FreeBlocks() == 0U);

  auto r2 = d.Alloc();
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.get_error() == JobPoolError::kPoolExhausted);

  RawSubmit(d.GetStore().GetBlock(r0.value()), 0U, 1U);
  d.Release(r0.value());
  REQUIRE(d.FreeBlocks() == 1U);

  auto r3 = d.Alloc();
  REQUIRE(r3.has_value());

  RawSubmit(d.GetStore().GetBlock(r1.value()), 0U, 1U);
  d.Release(r1.value());
  RawSubmit(d.GetStore().GetBlock(r3.value()), 0U, 1U);
  d.Release(r3.value());
}

TEST_CASE("Pool: alloc all blocks then release all", "[job_pool]") {
  constexpr uint32_t kN = 8U;
  Disp<16, kN> d;
  Disp<16, kN>::Config cfg;
  d.Init(cfg);

  uint32_t ids[kN];
  for (uint32_t i = 0U; i < kN; ++i) {
    auto r = d.Alloc();
    REQUIRE(r.has_value());
    ids[i] = r.value();
  }
  REQUIRE(d.FreeBlocks() == 0U);
  REQUIRE(d.AllocBlocks() == kN);

  for (uint32_t i = 0U; i < kN; ++i) {
    RawSubmit(d.GetStore().GetBlock(ids[i]), 0U, 1U);
    d.Release(ids[i]);
  }
  REQUIRE(d.FreeBlocks() == kN);
  REQUIRE(d.AllocBlocks() == 0U);
}

// ============================================================================
// Pool -- payload read/write
// ============================================================================

TEST_CASE("Pool: payload read and write", "[job_pool]") {
  Disp<128, 2> d;
  Disp<128, 2>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  uint8_t* payload = d.GetWritable(bid);
  REQUIRE(payload != nullptr);
  std::memset(payload, 0xAB, 128);

  RawSubmit(d.GetStore().GetBlock(bid), 128U, 1U);

  const uint8_t* ro = d.GetReadable(bid);
  REQUIRE(ro != nullptr);
  REQUIRE(ro[0] == 0xAB);
  REQUIRE(ro[127] == 0xAB);

  d.Release(bid);
}

TEST_CASE("Pool: payload pointer alignment", "[job_pool]") {
  Disp<256, 2> d;
  Disp<256, 2>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint8_t* payload = d.GetWritable(r.value());
  auto addr = reinterpret_cast<uintptr_t>(payload);  // NOLINT
  REQUIRE((addr % 8U) == 0U);

  RawSubmit(d.GetStore().GetBlock(r.value()), 0U, 1U);
  d.Release(r.value());
}

// ============================================================================
// Pool -- reference counting
// ============================================================================

TEST_CASE("Pool: refcount fan-out (3 consumers)", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  RawSubmit(d.GetStore().GetBlock(bid), 16U, 3U);

  REQUIRE_FALSE(d.Release(bid));  // 3->2
  REQUIRE(d.AllocBlocks() == 1U);

  REQUIRE_FALSE(d.Release(bid));  // 2->1
  REQUIRE(d.AllocBlocks() == 1U);

  REQUIRE(d.Release(bid));        // 1->0, recycled
  REQUIRE(d.AllocBlocks() == 0U);
  REQUIRE(d.FreeBlocks() == 4U);
}

TEST_CASE("Pool: AddRef for dynamic fan-out", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  RawSubmit(d.GetStore().GetBlock(bid), 8U, 1U);
  d.AddRef(bid, 2U);

  REQUIRE_FALSE(d.Release(bid));
  REQUIRE_FALSE(d.Release(bid));
  REQUIRE(d.Release(bid));
  REQUIRE(d.FreeBlocks() == 4U);
}

// ============================================================================
// Pool -- timeout detection
// ============================================================================

TEST_CASE("Pool: timeout detection fires", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();

  RawSubmit(d.GetStore().GetBlock(bid), 8U, 1U, 1U);  // 1ms timeout

  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  uint32_t timeout_bid = UINT32_MAX;
  uint32_t count = d.ScanTimeout(
      [](uint32_t block_id, void* ctx) {
        *static_cast<uint32_t*>(ctx) = block_id;
      },
      &timeout_bid);

  REQUIRE(count == 1U);
  REQUIRE(timeout_bid == bid);
  REQUIRE(d.FreeBlocks() == 4U);
}

TEST_CASE("Pool: no timeout when deadline is 0", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  RawSubmit(d.GetStore().GetBlock(r.value()), 8U, 1U, 0U);

  uint32_t count = d.ScanTimeout(nullptr, nullptr);
  REQUIRE(count == 0U);

  d.Release(r.value());
}

TEST_CASE("Pool: no timeout before deadline", "[job_pool]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  RawSubmit(d.GetStore().GetBlock(r.value()), 8U, 1U, 5000U);  // 5s

  uint32_t count = d.ScanTimeout(nullptr, nullptr);
  REQUIRE(count == 0U);

  d.Release(r.value());
}

// ============================================================================
// Pool -- concurrent alloc/release
// ============================================================================

TEST_CASE("Pool: concurrent alloc and release", "[job_pool]") {
  constexpr uint32_t kPoolSize = 32U;
  Disp<16, kPoolSize> d;
  Disp<16, kPoolSize>::Config cfg;
  d.Init(cfg);

  std::atomic<uint32_t> success_count{0U};
  constexpr uint32_t kThreads = 4U;
  constexpr uint32_t kOpsPerThread = 100U;

  auto worker = [&]() {
    for (uint32_t i = 0U; i < kOpsPerThread; ++i) {
      auto r = d.Alloc();
      if (r.has_value()) {
        success_count.fetch_add(1U, std::memory_order_relaxed);
        RawSubmit(d.GetStore().GetBlock(r.value()), 0U, 1U);
        d.Release(r.value());
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

  REQUIRE(d.FreeBlocks() == kPoolSize);
  REQUIRE(d.AllocBlocks() == 0U);
  REQUIRE(success_count.load() > 0U);
}

TEST_CASE("Pool: ScanTimeout vs Release concurrent safety", "[job_pool]") {
  constexpr uint32_t kPoolSize = 16U;
  Disp<64, kPoolSize> d;
  Disp<64, kPoolSize>::Config cfg;
  d.Init(cfg);
  constexpr uint32_t kIterations = 200U;

  std::atomic<uint32_t> release_count{0U};
  std::atomic<uint32_t> timeout_count{0U};

  for (uint32_t iter = 0U; iter < kIterations; ++iter) {
    auto r = d.Alloc();
    REQUIRE(r.has_value());
    uint32_t bid = r.value();
    RawSubmit(d.GetStore().GetBlock(bid), 8U, 1U, 1U);  // 1ms timeout

    std::this_thread::sleep_for(std::chrono::milliseconds(3));

    std::atomic<bool> go{false};
    bool release_last = false;
    uint32_t scan_found = 0U;

    std::thread t_release([&]() {
      while (!go.load(std::memory_order_acquire)) {}
      release_last = d.Release(bid);
    });

    std::thread t_scan([&]() {
      while (!go.load(std::memory_order_acquire)) {}
      scan_found = d.ScanTimeout(nullptr, nullptr);
    });

    go.store(true, std::memory_order_release);
    t_release.join();
    t_scan.join();

    if (release_last) {
      release_count.fetch_add(1U, std::memory_order_relaxed);
    }
    if (scan_found > 0U) {
      timeout_count.fetch_add(1U, std::memory_order_relaxed);
    }

    // Block must be recycled exactly once (no double-free)
    REQUIRE(d.GetBlockState(bid) == BlockState::kFree);
  }

  REQUIRE(d.FreeBlocks() == kPoolSize);
  REQUIRE(d.AllocBlocks() == 0U);
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
// Pipeline -- execution (via DataDispatcher)
// ============================================================================

TEST_CASE("Pipeline: single stage execution", "[job_pool][pipeline]") {
  std::atomic<uint32_t> call_count{0U};

  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto s0 = d.AddStage({"s0", handler, &call_count, 0U});
  d.SetEntryStage(s0);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  d.Submit(r.value(), 0U);

  REQUIRE(call_count.load() == 1U);
  REQUIRE(d.FreeBlocks() == 4U);
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

  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto sa = d.AddStage({"A", handler, &ctx_a, 0U});
  auto sb = d.AddStage({"B", handler, &ctx_b, 0U});
  auto sc = d.AddStage({"C", handler, &ctx_c, 0U});
  d.AddEdge(sa, sb);
  d.AddEdge(sb, sc);
  d.SetEntryStage(sa);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  d.Submit(r.value(), 0U);

  REQUIRE(order[0] < order[1]);
  REQUIRE(order[1] < order[2]);
  REQUIRE(d.FreeBlocks() == 4U);
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

  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  d.Init(cfg);

  auto sa = d.AddStage({"A", handler_a, nullptr, 0U});
  auto sb = d.AddStage({"B", handler_b, &b_count, 0U});
  auto sc = d.AddStage({"C", handler_c, &c_count, 0U});
  d.AddEdge(sa, sb);
  d.AddEdge(sa, sc);
  d.SetEntryStage(sa);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  d.Submit(r.value(), 0U);

  REQUIRE(b_count.load() == 1U);
  REQUIRE(c_count.load() == 1U);
  REQUIRE(d.FreeBlocks() == 4U);
}

// ============================================================================
// DataDispatcher -- integration
// ============================================================================

TEST_CASE("Dispatcher: basic alloc-submit-release",
          "[job_pool][dispatcher]") {
  Disp<1024, 8> d;
  Disp<1024, 8>::Config cfg;
  cfg.name = "test";
  d.Init(cfg);

  std::atomic<uint32_t> processed{0U};
  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  auto s0 = d.AddStage({"proc", handler, &processed, 0U});
  d.SetEntryStage(s0);

  auto bid = d.Alloc();
  REQUIRE(bid.has_value());

  uint8_t* payload = d.GetWritable(bid.value());
  std::memset(payload, 0x42, 64);

  d.Submit(bid.value(), 64U);

  REQUIRE(processed.load() == 1U);
  REQUIRE(d.FreeBlocks() == 8U);
  REQUIRE(d.AllocBlocks() == 0U);
}

TEST_CASE("Dispatcher: backpressure callback", "[job_pool][dispatcher]") {
  Disp<32, 4> d;
  Disp<32, 4>::Config cfg;
  cfg.name = "bp_test";
  cfg.backpressure_threshold = 3U;
  d.Init(cfg);

  auto s0 = d.AddStage({"noop", nullptr, nullptr, 0U});
  d.SetEntryStage(s0);

  std::atomic<uint32_t> bp_triggered{0U};
  d.SetBackpressureCallback(
      [](uint32_t /*free_count*/, void* ctx) {
        static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
      },
      &bp_triggered);

  auto r0 = d.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(bp_triggered.load() == 0U);

  auto r1 = d.Alloc();
  REQUIRE(r1.has_value());
  REQUIRE(bp_triggered.load() == 1U);

  d.Submit(r0.value(), 0U);
  d.Submit(r1.value(), 0U);
}

TEST_CASE("Dispatcher: timeout scan with fault reporter",
          "[job_pool][dispatcher]") {
  Disp<64, 4> d;
  Disp<64, 4>::Config cfg;
  cfg.name = "timeout_test";
  d.Init(cfg);

  std::atomic<uint32_t> fault_count{0U};
  FaultReporter reporter;
  reporter.fn = [](uint16_t /*idx*/, uint32_t /*detail*/,
                   FaultPriority /*pri*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };
  reporter.ctx = &fault_count;
  d.SetFaultReporter(reporter, 0U);

  auto r = d.Alloc();
  REQUIRE(r.has_value());
  RawSubmit(d.GetStore().GetBlock(r.value()), 8U, 1U, 1U);

  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  uint32_t timeouts = d.ScanTimeout();
  REQUIRE(timeouts == 1U);
  REQUIRE(fault_count.load() == 1U);
}

TEST_CASE("Dispatcher: capacity and payload queries",
          "[job_pool][dispatcher]") {
  Disp<2048, 16> d;
  REQUIRE(d.Capacity() == 16U);
  REQUIRE(d.PayloadCapacity() == 2048U);
  REQUIRE(d.FreeBlocks() == 16U);
  REQUIRE(d.AllocBlocks() == 0U);
}

TEST_CASE("Dispatcher: pipeline with data verification",
          "[job_pool][dispatcher]") {
  Disp<128, 4> d;
  Disp<128, 4>::Config cfg;
  cfg.name = "data_verify";
  d.Init(cfg);

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

  auto s0 = d.AddStage({"verify", handler, &result, 0U});
  d.SetEntryStage(s0);

  auto bid = d.Alloc();
  REQUIRE(bid.has_value());

  uint8_t* payload = d.GetWritable(bid.value());
  for (uint32_t i = 0U; i < 100U; ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }

  d.Submit(bid.value(), 100U);

  REQUIRE(result.valid);
  REQUIRE(result.sum == 4950U);
  REQUIRE(d.FreeBlocks() == 4U);
}

TEST_CASE("Dispatcher: multiple blocks through pipeline",
          "[job_pool][dispatcher]") {
  Disp<64, 8> d;
  Disp<64, 8>::Config cfg;
  cfg.name = "multi";
  d.Init(cfg);

  std::atomic<uint32_t> count{0U};
  auto handler = [](const uint8_t* /*data*/, uint32_t /*len*/,
                    uint32_t /*block_id*/, void* ctx) {
    static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U);
  };

  auto s0 = d.AddStage({"count", handler, &count, 0U});
  d.SetEntryStage(s0);

  for (uint32_t i = 0U; i < 20U; ++i) {
    auto bid = d.Alloc();
    REQUIRE(bid.has_value());
    d.Submit(bid.value(), 0U);
  }

  REQUIRE(count.load() == 20U);
  REQUIRE(d.FreeBlocks() == 8U);
}

// ============================================================================
// Phase 5: BlockState constexpr transition table
// ============================================================================

TEST_CASE("BlockState: valid transitions accepted", "[job_pool][state]") {
  using osp::BlockState;
  using osp::detail::IsValidBlockTransition;

  // Normal lifecycle: Free -> Alloc -> Ready -> Processing -> Free
  REQUIRE(IsValidBlockTransition(BlockState::kFree, BlockState::kAllocated));
  REQUIRE(IsValidBlockTransition(BlockState::kAllocated, BlockState::kReady));
  REQUIRE(IsValidBlockTransition(BlockState::kReady, BlockState::kProcessing));
  REQUIRE(IsValidBlockTransition(BlockState::kProcessing, BlockState::kFree));

  // Timeout/error paths
  REQUIRE(IsValidBlockTransition(BlockState::kReady, BlockState::kTimeout));
  REQUIRE(IsValidBlockTransition(BlockState::kReady, BlockState::kError));
  REQUIRE(IsValidBlockTransition(BlockState::kProcessing, BlockState::kTimeout));
  REQUIRE(IsValidBlockTransition(BlockState::kProcessing, BlockState::kError));

  // Recycle from terminal states
  REQUIRE(IsValidBlockTransition(BlockState::kDone, BlockState::kFree));
  REQUIRE(IsValidBlockTransition(BlockState::kTimeout, BlockState::kFree));
  REQUIRE(IsValidBlockTransition(BlockState::kError, BlockState::kFree));

  // Direct recycle from Ready (ScanTimeout/ForceCleanup)
  REQUIRE(IsValidBlockTransition(BlockState::kReady, BlockState::kFree));

  // Producer abandons allocated block
  REQUIRE(IsValidBlockTransition(BlockState::kAllocated, BlockState::kFree));
}

TEST_CASE("BlockState: invalid transitions rejected", "[job_pool][state]") {
  using osp::BlockState;
  using osp::detail::IsValidBlockTransition;

  // Cannot go backwards
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kReady,
                                        BlockState::kAllocated));
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kProcessing,
                                        BlockState::kReady));
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kProcessing,
                                        BlockState::kAllocated));

  // Cannot skip states forward
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kFree,
                                        BlockState::kReady));
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kFree,
                                        BlockState::kProcessing));
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kAllocated,
                                        BlockState::kProcessing));

  // Self-transitions are invalid
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kFree,
                                        BlockState::kFree));
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kAllocated,
                                        BlockState::kAllocated));

  // Out-of-range (cast to invalid enum value)
  auto invalid = static_cast<BlockState>(7U);
  REQUIRE_FALSE(IsValidBlockTransition(BlockState::kFree, invalid));
  REQUIRE_FALSE(IsValidBlockTransition(invalid, BlockState::kFree));
}

TEST_CASE("BlockState: constexpr table is compile-time evaluable",
          "[job_pool][state]") {
  using osp::kBlockStateCount;
  using osp::detail::kBlockStateTransition;

  // Verify dimensions
  static_assert(kBlockStateCount == 7U, "Expected 7 block states");

  // Verify a known transition at compile time
  static_assert(kBlockStateTransition[0][1] == true,
                "Free->Alloc must be valid");
  static_assert(kBlockStateTransition[0][0] == false,
                "Free->Free must be invalid");
  static_assert(kBlockStateTransition[2][3] == true,
                "Ready->Processing must be valid");

  REQUIRE(true);  // If we reach here, static_asserts passed
}

// ============================================================================
// Phase 3: ConsumerSlot -- ShmStore consumer tracking
// ============================================================================

// Helpers: allocate/free a POSIX shm region for testing.
static void* CreateTestShm(const char* name, uint32_t size) {
  shm_unlink(name);  // Cleanup any leftover
  int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, size) < 0) {
    close(fd);
    shm_unlink(name);
    return nullptr;
  }
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (ptr == MAP_FAILED) {
    shm_unlink(name);
    return nullptr;
  }
  return ptr;
}

static void CleanupTestShm(const char* name, void* ptr, uint32_t size) {
  if (ptr != nullptr && ptr != MAP_FAILED) {
    munmap(ptr, size);
  }
  shm_unlink(name);
}

// Type aliases for ShmStore tests.
using TestShmStore = osp::ShmStore<64, 8>;
using TestShmDisp = osp::DataDispatcher<TestShmStore>;

TEST_CASE("ShmStore: creator init and attach", "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_init";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  // Creator: init
  TestShmDisp creator;
  TestShmDisp::Config cfg;
  cfg.name = "test";
  creator.Init(cfg, ptr, size);

  REQUIRE(creator.FreeBlocks() == 8U);
  REQUIRE(creator.AllocBlocks() == 0U);
  REQUIRE(creator.Capacity() == 8U);
  REQUIRE(creator.PayloadCapacity() == 64U);

  // Opener: attach (same process, same mmap)
  TestShmDisp opener;
  opener.Attach(ptr);

  REQUIRE(opener.FreeBlocks() == 8U);
  REQUIRE(opener.Capacity() == 8U);

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ShmStore: alloc-submit-release lifecycle", "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_lifecycle";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  cfg.name = "lifecycle";
  d.Init(cfg, ptr, size);

  // Alloc
  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();
  REQUIRE(d.FreeBlocks() == 7U);
  REQUIRE(d.AllocBlocks() == 1U);
  REQUIRE(d.GetBlockState(bid) == osp::BlockState::kAllocated);

  // Fill and submit (consumer_count=1 for flat mode)
  uint8_t* w = d.GetWritable(bid);
  std::memset(w, 0xAB, 64U);
  d.Submit(bid, 64U, 1U);
  REQUIRE(d.GetBlockState(bid) == osp::BlockState::kReady);

  // Consume
  const uint8_t* r_ptr = d.GetReadable(bid);
  REQUIRE(r_ptr[0] == 0xAB);
  REQUIRE(d.GetPayloadSize(bid) == 64U);

  // Release (last consumer)
  bool last = d.Release(bid);
  REQUIRE(last);
  REQUIRE(d.FreeBlocks() == 8U);
  REQUIRE(d.AllocBlocks() == 0U);

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ShmStore: RequiredShmSize includes consumer slots",
          "[job_pool][ShmStore]") {
  uint32_t base = TestShmStore::kShmHeaderSize +
                  TestShmStore::kBlockStride * 8U;
  uint32_t with_slots = base +
      static_cast<uint32_t>(sizeof(osp::detail::ConsumerSlot)) *
      OSP_JOB_MAX_CONSUMERS;

  REQUIRE(TestShmStore::RequiredShmSize() == with_slots);
  REQUIRE(TestShmStore::RequiredShmSize() > base);
}

TEST_CASE("ConsumerSlot: register and unregister", "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_consumer";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  d.Init(cfg, ptr, size);

  // Register consumers
  int32_t s0 = d.RegisterConsumer(1000U);
  int32_t s1 = d.RegisterConsumer(2000U);
  REQUIRE(s0 >= 0);
  REQUIRE(s1 >= 0);
  REQUIRE(s0 != s1);

  // Verify slot data
  auto* slot0 = d.GetStore().GetConsumerSlot(static_cast<uint32_t>(s0));
  REQUIRE(slot0->active.load() == 1U);
  REQUIRE(slot0->pid.load() == 1000U);
  REQUIRE(slot0->holding_mask.load() == 0U);

  // Unregister
  d.UnregisterConsumer(s0);
  REQUIRE(slot0->active.load() == 0U);
  REQUIRE(slot0->pid.load() == 0U);

  // Re-register reuses freed slot
  int32_t s2 = d.RegisterConsumer(3000U);
  REQUIRE(s2 == s0);  // Reused the freed slot

  d.UnregisterConsumer(s1);
  d.UnregisterConsumer(s2);

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ConsumerSlot: register exhaustion", "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_exhaust";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  d.Init(cfg, ptr, size);

  // Fill all slots
  int32_t slots[OSP_JOB_MAX_CONSUMERS];
  for (uint32_t i = 0U; i < OSP_JOB_MAX_CONSUMERS; ++i) {
    slots[i] = d.RegisterConsumer(100U + i);
    REQUIRE(slots[i] >= 0);
  }

  // Next registration should fail
  int32_t overflow = d.RegisterConsumer(999U);
  REQUIRE(overflow == -1);

  // Unregister one and retry
  d.UnregisterConsumer(slots[3]);
  int32_t reuse = d.RegisterConsumer(999U);
  REQUIRE(reuse == slots[3]);

  for (uint32_t i = 0U; i < OSP_JOB_MAX_CONSUMERS; ++i) {
    d.UnregisterConsumer(static_cast<int32_t>(i));
  }

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ConsumerSlot: holding_mask tracking", "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_mask";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  d.Init(cfg, ptr, size);

  int32_t cid = d.RegisterConsumer(getpid());
  REQUIRE(cid >= 0);

  // Track holding blocks 0, 3, 7
  d.TrackBlockHold(cid, 0U);
  d.TrackBlockHold(cid, 3U);
  d.TrackBlockHold(cid, 7U);

  auto* slot = d.GetStore().GetConsumerSlot(static_cast<uint32_t>(cid));
  uint64_t mask = slot->holding_mask.load();
  REQUIRE((mask & (1ULL << 0U)) != 0U);
  REQUIRE((mask & (1ULL << 3U)) != 0U);
  REQUIRE((mask & (1ULL << 7U)) != 0U);
  REQUIRE((mask & (1ULL << 1U)) == 0U);

  // Release block 3
  d.TrackBlockRelease(cid, 3U);
  mask = slot->holding_mask.load();
  REQUIRE((mask & (1ULL << 3U)) == 0U);
  REQUIRE((mask & (1ULL << 0U)) != 0U);
  REQUIRE((mask & (1ULL << 7U)) != 0U);

  d.UnregisterConsumer(cid);
  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ConsumerSlot: cleanup dead consumer reclaims blocks",
          "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_cleanup";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  d.Init(cfg, ptr, size);

  // Allocate 3 blocks, submit with refcount=2 (simulating 2 consumers)
  uint32_t bids[3];
  for (int i = 0; i < 3; ++i) {
    auto r = d.Alloc();
    REQUIRE(r.has_value());
    bids[i] = r.value();
    d.Submit(bids[i], 10U, 2U);
  }
  REQUIRE(d.FreeBlocks() == 5U);
  REQUIRE(d.AllocBlocks() == 3U);

  // Consumer A registers and tracks blocks
  int32_t cA = d.RegisterConsumer(1000U);
  REQUIRE(cA >= 0);
  for (int i = 0; i < 3; ++i) {
    d.TrackBlockHold(cA, bids[i]);
  }

  // Consumer B registers and tracks blocks
  int32_t cB = d.RegisterConsumer(2000U);
  REQUIRE(cB >= 0);
  for (int i = 0; i < 3; ++i) {
    d.TrackBlockHold(cB, bids[i]);
  }

  // Consumer B "crashes" -- mark inactive but don't release blocks
  auto* slotB = d.GetStore().GetConsumerSlot(static_cast<uint32_t>(cB));
  slotB->active.store(0U, std::memory_order_release);
  // Note: holding_mask still has bits set (simulating crash)

  // Cleanup dead consumers
  uint32_t reclaimed = d.CleanupDeadConsumers();
  REQUIRE(reclaimed == 3U);

  // Blocks still held by consumer A (refcount decremented from 2 to 1)
  REQUIRE(d.AllocBlocks() == 3U);

  // Consumer A releases normally
  for (int i = 0; i < 3; ++i) {
    d.TrackBlockRelease(cA, bids[i]);
    bool last = d.Release(bids[i]);
    REQUIRE(last);
  }

  REQUIRE(d.FreeBlocks() == 8U);
  REQUIRE(d.AllocBlocks() == 0U);

  d.UnregisterConsumer(cA);
  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ConsumerSlot: cleanup dead consumer as sole holder recycles block",
          "[job_pool][ShmStore]") {
  const char* shm_name = "/osp_test_dd_sole";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp d;
  TestShmDisp::Config cfg;
  d.Init(cfg, ptr, size);

  // Alloc + submit with refcount=1
  auto r = d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();
  d.Submit(bid, 10U, 1U);

  // Consumer registers and holds the block
  int32_t cid = d.RegisterConsumer(5000U);
  REQUIRE(cid >= 0);
  d.TrackBlockHold(cid, bid);

  // Simulate crash
  d.GetStore().GetConsumerSlot(static_cast<uint32_t>(cid))
      ->active.store(0U, std::memory_order_release);

  REQUIRE(d.FreeBlocks() == 7U);

  // Cleanup should reclaim the block (sole consumer -> refcount 1->0)
  uint32_t reclaimed = d.CleanupDeadConsumers();
  REQUIRE(reclaimed == 1U);
  REQUIRE(d.FreeBlocks() == 8U);
  REQUIRE(d.AllocBlocks() == 0U);

  CleanupTestShm(shm_name, ptr, size);
}

// ============================================================================
// Phase 4: Cross-process fork tests (ShmStore)
// ============================================================================

TEST_CASE("ShmStore cross-process: producer alloc, consumer release",
          "[job_pool][ShmStore][fork]") {
#if defined(__SANITIZE_THREAD__)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#endif
#endif

  const char* shm_name = "/osp_test_dd_fork1";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  // Parent = creator
  TestShmDisp parent_d;
  TestShmDisp::Config cfg;
  cfg.name = "fork1";
  parent_d.Init(cfg, ptr, size);

  // Alloc + fill + submit (refcount=1 for single consumer child)
  auto r = parent_d.Alloc();
  REQUIRE(r.has_value());
  uint32_t bid = r.value();
  uint8_t* w = parent_d.GetWritable(bid);
  for (uint32_t i = 0; i < 64U; ++i) {
    w[i] = static_cast<uint8_t>(i * 3 + 7);
  }
  parent_d.Submit(bid, 64U, 1U);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: attach and consume
    TestShmDisp child_d;
    child_d.Attach(ptr);

    const uint8_t* data = child_d.GetReadable(bid);
    for (uint32_t i = 0; i < 64U; ++i) {
      if (data[i] != static_cast<uint8_t>(i * 3 + 7)) _exit(1);
    }
    if (child_d.GetPayloadSize(bid) != 64U) _exit(2);

    bool last = child_d.Release(bid);
    if (!last) _exit(3);

    // Verify free count updated
    if (child_d.FreeBlocks() != 8U) _exit(4);
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  // Parent sees the release too (shared memory)
  REQUIRE(parent_d.FreeBlocks() == 8U);
  REQUIRE(parent_d.AllocBlocks() == 0U);

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ShmStore cross-process: consumer crash recovery",
          "[job_pool][ShmStore][fork]") {
#if defined(__SANITIZE_THREAD__)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#endif
#endif

  const char* shm_name = "/osp_test_dd_crash";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  // Parent = creator
  TestShmDisp parent_d;
  TestShmDisp::Config cfg;
  cfg.name = "crash";
  parent_d.Init(cfg, ptr, size);

  // Alloc 2 blocks, submit with refcount=1
  auto r0 = parent_d.Alloc();
  auto r1 = parent_d.Alloc();
  REQUIRE(r0.has_value());
  REQUIRE(r1.has_value());
  parent_d.Submit(r0.value(), 10U, 1U);
  parent_d.Submit(r1.value(), 10U, 1U);

  REQUIRE(parent_d.FreeBlocks() == 6U);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: attach, register, hold blocks, then _exit without releasing
    TestShmDisp child_d;
    child_d.Attach(ptr);

    int32_t cid = child_d.RegisterConsumer(static_cast<uint32_t>(getpid()));
    if (cid < 0) _exit(1);

    child_d.TrackBlockHold(cid, r0.value());
    child_d.TrackBlockHold(cid, r1.value());

    // Simulate crash: mark inactive and exit
    child_d.GetStore().GetConsumerSlot(static_cast<uint32_t>(cid))
        ->active.store(0U, std::memory_order_release);
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  // Parent: blocks still allocated (child crashed without releasing)
  REQUIRE(parent_d.FreeBlocks() == 6U);

  // Cleanup dead consumers should reclaim both blocks
  uint32_t reclaimed = parent_d.CleanupDeadConsumers();
  REQUIRE(reclaimed == 2U);
  REQUIRE(parent_d.FreeBlocks() == 8U);
  REQUIRE(parent_d.AllocBlocks() == 0U);

  CleanupTestShm(shm_name, ptr, size);
}

TEST_CASE("ShmStore cross-process: concurrent alloc and release",
          "[job_pool][ShmStore][fork]") {
#if defined(__SANITIZE_THREAD__)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  SKIP("Skipped under ThreadSanitizer (fork + shm unreliable)");
#endif
#endif

  const char* shm_name = "/osp_test_dd_conc";
  uint32_t size = TestShmStore::RequiredShmSize();
  void* ptr = CreateTestShm(shm_name, size);
  REQUIRE(ptr != nullptr);

  TestShmDisp parent_d;
  TestShmDisp::Config cfg;
  cfg.name = "conc";
  parent_d.Init(cfg, ptr, size);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: alloc 4 blocks, submit, then release all
    TestShmDisp child_d;
    child_d.Attach(ptr);

    uint32_t bids[4];
    for (int i = 0; i < 4; ++i) {
      auto r = child_d.Alloc();
      if (!r.has_value()) _exit(1);
      bids[i] = r.value();
      child_d.Submit(bids[i], 8U, 1U);
    }
    for (int i = 0; i < 4; ++i) {
      child_d.Release(bids[i]);
    }
    _exit(0);
  }

  // Parent: alloc 4 blocks, submit, then release all
  uint32_t bids[4];
  for (int i = 0; i < 4; ++i) {
    auto r = parent_d.Alloc();
    REQUIRE(r.has_value());
    bids[i] = r.value();
    parent_d.Submit(bids[i], 8U, 1U);
  }
  for (int i = 0; i < 4; ++i) {
    parent_d.Release(bids[i]);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  // All 8 blocks should be free
  REQUIRE(parent_d.FreeBlocks() == 8U);
  REQUIRE(parent_d.AllocBlocks() == 0U);

  CleanupTestShm(shm_name, ptr, size);
}
