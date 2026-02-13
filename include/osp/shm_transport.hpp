/**
 * @file shm_transport.hpp
 * @brief Shared memory IPC transport with lock-free ring buffer.
 *
 * Provides POSIX shared memory RAII wrappers, lock-free MPSC ring buffer
 * for shared memory, and named channel abstraction with eventfd notification.
 * Linux-only, header-only, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SHM_TRANSPORT_HPP_
#define OSP_SHM_TRANSPORT_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(OSP_PLATFORM_LINUX)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <thread>
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

// ============================================================================
// ShmError
// ============================================================================

enum class ShmError : uint8_t {
  kCreateFailed = 0,
  kOpenFailed,
  kMapFailed,
  kFull,
  kEmpty,
  kTimeout,
  kClosed
};

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
  SharedMemorySegment() noexcept
      : fd_(-1), addr_(nullptr), size_(0), name_{} {}

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
      : fd_(other.fd_),
        addr_(other.addr_),
        size_(other.size_),
        name_(other.name_) {
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
   * @param size Size in bytes.
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> Create(
      const char* name, uint32_t size) noexcept {
    SharedMemorySegment seg;
    seg.name_.assign(TruncateToCapacity, "/osp_shm_");

    // Append user name
    uint32_t prefix_len = seg.name_.size();
    uint32_t name_len = 0;
    while (name[name_len] != '\0' &&
           (prefix_len + name_len) < OSP_SHM_CHANNEL_NAME_MAX) {
      ++name_len;
    }

    for (uint32_t i = 0; i < name_len; ++i) {
      char buf[2] = {name[i], '\0'};
      uint32_t current_len = seg.name_.size();
      if (current_len < OSP_SHM_CHANNEL_NAME_MAX) {
        char temp[OSP_SHM_CHANNEL_NAME_MAX + 1];
        std::memcpy(temp, seg.name_.c_str(), current_len);
        temp[current_len] = name[i];
        temp[current_len + 1] = '\0';
        seg.name_.assign(TruncateToCapacity, temp);
      }
    }

    seg.fd_ = ::shm_open(seg.name_.c_str(), O_CREAT | O_RDWR | O_EXCL, kShmPermissions);
    if (seg.fd_ < 0) {
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kCreateFailed);
    }

    if (::ftruncate(seg.fd_, size) != 0) {
      ::close(seg.fd_);
      ::shm_unlink(seg.name_.c_str());
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kCreateFailed);
    }

    seg.addr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       seg.fd_, 0);
    if (seg.addr_ == MAP_FAILED) {
      ::close(seg.fd_);
      ::shm_unlink(seg.name_.c_str());
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kMapFailed);
    }

    seg.size_ = size;
    return expected<SharedMemorySegment, ShmError>::success(
        static_cast<SharedMemorySegment&&>(seg));
  }

  /**
   * @brief Create a shared memory segment, removing any stale one first.
   *
   * Useful when a previous process crashed without calling Unlink().
   * Equivalent to shm_unlink + Create.
   *
   * @param name Segment name (will be prefixed with /osp_shm_).
   * @param size Size in bytes.
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> CreateOrReplace(
      const char* name, uint32_t size) noexcept {
    // Build the full name to unlink any stale segment
    FixedString<OSP_SHM_CHANNEL_NAME_MAX> full_name;
    full_name.assign(TruncateToCapacity, "/osp_shm_");
    uint32_t prefix_len = full_name.size();
    uint32_t name_len = 0;
    while (name[name_len] != '\0' &&
           (prefix_len + name_len) < OSP_SHM_CHANNEL_NAME_MAX) {
      ++name_len;
    }
    for (uint32_t i = 0; i < name_len; ++i) {
      uint32_t current_len = full_name.size();
      if (current_len < OSP_SHM_CHANNEL_NAME_MAX) {
        char temp[OSP_SHM_CHANNEL_NAME_MAX + 1];
        std::memcpy(temp, full_name.c_str(), current_len);
        temp[current_len] = name[i];
        temp[current_len + 1] = '\0';
        full_name.assign(TruncateToCapacity, temp);
      }
    }

    // Remove stale segment (ignore errors -- may not exist)
    ::shm_unlink(full_name.c_str());

    return Create(name, size);
  }

  /**
   * @brief Open an existing shared memory segment.
   * @param name Segment name (will be prefixed with /osp_shm_).
   * @return expected with SharedMemorySegment on success.
   */
  static expected<SharedMemorySegment, ShmError> Open(
      const char* name) noexcept {
    SharedMemorySegment seg;
    seg.name_.assign(TruncateToCapacity, "/osp_shm_");

    // Append user name
    uint32_t prefix_len = seg.name_.size();
    uint32_t name_len = 0;
    while (name[name_len] != '\0' &&
           (prefix_len + name_len) < OSP_SHM_CHANNEL_NAME_MAX) {
      ++name_len;
    }

    for (uint32_t i = 0; i < name_len; ++i) {
      uint32_t current_len = seg.name_.size();
      if (current_len < OSP_SHM_CHANNEL_NAME_MAX) {
        char temp[OSP_SHM_CHANNEL_NAME_MAX + 1];
        std::memcpy(temp, seg.name_.c_str(), current_len);
        temp[current_len] = name[i];
        temp[current_len + 1] = '\0';
        seg.name_.assign(TruncateToCapacity, temp);
      }
    }

    seg.fd_ = ::shm_open(seg.name_.c_str(), O_RDWR, kShmPermissions);
    if (seg.fd_ < 0) {
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kOpenFailed);
    }

    struct stat st;
    if (::fstat(seg.fd_, &st) != 0) {
      ::close(seg.fd_);
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kOpenFailed);
    }

    seg.size_ = static_cast<uint32_t>(st.st_size);
    seg.addr_ = ::mmap(nullptr, seg.size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, seg.fd_, 0);
    if (seg.addr_ == MAP_FAILED) {
      ::close(seg.fd_);
      return expected<SharedMemorySegment, ShmError>::error(
          ShmError::kMapFailed);
    }

    return expected<SharedMemorySegment, ShmError>::success(
        static_cast<SharedMemorySegment&&>(seg));
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
template <uint32_t SlotSize = OSP_SHM_SLOT_SIZE,
          uint32_t SlotCount = OSP_SHM_SLOT_COUNT>
