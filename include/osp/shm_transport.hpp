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
 * @file shm_transport.hpp
 * @brief Shared memory IPC transport with lock-free ring buffers.
 *
 * Provides POSIX shared memory RAII wrappers, lock-free MPSC slot-based ring
 * buffer, SPSC byte-stream ring buffer for large payloads (LiDAR, video),
 * and named channel abstractions with futex-based notification.
 * Linux-only, header-only, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SHM_TRANSPORT_HPP_
#define OSP_SHM_TRANSPORT_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <cstring>

#include <atomic>
#include <utility>

#if OSP_HAS_NETWORK

#if defined(OSP_PLATFORM_LINUX)
#include <chrono>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#endif

namespace osp {

// ============================================================================
// Compile-time Configuration
// ============================================================================

#ifndef OSP_SHM_SLOT_SIZE
#define OSP_SHM_SLOT_SIZE 4096
#endif

#ifndef OSP_SHM_SLOT_COUNT
#define OSP_SHM_SLOT_COUNT 256
#endif

#ifndef OSP_SHM_CHANNEL_NAME_MAX
#define OSP_SHM_CHANNEL_NAME_MAX 64
#endif

#ifndef OSP_SHM_BYTE_RING_CAPACITY
#define OSP_SHM_BYTE_RING_CAPACITY (1024 * 1024)  // 1 MB default
#endif

// ============================================================================
// ShmError
// ============================================================================

enum class ShmError : uint8_t { kCreateFailed = 0, kOpenFailed, kMapFailed, kFull, kEmpty, kTimeout, kClosed };

// Huge pages: reduces TLB misses for large shared memory segments (e.g. video
// frames).  Requires system-level huge page reservation:
//   echo 64 > /proc/sys/vm/nr_hugepages
// When enabled, mmap uses MAP_HUGETLB.  Falls back to normal pages on failure.
#ifdef OSP_SHM_HUGE_PAGES
static constexpr int kShmMmapFlags = MAP_SHARED | MAP_HUGETLB;
#else
static constexpr int kShmMmapFlags = MAP_SHARED;
#endif

#if defined(OSP_PLATFORM_LINUX)

// ============================================================================
// SharedMemorySegment - POSIX shm_open/mmap RAII wrapper
// ============================================================================

/**
 * @brief RAII wrapper for POSIX shared memory segment.
 *
 * Manages lifecycle of shm_open/mmap/munmap/shm_unlink.
 * Movable but not copyable.
 */
class SharedMemorySegment final {
  static constexpr mode_t kShmPermissions = 0600;  ///< Owner read/write

 public:
  SharedMemorySegment() noexcept : fd_(-1), addr_(nullptr), size_(0), name_{} {}

