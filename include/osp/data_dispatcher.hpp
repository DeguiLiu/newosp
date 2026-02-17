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
 * @file data_dispatcher.hpp
 * @brief Industrial-grade shared data block pipeline with StorePolicy.
 *
 * Architecture (design_job_pool_zh2.md, Plan A):
 *
 *   DataDispatcher<StorePolicy, NotifyPolicy, MaxStages, MaxEdges>
 *     +-- StorePolicy: detail::InProcStore<BS,MB> or detail::ShmStore<BS,MB>
 *     +-- NotifyPolicy: DirectNotify or ShmNotify
 *     +-- Pipeline<MaxStages, MaxEdges>  (static DAG)
 *     +-- FaultReporter  (optional)
 *     +-- BackpressureFn (optional)
 *
 * CAS lock-free logic is written once in DataDispatcher, shared by all
 * StorePolicy implementations. StorePolicy provides only accessor methods
 * (GetBlock, FreeHead, FreeCount, AllocCount, Capacity).
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_DATA_DISPATCHER_HPP_
#define OSP_DATA_DISPATCHER_HPP_

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

#ifndef OSP_JOB_MAX_CONSUMERS
#define OSP_JOB_MAX_CONSUMERS 8U
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

static constexpr uint8_t kBlockStateCount = 7U;

// ============================================================================
// BlockState transition validation (constexpr, zero cost in release)
// ============================================================================

namespace detail {

/// Valid state transitions. Row = from-state, Col = to-state.
/// Enforced by OSP_ASSERT in debug builds, optimized away in release.
///
/// Notes on transitions to kFree:
///   - kProcessing/kDone/kTimeout/kError -> kFree: normal Recycle paths
///   - kReady -> kFree: immediate Recycle (consumer releases without
///     entering kProcessing, e.g. ScanTimeout, ForceCleanup, or
///     single-refcount blocks recycled on Release)
///   - kAllocated -> kFree: producer abandons block without Submit
static constexpr bool kBlockStateTransition[kBlockStateCount][kBlockStateCount] = {
//                Free   Alloc  Ready  Proc   Done   Tmout  Error
/* Free    */ {   false, true,  false, false, false, false, false },
/* Alloc   */ {   true,  false, true,  false, false, false, false },
/* Ready   */ {   true,  false, false, true,  false, true,  true  },
/* Proc    */ {   true,  false, false, false, false, true,  true  },
/* Done    */ {   true,  false, false, false, false, false, false },
/* Timeout */ {   true,  false, false, false, false, false, false },
/* Error   */ {   true,  false, false, false, false, false, false },
};

inline bool IsValidBlockTransition(BlockState from, BlockState to) noexcept {
  auto f = static_cast<uint8_t>(from);
  auto t = static_cast<uint8_t>(to);
  if (f >= kBlockStateCount || t >= kBlockStateCount) return false;
  return kBlockStateTransition[f][t];
}

}  // namespace detail

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
    state.store(static_cast<uint8_t>(BlockState::kFree),
                std::memory_order_relaxed);
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
    OSP_ASSERT(detail::IsValidBlockTransition(GetState(), s));
    state.store(static_cast<uint8_t>(s), std::memory_order_release);
  }
};

static_assert(std::is_trivially_destructible<DataBlock>::value,
              "DataBlock must be trivially destructible");

// ============================================================================
// detail
// ============================================================================

namespace detail {

static constexpr uint32_t kJobInvalidIndex = UINT32_MAX;

/// Aligned header size for DataBlock.
static constexpr uint32_t kJobHeaderAlignedSize =
    (static_cast<uint32_t>(sizeof(DataBlock)) + OSP_JOB_BLOCK_ALIGN - 1U) &
    ~(OSP_JOB_BLOCK_ALIGN - 1U);

/// Compute block stride (header + payload, aligned).
static constexpr uint32_t JobBlockStride(uint32_t block_size) noexcept {
  return ((kJobHeaderAlignedSize + block_size + OSP_JOB_BLOCK_ALIGN - 1U) &
          ~(OSP_JOB_BLOCK_ALIGN - 1U));
}

}  // namespace detail

// ============================================================================
// detail::InProcStore<BlockSize, MaxBlocks> -- in-process embedded storage
//
// Storage is an embedded byte array (stack/static). Pool metadata (free_head,
// free_count, alloc_count) are member atomics.
//
// WARNING: For large payloads (e.g. LiDAR: BlockSize=16016, MaxBlocks=32
// -> ~512KB), objects using this store MUST NOT be placed on the stack.
// Use static/global storage or heap allocation.
// ============================================================================

namespace detail {

template <uint32_t BlockSize, uint32_t MaxBlocks>
struct InProcStore {
  static_assert(BlockSize > 0U, "BlockSize must be > 0");
  static_assert(MaxBlocks > 0U && MaxBlocks < detail::kJobInvalidIndex,
                "MaxBlocks must be in (0, UINT32_MAX)");

