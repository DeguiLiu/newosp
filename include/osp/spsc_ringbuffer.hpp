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
 * @file spsc_ringbuffer.hpp
 * @brief Lock-free, wait-free SPSC ring buffer.
 *
 * Adapted from https://github.com/DeguiLiu/ringbuffer (v2.0.0, MIT license).
 * Provides batch operations, Peek/At/Discard, PushFromCallback,
 * and FakeTSO mode for single-core MCUs.
 *
 * Supports both trivially copyable types (memcpy batch path) and
 * non-trivially copyable types (element-wise move path).
 * Compile-time dispatch via if constexpr -- zero overhead for POD types.
 *
 * Header-only, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SPSC_RINGBUFFER_HPP_
#define OSP_SPSC_RINGBUFFER_HPP_

#include "osp/platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace osp {

/// @brief Lock-free, wait-free SPSC (Single-Producer Single-Consumer) ring
///        buffer with compile-time dual-path optimization.
///
/// @tparam T            Element type. Trivially copyable types use memcpy
///                      for batch ops; non-trivially copyable use move.
/// @tparam BufferSize   Capacity (must be a power of 2).
/// @tparam FakeTSO      If true, use relaxed ordering (for single-core MCU
///                      or TSO architectures).
/// @tparam IndexT       Unsigned index type (uint8_t .. size_t).
///
/// Thread safety:
///   - Exactly ONE producer thread may call Push / PushBatch /
///     PushFromCallback / ProducerClear.
///   - Exactly ONE consumer thread may call Pop / PopBatch / Discard /
///     Peek / At / ConsumerClear.
///   - Size / Available / IsEmpty / IsFull / Capacity may be called from
///     either side.
///   - Using multiple producers or multiple consumers is UNDEFINED BEHAVIOR.
template <typename T, size_t BufferSize = 16, bool FakeTSO = false,
          typename IndexT = size_t>
class SpscRingbuffer {
 public:
  static_assert(BufferSize != 0, "Buffer size cannot be zero.");
  static_assert((BufferSize & (BufferSize - 1)) == 0,
                "Buffer size must be a power of 2.");
  static_assert(sizeof(IndexT) <= sizeof(size_t),
                "Index type size must not exceed size_t.");
  static_assert(std::is_unsigned<IndexT>::value,
                "Index type must be unsigned.");
  static_assert(BufferSize <= ((std::numeric_limits<IndexT>::max)() >> 1),
                "Buffer size is too large for the given indexing type.");

  /// Compile-time flag for optimized memcpy path selection.
  static constexpr bool kTriviallyCopyable =
      std::is_trivially_copyable<T>::value;

  SpscRingbuffer() noexcept {
    head_.value.store(0, std::memory_order_relaxed);
    tail_.value.store(0, std::memory_order_relaxed);
  }

  SpscRingbuffer(const SpscRingbuffer&) = delete;
  SpscRingbuffer& operator=(const SpscRingbuffer&) = delete;

  // ==== Producer API ====

  /// @brief Push one element by copy.
  /// @return true if successful, false if buffer is full.
  bool Push(const T& data) noexcept { return PushImpl(data); }

  /// @brief Push one element by move.
  /// @return true if successful, false if buffer is full.
  bool Push(T&& data) noexcept { return PushImpl(std::move(data)); }

  /// @brief Push one element produced by a callback, only if space is
  ///        available. Avoids computation when the buffer is full.
  /// @tparam Callable  Any callable returning T.
  /// @return true if callback was called and element was pushed.
  template <typename Callable>
  bool PushFromCallback(Callable&& callback) noexcept {
    const IndexT cur_head = head_.value.load(std::memory_order_relaxed);
    const IndexT cur_tail = tail_.value.load(AcquireOrder());
    if ((cur_head - cur_tail) == BufferSize) {
      return false;
    }
    data_buff_[cur_head & kMask] = callback();
    head_.value.store(cur_head + 1, ReleaseOrder());
    return true;
  }

