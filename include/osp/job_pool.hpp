/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file job_pool.hpp
 * @brief Industrial-grade shared data block pipeline.
 *
 * Provides a unified data distribution and pipeline execution framework:
 *   - Fixed-size memory pool with atomic refcount (lock-free alloc/release)
 *   - Static DAG pipeline (fan-out + serial stages)
 *   - Backpressure mechanism + FaultCollector integration
 *   - Timeout detection per data block
 *   - Consumers are agnostic to data source (intra/inter-process)
 *
 * Architecture:
 *   DataDispatcher (top-level API)
 *     +-- JobPool (lock-free data block pool with embedded free list)
 *     +-- Pipeline (static DAG: stages + edges)
 *     +-- FaultReporter (optional fault integration)
 *     +-- Backpressure callback
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_JOB_POOL_HPP_
#define OSP_JOB_POOL_HPP_

#include "osp/fault_collector.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <atomic>
#include <type_traits>

// ============================================================================
// Compile-time configuration
// ============================================================================

#ifndef OSP_JOB_POOL_MAGIC
#define OSP_JOB_POOL_MAGIC 0x4A4F4250U
#endif

#ifndef OSP_JOB_BLOCK_ALIGN
#define OSP_JOB_BLOCK_ALIGN 64U
#endif

#ifndef OSP_JOB_MAX_STAGES
#define OSP_JOB_MAX_STAGES 8U
#endif

#ifndef OSP_JOB_MAX_EDGES
#define OSP_JOB_MAX_EDGES 16U
#endif

namespace osp {

// ============================================================================
// Error codes
// ============================================================================

enum class JobPoolError : uint8_t {
  kPoolExhausted = 0,  ///< Memory pool full (backpressure)
  kInvalidBlockId,     ///< block_id out of range
  kBlockNotReady,      ///< Data block not in expected state
  kBlockTimeout,       ///< Processing timeout
  kPipelineFull,       ///< Stage/edge count exceeded
  kNoEntryStage,       ///< No entry stage configured
  kInvalidStage,       ///< Invalid stage index
};

// ============================================================================
// BlockState -- data block lifecycle
// ============================================================================

enum class BlockState : uint8_t {
  kFree = 0,        ///< In free list
  kAllocated = 1,   ///< Allocated, producer filling
  kReady = 2,       ///< Data ready, awaiting consumption
  kProcessing = 3,  ///< At least one consumer processing
  kDone = 4,        ///< All consumers done, pending recycle
  kTimeout = 5,     ///< Timed out
  kError = 6,       ///< Processing error
};

// ============================================================================
// DataBlock -- shared data block header (placed before payload)
// ============================================================================

struct DataBlock {
  std::atomic<uint32_t> refcount;   ///< Consumer reference count (atomic)
  std::atomic<uint8_t> state;       ///< BlockState enum
  uint8_t pad[3];
  uint32_t block_id;                ///< Pool index
  uint32_t payload_size;            ///< Actual data size in payload
  uint32_t fault_id;                ///< Associated fault code (0 = none)
  uint64_t alloc_time_us;           ///< Allocation timestamp (steady clock)
  uint64_t deadline_us;             ///< Timeout deadline (0 = no timeout)
  uint32_t next_free;               ///< Embedded free list pointer
  uint32_t reserved;

  void Reset() noexcept {
    refcount.store(0U, std::memory_order_relaxed);
    state.store(static_cast<uint8_t>(BlockState::kFree), std::memory_order_relaxed);
    payload_size = 0U;
    fault_id = 0U;
    alloc_time_us = 0U;
    deadline_us = 0U;
    reserved = 0U;
  }

  BlockState GetState() const noexcept {
    return static_cast<BlockState>(state.load(std::memory_order_acquire));
  }

  void SetState(BlockState s) noexcept {
    state.store(static_cast<uint8_t>(s), std::memory_order_release);
  }
};

static_assert(std::is_trivially_destructible<DataBlock>::value,
              "DataBlock must be trivially destructible");

// ============================================================================
// JobPool<BlockSize, MaxBlocks> -- lock-free data block pool
//
// Embedded free list with CAS-based allocation. Each block consists of a
// DataBlock header followed by BlockSize bytes of payload.
//
// Thread-safety:
//   - Alloc(): lock-free (CAS on free_head)
//   - Release(): lock-free (atomic refcount decrement + CAS recycle)
//   - GetPayload/GetPayloadReadOnly(): wait-free
// ============================================================================

namespace detail {
static constexpr uint32_t kJobInvalidIndex = UINT32_MAX;
}  // namespace detail

template <uint32_t BlockSize, uint32_t MaxBlocks>
class JobPool {
  static_assert(BlockSize > 0U, "BlockSize must be > 0");
  static_assert(MaxBlocks > 0U && MaxBlocks < detail::kJobInvalidIndex,
                "MaxBlocks must be in (0, UINT32_MAX)");

