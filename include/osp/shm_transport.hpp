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

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <atomic>
#include <utility>

#if OSP_HAS_NETWORK

#if defined(OSP_PLATFORM_LINUX)
#include <chrono>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
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
  kClosed,
  kAlreadyExists,
  kLayoutMismatch,
  kOwnerAlive,
  // Appended in layout v2; existing values must not change.
  kInvalidArgument,
  kInvalidState,
  kEvicted,
  kConsumerStuck,
  kCorrupt,
  kMessageTooLarge,
  kWaitInterrupted,
  kNone,
};

/// @brief Channel health state reported by Health().
enum class ShmState : uint8_t { kHealthy, kDegraded, kFaulted };

/// @brief Point-in-time health counters for one shared-memory endpoint.
struct ShmHealthSnapshot {
  ShmState state{ShmState::kHealthy};
  ShmError last_error{ShmError::kNone};
  uint64_t reap_epoch{0};
  uint32_t active_consumers{0};
  uint32_t stale_consumers{0};
  uint64_t oldest_pending_us{0};
  uint64_t reaped_total{0};
  uint64_t evicted_total{0};
  uint64_t read_evicted_total{0};
  uint64_t reap_deferred_total{0};
  uint64_t read_conflict_total{0};
  uint64_t consumer_stuck_total{0};
  uint64_t orphan_state2_total{0};
  uint64_t orphan_state3_total{0};
  uint64_t generation_wrap_total{0};
  uint64_t write_stall_events{0};
  uint64_t wait_timeouts{0};
  uint64_t corrupt_total{0};
  uint64_t dropped_newest{0};
};

/// @brief Fault event delivered outside the hot path.
struct ShmFaultEvent {
  uint16_t code{0U};
  uint32_t channel_hash{0U};
  uint32_t consumer_id{0U};
  uint64_t detail{0U};
};

using ShmFaultReporter = void (*)(const ShmFaultEvent&, void* ctx);

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
// Shared memory layout header - common prefix for every mapped segment
// ============================================================================

enum class ShmLayoutKind : uint32_t {
  kSlotRing = 1U,
  kSpscByteRing = 2U,
  kSpmcByteRing = 3U,
};

inline constexpr uint32_t kShmLayoutMagic = 0x4F53484DU;  // "OSHM"
// v2 adds the ring header size to the layout hash and a fault_flags word to
// the MPSC slot ring. v1 segments are rejected at attach.
inline constexpr uint16_t kShmLayoutVersion = 2U;

struct alignas(64) ShmLayoutHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t kind;
  uint32_t slot_size;
  uint32_t slot_count;
  uint32_t total_size;
  uint32_t layout_hash;
  uint64_t epoch;
  int32_t owner_pid;
  uint32_t flags;
  uint8_t reserved[16];
};

static_assert(sizeof(ShmLayoutHeader) == 64U, "ShmLayoutHeader must be 64 bytes");

inline constexpr uint32_t ShmLayoutHash(ShmLayoutKind kind, uint32_t slot_size, uint32_t slot_count,
                                        uint32_t total_size, uint32_t header_payload_size) noexcept {
  uint32_t hash = 2166136261U;  // FNV-1a
  const auto mix = [&hash](uint32_t value) { hash = (hash ^ value) * 16777619U; };
  mix(kShmLayoutVersion);
  mix(static_cast<uint32_t>(kind));
  mix(slot_size);
  mix(slot_count);
  mix(total_size);
  mix(header_payload_size);
  return hash;
}

inline void WriteShmLayoutHeader(void* base, ShmLayoutKind kind, uint32_t slot_size, uint32_t slot_count,
                                 uint32_t total_size, uint32_t header_payload_size) noexcept {
  auto* header = static_cast<ShmLayoutHeader*>(base);
  header->magic = kShmLayoutMagic;
  header->version = kShmLayoutVersion;
  header->header_size = static_cast<uint16_t>(sizeof(ShmLayoutHeader));
  header->kind = static_cast<uint32_t>(kind);
  header->slot_size = slot_size;
  header->slot_count = slot_count;
  header->total_size = total_size;
  header->layout_hash = ShmLayoutHash(kind, slot_size, slot_count, total_size, header_payload_size);
  header->epoch = SteadyNowUs();
  header->owner_pid = static_cast<int32_t>(::getpid());
  header->flags = 0U;
  std::memset(header->reserved, 0, sizeof(header->reserved));
}

inline expected<ShmLayoutHeader*, ShmError> ValidateShmLayout(void* base, uint32_t total_size, ShmLayoutKind kind,
                                                              uint32_t slot_size, uint32_t slot_count,
                                                              uint32_t header_payload_size) noexcept {
  if (base == nullptr || total_size < sizeof(ShmLayoutHeader)) {
    return expected<ShmLayoutHeader*, ShmError>::error(ShmError::kLayoutMismatch);
  }
  auto* header = static_cast<ShmLayoutHeader*>(base);
  if (header->magic != kShmLayoutMagic || header->version != kShmLayoutVersion ||
      header->header_size != sizeof(ShmLayoutHeader) || header->kind != static_cast<uint32_t>(kind) ||
      header->slot_size != slot_size || header->slot_count != slot_count || header->total_size != total_size ||
      header->layout_hash != ShmLayoutHash(kind, slot_size, slot_count, total_size, header_payload_size)) {
    return expected<ShmLayoutHeader*, ShmError>::error(ShmError::kLayoutMismatch);
  }
  return expected<ShmLayoutHeader*, ShmError>::success(header);
}

