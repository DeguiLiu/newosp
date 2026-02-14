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
 * @file vocabulary.hpp
 * @brief Vocabulary types for OSP-CPP: expected, optional, FixedVector,
 *        FixedString, FixedFunction, function_ref, not_null, NewType,
 *        ScopeGuard.
 *
 * Inspired by iceoryx early versions (v0.90-v1.0) and MCCC containers.
 * All types are stack-allocated with zero heap overhead, compatible with
 * -fno-exceptions -fno-rtti.
 */

#ifndef OSP_VOCABULARY_HPP_
#define OSP_VOCABULARY_HPP_

#include "osp/platform.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace osp {

// ============================================================================
// Error Enums (module-specific)
// ============================================================================

enum class ConfigError : uint8_t {
  kFileNotFound = 0,
  kParseError,
  kFormatNotSupported,
  kBufferFull
};

enum class TimerError : uint8_t {
  kSlotsFull = 0,
  kInvalidPeriod,
  kNotRunning,
  kAlreadyRunning
};

enum class ShellError : uint8_t {
  kRegistryFull = 0,
  kDuplicateName,
  kPortInUse,
  kNotRunning
};

enum class MemPoolError : uint8_t {
  kPoolExhausted = 0,
  kInvalidPointer
};

/// Queue backpressure level indicator.
/// Enum values are labels only; each module defines its own threshold logic.
enum class BackpressureLevel : uint8_t {
  kNormal   = 0U,  ///< Queue utilization is low
  kWarning  = 1U,  ///< Queue utilization is elevated
  kCritical = 2U,  ///< Queue utilization is near capacity
  kFull     = 3U   ///< Queue is at capacity
};

// ============================================================================
// expected<V, E> - Lightweight error-or-value type (iceoryx inspired)
// ============================================================================

/**
 * @brief Holds either a success value of type V or an error of type E.
 *
 * Use static factory methods success() and error() to construct.
 * Zero heap allocation. Compatible with -fno-exceptions.
 */
template <typename V, typename E>
class expected final {
 public:
  static expected success(const V& val) noexcept {
    expected e;
    e.has_value_ = true;
    ::new (&e.storage_) V(val);
    return e;
  }

  static expected success(V&& val) noexcept {
    expected e;
    e.has_value_ = true;
    ::new (&e.storage_) V(static_cast<V&&>(val));
    return e;
  }

  static expected error(E err) noexcept {
    expected e;
    e.has_value_ = false;
    e.err_ = err;
    return e;
  }

  expected(const expected& other) noexcept : storage_{}, err_(other.err_), has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) V(other.value());
    }
  }

  expected& operator=(const expected& other) noexcept {
    if (this != &other) {
      if (has_value_) {
        reinterpret_cast<V*>(&storage_)->~V();
      }
      has_value_ = other.has_value_;
      err_ = other.err_;
      if (has_value_) {
        ::new (&storage_) V(other.value());
      }
    }
    return *this;
  }

  expected(expected&& other) noexcept : storage_{}, err_(other.err_), has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) V(static_cast<V&&>(other.value()));
    }
  }

  expected& operator=(expected&& other) noexcept {
    if (this != &other) {
      if (has_value_) {
        reinterpret_cast<V*>(&storage_)->~V();
      }
      has_value_ = other.has_value_;
      err_ = other.err_;
      if (has_value_) {
        ::new (&storage_) V(static_cast<V&&>(other.value()));
      }
    }
    return *this;
  }

  ~expected() {
    if (has_value_) {
      reinterpret_cast<V*>(&storage_)->~V();
    }
  }

  bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  V& value() & noexcept {
    OSP_ASSERT(has_value_);
    return *reinterpret_cast<V*>(&storage_);
  }
  const V& value() const& noexcept {
    OSP_ASSERT(has_value_);
    return *reinterpret_cast<const V*>(&storage_);
  }

  E get_error() const noexcept {
    OSP_ASSERT(!has_value_);
    return err_;
  }

  V value_or(const V& default_val) const noexcept {
    return has_value_ ? value() : default_val;
  }

 private:
  expected() noexcept : storage_{}, err_{}, has_value_(false) {}

  typename std::aligned_storage<sizeof(V), alignof(V)>::type storage_;
  E err_;
  bool has_value_;
};

