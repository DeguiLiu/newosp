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
 * @file frame_channel.hpp
 * @brief Zero-copy slot-based frame channel for cross-process video frames.
 *
 * Designed for large per-frame payloads (e.g. 8 MB thermal frames @ 30 fps)
 * where newosp's ShmSpscByteRing incurs a memcpy on both Write and Read.  This
 * channel uses a fixed-slot layout: the producer memcpy's a frame directly into
 * a shared-memory slot, the consumer zero-copy views that same slot
 * (np.frombuffer on the Python side) -- neither side copies the payload.
 *
 * Layout (whole segment aligned to 64 bytes):
 *   [0 .. 63]      SegmentHeader (64 B)
 *   [64 ...]       N fixed slots; each slot = [FrameSlotHeader (64 B)][FrameBuf]
 *
 * Synchronization is SPSC (one producer / one consumer) using two monotonic
 * frame counters in the segment header:
 *   - write_index: number of frames committed so far (next frame to write).
 *     After committing frame f (0-based), write_index == f + 1, so the latest
 *     committed frame is always (write_index - 1).
 *   - read_index:  the last frame number the consumer acknowledged.
 *
 * Publish ordering follows the "write-then-release" pattern (cf. zeroipc ring,
 * ipc0cp ring_buffer.hpp): producer writes FrameBuf -> fills FrameSlotHeader ->
 * release fence -> committed=1 -> release store write_index.  The consumer
 * acquire-loads write_index, fast-forwards to the latest frame
 * (read_index = write_index - 1, dropping intermediates), validates
 * magic/frame_no/committed/CRC32, then returns a zero-copy view.
 *
 * Backends:
 *   - Windows (OSP_PLATFORM_WINDOWS): CreateFileMapping/MapViewOfFile.
 *     Mapping names are plain ("osp_fc_<name>", no backslash) so Python's
 *     multiprocessing.shared_memory can open the same segment.
 *   - Linux   (OSP_PLATFORM_LINUX):   shm_open/mmap/ftruncate
 *     ("/osp_fc_<name>").
 *
 * Header-only, C++17, -fno-exceptions -fno-rtti safe.
 */

#ifndef OSP_FRAME_CHANNEL_HPP_
#define OSP_FRAME_CHANNEL_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <climits>
#include <cstdint>
#include <cstring>

#include <atomic>

#if defined(OSP_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(OSP_PLATFORM_LINUX)
#include <chrono>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

/* WaitReadable()/Notify() below use detail::FutexWait/FutexWake, which live in
 * shm_transport.hpp.  Without this the header only compiles where the futex
 * path is compiled out. */
#include "osp/shm_transport.hpp"
#endif