inline bool IsProcessAlive(pid_t pid) noexcept {
  if (pid <= 0) {
    return false;
  }
#if defined(SYS_pidfd_open)
  const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  if (fd >= 0) {
    ::close(fd);
    return true;
  }
  if (errno == ESRCH) {
    return false;
  }
#endif
  if (::kill(pid, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

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
      return expected<SharedMemorySegment, ShmError>::error(errno == EEXIST ? ShmError::kAlreadyExists
                                                                            : ShmError::kCreateFailed);
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
   * @brief Replace an existing segment only when its recorded owner is dead.
   *
   * A live owner is rejected with kOwnerAlive. A segment without a valid
   * layout header is rejected with kLayoutMismatch; use ForceReplace() after
   * an explicit external liveness check when migrating legacy segments.
   */
  static expected<SharedMemorySegment, ShmError> CreateOrReplace(const char* name, uint32_t size) noexcept {
    auto existing = Open(name);
    if (!existing.has_value()) {
      if (existing.get_error() == ShmError::kOpenFailed) {
        return Create(name, size);
      }
      return expected<SharedMemorySegment, ShmError>::error(existing.get_error());
    }
    const auto& segment = existing.value();
    if (!segment.HasValidLayout()) {
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kLayoutMismatch);
    }
    if (IsProcessAlive(static_cast<pid_t>(segment.OwnerPid()))) {
      return expected<SharedMemorySegment, ShmError>::error(ShmError::kOwnerAlive);
    }
    return ForceReplace(name, size);
  }

  /**
   * @brief Unconditionally unlink and recreate a segment.
   *
   * This is the explicit escape hatch for a confirmed-dead owner or legacy
   * segments. Prefer Create()/CreateOrReplace() in normal operation.
   */
  static expected<SharedMemorySegment, ShmError> ForceReplace(const char* name, uint32_t size) noexcept {
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

  /// Epoch of the common layout header, or 0 when this is not a coact/osp
  /// shared-memory segment with a valid layout header.
  uint64_t Epoch() const noexcept {
    if (addr_ == nullptr || size_ < sizeof(ShmLayoutHeader)) {
      return 0U;
    }
    const auto* header = static_cast<const ShmLayoutHeader*>(addr_);
    return (header->magic == kShmLayoutMagic) ? header->epoch : 0U;
  }

  /// True when the mapped segment starts with a valid layout header.
  bool HasValidLayout() const noexcept {
    if (addr_ == nullptr || size_ < sizeof(ShmLayoutHeader)) {
      return false;
    }
    const auto* header = static_cast<const ShmLayoutHeader*>(addr_);
    return header->magic == kShmLayoutMagic && header->version == kShmLayoutVersion &&
           header->header_size == sizeof(ShmLayoutHeader) && header->total_size == size_;
  }

  /// Recorded creator PID, or 0 when no valid layout header exists.
  int32_t OwnerPid() const noexcept {
    if (!HasValidLayout()) {
      return 0;
    }
    return static_cast<const ShmLayoutHeader*>(addr_)->owner_pid;
  }

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
  static uint32_t PageAlign(uint32_t size) noexcept { return (size + 4095u) & ~4095u; }

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
  static constexpr uint32_t kLayoutOffset = sizeof(ShmLayoutHeader);

  /**
   * @brief Initialize the ring buffer at a shared memory address.
   * Must be called by the creator before any other operations.
   * @param shm_addr Pointer to shared memory (must be at least Size() bytes).
   * @return Pointer to the initialized ShmRingBuffer.
   */
  static ShmRingBuffer* InitAt(void* shm_addr, uint32_t total_size = Size()) noexcept {
    OSP_ASSERT(shm_addr != nullptr);
    WriteShmLayoutHeader(shm_addr, ShmLayoutKind::kSlotRing, SlotSize, SlotCount, total_size,
                         static_cast<uint32_t>(sizeof(ShmRingBuffer)));
    auto* rb = reinterpret_cast<ShmRingBuffer*>(  // NOLINT
        static_cast<uint8_t*>(shm_addr) + kLayoutOffset);

    // Initialize atomics using placement new
    new (&rb->producer_pos_) std::atomic<uint32_t>(0);
    new (&rb->consumer_pos_) std::atomic<uint32_t>(0);
    new (&rb->fault_flags_) std::atomic<uint32_t>(0);

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
  static expected<ShmRingBuffer*, ShmError> AttachChecked(void* shm_addr, uint32_t total_size) noexcept {
    auto valid = ValidateShmLayout(shm_addr, total_size, ShmLayoutKind::kSlotRing, SlotSize, SlotCount,
                                   static_cast<uint32_t>(sizeof(ShmRingBuffer)));
    if (!valid.has_value()) {
      return expected<ShmRingBuffer*, ShmError>::error(valid.get_error());
    }
    return expected<ShmRingBuffer*, ShmError>::success(reinterpret_cast<ShmRingBuffer*>(  // NOLINT
        static_cast<uint8_t*>(shm_addr) + kLayoutOffset));
  }

  /**
   * @brief Calculate required size for the ring buffer structure.
   * @return Size in bytes.
   */
  static constexpr uint32_t Size() noexcept { return kLayoutOffset + sizeof(ShmRingBuffer); }

  /// @brief Bit0: a defensive check found a corrupted slot or header.
  static constexpr uint32_t kFaultCorrupt = 1U << 0U;

  /// @brief Corruption flags raised by this ring (0 = healthy).
  uint32_t FaultFlags() const noexcept { return fault_flags_.load(std::memory_order_acquire); }

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
    const uint32_t msg_size = slot.size;
    if (msg_size > SlotSize) {
      // A peer process or an older writer may corrupt the slot size. Never
      // memcpy an untrusted length; leave the slot unpublished so the
      // consumer stops instead of reading past the slot.
      fault_flags_.fetch_or(kFaultCorrupt, std::memory_order_release);
      return false;
    }
    size = msg_size;
    std::memcpy(data, slot.data, msg_size);

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
  alignas(64) std::atomic<uint32_t> fault_flags_;
  Slot slots_[SlotCount];

 public:
  /// @brief Slot stride and offset from the ring object start (shared-memory ABI).
  static constexpr uint32_t kSlotStride = static_cast<uint32_t>(sizeof(Slot));
  static constexpr uint32_t kSlotsOffset = static_cast<uint32_t>(offsetof(ShmRingBuffer, slots_));

 private:
  // Private constructor - use InitAt/AttachChecked
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
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data(), channel.shm_segment_.Size());

    return expected<ShmChannel, ShmError>::success(static_cast<ShmChannel&&>(channel));
  }

  /**
   * @brief Create a writer endpoint, replacing only a dead owner's channel.
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
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data(), channel.shm_segment_.Size());

    return expected<ShmChannel, ShmError>::success(static_cast<ShmChannel&&>(channel));
  }

  /// Explicitly unlink and recreate the channel. Use only after confirming the
  /// previous writer is dead.
  static expected<ShmChannel, ShmError> ForceReplaceWriter(const char* name) noexcept {
    ShmChannel channel;
    channel.is_writer_ = true;
    uint32_t shm_size = RingBuffer::Size();
    auto result = SharedMemorySegment::ForceReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmChannel, ShmError>::error(result.get_error());
    }
    channel.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    channel.ring_buffer_ = RingBuffer::InitAt(channel.shm_segment_.Data(), channel.shm_segment_.Size());
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
    auto attached = RingBuffer::AttachChecked(channel.shm_segment_.Data(), channel.shm_segment_.Size());
    if (!attached.has_value()) {
      return expected<ShmChannel, ShmError>::error(attached.get_error());
    }
    channel.ring_buffer_ = attached.value();

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

  /// @brief Segment epoch (changes when CreateOrReplace rebuilds the segment).
  uint64_t Epoch() const noexcept { return shm_segment_.Epoch(); }

 private:
  SharedMemorySegment shm_segment_;
  RingBuffer* ring_buffer_;
  bool is_writer_;
};

// ============================================================================
// ShmSpscByteRing - SPSC byte-stream ring buffer for large payloads
// ============================================================================

/**
 * @brief Header for SPSC byte ring buffer (16 bytes).
 * head/tail are atomic (cross-thread read/write); capacity/reserved are set
 * once by InitAt, so they stay plain.
 */
struct ShmByteRingHeader {
  std::atomic<uint32_t> head;  ///< Producer write position (monotonically increasing)
  std::atomic<uint32_t> tail;  ///< Consumer read position (monotonically increasing)
  uint32_t capacity;           ///< Data area size (must be power of 2)
  uint32_t reserved;           ///< Alignment padding
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
  static constexpr uint32_t kLayoutOffset = sizeof(ShmLayoutHeader);

 public:
  ShmSpscByteRing() noexcept : header_(nullptr), data_(nullptr), mask_(0) {}

  /**
   * @brief Bind to shared memory as producer (initializes header).
   * @param shm_base Pointer to the start of shared memory.
   * @param total_size Total shared memory size (header + data).
   */
  static ShmSpscByteRing InitAt(void* shm_base, uint32_t total_size) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    OSP_ASSERT(total_size > kLayoutOffset + kHeaderSize);
    ShmSpscByteRing ring;
    uint32_t cap = RoundDownPow2(total_size - kLayoutOffset - kHeaderSize);
    WriteShmLayoutHeader(shm_base, ShmLayoutKind::kSpscByteRing, cap, 0U, total_size, kHeaderSize);
    ring.header_ = reinterpret_cast<ShmByteRingHeader*>(  // NOLINT
        static_cast<uint8_t*>(shm_base) + kLayoutOffset);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kLayoutOffset + kHeaderSize;
    ring.header_->head.store(0, std::memory_order_relaxed);
    ring.header_->tail.store(0, std::memory_order_relaxed);
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
  static expected<ShmSpscByteRing, ShmError> AttachChecked(void* shm_base, uint32_t total_size) noexcept {
    if (shm_base == nullptr || total_size <= kLayoutOffset + kHeaderSize) {
      return expected<ShmSpscByteRing, ShmError>::error(ShmError::kLayoutMismatch);
    }
    const uint32_t cap = RoundDownPow2(total_size - kLayoutOffset - kHeaderSize);
    auto valid = ValidateShmLayout(shm_base, total_size, ShmLayoutKind::kSpscByteRing, cap, 0U, kHeaderSize);
    if (!valid.has_value()) {
      return expected<ShmSpscByteRing, ShmError>::error(valid.get_error());
    }
    ShmSpscByteRing ring;
    ring.header_ = reinterpret_cast<ShmByteRingHeader*>(  // NOLINT
        static_cast<uint8_t*>(shm_base) + kLayoutOffset);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kLayoutOffset + kHeaderSize;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (ring.header_->capacity != cap) {
      return expected<ShmSpscByteRing, ShmError>::error(ShmError::kLayoutMismatch);
    }
    ring.mask_ = cap - 1;
    return expected<ShmSpscByteRing, ShmError>::success(ring);
  }

  /**
   * @brief Calculate minimum shared memory size for given data capacity.
   * @param data_capacity Desired data area size (will be rounded down to power of 2).
   */
  static constexpr uint32_t RequiredSize(uint32_t data_capacity) noexcept {
    return kLayoutOffset + kHeaderSize + data_capacity;
  }

  // ---- Producer API ----

  /**
   * @brief Write a length-prefixed message: [4B len][payload].
   * @param data Pointer to payload data.
   * @param len Payload length in bytes.
   * @return true if successful, false if not enough space.
   */
  bool Write(const void* data, uint32_t len) noexcept {
    // Validate before computing len + 4: a wrapped total would pass the
    // capacity check and turn into an out-of-bounds memcpy.
    if (len == 0U || header_->capacity <= 4U || len > header_->capacity - 4U) {
      return false;
    }
    const uint32_t total = len + 4U;
    if (WriteableBytes() < total) {
      return false;
    }
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    WriteRaw(head, &len, 4);
    WriteRaw(head + 4, data, len);
    header_->head.store(head + total, std::memory_order_release);
    return true;
  }

  /// @brief Available bytes for writing.
  uint32_t WriteableBytes() const noexcept {
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    const uint32_t tail = header_->tail.load(std::memory_order_acquire);
    const uint32_t used = head - tail;
    return (used >= header_->capacity) ? 0U : (header_->capacity - used);
  }

  // ---- Consumer API ----

  /**
   * @brief Read one length-prefixed message.
   * @param[out] out Buffer to receive payload.
   * @param max_len Maximum payload size.
   * @return Payload length, or 0 if no data available.
   */
  uint32_t Read(void* out, uint32_t max_len) noexcept {
    const uint32_t tail = header_->tail.load(std::memory_order_relaxed);
    const uint32_t head = header_->head.load(std::memory_order_acquire);
    const uint32_t available = head - tail;
    if (available < 4) {
      return 0;
    }
    uint32_t msg_len = 0;
    ReadRaw(tail, &msg_len, 4);
    if (msg_len == 0U || msg_len > available - 4U) {
      return 0;
    }
    if (msg_len > max_len) {
      // Message too large for output buffer; skip it
      header_->tail.store(tail + msg_len + 4, std::memory_order_release);
      return 0;
    }
    ReadRaw(tail + 4, out, msg_len);
    header_->tail.store(tail + msg_len + 4, std::memory_order_release);
    return msg_len;
  }

  /// @brief Available bytes for reading.
  uint32_t ReadableBytes() const noexcept {
    const uint32_t tail = header_->tail.load(std::memory_order_relaxed);
    const uint32_t head = header_->head.load(std::memory_order_acquire);
    return head - tail;
  }

  /// @brief Check if at least one complete message header is available.
  bool HasData() const noexcept { return ReadableBytes() >= 4; }

  /// @brief Data area capacity in bytes.
  uint32_t Capacity() const noexcept { return header_ ? header_->capacity : 0; }

  /// @brief Pointer to the head field (for futex wait/wake).
  std::atomic<uint32_t>* HeadPtr() noexcept { return &header_->head; }

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
    if (v == 0)
      return 0;
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

/**
 * @brief Wait on a futex word until it changes from expected_val or timeout.
 * @return 0 on wake, -1 on timeout/error.
 * reinterpret_cast to uint32_t* is safe via the enclosing static_assert.
 */
inline int FutexWait(std::atomic<uint32_t>* addr, uint32_t expected_val, uint32_t timeout_ms) noexcept {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);  // NOLINT
  uint32_t* raw = reinterpret_cast<uint32_t*>(addr);               // NOLINT
  return static_cast<int>(::syscall(SYS_futex, raw, FUTEX_WAIT, expected_val, &ts, nullptr, 0));
}

/// @brief Remaining time to an absolute microsecond deadline, rounded up.
inline uint32_t RemainingMsCeil(uint64_t deadline_us) noexcept {
  const uint64_t now = SteadyNowUs();
  if (now >= deadline_us) {
    return 0U;
  }
  const uint64_t ms = (deadline_us - now + 999U) / 1000U;
  return (ms > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(ms);
}

/**
 * @brief Classify a futex wait return value.
 * @return 0 = recheck the predicate (woken, EAGAIN, EINTR), 1 = ETIMEDOUT,
 *         2 = any other error.
 */
inline int FutexWaitOutcome(int rc) noexcept {
  if (rc == 0) {
    return 0;
  }
  if (errno == ETIMEDOUT) {
    return 1;
  }
  if (errno == EAGAIN || errno == EINTR) {
    return 0;
  }
  return 2;
}

/// @brief Wake one waiter on a futex word.
inline void FutexWake(std::atomic<uint32_t>* addr) noexcept {
  uint32_t* raw = reinterpret_cast<uint32_t*>(addr);  // NOLINT
  (void)::syscall(SYS_futex, raw, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

/// @brief Wake all waiters on a futex word (for SPMC broadcast).
inline void FutexWakeAll(std::atomic<uint32_t>* addr) noexcept {
  uint32_t* raw = reinterpret_cast<uint32_t*>(addr);  // NOLINT
  (void)::syscall(SYS_futex, raw, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
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
  static expected<ShmByteChannel, ShmError> CreateWriter(const char* name,
                                                         uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY) noexcept {
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
   * @brief Create a writer endpoint, replacing only a dead owner's channel.
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

  /// Explicitly unlink and recreate the channel. Use only after confirming the
  /// previous writer is dead.
  static expected<ShmByteChannel, ShmError> ForceReplaceWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY) noexcept {
    ShmByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpscByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::ForceReplace(name, shm_size);
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
    auto attached = ShmSpscByteRing::AttachChecked(ch.shm_segment_.Data(), ch.shm_segment_.Size());
    if (!attached.has_value()) {
      return expected<ShmByteChannel, ShmError>::error(attached.get_error());
    }
    ch.ring_ = attached.value();
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
  uint32_t Read(void* data, uint32_t max_len) noexcept { return ring_.Read(data, max_len); }

  /**
   * @brief Wait for data using futex (microsecond-level latency).
   * @param timeout_ms Timeout in milliseconds.
   */
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms) noexcept {
    const uint64_t deadline = SteadyNowUs() + static_cast<uint64_t>(timeout_ms) * 1000U;
    for (;;) {
      if (ring_.HasData()) {
        return expected<void, ShmError>::success();
      }
      // Read the wait word first, then re-check: a producer that publishes
      // between the two makes the futex return EAGAIN instead of sleeping.
      const uint32_t head = ring_.HeadPtr()->load(std::memory_order_acquire);
      if (ring_.HasData()) {
        return expected<void, ShmError>::success();
      }
      if (timeout_ms == 0U) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
#if OSP_SHM_WAIT_SPIN_US > 0
      const uint64_t spin_end = SteadyNowUs() + OSP_SHM_WAIT_SPIN_US;
      while (SteadyNowUs() < spin_end) {
        if (ring_.HasData()) {
          return expected<void, ShmError>::success();
        }
      }
#endif
      const uint32_t remaining = detail::RemainingMsCeil(deadline);
      if (remaining == 0U) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      const int outcome = detail::FutexWaitOutcome(detail::FutexWait(ring_.HeadPtr(), head, remaining));
      if (outcome == 1) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      if (outcome == 2) {
        return expected<void, ShmError>::error(ShmError::kWaitInterrupted);
      }
    }
  }

  /// @brief Notify waiting reader (explicit, for batch writes without per-write wake).
  void Notify() noexcept { detail::FutexWake(ring_.HeadPtr()); }

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

  /// @brief Segment epoch (changes when CreateOrReplace rebuilds the segment).
  uint64_t Epoch() const noexcept { return shm_segment_.Epoch(); }

 private:
  SharedMemorySegment shm_segment_;
  ShmSpscByteRing ring_;
  bool is_writer_;
};

// ============================================================================
// ShmSpmcByteRing - Single-Producer Multi-Consumer byte-stream ring buffer
// ============================================================================

/// @brief Consumer reaping policy (writer side).
enum class ReapPolicy : uint8_t { kNone, kOnFull, kSupervisor };

/// @brief Writer-side channel configuration.
struct ShmChannelConfig {
  uint64_t stale_timeout_us{3000000};  ///< Alive-but-idle consumer becomes "stuck" after this
  uint32_t reap_interval_ms{100};      ///< Supervisor tick period
  uint32_t grace_us{2000000};          ///< Max time allowed in a transient slot state
  ReapPolicy reap_policy{ReapPolicy::kNone};
  bool evict_alive_slow{false};  ///< Also reap alive consumers whose tail stalls
};

/// @brief Consumer identity returned by RegisterConsumer(const ShmChannelConfig&).
struct ConsumerHandle {
  uint32_t id{0U};
  int32_t pool_slot{-1};  ///< Lease pool slot, -1 when leasing is disabled
  uint32_t generation{0U};
};

/// @brief 16-byte block reference carried by the SPMC channel in lease mode.
struct ShmBlockRef {
  uint32_t block_id{0U};
  uint32_t block_generation{0U};
  uint32_t size{0U};
  uint32_t pool_hash{0U};
};

static_assert(sizeof(ShmBlockRef) == 16U, "ShmBlockRef must be 16 bytes");

/**
 * @brief Header for SPMC byte ring buffer (shared memory safe).
 * One head + per-consumer tails[]; fields written by one side and read by the
 * other are std::atomic (is_always_lock_free on ARM/x86) for cross-process
 * safety. Each consumer has an independent tail for independent reading.
 */
struct ShmSpmcByteRingHeader {
  static constexpr uint32_t kStateMask = 0x3U;
  static constexpr uint32_t kGuardShift = 2U;

  /// @brief Pack state (low 2 bits) and read guard (high 30 bits).
  static constexpr uint32_t Pack(uint32_t state, uint32_t guard) noexcept {
    return (guard << kGuardShift) | (state & kStateMask);
  }
  static constexpr uint32_t StateOf(uint32_t word) noexcept { return word & kStateMask; }
  static constexpr uint32_t GuardOf(uint32_t word) noexcept { return word >> kGuardShift; }

  std::atomic<uint32_t> head;                                    ///< Producer write position (monotonically increasing)
  uint32_t capacity;                                             ///< Data area size (must be power of 2)
  uint32_t max_consumers;                                        ///< Maximum number of consumers
  std::atomic<uint32_t> consumer_count;                          ///< Active consumer count (atomic CAS)
  std::atomic<uint32_t> space_seq;                               ///< Tail advance counter (wait plane)
  std::atomic<uint32_t> reap_epoch;                              ///< Diagnostics: successful reap count
  std::atomic<uint32_t> tails[OSP_SHM_SPMC_MAX_CONSUMERS];       ///< Per-consumer read positions
  std::atomic<uint32_t> slot_state[OSP_SHM_SPMC_MAX_CONSUMERS];  ///< state 0/1/2/3 + read guard
  std::atomic<uint32_t> generation[OSP_SHM_SPMC_MAX_CONSUMERS];  ///< Per-consumer generation
  std::atomic<uint32_t> flags[OSP_SHM_SPMC_MAX_CONSUMERS];       ///< evicted/stale/stuck/corrupt
  std::atomic<uint32_t> pid[OSP_SHM_SPMC_MAX_CONSUMERS];         ///< Consumer pid for liveness
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for shared memory use");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "std::atomic<uint32_t> must be layout-compatible with uint32_t for futex");

/**
 * @brief SPMC byte ring: one producer, many independent consumers.
 * Message format [4-byte LE length][payload]; memory ordering acquire/release.
 * The producer side is NOT multi-writer safe: concurrent Write() targets the
 * same range; for multi-producer use prefer ShmRingBuffer (MPSC).
 */
class ShmSpmcByteRing final {
 public:
  static constexpr uint32_t kHeaderSize = static_cast<uint32_t>(sizeof(ShmSpmcByteRingHeader));
  static constexpr uint32_t kLayoutOffset = sizeof(ShmLayoutHeader);

  /// @brief Per-consumer flag bits stored in header_->flags[].
  static constexpr uint32_t kFlagEvicted = 1U << 0U;
  static constexpr uint32_t kFlagStuck = 1U << 2U;
  static constexpr uint32_t kFlagCorrupt = 1U << 3U;

  ShmSpmcByteRing() noexcept : header_(nullptr), data_(nullptr), mask_(0) {}

  /**
   * @brief Initialize as producer (creates header).
   * @param shm_base Pointer to the start of shared memory.
   * @param total_size Total shared memory size (header + data).
   * @param max_consumers Maximum number of consumers (clamped to OSP_SHM_SPMC_MAX_CONSUMERS).
   */
  static ShmSpmcByteRing InitAt(void* shm_base, uint32_t total_size,
                                uint32_t max_consumers = OSP_SHM_SPMC_MAX_CONSUMERS) noexcept {
    OSP_ASSERT(shm_base != nullptr);
    OSP_ASSERT(total_size > kLayoutOffset + kHeaderSize);
    if (max_consumers > OSP_SHM_SPMC_MAX_CONSUMERS) {
      max_consumers = OSP_SHM_SPMC_MAX_CONSUMERS;
    }
    ShmSpmcByteRing ring;
    uint32_t cap = RoundDownPow2(total_size - kLayoutOffset - kHeaderSize);
    WriteShmLayoutHeader(shm_base, ShmLayoutKind::kSpmcByteRing, cap, max_consumers, total_size, kHeaderSize);
    ring.header_ = reinterpret_cast<ShmSpmcByteRingHeader*>(  // NOLINT
        static_cast<uint8_t*>(shm_base) + kLayoutOffset);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kLayoutOffset + kHeaderSize;
    ring.header_->head.store(0, std::memory_order_relaxed);
    ring.header_->capacity = cap;
    ring.header_->max_consumers = max_consumers;
    ring.header_->consumer_count.store(0, std::memory_order_relaxed);
    ring.header_->space_seq.store(0, std::memory_order_relaxed);
    ring.header_->reap_epoch.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < OSP_SHM_SPMC_MAX_CONSUMERS; ++i) {
      ring.header_->tails[i].store(0, std::memory_order_relaxed);
      ring.header_->slot_state[i].store(0, std::memory_order_relaxed);
      ring.header_->generation[i].store(0, std::memory_order_relaxed);
      ring.header_->flags[i].store(0, std::memory_order_relaxed);
      ring.header_->pid[i].store(0, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
    ring.mask_ = cap - 1;
    return ring;
  }

  /**
   * @brief Attach to existing shared memory (consumer or observer).
   * @param shm_base Pointer to the start of shared memory.
   */
  static expected<ShmSpmcByteRing, ShmError> AttachChecked(void* shm_base, uint32_t total_size) noexcept {
    if (shm_base == nullptr || total_size <= kLayoutOffset + kHeaderSize) {
      return expected<ShmSpmcByteRing, ShmError>::error(ShmError::kLayoutMismatch);
    }
    const auto* layout = static_cast<const ShmLayoutHeader*>(shm_base);
    const uint32_t cap = RoundDownPow2(total_size - kLayoutOffset - kHeaderSize);
    const uint32_t max_consumers = layout->slot_count;
    auto valid = ValidateShmLayout(shm_base, total_size, ShmLayoutKind::kSpmcByteRing, cap, max_consumers, kHeaderSize);
    if (!valid.has_value() || max_consumers == 0U || max_consumers > OSP_SHM_SPMC_MAX_CONSUMERS) {
      return expected<ShmSpmcByteRing, ShmError>::error(ShmError::kLayoutMismatch);
    }
    ShmSpmcByteRing ring;
    ring.header_ = reinterpret_cast<ShmSpmcByteRingHeader*>(  // NOLINT
        static_cast<uint8_t*>(shm_base) + kLayoutOffset);
    ring.data_ = static_cast<uint8_t*>(shm_base) + kLayoutOffset + kHeaderSize;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (ring.header_->capacity != cap || ring.header_->max_consumers != max_consumers) {
      return expected<ShmSpmcByteRing, ShmError>::error(ShmError::kLayoutMismatch);
    }
    ring.mask_ = cap - 1;
    return expected<ShmSpmcByteRing, ShmError>::success(ring);
  }

  /// @brief Calculate minimum shared memory size for given data capacity.
  static constexpr uint32_t RequiredSize(uint32_t data_capacity) noexcept {
    return kLayoutOffset + kHeaderSize + data_capacity;
  }

  // ---- Consumer registration ----

  /// @brief Register a consumer with generation tracking and return its handle.
  expected<ConsumerHandle, ShmError> RegisterConsumer(const ShmChannelConfig& config) noexcept {
    (void)config;  // Policies are applied on the writer side, not per consumer.
    for (uint32_t i = 0; i < header_->max_consumers; ++i) {
      uint32_t expected_word = 0U;
      if (!header_->slot_state[i].compare_exchange_strong(expected_word, ShmSpmcByteRingHeader::Pack(2U, 0U),
                                                          std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }
      const uint32_t next_generation = header_->generation[i].load(std::memory_order_relaxed) + 1U;
      if (next_generation == 0U) {
        // Generation exhausted: refuse instead of silently wrapping.
        uint32_t expected_free = ShmSpmcByteRingHeader::Pack(2U, 0U);
        (void)header_->slot_state[i].compare_exchange_strong(expected_free, 0U, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed);
        ++generation_wrap_total_;
        MarkFault(ShmError::kInvalidState);
        ReportFault(ShmError::kInvalidState, i, next_generation);
        return expected<ConsumerHandle, ShmError>::error(ShmError::kInvalidState);
      }
      header_->tails[i].store(header_->head.load(std::memory_order_acquire), std::memory_order_relaxed);
      header_->generation[i].store(next_generation, std::memory_order_release);
      header_->pid[i].store(static_cast<uint32_t>(::getpid()), std::memory_order_release);
      header_->flags[i].store(0U, std::memory_order_relaxed);
      uint32_t expected_active = ShmSpmcByteRingHeader::Pack(2U, 0U);
      if (!header_->slot_state[i].compare_exchange_strong(expected_active, ShmSpmcByteRingHeader::Pack(1U, 0U),
                                                          std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Tick reaped the registering slot; do not publish as active.
        uint32_t expected_release = ShmSpmcByteRingHeader::Pack(2U, 0U);
        (void)header_->slot_state[i].compare_exchange_strong(expected_release, 0U, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed);
        continue;
      }
      header_->consumer_count.fetch_add(1U, std::memory_order_release);
      detail::FutexWakeAll(&header_->consumer_count);
      return expected<ConsumerHandle, ShmError>::success(ConsumerHandle{i, -1, next_generation});
    }
    return expected<ConsumerHandle, ShmError>::error(ShmError::kFull);
  }

  /// @brief Unregister by handle. Idempotent when the handle is already stale.
  expected<void, ShmError> UnregisterConsumer(ConsumerHandle handle) noexcept {
    if (handle.id >= header_->max_consumers) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    uint32_t word = header_->slot_state[handle.id].load(std::memory_order_acquire);
    if (ShmSpmcByteRingHeader::StateOf(word) != 1U) {
      return expected<void, ShmError>::success();
    }
    if (ShmSpmcByteRingHeader::GuardOf(word) != 0U) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    if (header_->generation[handle.id].load(std::memory_order_acquire) != handle.generation) {
      return expected<void, ShmError>::success();
    }
    uint32_t expected_word = ShmSpmcByteRingHeader::Pack(1U, 0U);
    if (!header_->slot_state[handle.id].compare_exchange_strong(expected_word, 0U, std::memory_order_acq_rel,
                                                                std::memory_order_relaxed)) {
      return (ShmSpmcByteRingHeader::StateOf(expected_word) == 1U)
                 ? expected<void, ShmError>::error(ShmError::kInvalidState)
                 : expected<void, ShmError>::success();
    }
    header_->consumer_count.fetch_sub(1U, std::memory_order_release);
    detail::FutexWakeAll(&header_->consumer_count);
    return expected<void, ShmError>::success();
  }

  /// @brief Reopen after eviction: unregister (idempotent) then register again.
  expected<ConsumerHandle, ShmError> Reopen(ConsumerHandle handle, const ShmChannelConfig& config) noexcept {
    auto closed = UnregisterConsumer(handle);
    if (!closed.has_value()) {
      return expected<ConsumerHandle, ShmError>::error(closed.get_error());
    }
    return RegisterConsumer(config);
  }

  /// @brief Number of active consumers.
  uint32_t ConsumerCount() const noexcept { return header_->consumer_count.load(std::memory_order_relaxed); }

  // ---- Producer API ----

  /**
   * @brief Write a length-prefixed message: [4B len][payload].
   * Checks the slowest active consumer to prevent overwrite.
   * @return true if successful, false if not enough space.
   */
  bool Write(const void* data, uint32_t len) noexcept {
    // Validate before computing len + 4: a wrapped total would pass the
    // capacity check and turn into an out-of-bounds memcpy.
    if (len == 0U || header_->capacity <= 4U || len > header_->capacity - 4U) {
      return false;
    }
    const uint32_t total = len + 4U;
    if (WriteableBytes() < total) {
      (void)DetectUnderflow();
      return false;
    }
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    WriteRaw(head, &len, 4);
    WriteRaw(head + 4, data, len);
    header_->head.store(head + total, std::memory_order_release);
    return true;
  }

  /**
   * @brief Checked write. On full, applies ReapPolicy before reporting kFull.
   * @return kInvalidArgument for zero length, kMessageTooLarge beyond capacity.
   */
  expected<void, ShmError> WriteChecked(const void* data, uint32_t len,
                                        const ShmChannelConfig& cfg = ShmChannelConfig{}) noexcept {
    if (header_ == nullptr) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    if (len == 0U) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    if (header_->capacity <= 4U || len > header_->capacity - 4U) {
      return expected<void, ShmError>::error(ShmError::kMessageTooLarge);
    }
    const uint32_t total = len + 4U;
    if (WriteableBytes() < total) {
      if (DetectUnderflow()) {
        return expected<void, ShmError>::error(ShmError::kCorrupt);
      }
      if (cfg.reap_policy == ReapPolicy::kOnFull) {
        (void)ReapStaleConsumers(SteadyNowUs(), cfg);
      }
    }
    if (WriteableBytes() < total) {
      ++write_stall_events_;
      ++dropped_newest_;
      return expected<void, ShmError>::error(ShmError::kFull);
    }
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    WriteRaw(head, &len, 4U);
    WriteRaw(head + 4U, data, len);
    header_->head.store(head + total, std::memory_order_release);
    return expected<void, ShmError>::success();
  }

  /// @brief Available bytes for writing (limited by slowest consumer).
  uint32_t WriteableBytes() const noexcept {
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    const uint32_t slowest = SlowestTail();
    const uint32_t used = head - slowest;
    return (used >= header_->capacity) ? 0U : (header_->capacity - used);
  }

  // ---- Writer-side progress detection ----

  /**
   * @brief Reap dead consumers, and stuck live ones when explicitly allowed.
   * @param now_us Monotonic timestamp from SteadyNowUs().
   * @param cfg Thresholds; evict_alive_slow gates eviction of live consumers.
   * @return Number of slots reclaimed.
   */
  uint32_t ReapStaleConsumers(uint64_t now_us, const ShmChannelConfig& cfg = ShmChannelConfig{}) noexcept {
    if (header_ == nullptr) {
      return 0U;
    }
    uint32_t reaped = 0U;
    for (uint32_t i = 0; i < header_->max_consumers; ++i) {
      const uint32_t word = header_->slot_state[i].load(std::memory_order_acquire);
      if (ShmSpmcByteRingHeader::StateOf(word) != 1U) {
        continue;
      }
      const uint32_t generation = header_->generation[i].load(std::memory_order_acquire);
      const pid_t pid = static_cast<pid_t>(header_->pid[i].load(std::memory_order_acquire));
      const bool alive = IsProcessAlive(pid);
      if (!alive) {
        // Dead consumers are judged immediately; the generation CAS inside
        // TryReap still protects against a slot reused during this scan.
        if (TryReap(i, generation, true)) {
          ++reaped;
          observed_generation_[i] = header_->generation[i].load(std::memory_order_acquire);
          observed_tail_[i] = header_->tails[i].load(std::memory_order_acquire);
          last_progress_us_[i] = now_us;
        }
        continue;
      }
      if (generation != observed_generation_[i]) {
        // New consumer generation: restart stall tracking without judging it.
        observed_generation_[i] = generation;
        observed_tail_[i] = header_->tails[i].load(std::memory_order_acquire);
        last_progress_us_[i] = now_us;
        continue;
      }
      const uint32_t tail = header_->tails[i].load(std::memory_order_acquire);
      if (tail != observed_tail_[i]) {
        observed_tail_[i] = tail;
        last_progress_us_[i] = now_us;
        // A consumer that resumes must stop being reported as stuck.
        header_->flags[i].fetch_and(~kFlagStuck, std::memory_order_release);
        continue;
      }
      const bool backlog = (header_->head.load(std::memory_order_acquire) != tail);
      const bool stalled = (now_us - last_progress_us_[i]) >= cfg.stale_timeout_us;
      if (!backlog || !stalled) {
        continue;
      }
      // An alive but idle consumer is defective, not dead: mark it and only
      // evict when the deployment explicitly accepts the data loss.
      const uint32_t previous_flags = header_->flags[i].fetch_or(kFlagStuck, std::memory_order_release);
      if ((previous_flags & kFlagStuck) == 0U) {
        ++consumer_stuck_total_;
        ReportFault(ShmError::kConsumerStuck, i, now_us - last_progress_us_[i]);
      }
      if (!cfg.evict_alive_slow) {
        continue;
      }
      if (TryReap(i, generation, false)) {
        ++reaped;
        observed_generation_[i] = header_->generation[i].load(std::memory_order_acquire);
        observed_tail_[i] = header_->tails[i].load(std::memory_order_acquire);
        last_progress_us_[i] = now_us;
      }
    }
    return reaped;
  }

  /// @brief Supervisor entry: clean transient orphans, then scan consumers once.
  expected<void, ShmError> Tick(uint64_t now_us, const ShmChannelConfig& cfg = ShmChannelConfig{}) noexcept {
    if (header_ == nullptr) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    ReapOrphans(now_us, cfg.grace_us);
    (void)ReapStaleConsumers(now_us, cfg);
    return expected<void, ShmError>::success();
  }

  /**
   * @brief Reset all consumer state. The caller must stop every endpoint first.
   * @return kInvalidState when a slot is transient, being read, or generation
   *         exhausted; nothing is modified in that case.
   */
  expected<void, ShmError> ResetAfterQuiesce() noexcept {
    if (header_ == nullptr) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    const uint32_t n = header_->max_consumers;
    uint32_t snapshot[OSP_SHM_SPMC_MAX_CONSUMERS];
    uint32_t snapshot_generation[OSP_SHM_SPMC_MAX_CONSUMERS];
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t word = header_->slot_state[i].load(std::memory_order_acquire);
      const uint32_t state = ShmSpmcByteRingHeader::StateOf(word);
      const uint32_t generation = header_->generation[i].load(std::memory_order_acquire);
      if ((state != 0U && state != 1U) || ShmSpmcByteRingHeader::GuardOf(word) != 0U || generation == UINT32_MAX) {
        return expected<void, ShmError>::error(ShmError::kInvalidState);
      }
      snapshot[i] = word;
      snapshot_generation[i] = generation;
    }
    // Occupy every slot, including free ones, so a racing registration cannot
    // slip in and be wiped by the commit phase.
    for (uint32_t i = 0; i < n; ++i) {
      if (header_->generation[i].load(std::memory_order_acquire) != snapshot_generation[i]) {
        RollbackReset(snapshot, i);
        return expected<void, ShmError>::error(ShmError::kInvalidState);
      }
      uint32_t expected_word = snapshot[i];
      if (!header_->slot_state[i].compare_exchange_strong(expected_word, ShmSpmcByteRingHeader::Pack(3U, 0U),
                                                          std::memory_order_acq_rel, std::memory_order_relaxed)) {
        RollbackReset(snapshot, i);
        return expected<void, ShmError>::error(ShmError::kInvalidState);
      }
      // Close the free -> register -> free ABA window: a generation change
      // after the CAS means this slot now belongs to a new consumer.
      if (header_->generation[i].load(std::memory_order_acquire) != snapshot_generation[i]) {
        uint32_t occupied = ShmSpmcByteRingHeader::Pack(3U, 0U);
        (void)header_->slot_state[i].compare_exchange_strong(occupied, snapshot[i], std::memory_order_acq_rel,
                                                             std::memory_order_relaxed);
        RollbackReset(snapshot, i);
        return expected<void, ShmError>::error(ShmError::kInvalidState);
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      header_->slot_state[i].store(0U, std::memory_order_release);
      header_->generation[i].fetch_add(1U, std::memory_order_release);
      header_->tails[i].store(0U, std::memory_order_relaxed);
      header_->flags[i].store(0U, std::memory_order_relaxed);
      header_->pid[i].store(0U, std::memory_order_relaxed);
      observed_generation_[i] = 0U;
      observed_tail_[i] = 0U;
      last_progress_us_[i] = 0U;
      transient_since_us_[i] = 0U;
    }
    header_->consumer_count.store(0U, std::memory_order_release);
    header_->head.store(0U, std::memory_order_release);
    header_->space_seq.store(0U, std::memory_order_release);
    detail::FutexWakeAll(&header_->consumer_count);
    detail::FutexWakeAll(&header_->head);
    detail::FutexWakeAll(&header_->space_seq);
    reaped_total_ = 0U;
    evicted_total_ = 0U;
    read_evicted_total_ = 0U;
    reap_deferred_total_ = 0U;
    read_conflict_total_ = 0U;
    consumer_stuck_total_ = 0U;
    orphan_state2_total_ = 0U;
    orphan_state3_total_ = 0U;
    generation_wrap_total_ = 0U;
    write_stall_events_ = 0U;
    wait_timeouts_ = 0U;
    corrupt_total_ = 0U;
    dropped_newest_ = 0U;
    last_error_ = ShmError::kNone;
    return expected<void, ShmError>::success();
  }

  // ---- Wait plane ----

  /**
   * @brief Wait until a payload of @p needed bytes plus its 4-byte length prefix
   *        can be written.
   * @param needed Payload bytes required, in 1..Capacity()-4.
   * @param timeout_ms Relative timeout; 0 degenerates to a single check.
   */
  expected<void, ShmError> WaitWritable(uint32_t needed, uint32_t timeout_ms) noexcept {
    if (header_ == nullptr) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    if (needed == 0U || header_->capacity <= 4U || needed > header_->capacity - 4U) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    const uint32_t wire = needed + 4U;
    const uint64_t deadline = SteadyNowUs() + static_cast<uint64_t>(timeout_ms) * 1000U;
    for (;;) {
      if (WriteableBytes() >= wire) {
        return expected<void, ShmError>::success();
      }
      // Read the wait word first; a tail advance in between yields EAGAIN.
      const uint32_t seq = header_->space_seq.load(std::memory_order_acquire);
      if (WriteableBytes() >= wire) {
        return expected<void, ShmError>::success();
      }
      if (timeout_ms == 0U) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
#if OSP_SHM_WAIT_SPIN_US > 0
      const uint64_t spin_end = SteadyNowUs() + OSP_SHM_WAIT_SPIN_US;
      while (SteadyNowUs() < spin_end) {
        if (WriteableBytes() >= wire) {
          return expected<void, ShmError>::success();
        }
      }
#endif
      const uint32_t remaining = detail::RemainingMsCeil(deadline);
      if (remaining == 0U) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      const int outcome = detail::FutexWaitOutcome(detail::FutexWait(&header_->space_seq, seq, remaining));
      if (outcome == 1) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      if (outcome == 2) {
        return expected<void, ShmError>::error(ShmError::kWaitInterrupted);
      }
    }
  }

  /// @brief Wait until at least @p count consumers are registered.
  expected<void, ShmError> WaitConsumers(uint32_t count, uint32_t timeout_ms) noexcept {
    if (header_ == nullptr) {
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    if (count == 0U || count > header_->max_consumers) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    const uint64_t deadline = SteadyNowUs() + static_cast<uint64_t>(timeout_ms) * 1000U;
    for (;;) {
      if (header_->consumer_count.load(std::memory_order_acquire) >= count) {
        return expected<void, ShmError>::success();
      }
      const uint32_t observed = header_->consumer_count.load(std::memory_order_acquire);
      if (header_->consumer_count.load(std::memory_order_acquire) >= count) {
        return expected<void, ShmError>::success();
      }
      if (timeout_ms == 0U) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      const uint32_t remaining = detail::RemainingMsCeil(deadline);
      if (remaining == 0U) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      const int outcome = detail::FutexWaitOutcome(detail::FutexWait(&header_->consumer_count, observed, remaining));
      if (outcome == 1) {
        ++wait_timeouts_;
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      if (outcome == 2) {
        return expected<void, ShmError>::error(ShmError::kWaitInterrupted);
      }
    }
  }

  // ---- Consumer API ----

  /**
   * @brief Generation-checked read. Returns kEvicted once the slot is reaped.
   * The caller must serialize all reads that share one handle.
   */
  expected<uint32_t, ShmError> ReadChecked(ConsumerHandle handle, void* out, uint32_t max_len) noexcept {
    if (handle.id >= header_->max_consumers) {
      return expected<uint32_t, ShmError>::error(ShmError::kInvalidArgument);
    }
    const uint32_t idx = handle.id;
    const uint32_t word = header_->slot_state[idx].load(std::memory_order_acquire);
    if (ShmSpmcByteRingHeader::StateOf(word) != 1U) {
      ++read_evicted_total_;
      return expected<uint32_t, ShmError>::error(ShmError::kEvicted);
    }
    if (ShmSpmcByteRingHeader::GuardOf(word) != 0U) {
      ++read_conflict_total_;
      return expected<uint32_t, ShmError>::error(ShmError::kInvalidState);
    }
    uint32_t expected_word = ShmSpmcByteRingHeader::Pack(1U, 0U);
    if (!header_->slot_state[idx].compare_exchange_strong(expected_word, ShmSpmcByteRingHeader::Pack(1U, 1U),
                                                          std::memory_order_acq_rel, std::memory_order_relaxed)) {
      if (ShmSpmcByteRingHeader::StateOf(expected_word) == 1U) {
        ++read_conflict_total_;
        return expected<uint32_t, ShmError>::error(ShmError::kInvalidState);
      }
      ++read_evicted_total_;
      return expected<uint32_t, ShmError>::error(ShmError::kEvicted);
    }
    if (header_->generation[idx].load(std::memory_order_acquire) != handle.generation) {
      ReleaseGuard(idx);
      ++read_evicted_total_;
      return expected<uint32_t, ShmError>::error(ShmError::kEvicted);
    }
    const uint32_t tail = header_->tails[idx].load(std::memory_order_relaxed);
    const uint32_t head = header_->head.load(std::memory_order_acquire);
    const uint32_t available = head - tail;
    if (available < 4U) {
      ReleaseGuard(idx);
      return expected<uint32_t, ShmError>::error(ShmError::kEmpty);
    }
    uint32_t msg_len = 0U;
    ReadRaw(tail, &msg_len, 4U);
    if (msg_len == 0U || msg_len > available - 4U) {
      const uint32_t previous_flags = header_->flags[idx].fetch_or(kFlagCorrupt, std::memory_order_release);
      if ((previous_flags & kFlagCorrupt) == 0U) {
        ++corrupt_total_;
        MarkFault(ShmError::kCorrupt);
        ReportFault(ShmError::kCorrupt, idx, msg_len);
      }
      ReleaseGuard(idx);
      return expected<uint32_t, ShmError>::error(ShmError::kCorrupt);
    }
    if (msg_len > max_len) {
      ReleaseGuard(idx);
      return expected<uint32_t, ShmError>::error(ShmError::kMessageTooLarge);
    }
    ReadRaw(tail + 4U, out, msg_len);
    // Re-check before publishing tail: a reaper may have taken the slot while
    // we were copying only if the guard protocol was violated.
    const uint32_t check = header_->slot_state[idx].load(std::memory_order_acquire);
    if (ShmSpmcByteRingHeader::StateOf(check) != 1U ||
        header_->generation[idx].load(std::memory_order_acquire) != handle.generation) {
      ReleaseGuard(idx);
      ++read_evicted_total_;
      return expected<uint32_t, ShmError>::error(ShmError::kEvicted);
    }
    header_->tails[idx].store(tail + msg_len + 4U, std::memory_order_release);
    NotifyTailAdvanced();
    ReleaseGuard(idx);
    return expected<uint32_t, ShmError>::success(msg_len);
  }

  /// @brief Available bytes for reading for a specific consumer.
  uint32_t ReadableBytes(ConsumerHandle handle) const noexcept {
    if (handle.id >= header_->max_consumers) {
      return 0;
    }
    const uint32_t idx = handle.id;
    const uint32_t tail = header_->tails[idx].load(std::memory_order_relaxed);
    const uint32_t head = header_->head.load(std::memory_order_acquire);
    return head - tail;
  }

  /// @brief Check if consumer has at least one complete message header.
  bool HasData(ConsumerHandle handle) const noexcept { return ReadableBytes(handle) >= 4U; }

  /// @brief Data area capacity in bytes.
  uint32_t Capacity() const noexcept { return header_ ? header_->capacity : 0; }

  /// @brief Pointer to the head field (for futex wait/wake).
  std::atomic<uint32_t>* HeadPtr() noexcept { return &header_->head; }

  /// @brief Maximum consumers supported.
  uint32_t MaxConsumers() const noexcept { return header_ ? header_->max_consumers : 0; }

  /// @brief Install a fault reporter; events fire only on state transitions.
  void SetFaultReporter(ShmFaultReporter reporter, void* ctx) noexcept {
    fault_reporter_ = reporter;
    fault_ctx_ = ctx;
  }

  /// @brief Lock-free health snapshot. Call from the writer thread.
  ShmHealthSnapshot Health() const noexcept {
    ShmHealthSnapshot snapshot{};
    if (header_ == nullptr) {
      snapshot.state = ShmState::kFaulted;
      snapshot.last_error = ShmError::kInvalidState;
      return snapshot;
    }
    const uint64_t now_us = SteadyNowUs();
    uint32_t transient_slots = 0U;
    snapshot.reap_epoch = header_->reap_epoch.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < header_->max_consumers; ++i) {
      const uint32_t word = header_->slot_state[i].load(std::memory_order_acquire);
      const uint32_t state = ShmSpmcByteRingHeader::StateOf(word);
      if (state == 2U || state == 3U) {
        ++transient_slots;
        continue;
      }
      if (state != 1U) {
        continue;
      }
      ++snapshot.active_consumers;
      if ((header_->flags[i].load(std::memory_order_acquire) & kFlagStuck) != 0U) {
        ++snapshot.stale_consumers;
      }
      const uint32_t tail = header_->tails[i].load(std::memory_order_acquire);
      if (tail != header_->head.load(std::memory_order_acquire)) {
        const uint64_t since = last_progress_us_[i];
        const uint64_t pending = (since == 0U || now_us < since) ? 0U : (now_us - since);
        if (pending > snapshot.oldest_pending_us) {
          snapshot.oldest_pending_us = pending;
        }
      }
    }
    snapshot.reaped_total = reaped_total_;
    snapshot.evicted_total = evicted_total_;
    snapshot.read_evicted_total = read_evicted_total_;
    snapshot.reap_deferred_total = reap_deferred_total_;
    snapshot.read_conflict_total = read_conflict_total_;
    snapshot.consumer_stuck_total = consumer_stuck_total_;
    snapshot.orphan_state2_total = orphan_state2_total_;
    snapshot.orphan_state3_total = orphan_state3_total_;
    snapshot.generation_wrap_total = generation_wrap_total_;
    snapshot.write_stall_events = write_stall_events_;
    snapshot.wait_timeouts = wait_timeouts_;
    snapshot.corrupt_total = corrupt_total_;
    snapshot.dropped_newest = dropped_newest_;
    snapshot.last_error = last_error_;
    if (last_error_ == ShmError::kCorrupt || last_error_ == ShmError::kInvalidState) {
      snapshot.state = ShmState::kFaulted;
    } else if (snapshot.stale_consumers > 0U || transient_slots > 0U) {
      snapshot.state = ShmState::kDegraded;
    }
    return snapshot;
  }

 private:
  /// @brief Record the error that put the endpoint into the faulted state.
  void MarkFault(ShmError code) noexcept { last_error_ = code; }

  /// @brief Deliver a fault event outside the hot path.
  void ReportFault(ShmError code, uint32_t consumer_id, uint64_t detail) noexcept {
    if (fault_reporter_ == nullptr) {
      return;
    }
    const auto* layout = reinterpret_cast<const ShmLayoutHeader*>(  // NOLINT
        reinterpret_cast<const uint8_t*>(header_) - kLayoutOffset);
    const ShmFaultEvent event{static_cast<uint16_t>(code), layout->layout_hash, consumer_id, detail};
    fault_reporter_(event, fault_ctx_);
  }

  /// @brief Find the slowest (minimum) tail among active consumers.
  uint32_t SlowestTail() const noexcept {
    uint32_t slowest = header_->head.load(std::memory_order_relaxed);  // If no consumers, full capacity
    for (uint32_t i = 0; i < header_->max_consumers; ++i) {
      if (ShmSpmcByteRingHeader::StateOf(header_->slot_state[i].load(std::memory_order_acquire)) == 1U) {
        const uint32_t t = header_->tails[i].load(std::memory_order_acquire);
        // Use signed comparison for monotonic wrap-around
        if (static_cast<int32_t>(slowest - t) > 0) {
          slowest = t;
        }
      }
    }
    return slowest;
  }

  /// @brief Treat head - slowest > capacity as corruption and stop writing.
  bool DetectUnderflow() noexcept {
    if (last_error_ == ShmError::kCorrupt) {
      return true;  // already faulted and reported
    }
    const uint32_t head = header_->head.load(std::memory_order_relaxed);
    const uint32_t used = head - SlowestTail();
    if (used <= header_->capacity) {
      return false;
    }
    ++corrupt_total_;
    MarkFault(ShmError::kCorrupt);
    ReportFault(ShmError::kCorrupt, UINT32_MAX, used);
    return true;
  }

  /// @brief Restore occupied slots [0, count) to their pre-reset words.
  void RollbackReset(const uint32_t* snapshot, uint32_t count) noexcept {
    for (uint32_t j = 0; j < count; ++j) {
      uint32_t expected = ShmSpmcByteRingHeader::Pack(3U, 0U);
      (void)header_->slot_state[j].compare_exchange_strong(expected, snapshot[j], std::memory_order_acq_rel,
                                                           std::memory_order_relaxed);
    }
  }

  /// @brief Reap one slot. force=true is only used for a confirmed-dead owner.
  bool TryReap(uint32_t idx, uint32_t generation, bool force) noexcept {
    const uint32_t desired = ShmSpmcByteRingHeader::Pack(3U, 0U);
    if (force) {
      // The owner is gone, so a non-zero guard can never be released.
      uint32_t word = header_->slot_state[idx].load(std::memory_order_acquire);
      for (;;) {
        if (ShmSpmcByteRingHeader::StateOf(word) != 1U) {
          return false;
        }
        if (header_->slot_state[idx].compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
          break;
        }
      }
    } else {
      uint32_t expected = ShmSpmcByteRingHeader::Pack(1U, 0U);
      if (!header_->slot_state[idx].compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                            std::memory_order_relaxed)) {
        ++reap_deferred_total_;
        return false;  // A reader holds the guard: defer to the next scan.
      }
    }
    if (header_->generation[idx].load(std::memory_order_acquire) != generation) {
      // A new consumer registered between the CAS and the commit: roll back.
      uint32_t expected = desired;
      (void)header_->slot_state[idx].compare_exchange_strong(expected, ShmSpmcByteRingHeader::Pack(1U, 0U),
                                                             std::memory_order_acq_rel, std::memory_order_relaxed);
      return false;
    }
    header_->tails[idx].store(header_->head.load(std::memory_order_acquire), std::memory_order_release);
    header_->generation[idx].store(generation + 1U, std::memory_order_release);
    header_->flags[idx].fetch_or(kFlagEvicted, std::memory_order_release);
    header_->consumer_count.fetch_sub(1U, std::memory_order_release);
    detail::FutexWakeAll(&header_->consumer_count);
    header_->reap_epoch.fetch_add(1U, std::memory_order_relaxed);
    header_->slot_state[idx].store(ShmSpmcByteRingHeader::Pack(0U, 0U), std::memory_order_release);
    NotifyTailAdvanced();
    ++reaped_total_;
    ++evicted_total_;
    ReportFault(ShmError::kEvicted, idx, generation);
    return true;
  }

  /// @brief Recycle slots stuck in registering (2) or reaping (3) past grace.
  void ReapOrphans(uint64_t now_us, uint32_t grace_us) noexcept {
    for (uint32_t i = 0; i < header_->max_consumers; ++i) {
      const uint32_t word = header_->slot_state[i].load(std::memory_order_acquire);
      const uint32_t state = ShmSpmcByteRingHeader::StateOf(word);
      if (state != 2U && state != 3U) {
        transient_since_us_[i] = 0U;
        continue;
      }
      if (transient_since_us_[i] == 0U) {
        transient_since_us_[i] = (now_us == 0U) ? 1U : now_us;
        continue;
      }
      if ((now_us - transient_since_us_[i]) < grace_us) {
        continue;
      }
      uint32_t expected = word;
      if (header_->slot_state[i].compare_exchange_strong(expected, 0U, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
        if (state == 2U) {
          ++orphan_state2_total_;
        } else {
          ++orphan_state3_total_;
        }
        header_->generation[i].fetch_add(1U, std::memory_order_acq_rel);
        ReportFault(ShmError::kInvalidState, i, word);
        transient_since_us_[i] = 0U;
      }
    }
  }

  /// @brief Drop the read guard; the slot must still be in state 1.
  void ReleaseGuard(uint32_t idx) noexcept {
    uint32_t expected = ShmSpmcByteRingHeader::Pack(1U, 1U);
    (void)header_->slot_state[idx].compare_exchange_strong(expected, ShmSpmcByteRingHeader::Pack(1U, 0U),
                                                           std::memory_order_acq_rel, std::memory_order_relaxed);
  }

  /// @brief Publish a tail advance to the wait plane (wakes one writer).
  void NotifyTailAdvanced() noexcept {
    header_->space_seq.fetch_add(1U, std::memory_order_release);
    detail::FutexWake(&header_->space_seq);
  }

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

  static uint32_t RoundDownPow2(uint32_t v) noexcept {
    if (v == 0)
      return 0;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return (v >> 1) + 1;
  }

  ShmSpmcByteRingHeader* header_;
  uint8_t* data_;
  uint32_t mask_;
  uint32_t observed_generation_[OSP_SHM_SPMC_MAX_CONSUMERS]{};
  uint32_t observed_tail_[OSP_SHM_SPMC_MAX_CONSUMERS]{};
  uint64_t last_progress_us_[OSP_SHM_SPMC_MAX_CONSUMERS]{};
  uint64_t transient_since_us_[OSP_SHM_SPMC_MAX_CONSUMERS]{};
  uint64_t reaped_total_{0};
  uint64_t evicted_total_{0};
  uint64_t read_evicted_total_{0};
  uint64_t reap_deferred_total_{0};
  uint64_t read_conflict_total_{0};
  uint64_t consumer_stuck_total_{0};
  uint64_t orphan_state2_total_{0};
  uint64_t orphan_state3_total_{0};
  uint64_t generation_wrap_total_{0};
  uint64_t write_stall_events_{0};
  uint64_t wait_timeouts_{0};
  uint64_t corrupt_total_{0};
  uint64_t dropped_newest_{0};
  ShmError last_error_{ShmError::kNone};
  ShmFaultReporter fault_reporter_{nullptr};
  void* fault_ctx_{nullptr};
};

// ============================================================================
// ShmSpmcByteChannel - SPMC byte-stream channel with futex notification
// ============================================================================

/**
 * @brief Named SPMC byte-stream channel for data distribution.
 *
 * One producer writes data, multiple consumers each independently read
 * the full data stream. Ideal for sensor data distribution (e.g. LiDAR
 * point clouds to logging, fusion, and visualization subscribers).
 *
 * Writer calls Write() (auto-notifies all waiters via futex).
 * Each reader calls OpenReader() which auto-registers a consumer slot.
 * Reader calls WaitReadable() + Read().
 */
class ShmSpmcByteChannel final {
 public:
  ShmSpmcByteChannel() noexcept : is_writer_(false) { config_.reap_policy = ReapPolicy::kOnFull; }

  ~ShmSpmcByteChannel() {
    if (!is_writer_ && has_consumer_ && ring_.Capacity() > 0U) {
      (void)ring_.UnregisterConsumer(consumer_);
    }
  }

  // Non-copyable, movable
  ShmSpmcByteChannel(const ShmSpmcByteChannel&) = delete;
  ShmSpmcByteChannel& operator=(const ShmSpmcByteChannel&) = delete;

  ShmSpmcByteChannel(ShmSpmcByteChannel&& other) noexcept
      : shm_segment_(static_cast<SharedMemorySegment&&>(other.shm_segment_)),
        ring_(other.ring_),
        is_writer_(other.is_writer_),
        consumer_(other.consumer_),
        has_consumer_(other.has_consumer_),
        config_(other.config_) {
    other.ring_ = ShmSpmcByteRing();
    other.has_consumer_ = false;
  }

  ShmSpmcByteChannel& operator=(ShmSpmcByteChannel&& other) noexcept {
    if (this != &other) {
      // Unregister current consumer if active
      if (!is_writer_ && has_consumer_ && ring_.Capacity() > 0U) {
        (void)ring_.UnregisterConsumer(consumer_);
      }
      shm_segment_ = static_cast<SharedMemorySegment&&>(other.shm_segment_);
      ring_ = other.ring_;
      is_writer_ = other.is_writer_;
      consumer_ = other.consumer_;
      has_consumer_ = other.has_consumer_;
      config_ = other.config_;
      other.ring_ = ShmSpmcByteRing();
      other.has_consumer_ = false;
    }
    return *this;
  }

  /**
   * @brief Create a writer endpoint.
   * @param name Channel name.
   * @param capacity Data area capacity in bytes.
   * @param max_consumers Maximum number of consumers.
   */
  static expected<ShmSpmcByteChannel, ShmError> CreateWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY,
      uint32_t max_consumers = OSP_SHM_SPMC_MAX_CONSUMERS) noexcept {
    ShmSpmcByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpmcByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::Create(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpmcByteRing::InitAt(ch.shm_segment_.Data(), ch.shm_segment_.Size(), max_consumers);
    return expected<ShmSpmcByteChannel, ShmError>::success(static_cast<ShmSpmcByteChannel&&>(ch));
  }

  /**
   * @brief Create a writer endpoint, replacing only a dead owner's channel.
   */
  static expected<ShmSpmcByteChannel, ShmError> CreateOrReplaceWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY,
      uint32_t max_consumers = OSP_SHM_SPMC_MAX_CONSUMERS) noexcept {
    ShmSpmcByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpmcByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::CreateOrReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpmcByteRing::InitAt(ch.shm_segment_.Data(), ch.shm_segment_.Size(), max_consumers);
    return expected<ShmSpmcByteChannel, ShmError>::success(static_cast<ShmSpmcByteChannel&&>(ch));
  }

  /// Explicitly unlink and recreate the channel. Use only after confirming the
  /// previous writer is dead.
  static expected<ShmSpmcByteChannel, ShmError> ForceReplaceWriter(
      const char* name, uint32_t capacity = OSP_SHM_BYTE_RING_CAPACITY,
      uint32_t max_consumers = OSP_SHM_SPMC_MAX_CONSUMERS) noexcept {
    ShmSpmcByteChannel ch;
    ch.is_writer_ = true;
    uint32_t shm_size = ShmSpmcByteRing::RequiredSize(capacity);
    auto result = SharedMemorySegment::ForceReplace(name, shm_size);
    if (!result.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    ch.ring_ = ShmSpmcByteRing::InitAt(ch.shm_segment_.Data(), ch.shm_segment_.Size(), max_consumers);
    return expected<ShmSpmcByteChannel, ShmError>::success(static_cast<ShmSpmcByteChannel&&>(ch));
  }

  /**
   * @brief Open a reader endpoint. Auto-registers a consumer slot.
   * @param name Channel name.
   * @return Channel on success, kFull if max consumers reached.
   */
  static expected<ShmSpmcByteChannel, ShmError> OpenReader(const char* name) noexcept {
    ShmSpmcByteChannel ch;
    ch.is_writer_ = false;
    auto result = SharedMemorySegment::Open(name);
    if (!result.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<SharedMemorySegment&&>(result.value());
    auto attached = ShmSpmcByteRing::AttachChecked(ch.shm_segment_.Data(), ch.shm_segment_.Size());
    if (!attached.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(attached.get_error());
    }
    ch.ring_ = attached.value();
    auto registered = ch.ring_.RegisterConsumer(ch.config_);
    if (!registered.has_value()) {
      return expected<ShmSpmcByteChannel, ShmError>::error(registered.get_error());
    }
    ch.consumer_ = registered.value();
    ch.has_consumer_ = true;
    return expected<ShmSpmcByteChannel, ShmError>::success(static_cast<ShmSpmcByteChannel&&>(ch));
  }

  /**
   * @brief Write data and notify all waiting readers via futex.
   */
  expected<void, ShmError> Write(const void* data, uint32_t size) noexcept {
    auto written = ring_.WriteChecked(data, size, config_);
    if (!written.has_value()) {
      return written;
    }
    detail::FutexWakeAll(ring_.HeadPtr());
    return expected<void, ShmError>::success();
  }

  /**
   * @brief Read one message (consumer only).
   * @param data Buffer to receive payload.
   * @param max_len Maximum payload size.
   * @return Payload length, or 0 if no data.
   */
  expected<uint32_t, ShmError> Read(void* data, uint32_t max_len) noexcept {
    if (!has_consumer_) {
      return expected<uint32_t, ShmError>::error(ShmError::kInvalidState);
    }
    return ring_.ReadChecked(consumer_, data, max_len);
  }

  /**
   * @brief Wait for data using futex (microsecond-level latency).
   * @param timeout_ms Timeout in milliseconds.
   */
  expected<void, ShmError> WaitReadable(uint32_t timeout_ms) noexcept {
    const uint64_t deadline = SteadyNowUs() + static_cast<uint64_t>(timeout_ms) * 1000U;
    for (;;) {
      if (ring_.HasData(consumer_)) {
        return expected<void, ShmError>::success();
      }
      // Read the wait word first, then re-check: a producer that publishes
      // between the two makes the futex return EAGAIN instead of sleeping.
      const uint32_t head = ring_.HeadPtr()->load(std::memory_order_acquire);
      if (ring_.HasData(consumer_)) {
        return expected<void, ShmError>::success();
      }
      if (timeout_ms == 0U) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
#if OSP_SHM_WAIT_SPIN_US > 0
      const uint64_t spin_end = SteadyNowUs() + OSP_SHM_WAIT_SPIN_US;
      while (SteadyNowUs() < spin_end) {
        if (ring_.HasData(consumer_)) {
          return expected<void, ShmError>::success();
        }
      }
#endif
      const uint32_t remaining = detail::RemainingMsCeil(deadline);
      if (remaining == 0U) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      const int outcome = detail::FutexWaitOutcome(detail::FutexWait(ring_.HeadPtr(), head, remaining));
      if (outcome == 1) {
        return expected<void, ShmError>::error(ShmError::kTimeout);
      }
      if (outcome == 2) {
        return expected<void, ShmError>::error(ShmError::kWaitInterrupted);
      }
    }
  }

  /// @brief Supervisor tick for this channel's reaping policy.
  expected<void, ShmError> Tick(uint64_t now_us) noexcept { return ring_.Tick(now_us, config_); }

  /// @brief Wait until the ring can accept @p needed payload bytes.
  expected<void, ShmError> WaitWritable(uint32_t needed, uint32_t timeout_ms) noexcept {
    return ring_.WaitWritable(needed, timeout_ms);
  }

  /// @brief Wait until at least @p count readers are registered.
  expected<void, ShmError> WaitConsumers(uint32_t count, uint32_t timeout_ms) noexcept {
    return ring_.WaitConsumers(count, timeout_ms);
  }

  /// @brief Notify all waiting readers (explicit, for batch writes).
  void Notify() noexcept { detail::FutexWakeAll(ring_.HeadPtr()); }

  /// @brief Unlink the shared memory segment (writer only).
  void Unlink() noexcept {
    if (is_writer_) {
      shm_segment_.Unlink();
    }
  }

  /// @brief Available bytes for reading (this consumer).
  uint32_t ReadableBytes() const noexcept { return ring_.ReadableBytes(consumer_); }

  /// @brief Available bytes for writing (producer only).
  uint32_t WriteableBytes() const noexcept { return ring_.WriteableBytes(); }

  /// @brief Data area capacity.
  uint32_t Capacity() const noexcept { return ring_.Capacity(); }

  /// @brief Number of active consumers.
  uint32_t ConsumerCount() const noexcept { return ring_.ConsumerCount(); }

  /// @brief Health snapshot for this channel (writer side).
  ShmHealthSnapshot Health() const noexcept { return ring_.Health(); }

  /// @brief Install the fault reporter used by the underlying ring.
  void SetFaultReporter(ShmFaultReporter reporter, void* ctx) noexcept { ring_.SetFaultReporter(reporter, ctx); }

  /// @brief This reader's consumer ID (-1 if writer).
  int32_t ConsumerId() const noexcept { return has_consumer_ ? static_cast<int32_t>(consumer_.id) : -1; }

  /// @brief Generation-checked handle for this reader (invalid for writers).
  ConsumerHandle Handle() const noexcept { return consumer_; }

  /// @brief Segment epoch (changes when CreateOrReplace rebuilds the segment).
  uint64_t Epoch() const noexcept { return shm_segment_.Epoch(); }

 private:
  SharedMemorySegment shm_segment_;
  ShmSpmcByteRing ring_;
  bool is_writer_;
  ConsumerHandle consumer_{};
  bool has_consumer_{false};
  ShmChannelConfig config_{};
};

// ============================================================================
// Block lease adapters: ShmSpmcByteChannel carries a ShmBlockRef, the pool
// (DataDispatcher / ShmDispatcher) owns the payload and the refcount.
// ============================================================================

/// @brief Move-only RAII lease over one pool block.
template <typename DispatcherT>
class BlockLease final {
 public:
  BlockLease() noexcept = default;

  BlockLease(DispatcherT* pool, int32_t pool_slot, ShmBlockRef ref) noexcept
      : pool_(pool),
        pool_slot_(pool_slot),
        ref_(ref),
        data_((pool != nullptr) ? pool->GetReadable(ref.block_id, ref.block_generation) : nullptr) {}

  ~BlockLease() { Release(); }

  BlockLease(const BlockLease&) = delete;
  BlockLease& operator=(const BlockLease&) = delete;

  BlockLease(BlockLease&& other) noexcept
      : pool_(other.pool_), pool_slot_(other.pool_slot_), ref_(other.ref_), data_(other.data_) {
    other.pool_ = nullptr;
    other.data_ = nullptr;
  }

  BlockLease& operator=(BlockLease&& other) noexcept {
    if (this != &other) {
      Release();
      pool_ = other.pool_;
      pool_slot_ = other.pool_slot_;
      ref_ = other.ref_;
      data_ = other.data_;
      other.pool_ = nullptr;
      other.data_ = nullptr;
    }
    return *this;
  }

  const uint8_t* Data() const noexcept { return data_; }
  uint32_t Size() const noexcept { return (data_ != nullptr) ? ref_.size : 0U; }
  uint32_t Id() const noexcept { return ref_.block_id; }
  uint32_t Generation() const noexcept { return ref_.block_generation; }
  explicit operator bool() const noexcept { return data_ != nullptr; }

  void Release() noexcept {
    if (pool_ != nullptr && data_ != nullptr) {
      bool owns_reference = true;
      if (pool_slot_ >= 0) {
        // Whoever clears the hold bit owns the reference. A concurrent
        // RevokeConsumer that already claimed it must not be released twice.
        owns_reference = pool_->ReleaseBlockHold(pool_slot_, ref_.block_id);
      }
      if (owns_reference) {
        (void)pool_->Release(ref_.block_id, ref_.block_generation);
      }
    }
    pool_ = nullptr;
    data_ = nullptr;
  }

 private:
  DispatcherT* pool_{nullptr};
  int32_t pool_slot_{-1};
  ShmBlockRef ref_{};
  const uint8_t* data_{nullptr};
};

/// @brief Producer side: copy one payload into the pool and publish its ref.
template <typename DispatcherT>
class ShmLeaseProducer final {
 public:
  ShmLeaseProducer(ShmSpmcByteChannel& channel, DispatcherT& pool, uint32_t pool_hash = 0U) noexcept
      : channel_(channel), pool_(pool), pool_hash_(pool_hash) {}

  /**
   * @brief Publish @p size bytes as a leased block.
   * @param pool_slots Consumer pool slots whose holds are recorded now, so a
   *        consumer that dies before acquiring the lease cannot leak the block.
   * @param consumer_count Number of consumers expected to release the block.
   */
  expected<void, ShmError> Publish(const void* data, uint32_t size, const int32_t* pool_slots,
                                   uint32_t consumer_count) noexcept {
    if (data == nullptr || size == 0U || consumer_count == 0U || pool_slots == nullptr ||
        consumer_count > OSP_JOB_MAX_CONSUMERS) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    if (size > DispatcherT::PayloadCapacity()) {
      return expected<void, ShmError>::error(ShmError::kMessageTooLarge);
    }
    // One reference per distinct consumer slot: duplicates would add refcounts
    // that no hold bit can ever release.
    int32_t unique_slots[OSP_JOB_MAX_CONSUMERS];
    uint32_t unique_count = 0U;
    for (uint32_t i = 0; i < consumer_count; ++i) {
      const int32_t slot = pool_slots[i];
      if (slot < 0 || slot >= static_cast<int32_t>(OSP_JOB_MAX_CONSUMERS)) {
        return expected<void, ShmError>::error(ShmError::kInvalidArgument);
      }
      bool duplicate = false;
      for (uint32_t j = 0; j < unique_count; ++j) {
        if (unique_slots[j] == slot) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        unique_slots[unique_count++] = slot;
      }
    }
    if (unique_count == 0U) {
      return expected<void, ShmError>::error(ShmError::kInvalidArgument);
    }
    auto allocated = pool_.Alloc();
    if (!allocated.has_value()) {
      return expected<void, ShmError>::error(ShmError::kFull);
    }
    const uint32_t block_id = allocated.value();
    const uint32_t generation = pool_.GetGeneration(block_id);
    uint8_t* dst = pool_.GetWritable(block_id);
    if (dst == nullptr) {
      (void)pool_.Release(block_id, generation);
      return expected<void, ShmError>::error(ShmError::kInvalidState);
    }
    std::memcpy(dst, data, size);
    pool_.AddRef(block_id, unique_count);
    for (uint32_t i = 0; i < unique_count; ++i) {
      pool_.TrackBlockHold(unique_slots[i], block_id);
    }
    const ShmBlockRef ref{block_id, generation, size, pool_hash_};
    auto written = channel_.Write(&ref, sizeof(ref));
    if (!written.has_value()) {
      for (uint32_t i = 0; i < unique_count; ++i) {
        (void)pool_.TrackBlockRelease(unique_slots[i], block_id);
      }
      for (uint32_t i = 0; i < unique_count; ++i) {
        (void)pool_.Release(block_id, generation);
      }
      return written;
    }
    return expected<void, ShmError>::success();
  }

 private:
  ShmSpmcByteChannel& channel_;
  DispatcherT& pool_;
  uint32_t pool_hash_;
};

/// @brief Consumer side: read a block reference and hand back an RAII lease.
template <typename DispatcherT>
class ShmLeaseConsumer final {
 public:
  ShmLeaseConsumer(ShmSpmcByteChannel& channel, DispatcherT& pool, int32_t pool_slot) noexcept
      : channel_(channel), pool_(pool), pool_slot_(pool_slot) {}

  /// @brief Refresh the pool heartbeat. Call at least every
  ///        OSP_JOB_CONSUMER_TIMEOUT_US while holding a lease; otherwise the
  ///        producer's CleanupDeadConsumers may reclaim the block underneath it.
  void Heartbeat() noexcept {
    if (pool_slot_ >= 0) {
      pool_.ConsumerHeartbeat(pool_slot_);
    }
  }

  /// @brief Reclaim every block still held by this consumer and free its pool
  ///        slot. Use on abnormal shutdown; normal shutdown can rely on lease
  ///        destructors followed by pool unregistration.
  uint32_t Revoke() noexcept { return (pool_slot_ >= 0) ? pool_.RevokeConsumer(pool_slot_) : 0U; }

  expected<BlockLease<DispatcherT>, ShmError> ReadLease(uint32_t timeout_ms) noexcept {
    Heartbeat();
    auto ready = channel_.WaitReadable(timeout_ms);
    if (!ready.has_value()) {
      return expected<BlockLease<DispatcherT>, ShmError>::error(ready.get_error());
    }
    ShmBlockRef ref{};
    auto read = channel_.Read(&ref, sizeof(ref));
    if (!read.has_value()) {
      return expected<BlockLease<DispatcherT>, ShmError>::error(read.get_error());
    }
    if (read.value() != sizeof(ref)) {
      return expected<BlockLease<DispatcherT>, ShmError>::error(ShmError::kCorrupt);
    }
    Heartbeat();
    BlockLease<DispatcherT> lease(&pool_, pool_slot_, ref);
    if (!lease) {
      return expected<BlockLease<DispatcherT>, ShmError>::error(ShmError::kEvicted);
    }
    return expected<BlockLease<DispatcherT>, ShmError>::success(static_cast<BlockLease<DispatcherT>&&>(lease));
  }

 private:
  ShmSpmcByteChannel& channel_;
  DispatcherT& pool_;
  int32_t pool_slot_;
};

#endif  // OSP_PLATFORM_LINUX

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_SHM_TRANSPORT_HPP_
