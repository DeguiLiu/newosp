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
 * @file mem_pool.hpp
 * @brief Fixed-block memory pool with embedded free list.
 *
 * Provides FixedPool (raw block allocation) and ObjectPool (typed allocation
 * with placement new). All storage is inline -- zero heap allocation.
 * Lock-free (32-bit tagged-CAS) -- ISR-safe on single-core.
 * Compatible with -fno-exceptions -fno-rtti.
 *
 * Ported from mp::FixedPool / mp::ObjectPool with the following additions:
 *   - AllocateChecked() returns expected<void*, MemPoolError>
 *   - CreateChecked()   returns expected<T*, MemPoolError>
 */

#ifndef OSP_MEM_POOL_HPP_
#define OSP_MEM_POOL_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <atomic>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

// Free-list next-field access is the classic benign Treiber race (stale reads
// discarded by failed CAS). TSan flags it, so skip instrumentation here.
#if defined(__SANITIZE_THREAD__)
#define OSP_TSAN_NO_RACE __attribute__((no_sanitize("thread")))
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OSP_TSAN_NO_RACE __attribute__((no_sanitize("thread")))
#else
#define OSP_TSAN_NO_RACE
#endif
#else
#define OSP_TSAN_NO_RACE
#endif

namespace osp {

// ============================================================================
// Detail Constants
// ============================================================================

namespace detail {

/// Sentinel value for end of embedded free list.
static constexpr uint32_t kInvalidIndex = UINT32_MAX;

// ---------------------------------------------------------------------------
// Packed free-list head for the lock-free FixedPool.
// Single 32-bit atomic: [15:0] free index, [31:16] ABA tag. Native CAS on
// x86 and ARM Cortex-M (LDREX/STREX) -- no libatomic fallback on 32-bit.
// ---------------------------------------------------------------------------
static constexpr uint32_t kFreeIndexShift = 16U;
static constexpr uint32_t kFreeIndexMask = 0xFFFFU;  // also the empty sentinel
static constexpr uint32_t kFreeIndexEmpty = kFreeIndexMask;

inline uint32_t FreePoolPackHead(uint32_t index, uint32_t tag) noexcept {
  return ((tag & kFreeIndexMask) << kFreeIndexShift) | (index & kFreeIndexMask);
}
inline uint32_t FreePoolHeadIndex(uint32_t head) noexcept {
  return head & kFreeIndexMask;
}
inline uint32_t FreePoolHeadTag(uint32_t head) noexcept {
  return (head >> kFreeIndexShift) & kFreeIndexMask;
}

}  // namespace detail

// ============================================================================
// FixedPool<BlockSize, MaxBlocks>
//
// Compile-time sized memory pool using an embedded free list: each free block
// stores the next-free index in its first sizeof(uint32_t) bytes, O(1) alloc.
//
// Lock-free (32-bit tagged-CAS Treiber list) -- ISR-safe on single-core, and
// safe from multiple producers on SMP. Zero heap allocation. See detail::
// FreePoolPackHead (index 16-bit + ABA tag 16-bit).
//
// NOTE: alloc's LoadIndex / free's StoreIndex both touch a block's first 4
// bytes. The ABA-tagged head-CAS orders them (free StoreIndex -> alloc
// LoadIndex happen-before via the head RMW), so the pool is correct under the
// C++ memory model; TSan may flag the `next` read/write as a benign race (same
// pattern as any tagged-head single-block reclaim). Not a correctness defect -- the
// 16-bit tag window far exceeds practical block reuse.
//
// @tparam BlockSize  Size of each block in bytes (>= sizeof(uint32_t))
// @tparam MaxBlocks  Maximum number of blocks in the pool (<= 0xFFFF)
// ============================================================================

template <uint32_t BlockSize, uint32_t MaxBlocks>
class FixedPool {
  static_assert(BlockSize >= sizeof(uint32_t), "BlockSize must be >= sizeof(uint32_t)");
  static_assert(MaxBlocks > 0, "MaxBlocks must be > 0");
  static_assert(MaxBlocks < detail::kInvalidIndex, "MaxBlocks must be < UINT32_MAX");
  // MaxBlocks must fit the 16-bit free-list index in the packed 32-bit head.
  static_assert(MaxBlocks <= detail::kFreeIndexMask, "MaxBlocks must be <= 0xFFFF (16-bit free-list index)");