  /// @brief Push multiple elements from a contiguous buffer.
  /// @details Uses memcpy for trivially copyable T, element-wise copy otherwise.
  /// @return Number of elements actually pushed.
  size_t PushBatch(const T* buf, size_t count) noexcept {
    size_t written = 0;
    IndexT cur_head = head_.value.load(std::memory_order_relaxed);

    while (written < count) {
      const IndexT cur_tail = tail_.value.load(AcquireOrder());
      const IndexT space =
          static_cast<IndexT>(BufferSize) - (cur_head - cur_tail);
      if (space == 0) {
        break;
      }
      const size_t to_write =
          std::min(count - written, static_cast<size_t>(space));
      const size_t head_offset = cur_head & kMask;

      if constexpr (kTriviallyCopyable) {
        const size_t first_part =
            std::min(to_write, BufferSize - head_offset);
        std::memcpy(&data_buff_[head_offset], buf + written,
                     first_part * sizeof(T));
        if (to_write > first_part) {
          std::memcpy(&data_buff_[0], buf + written + first_part,
                       (to_write - first_part) * sizeof(T));
        }
      } else {
        for (size_t i = 0; i < to_write; ++i) {
          data_buff_[(head_offset + i) & kMask] = buf[written + i];
        }
      }

      written += to_write;
      cur_head += static_cast<IndexT>(to_write);
      head_.value.store(cur_head, ReleaseOrder());
    }
    return written;
  }

