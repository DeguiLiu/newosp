/**
 * @file mem_pool.hpp
 * @brief Fixed-block memory pool with embedded free list.
 *
 * Provides FixedPool (raw block allocation) and ObjectPool (typed allocation
 * with placement new). All storage is inline -- zero heap allocation.
 * Thread-safe via std::mutex. Compatible with -fno-exceptions -fno-rtti.
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

#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace osp {

// ============================================================================
// Detail Constants
// ============================================================================

namespace detail {

/// Sentinel value for end of embedded free list.
static constexpr uint32_t kInvalidIndex = UINT32_MAX;

}  // namespace detail

// ============================================================================
// FixedPool<BlockSize, MaxBlocks>
//
// Compile-time sized memory pool using an embedded free list.
// Each free block stores the index of the next free block in its first
// sizeof(uint32_t) bytes, providing O(1) allocation and deallocation.
//
// Thread-safe via std::mutex. Zero heap allocation -- all storage is inline.
//
// @tparam BlockSize  Size of each block in bytes (>= sizeof(uint32_t))
// @tparam MaxBlocks  Maximum number of blocks in the pool
// ============================================================================

template <uint32_t BlockSize, uint32_t MaxBlocks>
class FixedPool {
  static_assert(BlockSize >= sizeof(uint32_t),
                "BlockSize must be >= sizeof(uint32_t)");
  static_assert(MaxBlocks > 0, "MaxBlocks must be > 0");
  static_assert(MaxBlocks < detail::kInvalidIndex,
                "MaxBlocks must be < UINT32_MAX");

 public:
  /// @brief Construct pool and initialize embedded free list.
  FixedPool() : free_head_(0), used_count_(0) {
    // Build the embedded free list: block[i].next = i + 1
    for (uint32_t i = 0; i < MaxBlocks - 1; ++i) {
      StoreIndex(i, i + 1);
    }
    StoreIndex(MaxBlocks - 1, detail::kInvalidIndex);
    // Initialize allocated tracking array
    for (uint32_t i = 0; i < MaxBlocks; ++i) {
      allocated_[i] = false;
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
  void* Allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_head_ == detail::kInvalidIndex) {
      return nullptr;
    }
    uint32_t idx = free_head_;
    free_head_ = LoadIndex(idx);
    ++used_count_;
    allocated_[idx] = true;
    return BlockPtr(idx);
  }

  /// @brief Allocate a block from the pool (checked version).
  /// @return expected containing a pointer on success, or MemPoolError on
  ///         failure.
  expected<void*, MemPoolError> AllocateChecked() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_head_ == detail::kInvalidIndex) {
      return expected<void*, MemPoolError>::error(
          MemPoolError::kPoolExhausted);
    }
    uint32_t idx = free_head_;
    free_head_ = LoadIndex(idx);
    ++used_count_;
    allocated_[idx] = true;
    return expected<void*, MemPoolError>::success(BlockPtr(idx));
  }

  // --------------------------------------------------------------------------
  // Deallocation
  // --------------------------------------------------------------------------

  /// @brief Free a previously allocated block.
  ///
  /// The caller must ensure @p ptr was returned by Allocate() or
  /// AllocateChecked() on this pool instance.
  void Free(void* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    OSP_ASSERT(ptr != nullptr);
    OSP_ASSERT(OwnsPointerUnlocked(ptr));
    uint32_t idx = PtrToIndex(ptr);
    OSP_ASSERT(IsAllocatedUnlocked(idx));
    StoreIndex(idx, free_head_);
    free_head_ = idx;
    --used_count_;
    allocated_[idx] = false;
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /// @brief Check if a pointer belongs to this pool's address range and is
  ///        block-aligned.
  /// @return true for both allocated and free blocks within the pool.
  bool OwnsPointer(const void* ptr) const {
    return OwnsPointerUnlocked(ptr);
  }

  /// @brief Number of free blocks available.
  uint32_t FreeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return MaxBlocks - used_count_;
  }

  /// @brief Number of currently allocated blocks.
  uint32_t UsedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_count_;
  }

  /// @brief Total pool capacity (compile-time constant).
  static constexpr uint32_t Capacity() { return MaxBlocks; }

  /// @brief User-specified block size.
  static constexpr uint32_t BlockSizeValue() { return BlockSize; }

  /// @brief Actual stride between blocks (aligned).
  static constexpr size_t AlignedBlockSize() { return kAlignedBlockSize; }

  /// @brief Get pointer to block at given index.
  void* BlockPtr(uint32_t idx) {
    return &storage_[idx * kAlignedBlockSize];
  }

  /// @brief Convert a pointer back to a block index.
  uint32_t PtrToIndex(const void* ptr) const {
    auto offset = static_cast<size_t>(
        static_cast<const uint8_t*>(ptr) - storage_);
    return static_cast<uint32_t>(offset / kAlignedBlockSize);
  }

  /// @brief Check if a block index is currently allocated.
  bool IsAllocated(const void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::printf(
        "[%s] capacity=%u used=%u free=%u block_size=%u aligned_size=%zu\n",
        label, MaxBlocks, used_count_, MaxBlocks - used_count_, BlockSize,
        kAlignedBlockSize);
  }

 private:
  // Round up BlockSize to the nearest multiple of alignof(max_align_t).
  static constexpr size_t kAlignedBlockSize =
      (BlockSize + alignof(std::max_align_t) - 1) &
      ~(alignof(std::max_align_t) - 1);

  // Inline storage -- zero heap allocation.
  alignas(std::max_align_t) uint8_t storage_[kAlignedBlockSize * MaxBlocks];

  mutable std::mutex mutex_;
  uint32_t free_head_;
  uint32_t used_count_;
  bool allocated_[MaxBlocks];

  /// @brief Check if a block index is currently allocated (unlocked version).
  bool IsAllocatedUnlocked(uint32_t idx) const {
    return allocated_[idx];
  }

  /// @brief Store next-free index into a block (strict aliasing safe).
  void StoreIndex(uint32_t block_idx, uint32_t next_idx) {
    std::memcpy(&storage_[block_idx * kAlignedBlockSize], &next_idx,
                sizeof(uint32_t));
  }

  /// @brief Load next-free index from a block (strict aliasing safe).
  uint32_t LoadIndex(uint32_t block_idx) const {
    uint32_t idx;
    std::memcpy(&idx, &storage_[block_idx * kAlignedBlockSize],
                sizeof(uint32_t));
    return idx;
  }

  /// @brief Pointer ownership check without locking (for internal use and
  ///        const-correct public API).
  bool OwnsPointerUnlocked(const void* ptr) const {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
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
  static constexpr uint32_t kBlockSize = static_cast<uint32_t>(
      sizeof(T) > sizeof(uint32_t) ? sizeof(T) : sizeof(uint32_t));

 public:
  ObjectPool() {
    for (uint32_t i = 0; i < MaxObjects; ++i) {
      alive_[i] = false;
    }
  }

  ~ObjectPool() {
    // Destroy all alive objects
    for (uint32_t i = 0; i < MaxObjects; ++i) {
      if (alive_[i]) {
        void* ptr = pool_.BlockPtr(i);
        T* obj = static_cast<T*>(ptr);
        obj->~T();
        alive_[i] = false;
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
    alive_[idx] = true;
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
    alive_[idx] = true;
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
    OSP_ASSERT(alive_[idx]);
    obj->~T();
    alive_[idx] = false;
    pool_.Free(obj);
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /// @brief Check if a pointer belongs to this pool.
  bool OwnsPointer(const T* obj) const {
    return pool_.OwnsPointer(obj);
  }

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
  void DumpState(const char* label = "ObjectPool") const {
    pool_.DumpState(label);
  }

 private:
  FixedPool<kBlockSize, MaxObjects> pool_;
  bool alive_[MaxObjects];
};

}  // namespace osp

#endif  // OSP_MEM_POOL_HPP_