  static constexpr uint32_t kBlockStride =
      detail::JobBlockStride(BlockSize);
  static constexpr uint32_t kHeaderAlignedSize =
      detail::kJobHeaderAlignedSize;

  InProcStore() noexcept = default;

  /// @brief Initialize free list. Called once by DataDispatcher::Init().
  void Init() noexcept {
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

  /// @brief InProcStore does not support shm Init. Compile error if called.
  template <typename T = void>
  void Init(void*, uint32_t) noexcept {
    static_assert(sizeof(T) == 0,
        "InProcStore does not support Init(shm_base, size). "
        "Use ShmStore for inter-process mode.");
  }

  /// @brief InProcStore does not support Attach. Compile error if called.
  template <typename T = void>
  void Attach(void*) noexcept {
    static_assert(sizeof(T) == 0,
        "InProcStore does not support Attach(). "
        "Use ShmStore for inter-process mode.");
  }

  DataBlock* GetBlock(uint32_t id) noexcept {
    return reinterpret_cast<DataBlock*>(  // NOLINT
        &storage_[static_cast<size_t>(id) * kBlockStride]);
  }

  const DataBlock* GetBlock(uint32_t id) const noexcept {
    return reinterpret_cast<const DataBlock*>(  // NOLINT
        &storage_[static_cast<size_t>(id) * kBlockStride]);
  }

  std::atomic<uint32_t>& FreeHead() noexcept { return free_head_; }
  std::atomic<uint32_t>& FreeCount() noexcept { return free_count_; }
  std::atomic<uint32_t>& AllocCount() noexcept { return alloc_count_; }

  const std::atomic<uint32_t>& FreeHead() const noexcept { return free_head_; }
  const std::atomic<uint32_t>& FreeCount() const noexcept { return free_count_; }
  const std::atomic<uint32_t>& AllocCount() const noexcept { return alloc_count_; }

  static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
  static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

 private:
  static constexpr size_t kStorageSize =
      static_cast<size_t>(kBlockStride) * MaxBlocks;
  alignas(OSP_JOB_BLOCK_ALIGN) uint8_t storage_[kStorageSize];

  alignas(kCacheLineSize) std::atomic<uint32_t> free_head_{0U};
  alignas(kCacheLineSize) std::atomic<uint32_t> free_count_{MaxBlocks};
  alignas(kCacheLineSize) std::atomic<uint32_t> alloc_count_{0U};
};

}  // namespace detail

// Convenience alias: expose detail::InProcStore at osp:: level.
template <uint32_t BlockSize, uint32_t MaxBlocks>
using InProcStore = detail::InProcStore<BlockSize, MaxBlocks>;

// ============================================================================
// detail::ShmStore<BlockSize, MaxBlocks> -- shared memory storage
//
// Block pool resides in a user-provided shared memory region.
// Pool metadata (free_head, free_count, alloc_count) are stored in
// ShmPoolHeader at the start of the region, visible to all processes.
//
// Two initialization paths:
//   Creator: Init(shm_base, shm_size) -- zeroes and initializes free list
//   Opener:  Attach(shm_base)         -- validates magic/version/params
//
// The shared memory lifecycle (shm_open/mmap/munmap/shm_unlink) is
// managed by the caller, NOT by ShmStore.
// ============================================================================

namespace detail {

/// @brief Shared memory pool header. Placed at offset 0 of the shm region.
/// Atomics are cache-line aligned to prevent false sharing across processes.
struct ShmPoolHeader {
  uint32_t magic;               ///< OSP_JOB_POOL_MAGIC
  uint32_t version;             ///< Header version (1)
  uint32_t block_size;          ///< Template BlockSize
  uint32_t max_blocks;          ///< Template MaxBlocks
  uint32_t block_stride;        ///< Computed block stride
  uint32_t total_size;          ///< Total shm region size
  uint32_t max_consumers;       ///< Consumer slot count
  uint32_t consumer_slot_offset;  ///< ConsumerSlot array offset from shm base
  alignas(OSP_JOB_BLOCK_ALIGN) std::atomic<uint32_t> free_head;
  alignas(OSP_JOB_BLOCK_ALIGN) std::atomic<uint32_t> free_count;
  alignas(OSP_JOB_BLOCK_ALIGN) std::atomic<uint32_t> alloc_count;
};

/// @brief Per-consumer tracking slot in shared memory.
/// Each consumer process registers into a slot. The holding_mask bitmap
/// tracks which blocks the consumer currently holds (bit i = block i).
/// Supports up to 64 blocks (uint64_t bitmap).
struct ConsumerSlot {
  std::atomic<uint32_t> active;       ///< 0 = free, 1 = active
  std::atomic<uint32_t> pid;          ///< Consumer process PID
  std::atomic<uint64_t> heartbeat_us; ///< Last heartbeat timestamp (us)
  std::atomic<uint64_t> holding_mask; ///< Bitmap of held blocks
};

static_assert(std::is_trivially_destructible<ConsumerSlot>::value,
              "ConsumerSlot must be trivially destructible for shm");


template <uint32_t BlockSize, uint32_t MaxBlocks>
struct ShmStore {
  static_assert(BlockSize > 0U, "BlockSize must be > 0");
  static_assert(MaxBlocks > 0U && MaxBlocks < detail::kJobInvalidIndex,
                "MaxBlocks must be in (0, UINT32_MAX)");
  static_assert(MaxBlocks <= 64U,
                "MaxBlocks must be <= 64 (ConsumerSlot holding_mask is uint64_t)");