/**
 * @brief Void specialization - represents success or error with no value.
 */
template <typename E>
class expected<void, E> final {
 public:
  static expected success() noexcept {
    expected e;
    e.has_value_ = true;
    return e;
  }

  static expected error(E err) noexcept {
    expected e;
    e.has_value_ = false;
    e.err_ = err;
    return e;
  }

  bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  E get_error() const noexcept {
    OSP_ASSERT(!has_value_);
    return err_;
  }

 private:
  expected() noexcept : err_{}, has_value_(false) {}

  E err_;
  bool has_value_;
};

// ============================================================================
// optional<T> - Lightweight nullable value (iceoryx inspired)
// ============================================================================

/**
 * @brief Holds either a value of type T or nothing.
 *
 * Zero heap allocation. Compatible with -fno-exceptions.
 */
template <typename T>
class optional final {
 public:
  optional() noexcept : has_value_(false) {}

  optional(const T& val) noexcept : has_value_(true) {  // NOLINT
    ::new (&storage_) T(val);
  }

  optional(T&& val) noexcept : has_value_(true) {  // NOLINT
    ::new (&storage_) T(static_cast<T&&>(val));
  }

  optional(const optional& other) noexcept : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) T(other.value());
    }
  }

  optional& operator=(const optional& other) noexcept {
    if (this != &other) {
      reset();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (&storage_) T(other.value());
      }
    }
    return *this;
  }

  optional(optional&& other) noexcept : has_value_(other.has_value_) {
    if (has_value_) {
      ::new (&storage_) T(static_cast<T&&>(other.value()));
    }
  }

  optional& operator=(optional&& other) noexcept {
    if (this != &other) {
      reset();
      has_value_ = other.has_value_;
      if (has_value_) {
        ::new (&storage_) T(static_cast<T&&>(other.value()));
      }
    }
    return *this;
  }

  ~optional() { reset(); }

  bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  T& value() noexcept {
    OSP_ASSERT(has_value_);
    return *reinterpret_cast<T*>(&storage_);
  }
  const T& value() const noexcept {
    OSP_ASSERT(has_value_);
    return *reinterpret_cast<const T*>(&storage_);
  }

  T value_or(const T& default_val) const noexcept {
    return has_value_ ? value() : default_val;
  }

  void reset() noexcept {
    if (has_value_) {
      reinterpret_cast<T*>(&storage_)->~T();
      has_value_ = false;
    }
  }

 private:
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
  bool has_value_;
};

// ============================================================================
// FixedFunction<Sig, BufferSize> - SBO callback (no heap allocation)
// ============================================================================

template <typename Signature, size_t BufferSize = 2 * sizeof(void*)>
class FixedFunction;

/**
 * @brief Fixed-size callable wrapper with small buffer optimization.
 *
 * Compatible with -fno-exceptions. static_assert on oversized callable.
 */
template <typename Ret, typename... Args, size_t BufferSize>
class FixedFunction<Ret(Args...), BufferSize> final {
 public:
  FixedFunction() noexcept = default;