 public:
  /// Aligned stride: DataBlock header + payload, rounded up to alignment.
  static constexpr uint32_t kHeaderSize =
      (static_cast<uint32_t>(sizeof(DataBlock)) + OSP_JOB_BLOCK_ALIGN - 1U) &
      ~(OSP_JOB_BLOCK_ALIGN - 1U);
  static constexpr uint32_t kPayloadSize = BlockSize;
  static constexpr uint32_t kBlockStride =
      ((kHeaderSize + BlockSize + OSP_JOB_BLOCK_ALIGN - 1U) &
       ~(OSP_JOB_BLOCK_ALIGN - 1U));

  JobPool() noexcept { Init(); }

  ~JobPool() = default;

  JobPool(const JobPool&) = delete;
  JobPool& operator=(const JobPool&) = delete;
  JobPool(JobPool&&) = delete;
  JobPool& operator=(JobPool&&) = delete;

  // --------------------------------------------------------------------------
  // Producer API
  // --------------------------------------------------------------------------

  /// @brief Allocate a data block (CAS lock-free).
  /// @return block_id on success, or kPoolExhausted.
  expected<uint32_t, JobPoolError> Alloc() noexcept {
    uint32_t head = free_head_.load(std::memory_order_acquire);
    while (head != detail::kJobInvalidIndex) {
      DataBlock* blk = GetBlock(head);
      uint32_t next = blk->next_free;
      if (free_head_.compare_exchange_weak(head, next,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        free_count_.fetch_sub(1U, std::memory_order_relaxed);
        alloc_count_.fetch_add(1U, std::memory_order_relaxed);
        blk->SetState(BlockState::kAllocated);
        blk->alloc_time_us = SteadyNowUs();
        blk->payload_size = 0U;
        blk->fault_id = 0U;
        blk->deadline_us = 0U;
        blk->refcount.store(0U, std::memory_order_relaxed);
        return expected<uint32_t, JobPoolError>::success(head);
      }
      // head reloaded by CAS failure
    }
    return expected<uint32_t, JobPoolError>::error(
        JobPoolError::kPoolExhausted);
  }

  /// @brief Get writable payload pointer for filling data.
  uint8_t* GetPayload(uint32_t block_id) noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    return reinterpret_cast<uint8_t*>(GetBlock(block_id)) + kHeaderSize;  // NOLINT
  }

  /// @brief Mark block as ready with given payload size and refcount.
  /// @param block_id Block index.
  /// @param payload_size Actual data size.
  /// @param refcount Initial reference count (number of consumers).
  /// @param timeout_ms Per-block timeout (0 = no timeout).
  void Submit(uint32_t block_id, uint32_t payload_size,
              uint32_t refcount, uint32_t timeout_ms = 0U) noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    DataBlock* blk = GetBlock(block_id);
    blk->payload_size = payload_size;
    if (timeout_ms > 0U) {
      blk->deadline_us = SteadyNowUs() +
                          static_cast<uint64_t>(timeout_ms) * 1000U;
    } else {
      blk->deadline_us = 0U;
    }
    blk->refcount.store(refcount, std::memory_order_release);
    blk->SetState(BlockState::kReady);
  }

  // --------------------------------------------------------------------------
  // Consumer API
  // --------------------------------------------------------------------------

  /// @brief Get read-only payload pointer.
  const uint8_t* GetPayloadReadOnly(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    return reinterpret_cast<const uint8_t*>(GetBlockConst(block_id)) +  // NOLINT
           kHeaderSize;
  }

  /// @brief Get payload size for a block.
  uint32_t GetPayloadSize(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    return GetBlockConst(block_id)->payload_size;
  }