  static constexpr uint32_t kBlockStride =
      detail::JobBlockStride(BlockSize);
  static constexpr uint32_t kHeaderAlignedSize =
      detail::kJobHeaderAlignedSize;

  /// Aligned size of ShmPoolHeader (cache-line boundary).
  static constexpr uint32_t kShmHeaderSize =
      ((static_cast<uint32_t>(sizeof(ShmPoolHeader)) +
        OSP_JOB_BLOCK_ALIGN - 1U) & ~(OSP_JOB_BLOCK_ALIGN - 1U));

  /// Aligned size of ConsumerSlot array.
  static constexpr uint32_t kConsumerSlotArraySize =
      static_cast<uint32_t>(sizeof(ConsumerSlot)) * OSP_JOB_MAX_CONSUMERS;

  ShmStore() noexcept = default;

  /// @brief Creator path: initialize shared memory region with free list.
  /// @param shm_base Pointer to mmap'd shared memory region.
  /// @param shm_size Size of the region (must be >= RequiredShmSize()).
  void Init(void* shm_base, uint32_t shm_size) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    OSP_ASSERT(shm_size >= RequiredShmSize());
    base_ = static_cast<uint8_t*>(shm_base);
    header_ = reinterpret_cast<ShmPoolHeader*>(base_);  // NOLINT

    std::memset(base_, 0, shm_size);

    header_->magic = OSP_JOB_POOL_MAGIC;
    header_->version = 1U;
    header_->block_size = BlockSize;
    header_->max_blocks = MaxBlocks;
    header_->block_stride = kBlockStride;
    header_->total_size = shm_size;
    header_->max_consumers = OSP_JOB_MAX_CONSUMERS;
    header_->consumer_slot_offset = kShmHeaderSize +
        static_cast<uint32_t>(kBlockStride) * MaxBlocks;

    for (uint32_t i = 0U; i < MaxBlocks; ++i) {
      DataBlock* blk = GetBlock(i);
      blk->Reset();
      blk->block_id = i;
      blk->next_free = (i + 1U < MaxBlocks) ? (i + 1U)
                                              : detail::kJobInvalidIndex;
    }
    header_->free_head.store(0U, std::memory_order_release);
    header_->free_count.store(MaxBlocks, std::memory_order_release);
    header_->alloc_count.store(0U, std::memory_order_release);

    // Initialize consumer slots
    for (uint32_t i = 0U; i < OSP_JOB_MAX_CONSUMERS; ++i) {
      ConsumerSlot* slot = GetConsumerSlot(i);
      slot->active.store(0U, std::memory_order_relaxed);
      slot->pid.store(0U, std::memory_order_relaxed);
      slot->heartbeat_us.store(0U, std::memory_order_relaxed);
      slot->holding_mask.store(0U, std::memory_order_relaxed);
    }
  }

  /// @brief Opener path: attach to existing shared memory region.
  /// Validates magic, version, and template parameters.
  void Attach(void* shm_base) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    base_ = static_cast<uint8_t*>(shm_base);
    header_ = reinterpret_cast<ShmPoolHeader*>(base_);  // NOLINT
    OSP_ASSERT(header_->magic == OSP_JOB_POOL_MAGIC);
    OSP_ASSERT(header_->version == 1U);
    OSP_ASSERT(header_->block_size == BlockSize);
    OSP_ASSERT(header_->max_blocks == MaxBlocks);
  }

  /// @brief ShmStore does not support no-arg Init. Compile error if called.
  template <typename T = void>
  void Init() noexcept {
    static_assert(sizeof(T) == 0,
        "ShmStore requires Init(shm_base, shm_size). "
        "Use InProcStore for in-process mode.");
  }

  DataBlock* GetBlock(uint32_t id) noexcept {
    return reinterpret_cast<DataBlock*>(  // NOLINT
        base_ + kShmHeaderSize +
        static_cast<size_t>(id) * kBlockStride);
  }

  const DataBlock* GetBlock(uint32_t id) const noexcept {
    return reinterpret_cast<const DataBlock*>(  // NOLINT
        base_ + kShmHeaderSize +
        static_cast<size_t>(id) * kBlockStride);
  }

