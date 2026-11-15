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
 * @file span.hpp
 * @brief Non-owning view over a contiguous range of objects or bytes.
 *
 * C++17 subset of std::span: the pointer and the length travel together, so a
 * caller cannot hand a transport or socket API a mismatched (pointer, length)
 * pair. No heap, no exceptions, and no bounds check on operator[] (same
 * contract as std::span). The size is authoritative for read/write limits.
 */

#ifndef OSP_SPAN_HPP_
#define OSP_SPAN_HPP_

#include "osp/platform.hpp"

#include <cstddef>
#include <cstdint>

#include <type_traits>

namespace osp {

/**
 * @brief Non-owning view over @p T elements.
 *
 * @tparam T Element type; use `span<const std::byte>` for read-only byte
 *           buffers and `span<std::byte>` for writable ones.
 */
template <typename T>
class span {
 public:
  using element_type = T;
  using pointer = T*;
  using reference = T&;
  using size_type = std::size_t;

  /// @brief Empty view (nullptr, zero length).
  constexpr span() noexcept : data_(nullptr), size_(0U) {}

  /// @brief View over [data, data + size).
  constexpr span(pointer data, size_type size) noexcept : data_(data), size_(size) {}

  /// @brief View over a whole C array; the length comes from the array type.
  template <size_type N>
  constexpr span(element_type (&arr)[N]) noexcept : data_(arr), size_(N) {}

  /**
   * @brief Qualification-only conversion (e.g. span<uint8_t> -> span<const
   *        uint8_t>), mirroring std::span. Element types must match, so no
   *        implicit widening or reinterpretation can slip through.
   */
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>>>
  constexpr span(const span<U>& other) noexcept : data_(other.data()), size_(other.size()) {}

  constexpr pointer data() const noexcept { return data_; }
  constexpr size_type size() const noexcept { return size_; }
  constexpr size_type size_bytes() const noexcept { return size_ * sizeof(T); }
  constexpr bool empty() const noexcept { return 0U == size_; }
  constexpr reference operator[](size_type index) const noexcept { return data_[index]; }
  constexpr pointer begin() const noexcept { return data_; }
  // nullptr + 0 is not a valid pointer expression, so the empty case returns
  // data() unchanged.
  constexpr pointer end() const noexcept { return (0U == size_) ? data_ : data_ + size_; }

  /**
   * @brief Size narrowed to uint32_t for the byte-count based legacy APIs.
   * @note  Those APIs carry uint32_t lengths; embedded links never exceed
   *        4 GiB, and debug builds assert on a violation.
   */
  uint32_t size_u32() const noexcept {
    OSP_ASSERT(FitsU32());
    return static_cast<uint32_t>(size_);
  }

 private:
  constexpr bool FitsU32() const noexcept {
    if constexpr (sizeof(size_type) > sizeof(uint32_t)) {
      return size_ <= static_cast<size_type>(UINT32_MAX);
    } else {
      return true;
    }
  }

  pointer data_;
  size_type size_;
};

/// @brief Read-only byte view.
using byte_span = span<const std::byte>;

/// @brief Writable byte view.
using writable_byte_span = span<std::byte>;

/// @brief Build a view over (data, size).
template <typename T>
constexpr span<T> make_span(T* data, std::size_t size) noexcept {
  return span<T>(data, size);
}

/// @brief Build a view over a C array; the array extent is the length.
template <typename T, std::size_t N>
constexpr span<T> make_span(T (&arr)[N]) noexcept {
  return span<T>(arr);
}

/// @brief Reinterpret any object view as read-only bytes.
template <typename T>
span<const std::byte> as_bytes(span<T> s) noexcept {
  return byte_span(reinterpret_cast<const std::byte*>(s.data()), s.size_bytes());
}

/// @brief Reinterpret a writable object view as writable bytes.
template <typename T, typename = std::enable_if_t<!std::is_const_v<T>>>
writable_byte_span as_writable_bytes(span<T> s) noexcept {
  return writable_byte_span(reinterpret_cast<std::byte*>(s.data()), s.size_bytes());
}

}  // namespace osp

#endif  // OSP_SPAN_HPP_