  /// @brief Clear buffer from producer side (sets head = tail).
  /// @note Only call from the producer thread.
  void ProducerClear() noexcept {
    head_.value.store(tail_.value.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
  }

  // ==== Consumer API ====

  /// @brief Pop one element from the buffer.
  /// @param[out] data  Where to store the popped element.
  /// @return true if successful, false if buffer is empty.
  bool Pop(T& data) noexcept {
    const IndexT cur_tail = tail_.value.load(std::memory_order_relaxed);
    const IndexT cur_head = head_.value.load(AcquireOrder());
    if (cur_tail == cur_head) {
      return false;
    }
    if constexpr (kTriviallyCopyable) {
      data = data_buff_[cur_tail & kMask];
    } else {
      data = std::move(data_buff_[cur_tail & kMask]);
    }
    tail_.value.store(cur_tail + 1, ReleaseOrder());
    return true;
  }

  /// @brief Pop multiple elements into a contiguous buffer.
  /// @details Uses memcpy for trivially copyable T, element-wise move otherwise.
  /// @return Number of elements actually popped.
  size_t PopBatch(T* buf, size_t count) noexcept {
    size_t read = 0;
    IndexT cur_tail = tail_.value.load(std::memory_order_relaxed);

    while (read < count) {
      const IndexT cur_head = head_.value.load(AcquireOrder());
      const IndexT available = cur_head - cur_tail;
      if (available == 0) {
        break;
      }
      const size_t to_read =
          std::min(count - read, static_cast<size_t>(available));
      const size_t tail_offset = cur_tail & kMask;

      if constexpr (kTriviallyCopyable) {
        const size_t first_part =
            std::min(to_read, BufferSize - tail_offset);
        std::memcpy(buf + read, &data_buff_[tail_offset],
                     first_part * sizeof(T));
        if (to_read > first_part) {
          std::memcpy(buf + read + first_part, &data_buff_[0],
                       (to_read - first_part) * sizeof(T));
        }
      } else {
        for (size_t i = 0; i < to_read; ++i) {
          buf[read + i] = std::move(data_buff_[(tail_offset + i) & kMask]);
        }
      }

      read += to_read;
      cur_tail += static_cast<IndexT>(to_read);
      tail_.value.store(cur_tail, ReleaseOrder());
    }
    return read;
  }

  /// @brief Discard elements without reading them.
  /// @param count  Number of elements to discard (default 1).
  /// @return Number of elements actually discarded.
  size_t Discard(size_t count = 1) noexcept {
    const IndexT cur_tail = tail_.value.load(std::memory_order_relaxed);
    const IndexT cur_head = head_.value.load(AcquireOrder());
    const IndexT available = cur_head - cur_tail;
    const size_t to_discard =
        std::min(count, static_cast<size_t>(available));
    if (to_discard > 0) {
      tail_.value.store(cur_tail + static_cast<IndexT>(to_discard),
                         ReleaseOrder());
    }
    return to_discard;
  }

  /// @brief Peek at the front element without removing it.
  /// @return Pointer to the front element, or nullptr if empty.
  T* Peek() noexcept {
    const IndexT cur_tail = tail_.value.load(std::memory_order_relaxed);
    const IndexT cur_head = head_.value.load(AcquireOrder());
    if (cur_tail == cur_head) {
      return nullptr;
    }
    return &data_buff_[cur_tail & kMask];
  }

  /// @brief Access the n-th element from the consumer side with bounds
  ///        checking.
  /// @return Pointer to the element, or nullptr if index is out of range.
  T* At(size_t index) noexcept {
    const IndexT cur_tail = tail_.value.load(std::memory_order_relaxed);
    const IndexT cur_head = head_.value.load(AcquireOrder());
    if ((cur_head - cur_tail) <= static_cast<IndexT>(index)) {
      return nullptr;
    }
    return &data_buff_[(cur_tail + index) & kMask];
  }

  /// @brief Access the n-th element without bounds checking.
  /// @warning Undefined behavior if index >= Size().
  const T& operator[](size_t index) const noexcept {
    return data_buff_[(tail_.value.load(std::memory_order_relaxed) + index) &
                       kMask];
  }

  /// @brief Clear buffer from consumer side (sets tail = head).
  /// @note Only call from the consumer thread.
  void ConsumerClear() noexcept {
    tail_.value.store(head_.value.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
  }

  // ==== Query API (either side) ====

  /// @brief Number of elements available to read.
  IndexT Size() const noexcept {
    return head_.value.load(AcquireOrder()) -
           tail_.value.load(std::memory_order_relaxed);
  }

  /// @brief Number of free slots available for writing.
  IndexT Available() const noexcept {
    return static_cast<IndexT>(BufferSize) -
           (head_.value.load(std::memory_order_relaxed) -
            tail_.value.load(AcquireOrder()));
  }

  bool IsEmpty() const noexcept { return Size() == 0; }
  bool IsFull() const noexcept { return Available() == 0; }
  static constexpr size_t Capacity() noexcept { return BufferSize; }

 private:
  template <typename U>
  bool PushImpl(U&& data) noexcept {
    const IndexT cur_head = head_.value.load(std::memory_order_relaxed);
    const IndexT cur_tail = tail_.value.load(AcquireOrder());
    if ((cur_head - cur_tail) == BufferSize) {
      return false;
    }
    data_buff_[cur_head & kMask] = std::forward<U>(data);
    head_.value.store(cur_head + 1, ReleaseOrder());
    return true;
  }

  static constexpr std::memory_order AcquireOrder() noexcept {
    return FakeTSO ? std::memory_order_relaxed : std::memory_order_acquire;
  }

  static constexpr std::memory_order ReleaseOrder() noexcept {
    return FakeTSO ? std::memory_order_relaxed : std::memory_order_release;
  }

  static constexpr IndexT kMask = static_cast<IndexT>(BufferSize - 1U);

  // Cache-line padded atomic indices to avoid false sharing.
  struct alignas(kCacheLineSize) PaddedIndex {
    std::atomic<IndexT> value{0};
  };

  PaddedIndex head_;                                  // Producer writes
  PaddedIndex tail_;                                  // Consumer writes
  alignas(kCacheLineSize) std::array<T, BufferSize> data_buff_{}; // Ring storage
};

}  // namespace osp

#endif  // OSP_SPSC_RINGBUFFER_HPP_