  // NOLINTNEXTLINE(google-explicit-constructor)
  FixedFunction(std::nullptr_t) noexcept {}

  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, FixedFunction>::value &&
                !std::is_same<typename std::decay<F>::type, std::nullptr_t>::value>::type>
  FixedFunction(F&& f) noexcept {  // NOLINT
    using Decay = typename std::decay<F>::type;
    static_assert(sizeof(Decay) <= BufferSize,
                  "Callable too large for FixedFunction buffer");
    static_assert(alignof(Decay) <= alignof(Storage),
                  "Callable alignment exceeds buffer alignment");
    ::new (&storage_) Decay(static_cast<F&&>(f));
    invoker_ = [](const Storage& s, Args... args) -> Ret {
      return (*reinterpret_cast<const Decay*>(&s))(static_cast<Args&&>(args)...);
    };
    destroyer_ = [](Storage& s) {
      reinterpret_cast<Decay*>(&s)->~Decay();
    };
  }

  FixedFunction(FixedFunction&& other) noexcept
      : invoker_(other.invoker_), destroyer_(other.destroyer_) {
    if (other.invoker_) {
      std::memcpy(&storage_, &other.storage_, BufferSize);
      other.invoker_ = nullptr;
      other.destroyer_ = nullptr;
    }
  }

  FixedFunction& operator=(FixedFunction&& other) noexcept {
    if (this != &other) {
      if (destroyer_) {
        destroyer_(storage_);
      }
      invoker_ = other.invoker_;
      destroyer_ = other.destroyer_;
      if (other.invoker_) {
        std::memcpy(&storage_, &other.storage_, BufferSize);
        other.invoker_ = nullptr;
        other.destroyer_ = nullptr;
      }
    }
    return *this;
  }

  FixedFunction& operator=(std::nullptr_t) noexcept {
    if (destroyer_) {
      destroyer_(storage_);
    }
    invoker_ = nullptr;
    destroyer_ = nullptr;
    return *this;
  }

  ~FixedFunction() {
    if (destroyer_) {
      destroyer_(storage_);
    }
  }

  FixedFunction(const FixedFunction&) = delete;
  FixedFunction& operator=(const FixedFunction&) = delete;

  Ret operator()(Args... args) const {
    OSP_ASSERT(invoker_);
    return invoker_(storage_, static_cast<Args&&>(args)...);
  }

  explicit operator bool() const noexcept { return invoker_ != nullptr; }

 private:
  using Storage = typename std::aligned_storage<BufferSize, alignof(void*)>::type;
  using Invoker = Ret (*)(const Storage&, Args...);
  using Destroyer = void (*)(Storage&);

  Storage storage_{};
  Invoker invoker_ = nullptr;
  Destroyer destroyer_ = nullptr;
};

// ============================================================================
// function_ref<Sig> - Non-owning callable reference (iceoryx inspired)
// ============================================================================

template <typename Sig>
class function_ref;

/**
 * @brief Non-owning lightweight callable reference (2 pointers).
 *
 * The referenced callable must outlive the function_ref.
 */
template <typename Ret, typename... Args>
class function_ref<Ret(Args...)> final {
 public:
  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, function_ref>::value>::type>
  function_ref(F&& f) noexcept  // NOLINT
      : obj_(const_cast<void*>(static_cast<const void*>(&f))),
        invoker_([](void* o, Args... args) -> Ret {
          return (*static_cast<typename std::remove_reference<F>::type*>(o))(
              static_cast<Args&&>(args)...);
        }) {}

  function_ref(Ret (*fn)(Args...)) noexcept  // NOLINT
      : obj_(reinterpret_cast<void*>(fn)),
        invoker_([](void* o, Args... args) -> Ret {
          return reinterpret_cast<Ret (*)(Args...)>(o)(
              static_cast<Args&&>(args)...);
        }) {}

  Ret operator()(Args... args) const {
    return invoker_(obj_, static_cast<Args&&>(args)...);
  }

 private:
  void* obj_;
  Ret (*invoker_)(void*, Args...);
};

// ============================================================================
// TruncateToCapacity tag (iceoryx pattern)
// ============================================================================

struct TruncateToCapacity_t {};
static constexpr TruncateToCapacity_t TruncateToCapacity{};

// ============================================================================
// FixedString<Capacity> - Stack-allocated fixed-capacity string
// (ported from MCCC, namespace changed to osp)
// ============================================================================