  /// @brief Decrement refcount. Returns true if caller is the last consumer.
  /// When true, the block is automatically recycled to the free list.
  bool Release(uint32_t block_id) noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    DataBlock* blk = GetBlock(block_id);
    uint32_t prev = blk->refcount.fetch_sub(1U, std::memory_order_acq_rel);
    OSP_ASSERT(prev > 0U);
    if (prev == 1U) {
      // Last consumer -- recycle
      Recycle(block_id);
      return true;
    }
    return false;
  }

  /// @brief Add references to a block (for fan-out after stage completion).
  void AddRef(uint32_t block_id, uint32_t count) noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    GetBlock(block_id)->refcount.fetch_add(count, std::memory_order_release);
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  uint32_t FreeCount() const noexcept {
    return free_count_.load(std::memory_order_relaxed);
  }

  uint32_t AllocCount() const noexcept {
    return alloc_count_.load(std::memory_order_relaxed);
  }

  BlockState GetBlockState(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < MaxBlocks);
    return GetBlockConst(block_id)->GetState();
  }

  static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
  static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

  // --------------------------------------------------------------------------
  // Timeout detection
  // --------------------------------------------------------------------------

  /// @brief Scan all allocated blocks for timeout.
  /// @param on_timeout Callback for each timed-out block.
  /// @param ctx User context.
  /// @return Number of timed-out blocks found.
  uint32_t ScanTimeout(void (*on_timeout)(uint32_t block_id, void* ctx),
                        void* ctx) noexcept {
    uint32_t count = 0U;
    uint64_t now = SteadyNowUs();
    for (uint32_t i = 0U; i < MaxBlocks; ++i) {
      DataBlock* blk = GetBlock(i);
      auto st = blk->GetState();
      if (st == BlockState::kProcessing || st == BlockState::kReady) {
        if (blk->deadline_us != 0U && now > blk->deadline_us) {
          blk->SetState(BlockState::kTimeout);
          if (on_timeout != nullptr) {
            on_timeout(i, ctx);
          }
          // Force recycle
          blk->refcount.store(0U, std::memory_order_relaxed);
          Recycle(i);
          ++count;
        }
      }
    }
    return count;
  }

  /// @brief Force-release all blocks held by a crashed consumer.
  /// @param predicate Returns true for blocks that should be force-released.
  /// @return Number of blocks cleaned up.
  uint32_t ForceCleanup(bool (*predicate)(uint32_t block_id, void* ctx),
                         void* ctx) noexcept {
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < MaxBlocks; ++i) {
      DataBlock* blk = GetBlock(i);
      auto st = blk->GetState();
      if (st == BlockState::kProcessing || st == BlockState::kReady) {
        if (predicate != nullptr && predicate(i, ctx)) {
          blk->SetState(BlockState::kError);
          uint32_t prev = blk->refcount.fetch_sub(1U, std::memory_order_acq_rel);
          if (prev <= 1U) {
            Recycle(i);
          }
          ++count;
        }
      }
    }
    return count;
  }

 private:
  void Init() noexcept {
    // Build embedded free list
    for (uint32_t i = 0U; i < MaxBlocks; ++i) {
      DataBlock* blk = GetBlock(i);
      blk->Reset();
      blk->block_id = i;
      blk->next_free = (i + 1U < MaxBlocks) ? (i + 1U)
                                              : detail::kJobInvalidIndex;
    }
    free_head_.store(0U, std::memory_order_relaxed);
    free_count_.store(MaxBlocks, std::memory_order_relaxed);
    alloc_count_.store(0U, std::memory_order_relaxed);
  }

  void Recycle(uint32_t block_id) noexcept {
    DataBlock* blk = GetBlock(block_id);
    blk->SetState(BlockState::kFree);
    alloc_count_.fetch_sub(1U, std::memory_order_relaxed);
    // CAS push to free list head
    uint32_t head = free_head_.load(std::memory_order_acquire);
    do {
      blk->next_free = head;
    } while (!free_head_.compare_exchange_weak(head, block_id,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire));
    free_count_.fetch_add(1U, std::memory_order_relaxed);
  }

  DataBlock* GetBlock(uint32_t block_id) noexcept {
    return reinterpret_cast<DataBlock*>(  // NOLINT
        &storage_[static_cast<size_t>(block_id) * kBlockStride]);
  }

  const DataBlock* GetBlockConst(uint32_t block_id) const noexcept {
    return reinterpret_cast<const DataBlock*>(  // NOLINT
        &storage_[static_cast<size_t>(block_id) * kBlockStride]);
  }

  static constexpr size_t kStorageSize =
      static_cast<size_t>(kBlockStride) * MaxBlocks;
  alignas(OSP_JOB_BLOCK_ALIGN) uint8_t storage_[kStorageSize];

  alignas(kCacheLineSize) std::atomic<uint32_t> free_head_{0U};
  alignas(kCacheLineSize) std::atomic<uint32_t> free_count_{MaxBlocks};
  alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count_{0U};
};