  ~SharedMemorySegment() {
    if (addr_ != nullptr && addr_ != MAP_FAILED) {
      ::munmap(addr_, size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  // Non-copyable
  SharedMemorySegment(const SharedMemorySegment&) = delete;
  SharedMemorySegment& operator=(const SharedMemorySegment&) = delete;

  // Movable
  SharedMemorySegment(SharedMemorySegment&& other) noexcept
      : fd_(other.fd_), addr_(other.addr_), size_(other.size_), name_(other.name_) {
    other.fd_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.name_.clear();
  }

  SharedMemorySegment& operator=(SharedMemorySegment&& other) noexcept {
    if (this != &other) {
      // Clean up current resources
      if (addr_ != nullptr && addr_ != MAP_FAILED) {
        ::munmap(addr_, size_);
      }
      if (fd_ >= 0) {
        ::close(fd_);
      }

      // Move from other
      fd_ = other.fd_;
      addr_ = other.addr_;
      size_ = other.size_;
      name_ = other.name_;

      other.fd_ = -1;
      other.addr_ = nullptr;
      other.size_ = 0;
      other.name_.clear();
    }
    return *this;
  }

  /**
   * @brief Create a new shared memory segment.
   * @param name Segment name (will be prefixed with /osp_shm_).
   * @param size Size in bytes (will be page-aligned).
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> Create(const char* name, uint32_t size) noexcept {
    SharedMemorySegment seg;
    BuildName(seg.name_, name);

    seg.fd_ = ::shm_open(seg.name_.c_str(), O_CREAT | O_RDWR | O_EXCL, kShmPermissions);
    if (seg.fd_ < 0) {
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kCreateFailed);
    }

    uint32_t aligned_size = PageAlign(size);
    if (::ftruncate(seg.fd_, aligned_size) != 0) {
      ::close(seg.fd_);
      ::shm_unlink(seg.name_.c_str());
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kCreateFailed);
    }

    seg.addr_ = MmapWithFallback(seg.fd_, aligned_size);
    if (seg.addr_ == MAP_FAILED) {
      ::close(seg.fd_);
      ::shm_unlink(seg.name_.c_str());
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kMapFailed);
    }

    seg.size_ = aligned_size;
    return expected<SharedMemorySegment, ShmError>::success(static_cast<SharedMemorySegment&&>(seg));
  }

  /**
   * @brief Create a shared memory segment, removing any stale one first.
   *
   * Useful when a previous process crashed without calling Unlink().
   * Equivalent to shm_unlink + Create.
   *
   * @param name Segment name (will be prefixed with /osp_shm_).
   * @param size Size in bytes (will be page-aligned).
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> CreateOrReplace(const char* name, uint32_t size) noexcept {
    FixedString<OSP_SHM_CHANNEL_NAME_MAX> full_name;
    BuildName(full_name, name);
    ::shm_unlink(full_name.c_str());
    return Create(name, size);
  }

  /**
   * @brief Open an existing shared memory segment.
   * @param name Segment name (will be prefixed with /osp_shm_).
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> Open(const char* name) noexcept {
    SharedMemorySegment seg;
    BuildName(seg.name_, name);

    seg.fd_ = ::shm_open(seg.name_.c_str(), O_RDWR, kShmPermissions);
    if (seg.fd_ < 0) {
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kOpenFailed);
    }

    struct stat st;
    if (::fstat(seg.fd_, &st) != 0) {
      ::close(seg.fd_);
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kOpenFailed);
    }

    seg.size_ = static_cast<uint32_t>(st.st_size);
    seg.addr_ = MmapWithFallback(seg.fd_, seg.size_);
    if (seg.addr_ == MAP_FAILED) {
      ::close(seg.fd_);
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kMapFailed);
    }

    return expected<SharedMemorySegment, ShmError>::success(static_cast<SharedMemorySegment&&>(seg));
  }

  /**
   * @brief Mark the segment for deletion (shm_unlink).
   * The segment will be removed when all processes close it.
   */
  void Unlink() noexcept {
    if (!name_.empty()) {
      ::shm_unlink(name_.c_str());
    }
  }

  void* Data() noexcept { return addr_; }
  const void* Data() const noexcept { return addr_; }
  uint32_t Size() const noexcept { return size_; }
  const char* Name() const noexcept { return name_.c_str(); }

 private:
  /// @brief Build full shm name: "/osp_shm_" + user name.
  static void BuildName(FixedString<OSP_SHM_CHANNEL_NAME_MAX>& out, const char* name) noexcept {
    char temp[OSP_SHM_CHANNEL_NAME_MAX + 1];
    constexpr uint32_t kPrefixLen = 9;  // strlen("/osp_shm_")
    std::memcpy(temp, "/osp_shm_", kPrefixLen);
    uint32_t pos = kPrefixLen;
    for (uint32_t i = 0; name[i] != '\0' && pos < OSP_SHM_CHANNEL_NAME_MAX; ++i) {
      temp[pos++] = name[i];
    }
    temp[pos] = '\0';
    out.assign(TruncateToCapacity, temp);
  }

  /// @brief mmap with huge page fallback.
  static void* MmapWithFallback(int fd, uint32_t size) noexcept {
    void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, kShmMmapFlags, fd, 0);
#ifdef OSP_SHM_HUGE_PAGES
    if (addr == MAP_FAILED) {
      addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
#endif
    return addr;
  }

  /// @brief Round up to page boundary (4 KB).
  static uint32_t PageAlign(uint32_t size) noexcept {
    return (size + 4095u) & ~4095u;
  }

  int32_t fd_;
  void* addr_;
  uint32_t size_;
  FixedString<OSP_SHM_CHANNEL_NAME_MAX> name_;
};

// ============================================================================
// ShmRingBuffer - Lock-free MPSC ring buffer in shared memory
// ============================================================================

/**
 * @brief Lock-free MPSC ring buffer for shared memory.
 *
 * Uses CAS-based sequence numbers (similar to bus.hpp) to coordinate
 * multiple producers and a single consumer. All state is POD + std::atomic
 * so it can live in shared memory.
 *
 * @tparam SlotSize Maximum size of each message slot.
 * @tparam SlotCount Number of slots (must be power of 2).
 */
template <uint32_t SlotSize = OSP_SHM_SLOT_SIZE, uint32_t SlotCount = OSP_SHM_SLOT_COUNT>
class ShmRingBuffer final {
  static_assert((SlotCount & (SlotCount - 1)) == 0, "SlotCount must be power of 2");

 public:
  static constexpr uint32_t kSlotSize = SlotSize;
  static constexpr uint32_t kSlotCount = SlotCount;
  static constexpr uint32_t kBufferMask = SlotCount - 1;

  /**
   * @brief Initialize the ring buffer at a shared memory address.
   * Must be called by the creator before any other operations.
   * @param shm_addr Pointer to shared memory (must be at least Size() bytes).
   * @return Pointer to the initialized ShmRingBuffer.
   */
  static ShmRingBuffer* InitAt(void* shm_addr) noexcept {
    OSP_ASSERT(shm_addr != nullptr);
    auto* rb = static_cast<ShmRingBuffer*>(shm_addr);

    // Initialize atomics using placement new
    new (&rb->producer_pos_) std::atomic<uint32_t>(0);
    new (&rb->consumer_pos_) std::atomic<uint32_t>(0);

    for (uint32_t i = 0; i < SlotCount; ++i) {
      new (&rb->slots_[i].sequence) std::atomic<uint32_t>(i);
      rb->slots_[i].size = 0;
    }

    return rb;
  }

  /**
   * @brief Attach to an existing ring buffer in shared memory.
   * @param shm_addr Pointer to shared memory containing initialized buffer.
   * @return Pointer to the ShmRingBuffer.
   */
  static ShmRingBuffer* AttachAt(void* shm_addr) noexcept {
    OSP_ASSERT(shm_addr != nullptr);
    return static_cast<ShmRingBuffer*>(shm_addr);
  }

  /**
   * @brief Calculate required size for the ring buffer structure.
   * @return Size in bytes.
   */
  static constexpr uint32_t Size() noexcept { return sizeof(ShmRingBuffer); }

  /**
   * @brief Try to push data into the ring buffer (non-blocking).
   * @param data Pointer to data to copy.
   * @param size Size of data in bytes (must be <= SlotSize).
   * @return true if pushed successfully, false if full or size too large.
   */
  bool TryPush(const void* data, uint32_t size) noexcept {
    if (size > SlotSize) {
      return false;
    }

    // CAS loop to claim a producer slot
    uint32_t prod_pos;
    Slot* target;

    do {
      prod_pos = producer_pos_.load(std::memory_order_relaxed);
      target = &slots_[prod_pos & kBufferMask];

      uint32_t seq = target->sequence.load(std::memory_order_acquire);
      if (seq != prod_pos) {
        // Slot not available
        return false;
      }
    } while (!producer_pos_.compare_exchange_weak(prod_pos, prod_pos + 1, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed));

    // Fill slot
    target->size = size;
    std::memcpy(target->data, data, size);

    // ARM memory ordering: ensure memcpy completes before sequence store.
    // The release fence guarantees all prior writes (memcpy) are visible
    // to other threads that perform an acquire load of the sequence.
    std::atomic_thread_fence(std::memory_order_release);

    // Publish (make visible to consumer).
    // Release semantics ensure the data writes above are visible on ARM.
    target->sequence.store(prod_pos + 1, std::memory_order_release);

    return true;
  }

  /**
   * @brief Try to pop data from the ring buffer (non-blocking).
   * @param data Buffer to receive data (must be at least SlotSize bytes).
   * @param size Output parameter for actual data size.
   * @return true if popped successfully, false if empty.
   */
  bool TryPop(void* data, uint32_t& size) noexcept {
    uint32_t cons_pos = consumer_pos_.load(std::memory_order_relaxed);
    Slot& slot = slots_[cons_pos & kBufferMask];

    uint32_t expected_seq = cons_pos + 1;
    uint32_t seq = slot.sequence.load(std::memory_order_acquire);

    if (seq != expected_seq) {
      // No data available
      return false;
    }

    // ARM memory ordering: ensure producer's writes are visible.
    // Acquire fence guarantees we see all writes that happened-before
    // the producer's release store to sequence.
    std::atomic_thread_fence(std::memory_order_acquire);

    // Copy data out
    size = slot.size;
    std::memcpy(data, slot.data, size);

    // Release slot back to producers
    slot.sequence.store(cons_pos + SlotCount, std::memory_order_release);
    consumer_pos_.store(cons_pos + 1, std::memory_order_release);

    return true;
  }

  /**
   * @brief Get current depth (approximate).
   * @return Number of pending messages.
   */
  uint32_t Depth() const noexcept {
    uint32_t prod = producer_pos_.load(std::memory_order_acquire);
    uint32_t cons = consumer_pos_.load(std::memory_order_acquire);
    return prod - cons;
  }

 private:
  struct Slot {
    std::atomic<uint32_t> sequence;
    uint32_t size;
    char data[SlotSize];
  };

  static_assert(std::is_standard_layout<Slot>::value, "Slot must be standard layout for shared memory");

  // Cache line aligned to prevent false sharing between producer and consumer
  alignas(64) std::atomic<uint32_t> producer_pos_;
  char pad_[64 - sizeof(std::atomic<uint32_t>)];
  alignas(64) std::atomic<uint32_t> consumer_pos_;
  Slot slots_[SlotCount];

  // Private constructor - use InitAt/AttachAt
  ShmRingBuffer() = default;
};

// ============================================================================
// ShmChannel - Named channel with polling-based notification
// ============================================================================

/**
 * @brief Named shared memory channel with polling-based wait.
 *
 * Combines SharedMemorySegment + ShmRingBuffer for efficient
 * producer-consumer communication across processes. Uses polling
 * for WaitReadable since eventfd cannot be shared across processes.
 */
template <uint32_t SlotSize = OSP_SHM_SLOT_SIZE, uint32_t SlotCount = OSP_SHM_SLOT_COUNT>
class ShmChannel final {
 public:
  using RingBuffer = ShmRingBuffer<SlotSize, SlotCount>;

  ShmChannel() noexcept : ring_buffer_(nullptr), is_writer_(false) {}

  ~ShmChannel() = default;

  // Non-copyable, movable
  ShmChannel(const ShmChannel&) = delete;
  ShmChannel& operator=(const ShmChannel&) = delete;

  ShmChannel(ShmChannel&& other) noexcept
      : shm_segment_(static_cast<SharedMemorySegment&&>(other.shm_segment_)),
        ring_buffer_(other.ring_buffer_),
        is_writer_(other.is_writer_) {
    other.ring_buffer_ = nullptr;
  }

  ShmChannel& operator=(ShmChannel&& other) noexcept {
    if (this != &other) {
      shm_segment_ = static_cast<SharedMemorySegment&&>(other.shm_segment_);
      ring_buffer_ = other.ring_buffer_;
      is_writer_ = other.is_writer_;

      other.ring_buffer_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief Create a writer endpoint for a named channel.
   * @param name Channel name.
   * @return expected with ShmChannel on success.
   */
  static expected<ShmChannel, ShmError> CreateWriter(const char* name) noexcept {
    ShmChannel channel;
    channel.is_writer_ = true;

    uint32_t shm_size = RingBuffer::Size();
    auto result = SharedMemorySegment::Create(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmChannel, ShmError>::error(result.get_error());
    }

    channel.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data());

    return expected<ShmChannel, ShmError>::success(static_cast<ShmChannel&&>(channel));
  }

  /**
   * @brief Create a writer endpoint, removing any stale channel first.
   *
   * Useful when a previous process crashed without calling Unlink().
   *
   * @param name Channel name.
   * @return expected with ShmChannel on success.
   */
  static expected<ShmChannel, ShmError> CreateOrReplaceWriter(const char* name) noexcept {
    ShmChannel channel;
    channel.is_writer_ = true;

    uint32_t shm_size = RingBuffer::Size();
    auto result = SharedMemorySegment::CreateOrReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmChannel, ShmError>::error(result.get_error());
    }

    channel.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data());

    return expected<ShmChannel, ShmError>::success(static_cast<ShmChannel&&>(channel));
  }

  /**
   * @brief Open a reader endpoint for an existing channel.
   * @param name Channel name.
   * @return expected with ShmChannel on success.
   */
  static expected<ShmChannel, ShmError> OpenReader(const char* name) noexcept {
    ShmChannel channel;
    channel.is_writer_ = false;

    auto result = SharedMemorySegment::Open(name);
    if (!result.has_value()) {
      return expected<ShmChannel, ShmError>::error(result.get_error());
    }

    channel.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    channel.ring_buffer_ = RingBuffer::AttachAt(channel.shm_segment_.Data());

    return expected<ShmChannel, ShmError>::success(static_cast<ShmChannel&&>(channel));
  }

  /**
   * @brief Write data to the channel.
   * @param data Pointer to data.
   * @param size Size in bytes.
   * @return expected<void, ShmError> - success or kFull.
   */
  expected<void, ShmError> Write(const void* data, uint32_t size) noexcept {
    OSP_ASSERT(ring_buffer_ != nullptr);
    if (ring_buffer_->TryPush(data, size)) {
      return expected<void, ShmError>::success();
    }
    return expected<void, ShmError>::error(ShmError::kFull);
  }

  /**
   * @brief Read data from the channel.
   * @param data Buffer to receive data (must be at least SlotSize bytes).
   * @param size Output parameter for actual data size.
   * @return expected<void, ShmError> - success or kEmpty.
   */
  expected<void, ShmError> Read(void* data, uint32_t& size) noexcept {
    OSP_ASSERT(ring_buffer_ != nullptr);
    if (ring_buffer_->TryPop(data, size)) {
      return expected<void, ShmError>::success();
    }
    return expected<void, ShmError>::error(ShmError::kEmpty);
  }

  /**
   * @brief Wait for data to become readable (polling with timeout).
   * @param timeout_ms Timeout in milliseconds.
   * @return expected<void, ShmError> - success or kTimeout.
   */
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms) noexcept {
    OSP_ASSERT(ring_buffer_ != nullptr);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // Try immediately first
    if (ring_buffer_->Depth() > 0) {
      return expected<void, ShmError>::success();
    }

    // Poll with exponential backoff
    constexpr uint32_t kInitialSleepUs = 50;
    constexpr uint32_t kMaxSleepUs = 1000;
    uint32_t sleep_us = kInitialSleepUs;

    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));