/**
 * @brief Fixed-capacity, stack-allocated, null-terminated string.
 *
 * Inspired by iceoryx iox::string<N>. Ported from MCCC.
 *
 * @tparam Capacity Maximum number of characters (excluding null terminator)
 */
template <uint32_t Capacity>
class FixedString {
  static_assert(Capacity > 0U, "FixedString capacity must be > 0");

 public:
  constexpr FixedString() noexcept : buf_{'\0'}, size_(0U) {}

  template <uint32_t N, typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
  FixedString(const char (&str)[N]) noexcept : size_(N - 1U) {  // NOLINT
    static_assert(N > 0U, "String literal must include null terminator");
    static_assert(N - 1U <= Capacity, "String literal exceeds FixedString capacity");
    (void)std::memcpy(buf_, str, N);
  }

  FixedString(TruncateToCapacity_t /*tag*/, const char* str) noexcept : size_(0U) {
    if (str != nullptr) {
      uint32_t i = 0U;
      while ((i < Capacity) && (str[i] != '\0')) {
        buf_[i] = str[i];
        ++i;
      }
      size_ = i;
    }
    buf_[size_] = '\0';
  }

  FixedString(TruncateToCapacity_t /*tag*/, const char* str, uint32_t count) noexcept
      : size_(0U) {
    if (str != nullptr) {
      size_ = (count < Capacity) ? count : Capacity;
      (void)std::memcpy(buf_, str, size_);
    }
    buf_[size_] = '\0';
  }

  constexpr const char* c_str() const noexcept { return buf_; }
  constexpr uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  constexpr bool empty() const noexcept { return size_ == 0U; }

  template <uint32_t N>
  bool operator==(const FixedString<N>& rhs) const noexcept {
    if (size_ != rhs.size()) {
      return false;
    }
    return std::memcmp(buf_, rhs.c_str(), size_) == 0;
  }

  template <uint32_t N>
  bool operator!=(const FixedString<N>& rhs) const noexcept {
    return !(*this == rhs);
  }

  template <uint32_t N>
  bool operator==(const char (&str)[N]) const noexcept {
    if (size_ != (N - 1U)) {
      return false;
    }
    return std::memcmp(buf_, str, size_) == 0;
  }

  template <uint32_t N, typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
  FixedString& operator=(const char (&str)[N]) noexcept {
    static_assert(N - 1U <= Capacity, "String literal exceeds FixedString capacity");
    size_ = N - 1U;
    (void)std::memcpy(buf_, str, N);
    return *this;
  }

  FixedString& assign(TruncateToCapacity_t /*tag*/, const char* str) noexcept {
    size_ = 0U;
    if (str != nullptr) {
      uint32_t i = 0U;
      while ((i < Capacity) && (str[i] != '\0')) {
        buf_[i] = str[i];
        ++i;
      }
      size_ = i;
    }
    buf_[size_] = '\0';
    return *this;
  }

  void clear() noexcept {
    size_ = 0U;
    buf_[0] = '\0';
  }

 private:
  char buf_[Capacity + 1U];
  uint32_t size_;
};

// ============================================================================
// FixedVector<T, Capacity> - Stack-allocated fixed-capacity vector
// (ported from MCCC, namespace changed to osp)
// ============================================================================

/**
 * @brief Fixed-capacity, stack-allocated vector with no heap allocation.
 *
 * Inspired by iceoryx iox::vector<T,N>. Ported from MCCC.
 *
 * @tparam T Element type
 * @tparam Capacity Maximum number of elements
 */
template <typename T, uint32_t Capacity>
class FixedVector final {
  static_assert(Capacity > 0U, "FixedVector capacity must be > 0");

 public:
  using value_type = T;
  using size_type = uint32_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = T*;
  using const_iterator = const T*;