namespace osp {

// ============================================================================
// Constants and error codes
// ============================================================================

enum class FrameChannelError : uint8_t {
  kNone = 0,
  kCreateFailed,
  kOpenFailed,
  kMapFailed,
  kNotSupported,
  kInvalidArg,
  kNotInitialized,
  kFull,
  kEmpty,
  kTimeout,
  kCorrupt
};

enum class FrameDtype : uint32_t {
  kUint8 = 0,
  kUint16 = 1,
  kFloat32 = 2,
  kFloat64 = 3
};

/// @brief Bytes in one data element for a FrameDtype.
constexpr uint32_t FrameDtypeSize(FrameDtype d) noexcept {
  switch (d) {
    case FrameDtype::kUint8:
      return 1;
    case FrameDtype::kUint16:
      return 2;
    case FrameDtype::kFloat32:
      return 4;
    case FrameDtype::kFloat64:
      return 8;
  }
  return 0;
}

/// @brief Segment magic ('HSCF'). Written by the producer at Create time.
static constexpr uint32_t kFrameChannelMagic = 0x48534346u;
/// @brief Slot header magic ('HSLT'). Written on every committed frame.
static constexpr uint32_t kFrameSlotMagic = 0x48534C54u;
/// @brief Wire-format version.
static constexpr uint32_t kFrameChannelVersion = 1u;

static constexpr uint32_t kFrameSegmentHeaderBytes = 64u;  ///< SegmentHeader size
static constexpr uint32_t kFrameSlotHeaderBytes = 64u;     ///< FrameSlotHeader size

// ============================================================================
// CRC-32 (IEEE 802.3 / zlib), slice-by-8 table-driven.  Matches Python
// zlib.crc32 byte-for-byte (poly 0xEDB88320, init 0xFFFFFFFF, final xor).
//
// The inner loop consumes 8 bytes per iteration using the standard zlib
// slice-by-8 tables: v[0] is the ordinary byte-at-a-time table and v[j] is the
// contribution of a byte followed by j zero bytes (zlib make_crc_table
// construction).  Pure C++17, no SIMD/ISA dependency, so it is platform-neutral
// (ARM-Linux / GCC / Clang / MSVC).
// ============================================================================

namespace frame_channel_detail {

constexpr uint32_t kCrc32Poly = 0xEDB88320u;

constexpr uint32_t Crc32TableEntry(uint32_t i) noexcept {
  uint32_t c = i;
  for (uint32_t k = 0; k < 8; ++k) {
    c = (c & 1u) ? (kCrc32Poly ^ (c >> 1u)) : (c >> 1u);
  }
  return c;
}

struct Crc32Slice8Table {
  uint32_t v[8][256];
  constexpr Crc32Slice8Table() noexcept : v{} {
    // v[0] = standard byte-at-a-time table.
    for (uint32_t i = 0; i < 256; ++i) {
      v[0][i] = Crc32TableEntry(i);
    }
    // v[j][i] = v[0][i] advanced by j zero bytes (byte followed by j zeros).
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = v[0][i];
      for (uint32_t j = 1; j < 8; ++j) {
        c = v[0][c & 0xFFu] ^ (c >> 8u);
        v[j][i] = c;
      }
    }
  }
};

}  // namespace frame_channel_detail

/// @brief CRC-32 (zlib-compatible) over [data, data+len).
inline uint32_t FrameCrc32(const uint8_t* data, uint32_t len) noexcept {
  static constexpr frame_channel_detail::Crc32Slice8Table kTable{};
  uint32_t crc = 0xFFFFFFFFu;
  while (len >= 8u) {
    const uint32_t b0 = data[0];
    const uint32_t b1 = data[1];
    const uint32_t b2 = data[2];
    const uint32_t b3 = data[3];
    const uint32_t x = crc ^ (b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u));
    crc = kTable.v[7][x & 0xFFu] ^ kTable.v[6][(x >> 8u) & 0xFFu] ^
          kTable.v[5][(x >> 16u) & 0xFFu] ^ kTable.v[4][(x >> 24u) & 0xFFu] ^
          kTable.v[3][data[4]] ^ kTable.v[2][data[5]] ^ kTable.v[1][data[6]] ^
          kTable.v[0][data[7]];
    data += 8;
    len -= 8u;
  }
  while (len > 0u) {
    crc = kTable.v[0][(crc ^ data[0]) & 0xFFu] ^ (crc >> 8u);
    ++data;
    --len;
  }
  return crc ^ 0xFFFFFFFFu;
}

// ============================================================================
// Shared-memory wire structs (byte layout mirrored by hcs_core/frame_channel.py)
// ============================================================================

/**
 * @brief Segment header (64 bytes).
 *
 * Field offsets (little-endian uint32):
 *   0  magic         -- kFrameChannelMagic
 *   4  version       -- kFrameChannelVersion
 *   8  slot_size     -- total bytes of one slot (64-byte aligned)
 *   12 num_slots     -- number of slots
 *   16 write_index   -- atomic; frames committed so far (next frame to write)
 *   20 read_index    -- atomic; last frame the consumer acknowledged
 *   24 reserved[10]  -- zero
 */
struct FrameSegmentHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t slot_size;
  uint32_t num_slots;
  std::atomic<uint32_t> write_index;
  std::atomic<uint32_t> read_index;
  uint32_t reserved[10];
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for shared memory use");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "std::atomic<uint32_t> must be layout-compatible with uint32_t");
static_assert(sizeof(FrameSegmentHeader) == kFrameSegmentHeaderBytes,
              "FrameSegmentHeader must be 64 bytes");