      if (ring_buffer_->Depth() > 0) {
        return expected<void, ShmError>::success();
      }

      // Exponential backoff
      if (sleep_us < kMaxSleepUs) {
        sleep_us *= 2;
        if (sleep_us > kMaxSleepUs) {
          sleep_us = kMaxSleepUs;
        }
      }
    }

    // Final check at deadline
    if (ring_buffer_->Depth() > 0) {
      return expected<void, ShmError>::success();
    }

    return expected<void, ShmError>::error(ShmError::kTimeout);
  }

  /**
   * @brief Notify waiting readers (no-op in polling implementation).
   * Kept for API compatibility.
   */
  void Notify() noexcept {
    // No-op: polling-based implementation doesn't need explicit notification
  }

  /**
   * @brief Unlink the shared memory segment (writer only).
   */
  void Unlink() noexcept {
    if (is_writer_) {
      shm_segment_.Unlink();
    }
  }

  /**
   * @brief Get current depth of the ring buffer.
   */
  uint32_t Depth() const noexcept { return ring_buffer_ ? ring_buffer_->Depth() : 0; }

 private:
  SharedMemorySegment shm_segment_;
  RingBuffer* ring_buffer_;
  bool is_writer_;
};

// ============================================================================
// ShmSpscByteRing - SPSC byte-stream ring buffer for large payloads
// ============================================================================