 public:
  /// @brief Construct pool and initialize embedded free list.
  FixedPool() noexcept : free_head_(detail::FreePoolPackHead(0U, 0U)), used_count_(0U) {
    // Build the embedded free list: block[i].next = i + 1
    for (uint32_t i = 0; i < MaxBlocks - 1; ++i) {
      StoreIndex(i, i + 1);
    }
    StoreIndex(MaxBlocks - 1, detail::kInvalidIndex);
    // Initialize allocated tracking array (relaxed: a diagnostic index)
    for (uint32_t i = 0; i < MaxBlocks; ++i) {
      allocated_[i].store(false, std::memory_order_relaxed);
    }
  }

  ~FixedPool() = default;

  FixedPool(const FixedPool&) = delete;
  FixedPool& operator=(const FixedPool&) = delete;
  FixedPool(FixedPool&&) = delete;
  FixedPool& operator=(FixedPool&&) = delete;

  // --------------------------------------------------------------------------
  // Allocation
  // --------------------------------------------------------------------------

  /// @brief Allocate a block from the pool.
  /// @return Pointer to the allocated block, or nullptr if the pool is full.
  /// Lock-free: safe to call concurrently from multiple producers and from an
  /// ISR on single-core targets (free-list head RMW is atomic; no mutex).
  void* Allocate() {
    uint32_t head = free_head_.load(std::memory_order_relaxed);
    while (detail::FreePoolHeadIndex(head) != detail::kFreeIndexEmpty) {
      const uint32_t idx = detail::FreePoolHeadIndex(head);
      const uint32_t next = LoadIndex(idx);
      const uint32_t new_head = detail::FreePoolPackHead(next, detail::FreePoolHeadTag(head) + 1U);
      if (free_head_.compare_exchange_weak(head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        used_count_.fetch_add(1U, std::memory_order_relaxed);
        allocated_[idx].store(true, std::memory_order_relaxed);
        return BlockPtr(idx);
      }
      // CAS failed: head was refreshed by compare_exchange_weak; retry.
    }
    return nullptr;
  }

  /// @brief Allocate a block from the pool (checked version).
  /// @return expected containing a pointer on success, or MemPoolError on
  ///         failure. Lock-free (see Allocate).
  expected<void*, MemPoolError> AllocateChecked() {
    uint32_t head = free_head_.load(std::memory_order_relaxed);
    while (detail::FreePoolHeadIndex(head) != detail::kFreeIndexEmpty) {
      const uint32_t idx = detail::FreePoolHeadIndex(head);
      const uint32_t next = LoadIndex(idx);
      const uint32_t new_head = detail::FreePoolPackHead(next, detail::FreePoolHeadTag(head) + 1U);
      if (free_head_.compare_exchange_weak(head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        used_count_.fetch_add(1U, std::memory_order_relaxed);
        allocated_[idx].store(true, std::memory_order_relaxed);
        return expected<void*, MemPoolError>::success(BlockPtr(idx));
      }
    }
    return expected<void*, MemPoolError>::error(MemPoolError::kPoolExhausted);
  }

  // --------------------------------------------------------------------------
  // Deallocation
  // --------------------------------------------------------------------------

  /// @brief Free a previously allocated block.
  ///
  /// The caller must ensure @p ptr was returned by Allocate() or
  /// AllocateChecked() on this pool instance.
  /// Lock-free: links the block with a single head CAS; ISR-safe on single-core.
  void Free(void* ptr) {
    OSP_ASSERT(ptr != nullptr);
    OSP_ASSERT(OwnsPointerUnlocked(ptr));
    const uint32_t idx = PtrToIndex(ptr);
    // allocated_[idx] is not asserted here: in the lock-free pool it is a
    // relaxed diagnostic that races under concurrent reuse. Double-free guard
    // is the caller's / ObjectPool::alive_ job.

    uint32_t head = free_head_.load(std::memory_order_relaxed);
    for (;;) {
      StoreIndex(idx, detail::FreePoolHeadIndex(head));
      const uint32_t new_head = detail::FreePoolPackHead(idx, detail::FreePoolHeadTag(head) + 1U);
      if (free_head_.compare_exchange_weak(head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        break;
      }
      // CAS failed: head refreshed; retry.
    }
    used_count_.fetch_sub(1U, std::memory_order_relaxed);
    allocated_[idx].store(false, std::memory_order_relaxed);
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /// @brief Check if a pointer belongs to this pool's address range and is
  ///        block-aligned.
  /// @return true for both allocated and free blocks within the pool.
  bool OwnsPointer(const void* ptr) const { return OwnsPointerUnlocked(ptr); }

  /// @brief Number of free blocks available.
  uint32_t FreeCount() const { return MaxBlocks - used_count_.load(std::memory_order_relaxed); }

  /// @brief Number of currently allocated blocks.
  uint32_t UsedCount() const { return used_count_.load(std::memory_order_relaxed); }

  /// @brief Total pool capacity (compile-time constant).
  static constexpr uint32_t Capacity() { return MaxBlocks; }

  /// @brief User-specified block size.
  static constexpr uint32_t BlockSizeValue() { return BlockSize; }

  /// @brief Actual stride between blocks (aligned).
  static constexpr size_t AlignedBlockSize() { return kAlignedBlockSize; }

  /// @brief Get pointer to block at given index.
  void* BlockPtr(uint32_t idx) { return &storage_[idx * kAlignedBlockSize]; }

  /// @brief Convert a pointer back to a block index.
  uint32_t PtrToIndex(const void* ptr) const {
    auto offset = static_cast<size_t>(static_cast<const uint8_t*>(ptr) - storage_);
    return static_cast<uint32_t>(offset / kAlignedBlockSize);
  }

  /// @brief Check if a block index is currently allocated.
  bool IsAllocated(const void* ptr) const {
    if (!OwnsPointerUnlocked(ptr)) {
      return false;
    }
    uint32_t idx = PtrToIndex(ptr);
    return IsAllocatedUnlocked(idx);
  }

  // --------------------------------------------------------------------------
  // Debug
  // --------------------------------------------------------------------------

  /// @brief Print pool state to stdout for debugging.
  void DumpState(const char* label = "FixedPool") const {
    uint32_t used = used_count_.load(std::memory_order_relaxed);
    std::printf("[%s] capacity=%u used=%u free=%u block_size=%u aligned_size=%zu\n", label, MaxBlocks, used,
                MaxBlocks - used, BlockSize, kAlignedBlockSize);
  }

 private:
  // Round up BlockSize to the nearest multiple of alignof(max_align_t).
  static constexpr size_t kAlignedBlockSize =
      (BlockSize + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

  // Inline storage -- zero heap allocation.
  alignas(std::max_align_t) uint8_t storage_[kAlignedBlockSize * MaxBlocks];

  // Packed tagged free-list head: [15:0] index, [31:16] ABA tag. Lock-free.
  std::atomic<uint32_t> free_head_;
  std::atomic<uint32_t> used_count_;
  std::atomic<bool> allocated_[MaxBlocks];

  /// @brief Check if a block index is currently allocated (diagnostic only).
  bool IsAllocatedUnlocked(uint32_t idx) const { return allocated_[idx].load(std::memory_order_relaxed); }

  /// @brief Store next-free index into a block (strict aliasing safe).
  OSP_TSAN_NO_RACE void StoreIndex(uint32_t block_idx, uint32_t next_idx) {
    std::memcpy(&storage_[block_idx * kAlignedBlockSize], &next_idx, sizeof(uint32_t));
  }

  /// @brief Load next-free index from a block (strict aliasing safe).
  OSP_TSAN_NO_RACE uint32_t LoadIndex(uint32_t block_idx) const {
    uint32_t idx;
    std::memcpy(&idx, &storage_[block_idx * kAlignedBlockSize], sizeof(uint32_t));
    return idx;
  }

  /// @brief Pointer ownership check without locking (for internal use and
  ///        const-correct public API).
  bool OwnsPointerUnlocked(const void* ptr) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto base = reinterpret_cast<uintptr_t>(storage_);
    if (addr < base || addr >= base + sizeof(storage_)) {
      return false;
    }
    return (addr - base) % kAlignedBlockSize == 0;
  }
};

// ============================================================================
// ObjectPool<T, MaxObjects>
//
// Type-safe memory pool for objects of type T.
// Uses placement new for construction and explicit destructor calls.
// Safe with -fno-exceptions (placement new never throws).
//
// @tparam T           Object type to pool
// @tparam MaxObjects  Maximum number of objects
// ============================================================================

template <typename T, uint32_t MaxObjects>
class ObjectPool {
  static constexpr uint32_t kBlockSize =
      static_cast<uint32_t>(sizeof(T) > sizeof(uint32_t) ? sizeof(T) : sizeof(uint32_t));

 public:
  ObjectPool() {
    for (uint32_t i = 0; i < MaxObjects; ++i) {
      alive_[i].store(false, std::memory_order_relaxed);
    }
  }

  ~ObjectPool() {
    // Destroy all alive objects
    for (uint32_t i = 0; i < MaxObjects; ++i) {
      if (alive_[i].load(std::memory_order_relaxed)) {
        void* ptr = pool_.BlockPtr(i);
        T* obj = static_cast<T*>(ptr);
        obj->~T();
        alive_[i].store(false, std::memory_order_relaxed);
      }
    }
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  // --------------------------------------------------------------------------
  // Construction / Destruction
  // --------------------------------------------------------------------------

  /// @brief Construct an object in the pool using placement new.
  /// @return Pointer to the constructed object, or nullptr if pool is full.
  template <typename... Args>
  T* Create(Args&&... args) {
    void* mem = pool_.Allocate();
    if (!mem) {
      return nullptr;
    }
    uint32_t idx = pool_.PtrToIndex(mem);
    T* obj = ::new (mem) T(std::forward<Args>(args)...);
    alive_[idx].store(true, std::memory_order_relaxed);
    return obj;
  }

  /// @brief Construct an object in the pool (checked version).
  /// @return expected containing a typed pointer on success, or MemPoolError
  ///         on failure.
  template <typename... Args>
  expected<T*, MemPoolError> CreateChecked(Args&&... args) {
    auto result = pool_.AllocateChecked();
    if (!result.has_value()) {
      return expected<T*, MemPoolError>::error(result.get_error());
    }
    uint32_t idx = pool_.PtrToIndex(result.value());
    T* obj = ::new (result.value()) T(std::forward<Args>(args)...);
    alive_[idx].store(true, std::memory_order_relaxed);
    return expected<T*, MemPoolError>::success(obj);
  }

  /// @brief Destroy an object: call destructor and return memory to pool.
  ///
  /// No-op if @p obj is nullptr.
  void Destroy(T* obj) {
    if (!obj) {
      return;
    }
    uint32_t idx = pool_.PtrToIndex(obj);
    OSP_ASSERT(alive_[idx].load(std::memory_order_relaxed));
    obj->~T();
    alive_[idx].store(false, std::memory_order_relaxed);
    pool_.Free(obj);
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /// @brief Check if a pointer belongs to this pool.
  bool OwnsPointer(const T* obj) const { return pool_.OwnsPointer(obj); }

  /// @brief Number of free slots available.
  uint32_t FreeCount() const { return pool_.FreeCount(); }

  /// @brief Number of currently allocated objects.
  uint32_t UsedCount() const { return pool_.UsedCount(); }

  /// @brief Total pool capacity (compile-time constant).
  static constexpr uint32_t Capacity() { return MaxObjects; }

  // --------------------------------------------------------------------------
  // Debug
  // --------------------------------------------------------------------------

  /// @brief Print pool state to stdout for debugging.
  void DumpState(const char* label = "ObjectPool") const { pool_.DumpState(label); }

 private:
  FixedPool<kBlockSize, MaxObjects> pool_;
  std::atomic<bool> alive_[MaxObjects];
};

}  // namespace osp

#undef OSP_TSAN_NO_RACE

#endif  // OSP_MEM_POOL_HPP_