// ============================================================================
// JobHandler -- stage processing function
// ============================================================================

/// @brief Handler function for a pipeline stage.
/// @param data Pointer to payload data.
/// @param len Payload size in bytes.
/// @param block_id Block index (for advanced use).
/// @param ctx User context pointer.
using JobHandler = void (*)(const uint8_t* data, uint32_t len,
                            uint32_t block_id, void* ctx);

// ============================================================================
// StageConfig -- DAG node configuration
// ============================================================================

struct StageConfig {
  const char* name;          ///< Stage name (debug/diagnostics)
  JobHandler handler;        ///< Processing function
  void* handler_ctx;         ///< Handler context
  uint32_t timeout_ms;       ///< Per-block timeout for this stage (0 = none)
};

// ============================================================================
// Pipeline<MaxStages, MaxEdges> -- static DAG pipeline
//
// Configured at initialization, immutable at runtime.
// Supports fan-out (A -> B, A -> C) and serial (A -> B -> C) topologies.
// ============================================================================

template <uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
class Pipeline {
 public:
  Pipeline() noexcept : stage_count_(0U), entry_stage_(-1) {}

  // --------------------------------------------------------------------------
  // Build (call during initialization only)
  // --------------------------------------------------------------------------

  /// @brief Add a stage to the pipeline.
  /// @return Stage index on success, or -1 if full.
  int32_t AddStage(const StageConfig& cfg) noexcept {
    if (stage_count_ >= MaxStages) {
      return -1;
    }
    auto& slot = stages_[stage_count_];
    slot.config = cfg;
    slot.successor_count = 0U;
    return static_cast<int32_t>(stage_count_++);
  }

  /// @brief Add a directed edge: when 'from' completes, trigger 'to'.
  /// @return true on success, false if edge limit reached or invalid indices.
  bool AddEdge(int32_t from, int32_t to) noexcept {
    if (from < 0 || to < 0) return false;
    auto uf = static_cast<uint32_t>(from);
    auto ut = static_cast<uint32_t>(to);
    if (uf >= stage_count_ || ut >= stage_count_) return false;
    auto& slot = stages_[uf];
    if (slot.successor_count >= MaxEdges) return false;
    slot.successors[slot.successor_count++] = to;
    return true;
  }

  /// @brief Set the entry stage (first stage to receive submitted blocks).
  void SetEntryStage(int32_t stage_id) noexcept {
    entry_stage_ = stage_id;
  }

  // --------------------------------------------------------------------------
  // Execution
  // --------------------------------------------------------------------------

  /// @brief Execute a block through the entry stage.
  /// @tparam PoolType JobPool type.
  /// @param pool Reference to the JobPool.
  /// @param block_id Block to process.
  /// @return true if entry stage was executed.
  template <typename PoolType>
  bool Execute(PoolType& pool, uint32_t block_id) noexcept {
    if (entry_stage_ < 0 ||
        static_cast<uint32_t>(entry_stage_) >= stage_count_) {
      return false;
    }
    ExecuteStage(pool, entry_stage_, block_id);
    return true;
  }