/// @brief POD header for SPSC byte ring buffer (16 bytes).
/// Stored at the start of the shared memory region.
struct ShmByteRingHeader {
  uint32_t head;      ///< Producer write position (monotonically increasing)
  uint32_t tail;      ///< Consumer read position (monotonically increasing)
  uint32_t capacity;  ///< Data area size (must be power of 2)
  uint32_t reserved;  ///< Alignment padding
};

static_assert(sizeof(ShmByteRingHeader) == 16, "ShmByteRingHeader must be 16 bytes");

/**
 * @brief SPSC byte-level ring buffer for shared memory IPC.
 *
 * Designed for large variable-length payloads (e.g. LiDAR point clouds,
 * video frames) where fixed-slot MPSC wastes memory. Uses monotonically
 * increasing head/tail indices with power-of-2 bitmask wrap-around.
 *
 * Memory layout:
 *   [0..15]  : ShmByteRingHeader (head, tail, capacity, reserved)
 *   [16..N]  : Data area (circular buffer)
 *
 * Message format: [4-byte LE length][payload]
 *
 * Memory ordering: acquire/release fences (not seq_cst).
 * Thread/process safety: SPSC only (one producer, one consumer).
 */
class ShmSpscByteRing final {
  static constexpr uint32_t kHeaderSize = 16;