  /// @brief Get consumer slot by index.
  ConsumerSlot* GetConsumerSlot(uint32_t idx) noexcept {
    OSP_ASSERT(header_ != nullptr);
    OSP_ASSERT(idx < header_->max_consumers);
    return reinterpret_cast<ConsumerSlot*>(  // NOLINT
        base_ + header_->consumer_slot_offset) + idx;
  }

  const ConsumerSlot* GetConsumerSlot(uint32_t idx) const noexcept {
    OSP_ASSERT(header_ != nullptr);
    OSP_ASSERT(idx < header_->max_consumers);
    return reinterpret_cast<const ConsumerSlot*>(  // NOLINT
        base_ + header_->consumer_slot_offset) + idx;
  }

  uint32_t MaxConsumers() const noexcept {
    return (header_ != nullptr) ? header_->max_consumers : 0U;
  }

  std::atomic<uint32_t>& FreeHead() noexcept { return header_->free_head; }
  std::atomic<uint32_t>& FreeCount() noexcept { return header_->free_count; }
  std::atomic<uint32_t>& AllocCount() noexcept { return header_->alloc_count; }

  const std::atomic<uint32_t>& FreeHead() const noexcept {
    return header_->free_head;
  }
  const std::atomic<uint32_t>& FreeCount() const noexcept {
    return header_->free_count;
  }
  const std::atomic<uint32_t>& AllocCount() const noexcept {
    return header_->alloc_count;
  }

  static constexpr uint32_t Capacity() noexcept { return MaxBlocks; }
  static constexpr uint32_t PayloadCapacity() noexcept { return BlockSize; }

  /// @brief Required shared memory size in bytes (header + blocks + slots).
  static constexpr uint32_t RequiredShmSize() noexcept {
    return kShmHeaderSize +
           static_cast<uint32_t>(kBlockStride) * MaxBlocks +
           kConsumerSlotArraySize;
  }

 private:
  uint8_t* base_{nullptr};
  ShmPoolHeader* header_{nullptr};
};

}  // namespace detail

// Convenience alias: expose detail::ShmStore at osp:: level.
template <uint32_t BlockSize, uint32_t MaxBlocks>
using ShmStore = detail::ShmStore<BlockSize, MaxBlocks>;

// ============================================================================
// NotifyPolicy implementations
// ============================================================================

/// @brief In-process notify: no-op. Pipeline execution is driven by
/// DataDispatcher::Submit() directly.
struct DirectNotify {
  void OnSubmit(uint32_t /*block_id*/,
                uint32_t /*payload_size*/) noexcept {}
};

/// @brief Inter-process notify: push block_id via user-injected callback.
/// The callback typically writes 4B block_id to ShmSpmcByteChannel.
struct ShmNotify {
  using NotifyFn = void (*)(uint32_t block_id, uint32_t payload_size,
                            void* ctx);
  NotifyFn fn{nullptr};
  void* ctx{nullptr};

  void OnSubmit(uint32_t block_id, uint32_t payload_size) noexcept {
    if (fn != nullptr) {
      fn(block_id, payload_size, ctx);
    }
  }
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
//
// NOTE: Synchronous execution uses recursive ExecuteStage() calls.
// Maximum recursion depth equals the longest chain in the DAG (bounded
// by MaxStages). Default MaxStages=8 is safe for typical embedded stacks.
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
  /// @tparam DispatcherType DataDispatcher type (provides pool-like interface).
  /// @param disp Reference to the DataDispatcher.
  /// @param block_id Block to process.
  /// @return true if entry stage was executed.
  template <typename DispatcherType>
  bool Execute(DispatcherType& disp, uint32_t block_id) noexcept {
    if (entry_stage_ < 0 ||
        static_cast<uint32_t>(entry_stage_) >= stage_count_) {
      return false;
    }
    ExecuteStage(disp, entry_stage_, block_id);
    return true;
  }