  /// @brief Get the fan-out count for the entry stage.
  /// Used to set initial refcount on Submit.
  uint32_t EntryFanOut() const noexcept {
    if (entry_stage_ < 0 ||
        static_cast<uint32_t>(entry_stage_) >= stage_count_) {
      return 1U;
    }
    // Entry stage itself counts as 1 consumer.
    // Its successors will be added when it completes.
    return 1U;
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  uint32_t StageCount() const noexcept { return stage_count_; }

  const char* StageName(int32_t stage_id) const noexcept {
    if (stage_id < 0 || static_cast<uint32_t>(stage_id) >= stage_count_) {
      return "unknown";
    }
    return stages_[static_cast<uint32_t>(stage_id)].config.name;
  }

  int32_t EntryStage() const noexcept { return entry_stage_; }

  uint32_t SuccessorCount(int32_t stage_id) const noexcept {
    if (stage_id < 0 || static_cast<uint32_t>(stage_id) >= stage_count_) {
      return 0U;
    }
    return stages_[static_cast<uint32_t>(stage_id)].successor_count;
  }

 private:
  /// @brief Execute a single stage and trigger successors.
  template <typename PoolType>
  void ExecuteStage(PoolType& pool, int32_t stage_id,
                    uint32_t block_id) noexcept {
    auto idx = static_cast<uint32_t>(stage_id);
    if (idx >= stage_count_) return;

    auto& slot = stages_[idx];

    // Set block state to processing
    // (only if transitioning from kReady; already kProcessing is fine for
    //  pipeline stages after the first)

    // Execute handler
    if (slot.config.handler != nullptr) {
      const uint8_t* data = pool.GetPayloadReadOnly(block_id);
      uint32_t len = pool.GetPayloadSize(block_id);
      slot.config.handler(data, len, block_id, slot.config.handler_ctx);
    }

    // Trigger successors
    OnStageComplete(pool, stage_id, block_id);
  }

  /// @brief Called when a stage finishes. Adds refs for successors, then
  /// releases this stage's ref. If refcount hits 0, block is recycled.
  template <typename PoolType>
  void OnStageComplete(PoolType& pool, int32_t stage_id,
                       uint32_t block_id) noexcept {
    auto idx = static_cast<uint32_t>(stage_id);
    auto& slot = stages_[idx];

    // Add refs for each successor before releasing our own
    if (slot.successor_count > 0U) {
      pool.AddRef(block_id, slot.successor_count);
    }

    // Release this stage's reference
    bool last = pool.Release(block_id);

    // If not last, successors will run when they get scheduled.
    // For synchronous execution, we run successors inline here.
    if (!last) {
      // Block still alive, execute successors
      for (uint32_t i = 0U; i < slot.successor_count; ++i) {
        ExecuteStage(pool, slot.successors[i], block_id);
      }
    } else {
      // Block was recycled (refcount hit 0).
      // This happens when there are no successors and this was the last ref.
    }
  }

  struct StageSlot {
    StageConfig config;
    int32_t successors[MaxEdges];
    uint32_t successor_count;
  };

  StageSlot stages_[MaxStages];
  uint32_t stage_count_;
  int32_t entry_stage_;
};

// ============================================================================
// BackpressureCallback
// ============================================================================

/// @brief Callback invoked when free block count drops below threshold.
/// @param free_count Current number of free blocks.
/// @param ctx User context.
using BackpressureFn = void (*)(uint32_t free_count, void* ctx);

// ============================================================================
// DataDispatcher -- unified top-level API
//
// Combines JobPool + Pipeline + backpressure + fault integration.
// This is the primary user-facing class.
//
// @tparam BlockSize  Payload size per block (bytes).
// @tparam MaxBlocks  Total number of blocks in the pool.
// @tparam MaxStages  Maximum pipeline stages.
// @tparam MaxEdgesPerStage Maximum edges per stage.
// ============================================================================

template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdgesPerStage = OSP_JOB_MAX_EDGES>
class DataDispatcher {
 public:
  using PoolType = JobPool<BlockSize, MaxBlocks>;
  using PipelineType = Pipeline<MaxStages, MaxEdgesPerStage>;

  struct Config {
    const char* name;                 ///< Channel name (diagnostics)
    uint32_t default_timeout_ms;      ///< Global per-block timeout (0 = none)
    uint32_t backpressure_threshold;  ///< Trigger when free < this value

    Config() noexcept
        : name("dispatcher"),
          default_timeout_ms(0U),
          backpressure_threshold(0U) {}
  };

  DataDispatcher() noexcept = default;
  ~DataDispatcher() = default;

  DataDispatcher(const DataDispatcher&) = delete;
  DataDispatcher& operator=(const DataDispatcher&) = delete;

  // --------------------------------------------------------------------------
  // Initialization
  // --------------------------------------------------------------------------

  void Init(const Config& cfg) noexcept {
    cfg_ = cfg;
  }

  // --------------------------------------------------------------------------
  // Pipeline configuration (call during init, before any Submit)
  // --------------------------------------------------------------------------

  int32_t AddStage(const StageConfig& cfg) noexcept {
    return pipeline_.AddStage(cfg);
  }

  bool AddEdge(int32_t from, int32_t to) noexcept {
    return pipeline_.AddEdge(from, to);
  }

  void SetEntryStage(int32_t stage_id) noexcept {
    pipeline_.SetEntryStage(stage_id);
  }