 public:
  ShmSpscByteRing() noexcept : header_(nullptr), data_(nullptr), mask_(0) {}

  /**
   * @brief Bind to shared memory as producer (initializes header).
   * @param shm_base Pointer to the start of shared memory.
   * @param total_size Total shared memory size (header + data).
   */
  static ShmSpscByteRing InitAt(void* shm_base, uint32_t total_size) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    OSP_ASSERT(total_size > kHeaderSize);
    ShmSpscByteRing ring;
    ring.header_ = static_cast<ShmByteRingHeader*>(shm_base);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kHeaderSize;
    uint32_t cap = RoundDownPow2(total_size - kHeaderSize);
    ring.header_->head = 0;
    ring.header_->tail = 0;
    ring.header_->capacity = cap;
    ring.header_->reserved = 0;
    std::atomic_thread_fence(std::memory_order_release);
    ring.mask_ = cap - 1;
    return ring;
  }

  /**
   * @brief Bind to shared memory as consumer (reads existing header).
   * @param shm_base Pointer to the start of shared memory.
   */
  static ShmSpscByteRing AttachAt(void* shm_base) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    ShmSpscByteRing ring;
    ring.header_ = static_cast<ShmByteRingHeader*>(shm_base);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kHeaderSize;
    std::atomic_thread_fence(std::memory_order_acquire);
    ring.mask_ = ring.header_->capacity - 1;
    return ring;
  }

  /**
   * @brief Calculate minimum shared memory size for given data capacity.
   * @param data_capacity Desired data area size (will be rounded down to power of 2).
   */
  static constexpr uint32_t RequiredSize(uint32_t data_capacity) noexcept {
    return kHeaderSize + data_capacity;
  }

  // ---- Producer API ----

  /**
   * @brief Write a length-prefixed message: [4B len][payload].
   * @param data Pointer to payload data.
   * @param len Payload length in bytes.
   * @return true if successful, false if not enough space.
   */
  bool Write(const void* data, uint32_t len) noexcept {
    const uint32_t total = len + 4;
    if (WriteableBytes() < total) {
      return false;
    }
    const uint32_t head = header_->head;
    WriteRaw(head, &len, 4);
    WriteRaw(head + 4, data, len);
    std::atomic_thread_fence(std::memory_order_release);
    header_->head = head + total;
    return true;
  }

  /// @brief Available bytes for writing.
  uint32_t WriteableBytes() const noexcept {
    const uint32_t head = header_->head;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t tail = header_->tail;
    return header_->capacity - (head - tail);
  }

  // ---- Consumer API ----

  /**
   * @brief Read one length-prefixed message.
   * @param[out] out Buffer to receive payload.
   * @param max_len Maximum payload size.
   * @return Payload length, or 0 if no data available.
   */
  uint32_t Read(void* out, uint32_t max_len) noexcept {
    const uint32_t tail = header_->tail;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t head = header_->head;
    const uint32_t available = head - tail;
    if (available < 4) {
      return 0;
    }
    uint32_t msg_len = 0;
    ReadRaw(tail, &msg_len, 4);
    if (msg_len == 0 || available < msg_len + 4) {
      return 0;
    }
    if (msg_len > max_len) {
      // Message too large for output buffer; skip it
      std::atomic_thread_fence(std::memory_order_release);
      header_->tail = tail + msg_len + 4;
      return 0;
    }
    ReadRaw(tail + 4, out, msg_len);
    std::atomic_thread_fence(std::memory_order_release);
    header_->tail = tail + msg_len + 4;
    return msg_len;
  }

  /// @brief Available bytes for reading.
  uint32_t ReadableBytes() const noexcept {
    const uint32_t tail = header_->tail;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t head = header_->head;
    return head - tail;
  }

  /// @brief Check if at least one complete message header is available.
  bool HasData() const noexcept { return ReadableBytes() >= 4; }

  /// @brief Data area capacity in bytes.
  uint32_t Capacity() const noexcept { return header_ ? header_->capacity : 0; }

  /// @brief Pointer to the head field (for futex wait/wake).
  uint32_t* HeadPtr() noexcept { return &header_->head; }

 private:
  void WriteRaw(uint32_t pos, const void* src, uint32_t len) noexcept {
    const uint32_t offset = pos & mask_;
    const uint32_t first = header_->capacity - offset;
    if (first >= len) {
      std::memcpy(data_ + offset, src, len);
    } else {
      std::memcpy(data_ + offset, src, first);
      std::memcpy(data_, static_cast<const uint8_t*>(src) + first, len - first);
    }
  }

  void ReadRaw(uint32_t pos, void* dst, uint32_t len) const noexcept {
    const uint32_t offset = pos & mask_;
    const uint32_t first = header_->capacity - offset;
    if (first >= len) {
      std::memcpy(dst, data_ + offset, len);
    } else {
      std::memcpy(dst, data_ + offset, first);
      std::memcpy(static_cast<uint8_t*>(dst) + first, data_, len - first);
    }
  }

  /// @brief Round down to the nearest power of 2.
  static uint32_t RoundDownPow2(uint32_t v) noexcept {
    if (v == 0) return 0;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return (v >> 1) + 1;
  }

  ShmByteRingHeader* header_;
  uint8_t* data_;
  uint32_t mask_;
};