/**
 * @brief Per-slot header (64 bytes).
 *
 * Field offsets (little-endian uint32):
 *   0  magic        -- kFrameSlotMagic
 *   4  frame_no     -- frame number stored in this slot
 *   8  width        -- frame width (pixels)
 *   12 height       -- frame height (pixels)
 *   16 dtype        -- FrameDtype
 *   20 payload_size -- payload bytes (must equal width*height*FrameDtypeSize)
 *   24 crc32        -- FrameCrc32 over the payload
 *   28 committed    -- atomic; 0 while being written, 1 after release publish
 *   32 reserved[8]  -- zero
 */
struct FrameSlotHeader {
  uint32_t magic;
  uint32_t frame_no;
  uint32_t width;
  uint32_t height;
  uint32_t dtype;
  uint32_t payload_size;
  uint32_t crc32;
  std::atomic<uint32_t> committed;
  uint32_t reserved[8];
};

static_assert(sizeof(FrameSlotHeader) == kFrameSlotHeaderBytes,
              "FrameSlotHeader must be 64 bytes");

// ============================================================================
// FrameShmSegment - cross-platform shared-memory RAII
// ============================================================================

/**
 * @brief RAII wrapper around a shared-memory segment.
 *
 * Windows: CreateFileMapping/MapViewOfFile. Linux: shm_open/mmap.
 * Movable but not copyable.
 */
class FrameShmSegment final {
 public:
  static constexpr uint32_t kNameMax = OSP_SHM_CHANNEL_NAME_MAX;

  FrameShmSegment() noexcept : addr_(nullptr), size_(0) {
    name_[0] = '\0';
#if defined(OSP_PLATFORM_WINDOWS)
    h_ = nullptr;
#else
    fd_ = -1;
#endif
  }

  ~FrameShmSegment() noexcept { Close(); }

  FrameShmSegment(const FrameShmSegment&) = delete;
  FrameShmSegment& operator=(const FrameShmSegment&) = delete;

  FrameShmSegment(FrameShmSegment&& other) noexcept : addr_(other.addr_), size_(other.size_) {
    std::memcpy(name_, other.name_, sizeof(name_));
    other.addr_ = nullptr;
    other.size_ = 0;
    other.name_[0] = '\0';
#if defined(OSP_PLATFORM_WINDOWS)
    h_ = other.h_;
    other.h_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }

  FrameShmSegment& operator=(FrameShmSegment&& other) noexcept {
    if (this != &other) {
      Close();
      addr_ = other.addr_;
      size_ = other.size_;
      std::memcpy(name_, other.name_, sizeof(name_));
      other.addr_ = nullptr;
      other.size_ = 0;
      other.name_[0] = '\0';
#if defined(OSP_PLATFORM_WINDOWS)
      h_ = other.h_;
      other.h_ = nullptr;
#else
      fd_ = other.fd_;
      other.fd_ = -1;
#endif
    }
    return *this;
  }

  static expected<FrameShmSegment, FrameChannelError> Create(const char* name, uint32_t size) noexcept;
  static expected<FrameShmSegment, FrameChannelError> CreateOrReplace(const char* name, uint32_t size) noexcept;
  static expected<FrameShmSegment, FrameChannelError> Open(const char* name) noexcept;

  void* Data() const noexcept { return addr_; }
  uint32_t Size() const noexcept { return size_; }
  const char* Name() const noexcept { return name_; }

  void Close() noexcept {
    if (addr_ != nullptr) {
#if defined(OSP_PLATFORM_WINDOWS)
      ::UnmapViewOfFile(addr_);
      addr_ = nullptr;
      if (h_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(h_));
        h_ = nullptr;
      }
#else
      ::munmap(addr_, size_);
      addr_ = nullptr;
      if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
      }
#endif
      size_ = 0;
      name_[0] = '\0';
    }
  }

  /// @brief Remove the named segment. On Linux this is shm_unlink; on Windows
  /// a section object is destroyed automatically when the last handle closes,
  /// so this is a no-op (the mapping dies with our handle).
  void Unlink() noexcept {
#if !defined(OSP_PLATFORM_WINDOWS)
    if (name_[0] != '\0') {
      ::shm_unlink(name_);
    }
#else
    (void)0;
#endif
  }

 private:
  static void BuildFullName(char (&out)[kNameMax + 1], const char* name) noexcept {
    // Windows keeps a plain name (no backslash) so Python's
    // multiprocessing.shared_memory can open it; Linux needs a leading '/' for
    // shm_open.
    const char* prefix =
#if defined(OSP_PLATFORM_WINDOWS)
        "osp_fc_";
#else
        "/osp_fc_";
#endif
    uint32_t p = 0;
    for (uint32_t i = 0; prefix[i] != '\0' && p < kNameMax; ++i) {
      out[p++] = prefix[i];
    }
    for (uint32_t i = 0; name[i] != '\0' && p < kNameMax; ++i) {
      out[p++] = name[i];
    }
    out[p] = '\0';
  }