  // --------------------------------------------------------------------------
  // Producer API
  // --------------------------------------------------------------------------

  /// @brief Allocate a data block. Triggers backpressure if threshold reached.
  expected<uint32_t, JobPoolError> AllocBlock() noexcept {
    // Check backpressure before allocation
    uint32_t free = pool_.FreeCount();
    if (cfg_.backpressure_threshold > 0U &&
        free <= cfg_.backpressure_threshold) {
      if (bp_callback_ != nullptr) {
        bp_callback_(free, bp_ctx_);
      }
      if (fault_reporter_.fn != nullptr) {
        fault_reporter_.Report(fault_slot_,
                               free,
                               FaultPriority::kLow);
      }
    }
    return pool_.Alloc();
  }

  /// @brief Get writable payload pointer.
  uint8_t* GetBlockPayload(uint32_t block_id) noexcept {
    return pool_.GetPayload(block_id);
  }

  /// @brief Submit a filled block into the pipeline.
  /// Sets refcount based on pipeline entry fan-out, then executes
  /// the pipeline synchronously.
  void SubmitBlock(uint32_t block_id, uint32_t payload_size) noexcept {
    uint32_t refcount = pipeline_.EntryFanOut();
    uint32_t timeout = cfg_.default_timeout_ms;
    pool_.Submit(block_id, payload_size, refcount, timeout);
    pipeline_.Execute(pool_, block_id);
  }

  // --------------------------------------------------------------------------
  // Consumer API (for external consumers not in the pipeline)
  // --------------------------------------------------------------------------

  /// @brief Get read-only payload.
  const uint8_t* GetBlockPayloadReadOnly(uint32_t block_id) const noexcept {
    return pool_.GetPayloadReadOnly(block_id);
  }

  /// @brief Get payload size.
  uint32_t GetBlockPayloadSize(uint32_t block_id) const noexcept {
    return pool_.GetPayloadSize(block_id);
  }

  /// @brief Release a block reference. Returns true if last consumer.
  bool ReleaseBlock(uint32_t block_id) noexcept {
    return pool_.Release(block_id);
  }

  /// @brief Add references to a block (for external fan-out).
  void AddBlockRef(uint32_t block_id, uint32_t count) noexcept {
    pool_.AddRef(block_id, count);
  }

  // --------------------------------------------------------------------------
  // Backpressure
  // --------------------------------------------------------------------------

  void SetBackpressureCallback(BackpressureFn cb, void* ctx) noexcept {
    bp_callback_ = cb;
    bp_ctx_ = ctx;
  }

  // --------------------------------------------------------------------------
  // Fault integration
  // --------------------------------------------------------------------------

  void SetFaultReporter(const FaultReporter& reporter,
                        uint16_t slot) noexcept {
    fault_reporter_ = reporter;
    fault_slot_ = slot;
  }

  // --------------------------------------------------------------------------
  // Monitoring
  // --------------------------------------------------------------------------

  /// @brief Scan for timed-out blocks.
  uint32_t ScanTimeout() noexcept {
    return pool_.ScanTimeout(
        [](uint32_t block_id, void* ctx) {
          auto* self = static_cast<DataDispatcher*>(ctx);
          if (self->fault_reporter_.fn != nullptr) {
            self->fault_reporter_.Report(self->fault_slot_, block_id,
                                         FaultPriority::kHigh);
          }
        },
        this);
  }

  uint32_t FreeBlocks() const noexcept { return pool_.FreeCount(); }
  uint32_t AllocBlocks() const noexcept { return pool_.AllocCount(); }
  static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
  static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

  // --------------------------------------------------------------------------
  // Access to internals (for advanced use / testing)
  // --------------------------------------------------------------------------

  PoolType& Pool() noexcept { return pool_; }
  const PoolType& Pool() const noexcept { return pool_; }
  PipelineType& GetPipeline() noexcept { return pipeline_; }
  const PipelineType& GetPipeline() const noexcept { return pipeline_; }
  const Config& GetConfig() const noexcept { return cfg_; }

 private:
  PoolType pool_;
  PipelineType pipeline_;
  Config cfg_;

  // Fault
  FaultReporter fault_reporter_;
  uint16_t fault_slot_{0U};

  // Backpressure
  BackpressureFn bp_callback_{nullptr};
  void* bp_ctx_{nullptr};
};

}  // namespace osp

#endif  // OSP_JOB_POOL_HPP_