  /// @brief Get the fan-out count for the entry stage.
  /// Used to set initial refcount on Submit.
  uint32_t EntryFanOut() const noexcept {
    if (entry_stage_ < 0 ||
        static_cast<uint32_t>(entry_stage_) >= stage_count_) {
      return 1U;
    }
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
  template <typename DispatcherType>
  void ExecuteStage(DispatcherType& disp, int32_t stage_id,
                    uint32_t block_id) noexcept {
    auto idx = static_cast<uint32_t>(stage_id);
    if (idx >= stage_count_) return;

    auto& slot = stages_[idx];

    // Execute handler
    if (slot.config.handler != nullptr) {
      const uint8_t* data = disp.GetReadable(block_id);
      uint32_t len = disp.GetPayloadSize(block_id);
      slot.config.handler(data, len, block_id, slot.config.handler_ctx);
    }

    // Trigger successors
    OnStageComplete(disp, stage_id, block_id);
  }

  /// @brief Called when a stage finishes. Adds refs for successors, then
  /// releases this stage's ref. If refcount hits 0, block is recycled.
  template <typename DispatcherType>
  void OnStageComplete(DispatcherType& disp, int32_t stage_id,
                       uint32_t block_id) noexcept {
    auto idx = static_cast<uint32_t>(stage_id);
    auto& slot = stages_[idx];

    // Add refs for each successor before releasing our own
    if (slot.successor_count > 0U) {
      disp.AddRef(block_id, slot.successor_count);
    }

    // Release this stage's reference
    bool last = disp.Release(block_id);

    // If not last, successors will run when they get scheduled.
    // For synchronous execution, we run successors inline here.
    if (!last) {
      for (uint32_t i = 0U; i < slot.successor_count; ++i) {
        ExecuteStage(disp, slot.successors[i], block_id);
      }
    }
    // If last: block was recycled (refcount hit 0). No successors to run.
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
using BackpressureFn = void (*)(uint32_t free_count, void* ctx);

// ============================================================================
// DataDispatcher<Store, Notify> -- unified top-level API
//
// Combines StorePolicy + NotifyPolicy + Pipeline + backpressure + fault.
// CAS lock-free logic (Alloc/Release/Recycle/ScanTimeout/ForceCleanup)
// is written once here, shared by all StorePolicy implementations.
//
// @tparam Store   InProcStore<BS,MB> or ShmStore<BS,MB>
// @tparam Notify  DirectNotify (default) or ShmNotify
// @tparam MaxStages  Maximum pipeline stages.
// @tparam MaxEdgesPerStage Maximum edges per stage.
// ============================================================================

template <typename Store,
          typename Notify = DirectNotify,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdgesPerStage = OSP_JOB_MAX_EDGES>
class DataDispatcher {
 public:
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

  /// @brief InProc initialization: store_.Init() zeroes embedded storage.
  void Init(const Config& cfg) noexcept {
    cfg_ = cfg;
    store_.Init();
  }

  /// @brief InterProc Creator: store_.Init(shm_base, size).
  void Init(const Config& cfg, void* shm_base, uint32_t shm_size) noexcept {
    cfg_ = cfg;
    store_.Init(shm_base, shm_size);
  }

  /// @brief InterProc Opener: store_.Attach(shm_base).
  void Attach(void* shm_base) noexcept {
    store_.Attach(shm_base);
  }

  /// @brief InterProc Opener with config: store_.Attach(shm_base).
  void Attach(const Config& cfg, void* shm_base) noexcept {
    cfg_ = cfg;
    store_.Attach(shm_base);
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
  /// Lock-free CAS on free list head.
  expected<uint32_t, JobPoolError> Alloc() noexcept {
    // Check backpressure before allocation
    uint32_t free = store_.FreeCount().load(std::memory_order_relaxed);
    if (cfg_.backpressure_threshold > 0U &&
        free <= cfg_.backpressure_threshold) {
      if (bp_fn_ != nullptr) {
        bp_fn_(free, bp_ctx_);
      }
      if (fault_reporter_.fn != nullptr) {
        fault_reporter_.Report(fault_slot_, free, FaultPriority::kLow);
      }
    }
    // CAS free list pop (single implementation for all Store types)
    uint32_t head = store_.FreeHead().load(std::memory_order_acquire);
    while (head != detail::kJobInvalidIndex) {
      DataBlock* blk = store_.GetBlock(head);
      uint32_t next = blk->next_free;
      if (store_.FreeHead().compare_exchange_weak(
              head, next,
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        store_.FreeCount().fetch_sub(1U, std::memory_order_relaxed);
        store_.AllocCount().fetch_add(1U, std::memory_order_relaxed);
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
  uint8_t* GetWritable(uint32_t block_id) noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    return reinterpret_cast<uint8_t*>(  // NOLINT
               store_.GetBlock(block_id)) +
           Store::kHeaderAlignedSize;
  }

  /// @brief Submit a filled block into the pipeline.
  /// @param block_id Block index.
  /// @param payload_size Actual data size.
  /// @param consumer_count Explicit refcount for InterProc mode (number of
  ///        remote consumers). If 0, uses pipeline_.EntryFanOut() (InProc).
  void Submit(uint32_t block_id, uint32_t payload_size,
              uint32_t consumer_count = 0U) noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    DataBlock* blk = store_.GetBlock(block_id);
    blk->payload_size = payload_size;
    uint32_t timeout = cfg_.default_timeout_ms;
    if (timeout > 0U) {
      blk->deadline_us = SteadyNowUs() +
                          static_cast<uint64_t>(timeout) * 1000U;
    } else {
      blk->deadline_us = 0U;
    }
    uint32_t refcount = (consumer_count > 0U) ? consumer_count
                                               : pipeline_.EntryFanOut();
    blk->refcount.store(refcount, std::memory_order_release);
    blk->SetState(BlockState::kReady);
    // Pipeline execution (includes all stages: business + NotifyStage).
    // For InterProc: notification is done inside a NotifyStage handler
    // which calls AddRef(num_consumers) before pushing block_id, ensuring
    // the block is not recycled prematurely. See design_job_pool_zh2.md 8.1.
    bool executed = pipeline_.Execute(*this, block_id);
    // If no pipeline configured, notify directly (pure 1:N notification).
    if (!executed) {
      notify_.OnSubmit(block_id, payload_size);
    }
  }

  // --------------------------------------------------------------------------
  // Consumer API
  // --------------------------------------------------------------------------

  /// @brief Get read-only payload pointer.
  const uint8_t* GetReadable(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    return reinterpret_cast<const uint8_t*>(  // NOLINT
               store_.GetBlock(block_id)) +
           Store::kHeaderAlignedSize;
  }

  /// @brief Get payload size for a block.
  uint32_t GetPayloadSize(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    return store_.GetBlock(block_id)->payload_size;
  }

  /// @brief Decrement refcount. Returns true if caller is the last consumer.
  /// When true, the block is automatically recycled to the free list.
  ///
  /// Thread-safety: uses CAS to prevent underflow when ScanTimeout or
  /// ForceCleanup concurrently sets refcount to 0.
  bool Release(uint32_t block_id) noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    DataBlock* blk = store_.GetBlock(block_id);
    uint32_t cur = blk->refcount.load(std::memory_order_acquire);
    while (cur > 0U) {
      if (blk->refcount.compare_exchange_weak(
              cur, cur - 1U,
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        if (cur == 1U) {
          Recycle(block_id);
          return true;
        }
        return false;
      }
    }
    // refcount was already 0 (block reclaimed by ScanTimeout/ForceCleanup)
    return false;
  }

  /// @brief Add references to a block (for fan-out after stage completion).
  void AddRef(uint32_t block_id, uint32_t count) noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    store_.GetBlock(block_id)->refcount.fetch_add(
        count, std::memory_order_release);
  }

  // --------------------------------------------------------------------------
  // Backpressure
  // --------------------------------------------------------------------------

  void SetBackpressureCallback(BackpressureFn cb, void* ctx) noexcept {
    bp_fn_ = cb;
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
  // Notify configuration (ShmNotify mode)
  // --------------------------------------------------------------------------

  Notify& GetNotify() noexcept { return notify_; }

  // --------------------------------------------------------------------------
  // Monitoring
  // --------------------------------------------------------------------------

  /// @brief Scan for timed-out blocks.
  /// Thread-safety: uses CAS to atomically claim refcount before recycling.
  uint32_t ScanTimeout(void (*on_timeout)(uint32_t block_id, void* ctx),
                        void* ctx) noexcept {
    uint32_t count = 0U;
    uint64_t now = SteadyNowUs();
    for (uint32_t i = 0U; i < Store::Capacity(); ++i) {
      DataBlock* blk = store_.GetBlock(i);
      auto st = blk->GetState();
      if (st == BlockState::kProcessing || st == BlockState::kReady) {
        if (blk->deadline_us != 0U && now > blk->deadline_us) {
          // CAS loop: atomically claim refcount (set to 0).
          // Only the thread that wins the CAS sets kTimeout and recycles.
          // This prevents a race where Release recycles (kFree) but
          // ScanTimeout overwrites state to kTimeout afterwards.
          uint32_t cur = blk->refcount.load(std::memory_order_acquire);
          bool claimed = false;
          while (cur > 0U) {
            if (blk->refcount.compare_exchange_weak(
                    cur, 0U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
              claimed = true;
              break;
            }
          }
          if (claimed) {
            blk->SetState(BlockState::kTimeout);
            if (on_timeout != nullptr) {
              on_timeout(i, ctx);
            }
            Recycle(i);
            ++count;
          }
          // If !claimed: Release already recycled this block. Skip.
        }
      }
    }
    return count;
  }

  /// @brief Convenience overload: ScanTimeout with fault reporting.
  uint32_t ScanTimeout() noexcept {
    return ScanTimeout(
        [](uint32_t block_id, void* ctx) {
          auto* self = static_cast<DataDispatcher*>(ctx);
          if (self->fault_reporter_.fn != nullptr) {
            self->fault_reporter_.Report(self->fault_slot_, block_id,
                                         FaultPriority::kHigh);
          }
        },
        this);
  }

  /// @brief Force-release all blocks matching predicate.
  /// Thread-safety: uses CAS to atomically set refcount to 0.
  uint32_t ForceCleanup(bool (*predicate)(uint32_t block_id, void* ctx),
                         void* ctx) noexcept {
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < Store::Capacity(); ++i) {
      DataBlock* blk = store_.GetBlock(i);
      auto st = blk->GetState();
      if (st == BlockState::kProcessing || st == BlockState::kReady) {
        if (predicate != nullptr && predicate(i, ctx)) {
          uint32_t cur = blk->refcount.load(std::memory_order_acquire);
          bool claimed = false;
          while (cur > 0U) {
            if (blk->refcount.compare_exchange_weak(
                    cur, 0U,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
              claimed = true;
              break;
            }
          }
          if (claimed) {
            blk->SetState(BlockState::kError);
            Recycle(i);
            ++count;
          }
        }
      }
    }
    return count;
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  uint32_t FreeBlocks() const noexcept {
    return store_.FreeCount().load(std::memory_order_relaxed);
  }

  uint32_t AllocBlocks() const noexcept {
    return store_.AllocCount().load(std::memory_order_relaxed);
  }

  static constexpr uint32_t Capacity() noexcept {
    return Store::Capacity();
  }

  static constexpr uint32_t PayloadCapacity() noexcept {
    return Store::PayloadCapacity();
  }

  BlockState GetBlockState(uint32_t block_id) const noexcept {
    OSP_ASSERT(block_id < Store::Capacity());
    return store_.GetBlock(block_id)->GetState();
  }

  // --------------------------------------------------------------------------
  // Pipeline-compatible interface (called by Pipeline::ExecuteStage)
  // --------------------------------------------------------------------------

  /// @brief Alias for GetReadable (Pipeline uses GetPayloadReadOnly).
  const uint8_t* GetPayloadReadOnly(uint32_t id) const noexcept {
    return GetReadable(id);
  }

  // --------------------------------------------------------------------------
  // Access to internals (for advanced use / testing)
  // --------------------------------------------------------------------------

  Store& GetStore() noexcept { return store_; }
  const Store& GetStore() const noexcept { return store_; }
  PipelineType& GetPipeline() noexcept { return pipeline_; }
  const PipelineType& GetPipeline() const noexcept { return pipeline_; }
  const Config& GetConfig() const noexcept { return cfg_; }

  // --------------------------------------------------------------------------
  // Consumer tracking (ShmStore only -- compile-time SFINAE guard)
  //
  // These methods manage per-consumer slots in shared memory for:
  //   - Process registration/unregistration
  //   - Heartbeat updates
  //   - Block holding bitmap tracking
  //   - Crash recovery (cleanup orphaned blocks)
  //
  // For InProcStore, these methods are not available (compile error).
  // --------------------------------------------------------------------------

  /// @brief Register a consumer process. Returns slot index or -1 if full.
  /// @note ShmStore only. Call from consumer process after Attach.
  template <typename S = Store>
  auto RegisterConsumer(uint32_t pid) noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), int32_t()) {
    uint32_t max_c = store_.MaxConsumers();
    for (uint32_t i = 0U; i < max_c; ++i) {
      detail::ConsumerSlot* slot = store_.GetConsumerSlot(i);
      uint32_t expected = 0U;
      if (slot->active.compare_exchange_strong(
              expected, 1U, std::memory_order_acq_rel)) {
        slot->pid.store(pid, std::memory_order_relaxed);
        slot->heartbeat_us.store(SteadyNowUs(), std::memory_order_relaxed);
        slot->holding_mask.store(0U, std::memory_order_relaxed);
        return static_cast<int32_t>(i);
      }
    }
    return -1;  // All slots full
  }

  /// @brief Unregister a consumer (normal shutdown).
  /// Clears holding_mask and marks slot inactive.
  template <typename S = Store>
  auto UnregisterConsumer(int32_t slot_id) noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), void()) {
    if (slot_id < 0 ||
        static_cast<uint32_t>(slot_id) >= store_.MaxConsumers()) {
      return;
    }
    detail::ConsumerSlot* slot =
        store_.GetConsumerSlot(static_cast<uint32_t>(slot_id));
    slot->holding_mask.store(0U, std::memory_order_relaxed);
    slot->pid.store(0U, std::memory_order_relaxed);
    slot->heartbeat_us.store(0U, std::memory_order_relaxed);
    slot->active.store(0U, std::memory_order_release);
  }

  /// @brief Update consumer heartbeat timestamp.
  template <typename S = Store>
  auto ConsumerHeartbeat(int32_t slot_id) noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), void()) {
    if (slot_id < 0 ||
        static_cast<uint32_t>(slot_id) >= store_.MaxConsumers()) {
      return;
    }
    store_.GetConsumerSlot(static_cast<uint32_t>(slot_id))
        ->heartbeat_us.store(SteadyNowUs(), std::memory_order_relaxed);
  }

  /// @brief Mark a block as held by consumer (set bit in holding_mask).
  /// Call after GetReadable, before processing.
  template <typename S = Store>
  auto TrackBlockHold(int32_t consumer_id, uint32_t block_id) noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), void()) {
    if (consumer_id < 0 ||
        static_cast<uint32_t>(consumer_id) >= store_.MaxConsumers()) {
      return;
    }
    OSP_ASSERT(block_id < Store::Capacity());
    detail::ConsumerSlot* slot =
        store_.GetConsumerSlot(static_cast<uint32_t>(consumer_id));
    uint64_t bit = static_cast<uint64_t>(1U) << block_id;
    slot->holding_mask.fetch_or(bit, std::memory_order_relaxed);
  }