#if defined(OSP_PLATFORM_WINDOWS)
  void* h_;  ///< HANDLE from CreateFileMapping/OpenFileMapping
#endif
  int fd_;  ///< Linux shm fd
  void* addr_;
  uint32_t size_;
  char name_[kNameMax + 1];
};

// ----------------------------------------------------------------------------
// FrameShmSegment implementations
// ----------------------------------------------------------------------------

inline expected<FrameShmSegment, FrameChannelError> FrameShmSegment::Create(const char* name,
                                                                            uint32_t size) noexcept {
  FrameShmSegment seg;
  BuildFullName(seg.name_, name);
#if defined(OSP_PLATFORM_WINDOWS)
  HANDLE h = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, seg.name_);
  if (h == nullptr) {
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kCreateFailed);
  }
  void* addr = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (addr == nullptr) {
    ::CloseHandle(h);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kMapFailed);
  }
  seg.h_ = h;
  seg.addr_ = addr;
  seg.size_ = size;
  return expected<FrameShmSegment, FrameChannelError>::success(static_cast<FrameShmSegment&&>(seg));
#elif defined(OSP_PLATFORM_LINUX)
  int fd = ::shm_open(seg.name_, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (fd < 0) {
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kCreateFailed);
  }
  if (::ftruncate(fd, size) != 0) {
    ::close(fd);
    ::shm_unlink(seg.name_);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kCreateFailed);
  }
  void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    ::close(fd);
    ::shm_unlink(seg.name_);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kMapFailed);
  }
  seg.fd_ = fd;
  seg.addr_ = addr;
  seg.size_ = size;
  return expected<FrameShmSegment, FrameChannelError>::success(static_cast<FrameShmSegment&&>(seg));
#else
  (void)seg;
  (void)size;
  return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kNotSupported);
#endif
}

inline expected<FrameShmSegment, FrameChannelError> FrameShmSegment::CreateOrReplace(
    const char* name, uint32_t size) noexcept {
#if defined(OSP_PLATFORM_WINDOWS)
  // Windows section objects are destroyed when the last handle closes, so a
  // stale segment cannot survive a crash. "Replace" therefore just creates;
  // a live existing segment is reused, which is harmless for our use case.
  return Create(name, size);
#elif defined(OSP_PLATFORM_LINUX)
  FrameShmSegment seg;
  BuildFullName(seg.name_, name);
  ::shm_unlink(seg.name_);  // Remove stale segment left by a crashed process.
  return Create(name, size);
#else
  (void)name;
  (void)size;
  return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kNotSupported);
#endif
}

inline expected<FrameShmSegment, FrameChannelError> FrameShmSegment::Open(const char* name) noexcept {
  FrameShmSegment seg;
  BuildFullName(seg.name_, name);
#if defined(OSP_PLATFORM_WINDOWS)
  HANDLE h = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, seg.name_);
  if (h == nullptr) {
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kOpenFailed);
  }
  // dwNumberOfBytesToMap == 0 maps the entire section; the size is then queried
  // via VirtualQuery for diagnostics (FrameChannel reads geometry from header).
  void* addr = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (addr == nullptr) {
    ::CloseHandle(h);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kMapFailed);
  }
  MEMORY_BASIC_INFORMATION mbi;
  SIZE_T qr = ::VirtualQuery(addr, &mbi, sizeof(mbi));
  seg.h_ = h;
  seg.addr_ = addr;
  seg.size_ = (qr != 0 && mbi.RegionSize <= 0xFFFFFFFFull)
                  ? static_cast<uint32_t>(mbi.RegionSize)
                  : 0u;
  return expected<FrameShmSegment, FrameChannelError>::success(static_cast<FrameShmSegment&&>(seg));