  FixedVector() noexcept : size_(0U) {}
  ~FixedVector() noexcept { clear(); }

  FixedVector(const FixedVector& other) noexcept : size_(0U) {
    for (uint32_t i = 0U; i < other.size_; ++i) {
      (void)push_back(other.at_unchecked(i));
    }
  }

  FixedVector& operator=(const FixedVector& other) noexcept {
    if (this != &other) {
      clear();
      for (uint32_t i = 0U; i < other.size_; ++i) {
        (void)push_back(other.at_unchecked(i));
      }
    }
    return *this;
  }

  FixedVector(FixedVector&& other) noexcept : size_(0U) {
    for (uint32_t i = 0U; i < other.size_; ++i) {
      (void)emplace_back(static_cast<T&&>(other.at_unchecked(i)));
    }
    other.clear();
  }

  FixedVector& operator=(FixedVector&& other) noexcept {
    if (this != &other) {
      clear();
      for (uint32_t i = 0U; i < other.size_; ++i) {
        (void)emplace_back(static_cast<T&&>(other.at_unchecked(i)));
      }
      other.clear();
    }
    return *this;
  }

  reference operator[](uint32_t index) noexcept { return at_unchecked(index); }
  const_reference operator[](uint32_t index) const noexcept {
    return at_unchecked(index);
  }

  reference front() noexcept { return at_unchecked(0U); }
  const_reference front() const noexcept { return at_unchecked(0U); }
  reference back() noexcept { return at_unchecked(size_ - 1U); }
  const_reference back() const noexcept { return at_unchecked(size_ - 1U); }

  pointer data() noexcept { return reinterpret_cast<T*>(storage_); }
  const_pointer data() const noexcept {
    return reinterpret_cast<const T*>(storage_);
  }

  iterator begin() noexcept { return data(); }
  const_iterator begin() const noexcept { return data(); }
  iterator end() noexcept { return data() + size_; }
  const_iterator end() const noexcept { return data() + size_; }

  bool empty() const noexcept { return size_ == 0U; }
  uint32_t size() const noexcept { return size_; }
  static constexpr uint32_t capacity() noexcept { return Capacity; }
  bool full() const noexcept { return size_ >= Capacity; }

  bool push_back(const T& value) noexcept { return emplace_back(value); }
  bool push_back(T&& value) noexcept {
    return emplace_back(static_cast<T&&>(value));
  }

  template <typename... CtorArgs>
  bool emplace_back(CtorArgs&&... args) noexcept {
    if (size_ >= Capacity) {
      return false;
    }
    ::new (&storage_[size_ * sizeof(T)]) T{static_cast<CtorArgs&&>(args)...};
    ++size_;
    return true;
  }

  bool pop_back() noexcept {
    if (size_ == 0U) {
      return false;
    }
    --size_;
    at_unchecked(size_).~T();
    return true;
  }

  bool erase_unordered(uint32_t index) noexcept {
    if (index >= size_) {
      return false;
    }
    --size_;
    if (index != size_) {
      at_unchecked(index) = static_cast<T&&>(at_unchecked(size_));
    }
    at_unchecked(size_).~T();
    return true;
  }

  void clear() noexcept {
    while (size_ > 0U) {
      --size_;
      at_unchecked(size_).~T();
    }
  }

 private:
  reference at_unchecked(uint32_t index) noexcept {
    return *(reinterpret_cast<T*>(storage_) + index);
  }
  const_reference at_unchecked(uint32_t index) const noexcept {
    return *(reinterpret_cast<const T*>(storage_) + index);
  }

  alignas(T) uint8_t storage_[sizeof(T) * Capacity];
  uint32_t size_;
};

// ============================================================================
// not_null<T> - Semantic non-null pointer wrapper (iceoryx inspired)
// ============================================================================

/**
 * @brief Wrapper that asserts pointer is non-null (debug mode).
 */