  /// @brief Clear a block hold bit (after Release).
  template <typename S = Store>
  auto TrackBlockRelease(int32_t consumer_id, uint32_t block_id) noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), void()) {
    if (consumer_id < 0 ||
        static_cast<uint32_t>(consumer_id) >= store_.MaxConsumers()) {
      return;
    }
    OSP_ASSERT(block_id < Store::Capacity());
    detail::ConsumerSlot* slot =
        store_.GetConsumerSlot(static_cast<uint32_t>(consumer_id));
    uint64_t bit = static_cast<uint64_t>(1U) << block_id;
    slot->holding_mask.fetch_and(~bit, std::memory_order_relaxed);
  }

  /// @brief Cleanup blocks held by dead/inactive consumers.
  /// For each slot with active==0 but non-zero holding_mask:
  ///   atomically take the mask, then CAS-release each held block.
  /// @return Number of blocks reclaimed.
  template <typename S = Store>
  auto CleanupDeadConsumers() noexcept
      -> decltype(std::declval<S>().GetConsumerSlot(0U), uint32_t()) {
    uint32_t reclaimed = 0U;
    uint32_t max_c = store_.MaxConsumers();
    for (uint32_t i = 0U; i < max_c; ++i) {
      detail::ConsumerSlot* slot = store_.GetConsumerSlot(i);
      // Only cleanup inactive consumers with non-zero holding_mask
      if (slot->active.load(std::memory_order_acquire) != 0U) {
        continue;
      }
      uint64_t mask = slot->holding_mask.exchange(
          0U, std::memory_order_acq_rel);
      if (mask == 0U) continue;

      // Release each held block
      for (uint32_t b = 0U; b < Store::Capacity(); ++b) {
        if ((mask & (static_cast<uint64_t>(1U) << b)) == 0U) continue;
        DataBlock* blk = store_.GetBlock(b);
        // CAS loop: decrement refcount for this consumer
        uint32_t cur = blk->refcount.load(std::memory_order_acquire);
        while (cur > 0U) {
          if (blk->refcount.compare_exchange_weak(
                  cur, cur - 1U,
                  std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            if (cur == 1U) {
              // Last consumer: recycle the block
              Recycle(b);
            }
            ++reclaimed;
            break;
          }
        }
      }
    }
    return reclaimed;
  }

 private:
  void Recycle(uint32_t block_id) noexcept {
    DataBlock* blk = store_.GetBlock(block_id);
    blk->SetState(BlockState::kFree);
    store_.AllocCount().fetch_sub(1U, std::memory_order_relaxed);
    // CAS push to free list head
    uint32_t head = store_.FreeHead().load(std::memory_order_acquire);
    do {
      blk->next_free = head;
    } while (!store_.FreeHead().compare_exchange_weak(
        head, block_id,
        std::memory_order_acq_rel,
        std::memory_order_acquire));
    store_.FreeCount().fetch_add(1U, std::memory_order_relaxed);
  }

  Store store_;
  Notify notify_;
  PipelineType pipeline_;
  Config cfg_;

  // Fault
  FaultReporter fault_reporter_;
  uint16_t fault_slot_{0U};

  // Backpressure
  BackpressureFn bp_fn_{nullptr};
  void* bp_ctx_{nullptr};
};

// ============================================================================
// Convenience aliases to reduce template noise
// ============================================================================

/// @brief InProc DataDispatcher: embedded storage + synchronous pipeline.
/// Usage: InProcDispatcher<BlockSize, MaxBlocks>
template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
using InProcDispatcher = DataDispatcher<InProcStore<BlockSize, MaxBlocks>,
                                        DirectNotify, MaxStages, MaxEdges>;

/// @brief InterProc DataDispatcher: shared memory storage + ShmNotify.
/// Usage: ShmDispatcher<BlockSize, MaxBlocks>
template <uint32_t BlockSize, uint32_t MaxBlocks,
          uint32_t MaxStages = OSP_JOB_MAX_STAGES,
          uint32_t MaxEdges = OSP_JOB_MAX_EDGES>
using ShmDispatcher = DataDispatcher<ShmStore<BlockSize, MaxBlocks>,
                                      ShmNotify, MaxStages, MaxEdges>;

}  // namespace osp

#endif  // OSP_DATA_DISPATCHER_HPP_