// ============================================================================
// Futex helpers - low-latency wait/wake for shared memory
// ============================================================================

namespace detail {

/// @brief Wait on a futex word until it changes from expected_val or timeout.
/// @return 0 on wake, -1 on timeout/error.
inline int FutexWait(uint32_t* addr, uint32_t expected_val, uint32_t timeout_ms) noexcept {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);  // NOLINT
  return static_cast<int>(
      ::syscall(SYS_futex, addr, FUTEX_WAIT, expected_val, &ts, nullptr, 0));
}

/// @brief Wake one waiter on a futex word.
inline void FutexWake(uint32_t* addr) noexcept {
  (void)::syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

}  // namespace detail

// ============================================================================
// ShmByteChannel - SPSC byte-stream channel with futex notification
// ============================================================================

/**
 * @brief Named SPSC byte-stream channel for large variable-length payloads.
 *
 * Combines SharedMemorySegment + ShmSpscByteRing + futex notification.
 * Ideal for LiDAR point clouds, video frames, and other large sensor data
 * where fixed-slot MPSC wastes memory.
 *
 * Writer calls Write() (auto-notifies via futex). Reader calls WaitReadable() + Read().
 */
class ShmByteChannel final {
 public:
  ShmByteChannel() noexcept : is_writer_(false) {}
  ~ShmByteChannel() = default;