template <typename T>
class not_null final {
  static_assert(std::is_pointer<T>::value, "not_null requires a pointer type");

 public:
  explicit not_null(T ptr) noexcept : ptr_(ptr) {
    OSP_ASSERT(ptr_ != nullptr);
  }

  not_null(std::nullptr_t) = delete;

  T get() const noexcept { return ptr_; }
  T operator->() const noexcept { return ptr_; }
  typename std::remove_pointer<T>::type& operator*() const noexcept {
    return *ptr_;
  }

 private:
  T ptr_;
};

// ============================================================================
// NewType<T, Tag> - Strong type wrapper (iceoryx inspired)
// ============================================================================

/**
 * @brief Prevents accidental mixing of semantically different IDs.
 *
 * Example:
 *   struct TimerTaskIdTag {};
 *   using TimerTaskId = NewType<uint32_t, TimerTaskIdTag>;
 */
template <typename T, typename Tag>
class NewType final {
 public:
  constexpr explicit NewType(T val) noexcept : val_(val) {}
  constexpr T value() const noexcept { return val_; }

  constexpr bool operator==(NewType rhs) const noexcept {
    return val_ == rhs.val_;
  }
  constexpr bool operator!=(NewType rhs) const noexcept {
    return val_ != rhs.val_;
  }
  constexpr bool operator<(NewType rhs) const noexcept {
    return val_ < rhs.val_;
  }

 private:
  T val_;
};

// Common NewType aliases
struct TimerTaskIdTag {};
struct SessionIdTag {};
using TimerTaskId = NewType<uint32_t, TimerTaskIdTag>;
using SessionId = NewType<uint32_t, SessionIdTag>;

// ============================================================================
// ScopeGuard - RAII cleanup guard (iceoryx inspired)
// ============================================================================

/**
 * @brief Executes a cleanup callback on scope exit unless released.
 *
 * Uses FixedFunction for zero heap allocation.
 */
class ScopeGuard final {
 public:
  explicit ScopeGuard(FixedFunction<void()> cleanup) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(cleanup)), active_(true) {}

  ~ScopeGuard() {
    if (active_ && cleanup_) {
      cleanup_();
    }
  }

  void release() noexcept { active_ = false; }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

  ScopeGuard(ScopeGuard&& other) noexcept
      : cleanup_(static_cast<FixedFunction<void()>&&>(other.cleanup_)),
        active_(other.active_) {
    other.active_ = false;
  }

  ScopeGuard& operator=(ScopeGuard&&) = delete;

 private:
  FixedFunction<void()> cleanup_;
  bool active_;
};

/**
 * @brief Convenience macro for scope-exit cleanup.
 *
 * Usage:
 *   FILE* f = fopen("x.txt", "r");
 *   OSP_SCOPE_EXIT(fclose(f));
 */
#define OSP_SCOPE_EXIT(...)                                                 \
  ::osp::ScopeGuard OSP_CONCAT(_scope_guard_, __LINE__) {                  \
    ::osp::FixedFunction<void()> { [&]() { __VA_ARGS__; } }               \
  }

// ============================================================================
// and_then / or_else - Functional chaining for expected (iceoryx inspired)
// ============================================================================

/**
 * @brief If result has value, invoke fn with it and return the result.
 */
template <typename V, typename E, typename F>
auto and_then(const expected<V, E>& result, F&& fn)
    -> decltype(fn(result.value())) {
  using ReturnType = decltype(fn(result.value()));
  if (result.has_value()) {
    return fn(result.value());
  }
  return ReturnType::error(result.get_error());
}

/**
 * @brief If result has error, invoke fn with it. Returns result unchanged.
 */
template <typename V, typename E, typename F>
const expected<V, E>& or_else(const expected<V, E>& result, F&& fn) {
  if (!result.has_value()) {
    fn(result.get_error());
  }
  return result;
}

}  // namespace osp

#endif  // OSP_VOCABULARY_HPP_