class ShmRingBuffer final {
  static_assert((SlotCount & (SlotCount - 1)) == 0,
                "SlotCount must be power of 2");

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
  static constexpr uint32_t Size() noexcept {
    return sizeof(ShmRingBuffer);
  }

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
    } while (!producer_pos_.compare_exchange_weak(
        prod_pos, prod_pos + 1,
        std::memory_order_acq_rel, std::memory_order_relaxed));

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

  static_assert(std::is_standard_layout<Slot>::value,
                "Slot must be standard layout for shared memory");

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
template <uint32_t SlotSize = OSP_SHM_SLOT_SIZE,
          uint32_t SlotCount = OSP_SHM_SLOT_COUNT>
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

    return expected<ShmChannel, ShmError>::success(
        static_cast<ShmChannel&&>(channel));
  }

  /**
   * @brief Create a writer endpoint, removing any stale channel first.
   *
   * Useful when a previous process crashed without calling Unlink().
   *
   * @param name Channel name.
   * @return expected with ShmChannel on success.
   */
  static expected<ShmChannel, ShmError> CreateOrReplaceWriter(
      const char* name) noexcept {
    ShmChannel channel;
    channel.is_writer_ = true;

    uint32_t shm_size = RingBuffer::Size();
    auto result = SharedMemorySegment::CreateOrReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmChannel, ShmError>::error(result.get_error());
    }

    channel.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data());

    return expected<ShmChannel, ShmError>::success(
        static_cast<ShmChannel&&>(channel));
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

    return expected<ShmChannel, ShmError>::success(
        static_cast<ShmChannel&&>(channel));
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

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

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
  uint32_t Depth() const noexcept {
    return ring_buffer_ ? ring_buffer_->Depth() : 0;
  }

 private:
  SharedMemorySegment shm_segment_;
  RingBuffer* ring_buffer_;
  bool is_writer_;
};

#endif  // OSP_PLATFORM_LINUX

}  // namespace osp

#endif  // OSP_SHM_TRANSPORT_HPP_