#elif defined(OSP_PLATFORM_LINUX)
  int fd = ::shm_open(seg.name_, O_RDWR, 0600);
  if (fd < 0) {
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kOpenFailed);
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kOpenFailed);
  }
  const uint32_t size = static_cast<uint32_t>(st.st_size);
  void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    ::close(fd);
    return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kMapFailed);
  }
  seg.fd_ = fd;
  seg.addr_ = addr;
  seg.size_ = size;
  return expected<FrameShmSegment, FrameChannelError>::success(static_cast<FrameShmSegment&&>(seg));
#else
  (void)seg;
  return expected<FrameShmSegment, FrameChannelError>::error(FrameChannelError::kNotSupported);
#endif
}

// ============================================================================
// Frame metadata / view
// ============================================================================

/// @brief Producer metadata describing a frame to publish.
struct FrameMeta {
  uint32_t width;
  uint32_t height;
  FrameDtype dtype;
};

/// @brief Zero-copy view of the latest committed frame (consumer).
struct FrameView {
  uint32_t frame_no = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dtype = 0;
  uint32_t payload_size = 0;
  uint32_t crc32 = 0;
  const uint8_t* data = nullptr;  ///< Pointer into shared memory (zero-copy).
};

// ============================================================================
// FrameChannel - SPSC zero-copy slot frame channel
// ============================================================================

/**
 * @brief Cross-process zero-copy frame channel (single producer / single
 * consumer).
 *
 * Producer: WriteFrame(meta, data, payload) acquires a free slot, memcpy's the
 * frame into shared memory, writes CRC + committed, then release-stores
 * write_index (publish).  Returns kFull if the consumer is more than
 * (num_slots - 1) frames behind (prevents overwriting the slot the consumer is
 * currently viewing).
 *
 * Consumer: ReadLatestFrame() acquire-loads write_index, fast-forwards to the
 * latest frame (read_index = write_index - 1, dropping intermediates),
 * validates magic/frame_no/committed/CRC32, and returns a zero-copy FrameView.
 *
 * WaitReadable/Notify: Linux uses futex (microsecond wakeup); Windows uses a
 * polling loop (Notify is a no-op there).  Python consumers poll; the
 * wait/notify pair targets C++ readers.
 */
class FrameChannel final {
 public:
  /// @brief Sentinel for "no frame consumed yet". Avoids skipping frame 0,
  /// whose number collides with a zero-initialized last_read.
  static constexpr uint32_t kNeverRead = 0xFFFFFFFFu;

  FrameChannel() noexcept : header_(nullptr), num_slots_(0), last_read_(kNeverRead), is_writer_(false) {}

  ~FrameChannel() noexcept = default;

  FrameChannel(const FrameChannel&) = delete;
  FrameChannel& operator=(const FrameChannel&) = delete;

  FrameChannel(FrameChannel&& other) noexcept
      : shm_segment_(static_cast<FrameShmSegment&&>(other.shm_segment_)),
        header_(other.header_),
        num_slots_(other.num_slots_),
        last_read_(other.last_read_),
        is_writer_(other.is_writer_) {
    other.header_ = nullptr;
    other.num_slots_ = 0;
    other.last_read_ = kNeverRead;
    other.is_writer_ = false;
  }

  FrameChannel& operator=(FrameChannel&& other) noexcept {
    if (this != &other) {
      shm_segment_ = static_cast<FrameShmSegment&&>(other.shm_segment_);
      header_ = other.header_;
      num_slots_ = other.num_slots_;
      last_read_ = other.last_read_;
      is_writer_ = other.is_writer_;
      other.header_ = nullptr;
      other.num_slots_ = 0;
      other.last_read_ = kNeverRead;
      other.is_writer_ = false;
    }
    return *this;
  }

  /// @brief Total segment size for a channel with the given slot geometry.
  static constexpr uint32_t RequiredSegmentSize(uint32_t slot_size, uint32_t num_slots) noexcept {
    return kFrameSegmentHeaderBytes + slot_size * num_slots;
  }