  // Non-copyable, movable
  ShmByteChannel(const ShmByteChannel&) = delete;
  ShmByteChannel& operator=(const ShmByteChannel&) = delete;

  ShmByteChannel(ShmByteChannel&& other) noexcept
      : shm_segment_(static_cast<SharedMemorySegment&&>(other.shm_segment_)),
        ring_(other.ring_),
        is_writer_(other.is_writer_) {
    other.ring_ = ShmSpscByteRing();
  }

  ShmByteChannel& operator=(ShmByteChannel&& other) noexcept {
    if (this != &other) {
      shm_segment_ = static_cast<SharedMemorySegment&&>(other.shm_segment_);
      ring_ = other.ring_;
      is_writer_ = other.is_writer_;
      other.ring_ = ShmSpscByteRing();
    }
    return *this;
  }

  /**
   * @brief Create a writer endpoint.
   * @param name Channel name.
   * @param capacity Data area capacity in bytes (rounded down to power of 2).
   */
  static expected<ShmByteChannel, ShmError> CreateWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY) noexcept {
    ShmByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpscByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::Create(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpscByteRing::InitAt(ch.shm_segment_.Data(), ch.shm_segment_.Size());
    return expected<ShmByteChannel, ShmError>::success(static_cast<ShmByteChannel&&>(ch));
  }

  /**
   * @brief Create a writer endpoint, removing any stale channel first.
   * @param name Channel name.
   * @param capacity Data area capacity in bytes (rounded down to power of 2).
   */
  static expected<ShmByteChannel, ShmError> CreateOrReplaceWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY) noexcept {
    ShmByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpscByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::CreateOrReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpscByteRing::InitAt(ch.shm_segment_.Data(), ch.shm_segment_.Size());
    return expected<ShmByteChannel, ShmError>::success(static_cast<ShmByteChannel&&>(ch));
  }

  /**
   * @brief Open a reader endpoint for an existing channel.
   * @param name Channel name.
   */
  static expected<ShmByteChannel, ShmError> OpenReader(const char* name) noexcept {
    ShmByteChannel ch;
    ch.is_writer_ = false;
    auto result = SharedMemorySegment::Open(name);
    if (!result.has_value()) {
      return expected<ShmByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpscByteRing::AttachAt(ch.shm_segment_.Data());
    return expected<ShmByteChannel, ShmError>::success(static_cast<ShmByteChannel&&>(ch));
  }

  /**
   * @brief Write data and notify waiting reader via futex.
   * @param data Pointer to payload data.
   * @param size Payload size in bytes.
   */
  expected<void, ShmError> Write(const void* data, uint32_t size) noexcept {
    if (!ring_.Write(data, size)) {
      return expected<void, ShmError>::error(ShmError::kFull);
    }
    detail::FutexWake(ring_.HeadPtr());
    return expected<void, ShmError>::success();
  }

  /**
   * @brief Read one message from the channel.
   * @param data Buffer to receive payload.
   * @param max_len Maximum payload size.
   * @return Payload length, or 0 if no data.
   */
  uint32_t Read(void* data, uint32_t max_len) noexcept {
    return ring_.Read(data, max_len);
  }

  /**
   * @brief Wait for data using futex (microsecond-level latency).
   * @param timeout_ms Timeout in milliseconds.
   */
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms) noexcept {
    // Fast path: data already available
    if (ring_.HasData()) {
      return expected<void, ShmError>::success();
    }
    // Futex wait: sleep until head changes
    uint32_t cur_head = *ring_.HeadPtr();
    detail::FutexWait(ring_.HeadPtr(), cur_head, timeout_ms);
    // Re-check after wake
    if (ring_.HasData()) {
      return expected<void, ShmError>::success();
    }
    return expected<void, ShmError>::error(ShmError::kTimeout);
  }

  /// @brief Notify waiting reader (explicit, for batch writes without per-write wake).
  void Notify() noexcept {
    detail::FutexWake(ring_.HeadPtr());
  }

  /// @brief Unlink the shared memory segment (writer only).
  void Unlink() noexcept {
    if (is_writer_) {
      shm_segment_.Unlink();
    }
  }

  /// @brief Available bytes for reading.
  uint32_t ReadableBytes() const noexcept { return ring_.ReadableBytes(); }

  /// @brief Available bytes for writing.
  uint32_t WriteableBytes() const noexcept { return ring_.WriteableBytes(); }

  /// @brief Data area capacity.
  uint32_t Capacity() const noexcept { return ring_.Capacity(); }

 private:
  SharedMemorySegment shm_segment_;
  ShmSpscByteRing ring_;
  bool is_writer_;
};

#endif  // OSP_PLATFORM_LINUX

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_SHM_TRANSPORT_HPP_