  /// @brief Round a slot size up to a multiple of 64 bytes.
  static constexpr uint32_t Align64(uint32_t v) noexcept { return (v + 63u) & ~63u; }

  /// @brief Minimum slot_size needed to hold `frame_bytes` of payload.
  static constexpr uint32_t RequiredSlotSize(uint32_t frame_bytes) noexcept {
    return Align64(kFrameSlotHeaderBytes + frame_bytes);
  }

  /**
   * @brief Create a producer endpoint (new segment).
   * @param name Channel name (prefix "osp_fc_" is added).
   * @param slot_size Total bytes per slot (>= 128, multiple of 64).
   * @param num_slots Number of slots (>= 2).
   */
  static expected<FrameChannel, FrameChannelError> OpenProducer(const char* name, uint32_t slot_size,
                                                                uint32_t num_slots) noexcept {
    FrameChannel ch;
    ch.is_writer_ = true;
    auto result = FrameShmSegment::Create(name, RequiredSegmentSize(slot_size, num_slots));
    if (!result.has_value()) {
      return expected<FrameChannel, FrameChannelError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<FrameShmSegment&&>(result.value());
    ch.InitSegment(slot_size, num_slots);
    return expected<FrameChannel, FrameChannelError>::success(static_cast<FrameChannel&&>(ch));
  }

  /**
   * @brief Create a producer endpoint, replacing any stale segment first.
   */
  static expected<FrameChannel, FrameChannelError> CreateOrReplace(const char* name, uint32_t slot_size,
                                                                   uint32_t num_slots) noexcept {
    FrameChannel ch;
    ch.is_writer_ = true;
    auto result = FrameShmSegment::CreateOrReplace(name, RequiredSegmentSize(slot_size, num_slots));
    if (!result.has_value()) {
      return expected<FrameChannel, FrameChannelError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<FrameShmSegment&&>(result.value());
    ch.InitSegment(slot_size, num_slots);
    return expected<FrameChannel, FrameChannelError>::success(static_cast<FrameChannel&&>(ch));
  }

  /**
   * @brief Open a consumer endpoint for an existing channel.
   * The consumer fast-forwards to the latest frame on first read.
   */
  static expected<FrameChannel, FrameChannelError> OpenConsumer(const char* name) noexcept {
    FrameChannel ch;
    ch.is_writer_ = false;
    auto result = FrameShmSegment::Open(name);
    if (!result.has_value()) {
      return expected<FrameChannel, FrameChannelError>::error(result.get_error());
    }
    ch.shm_segment_ = static_cast<FrameShmSegment&&>(result.value());
    auto* h = static_cast<FrameSegmentHeader*>(ch.shm_segment_.Data());
    if (h == nullptr || h->magic != kFrameChannelMagic || h->version != kFrameChannelVersion) {
      return expected<FrameChannel, FrameChannelError>::error(FrameChannelError::kCorrupt);
    }
    ch.header_ = h;
    ch.num_slots_ = h->num_slots;
    ch.last_read_ = kNeverRead;  // Fresh consumer: first read grabs the latest frame.
    return expected<FrameChannel, FrameChannelError>::success(static_cast<FrameChannel&&>(ch));
  }

  // ---- Producer API ----

  /**
   * @brief Publish one frame (acquire slot + memcpy + commit + notify).
   * @param meta Frame width/height/dtype.
   * @param data Payload bytes (must be >= payload_size; payload must equal
   *             width*height*FrameDtypeSize(dtype)).
   * @param payload_size Payload size in bytes (<= FrameCapacity()).
   * @return kFull if the consumer is too far behind; kInvalidArg on bad args.
   */
  expected<void, FrameChannelError> WriteFrame(const FrameMeta& meta, const void* data,
                                               uint32_t payload_size) noexcept {
    OSP_ASSERT(header_ != nullptr);
    if (payload_size > FrameCapacity()) {
      return expected<void, FrameChannelError>::error(FrameChannelError::kInvalidArg);
    }

    const uint32_t wi = header_->write_index.load(std::memory_order_relaxed);
    const uint32_t ri = header_->read_index.load(std::memory_order_acquire);
    // Keep at most (num_slots - 1) frames in flight so we never clobber the
    // slot the consumer is currently viewing (slot index = read_index % N).
    if (wi - ri >= num_slots_) {
      return expected<void, FrameChannelError>::error(FrameChannelError::kFull);
    }

    FrameSlotHeader* slot = SlotAt(wi % num_slots_);
    slot->committed.store(0, std::memory_order_relaxed);  // Invalidate stale slot.

    uint8_t* dst = SlotData(wi % num_slots_);
    if (data != nullptr) {
      std::memcpy(dst, data, payload_size);
    } else {
      std::memset(dst, 0, payload_size);
    }

    slot->magic = kFrameSlotMagic;
    slot->frame_no = wi;
    slot->width = meta.width;
    slot->height = meta.height;
    slot->dtype = static_cast<uint32_t>(meta.dtype);
    slot->payload_size = payload_size;
    slot->crc32 = FrameCrc32(dst, payload_size);

    // ARM weak ordering: ensure the payload + header writes are visible before
    // the committed flag and before the write_index publish below.
    std::atomic_thread_fence(std::memory_order_release);
    slot->committed.store(1, std::memory_order_release);
    header_->write_index.store(wi + 1, std::memory_order_release);

    Notify();
    return expected<void, FrameChannelError>::success();
  }

  // ---- Consumer API ----

  /**
   * @brief Read the latest committed frame (zero-copy, fast-forwards/drops
   * intermediates).  Advances the shared read_index.
   * @return FrameView on success, kEmpty if no new frame since the last read.
   */
  expected<FrameView, FrameChannelError> ReadLatestFrame() noexcept {
    OSP_ASSERT(header_ != nullptr);
    const uint32_t wi = header_->write_index.load(std::memory_order_acquire);
    if (wi == 0) {
      return expected<FrameView, FrameChannelError>::error(FrameChannelError::kEmpty);
    }
    const uint32_t latest = wi - 1;
    if (latest == last_read_) {
      return expected<FrameView, FrameChannelError>::error(FrameChannelError::kEmpty);
    }

    FrameSlotHeader* slot = SlotAt(latest % num_slots_);
    if (slot->committed.load(std::memory_order_acquire) != 1 || slot->magic != kFrameSlotMagic ||
        slot->frame_no != latest) {
      return expected<FrameView, FrameChannelError>::error(FrameChannelError::kCorrupt);
    }
    if (slot->payload_size > FrameCapacity()) {
      return expected<FrameView, FrameChannelError>::error(FrameChannelError::kCorrupt);
    }
    const uint8_t* payload = SlotData(latest % num_slots_);
    if (FrameCrc32(payload, slot->payload_size) != slot->crc32) {
      return expected<FrameView, FrameChannelError>::error(FrameChannelError::kCorrupt);
    }

    last_read_ = latest;
    header_->read_index.store(latest, std::memory_order_release);  // Ack (unblock producer).

    FrameView view;
    view.frame_no = slot->frame_no;
    view.width = slot->width;
    view.height = slot->height;
    view.dtype = slot->dtype;
    view.payload_size = slot->payload_size;
    view.crc32 = slot->crc32;
    view.data = payload;
    return expected<FrameView, FrameChannelError>::success(view);
  }

  /// @brief Whether a new committed frame is available since the last read.
  bool HasNewFrame() const noexcept {
    if (header_ == nullptr) {
      return false;
    }
    const uint32_t wi = header_->write_index.load(std::memory_order_acquire);
    if (wi == 0) {
      return false;
    }
    return (wi - 1) != last_read_;
  }

  /**
   * @brief Wait until a new frame is available.
   * Linux: futex wait on write_index (microsecond wakeup).
   * Windows/others: polling with exponential backoff.
   *
   * Windows wait strategy note (deliberately deferred): the non-Linux branch
   * below polls with exponential backoff starting at 50 us and doubling to a
   * 1000 us ceiling. This trades a small amount of CPU for bounded P99 wake
   * latency. Before upgrading to WaitOnAddress / a proper event wakeup, first
   * measure the polling path's per-wait CPU cost and P99 under the real
   * 30 fps / 640x480 / 1920x1440 workload; only switch if the baseline shows
   * the polling cost is material. Do not add an event/affinity wakeup
   * speculatively (see docs/pyside6_multithread_performance_plan.md §9.5).
   */
  expected<void, FrameChannelError> WaitReadable(uint32_t timeout_ms) noexcept {
    OSP_ASSERT(header_ != nullptr);
    if (HasNewFrame()) {
      return expected<void, FrameChannelError>::success();
    }
#if defined(OSP_PLATFORM_LINUX)
    const uint32_t cur = header_->write_index.load(std::memory_order_acquire);
    detail::FutexWait(&header_->write_index, cur, timeout_ms);
    return HasNewFrame() ? expected<void, FrameChannelError>::success()
                         : expected<void, FrameChannelError>::error(FrameChannelError::kTimeout);
#else
    const uint64_t deadline_us = SteadyNowUs() + static_cast<uint64_t>(timeout_ms) * 1000u;
    uint32_t sleep_us = 50;
    while (SteadyNowUs() < deadline_us) {
      ThreadSleepUs(sleep_us);
      if (HasNewFrame()) {
        return expected<void, FrameChannelError>::success();
      }
      if (sleep_us < 1000) {
        sleep_us *= 2;
        if (sleep_us > 1000) {
          sleep_us = 1000;
        }
      }
    }
    return HasNewFrame() ? expected<void, FrameChannelError>::success()
                         : expected<void, FrameChannelError>::error(FrameChannelError::kTimeout);
#endif
  }

  /// @brief Wake a waiting reader. Linux: futex wake; Windows: no-op (polling).
  void Notify() noexcept {
#if defined(OSP_PLATFORM_LINUX)
    if (header_ != nullptr) {
      detail::FutexWake(&header_->write_index);
    }
#else
    // No-op: polling-based wait does not need explicit notification.
#endif
  }

  /// @brief Remove the named segment (producer only). On Windows the section
  /// dies automatically when the last handle closes.
  void Unlink() noexcept {
    if (is_writer_) {
      shm_segment_.Unlink();
    }
  }

  // ---- Accessors ----

  uint32_t NumSlots() const noexcept { return num_slots_; }
  uint32_t SlotSize() const noexcept { return header_ ? header_->slot_size : 0; }
  uint32_t FrameCapacity() const noexcept { return header_ ? header_->slot_size - kFrameSlotHeaderBytes : 0; }
  bool IsWriter() const noexcept { return is_writer_; }
  uint32_t WriteIndex() const noexcept { return header_ ? header_->write_index.load(std::memory_order_relaxed) : 0; }
  uint32_t ReadIndex() const noexcept { return header_ ? header_->read_index.load(std::memory_order_relaxed) : 0; }
  uint32_t LastRead() const noexcept { return last_read_; }
  const char* Name() const noexcept { return shm_segment_.Name(); }

 private:
  void InitSegment(uint32_t slot_size, uint32_t num_slots) noexcept {
    header_ = static_cast<FrameSegmentHeader*>(shm_segment_.Data());
    header_->magic = kFrameChannelMagic;
    header_->version = kFrameChannelVersion;
    header_->slot_size = slot_size;
    header_->num_slots = num_slots;
    header_->write_index.store(0, std::memory_order_relaxed);
    header_->read_index.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < 10; ++i) {
      header_->reserved[i] = 0;
    }
    num_slots_ = num_slots;
    last_read_ = kNeverRead;
    std::atomic_thread_fence(std::memory_order_release);
  }

  FrameSlotHeader* SlotAt(uint32_t index) const noexcept {
    uint8_t* base = reinterpret_cast<uint8_t*>(header_) + kFrameSegmentHeaderBytes;
    return reinterpret_cast<FrameSlotHeader*>(base + index * header_->slot_size);
  }

  uint8_t* SlotData(uint32_t index) const noexcept {
    return reinterpret_cast<uint8_t*>(SlotAt(index)) + kFrameSlotHeaderBytes;
  }

  FrameShmSegment shm_segment_;
  FrameSegmentHeader* header_;
  uint32_t num_slots_;
  uint32_t last_read_;  ///< Last frame consumed by this endpoint (local).
  bool is_writer_;
};

}  // namespace osp

#endif  // OSP_FRAME_CHANNEL_HPP_
