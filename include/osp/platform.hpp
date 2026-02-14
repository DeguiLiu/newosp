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
 * @file platform.hpp
 * @brief Platform detection, compiler hints, and assertion macros.
 *
 * Replaces the original oscbb.h conditional compilation and custom type macros.
 */

#ifndef OSP_PLATFORM_HPP_
#define OSP_PLATFORM_HPP_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace osp {

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(__linux__)
#define OSP_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define OSP_PLATFORM_MACOS 1
#elif defined(_WIN32)
#define OSP_PLATFORM_WINDOWS 1
#endif

// ============================================================================
// Architecture Detection
// ============================================================================

#if defined(__arm__) || defined(__aarch64__)
#define OSP_ARCH_ARM 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#define OSP_ARCH_X86 1
#endif

// ============================================================================
// Cache Line Size
// ============================================================================

static constexpr size_t kCacheLineSize = 64;

// ============================================================================
// Compiler Hints
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define OSP_LIKELY(x) __builtin_expect(!!(x), 1)
#define OSP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define OSP_UNUSED __attribute__((unused))
#else
#define OSP_LIKELY(x) (x)
#define OSP_UNLIKELY(x) (x)
#define OSP_UNUSED
#endif

// ============================================================================
// Assert Macro
// ============================================================================

namespace detail {

/**
 * @brief Called when an assertion fails in debug mode.
 *
 * Prints the failed condition, file, and line to stderr, then aborts.
 */
inline void AssertFail(const char* cond, const char* file, int line) {
  (void)std::fprintf(stderr, "OSP_ASSERT failed: %s at %s:%d\n", cond, file, line);
  std::abort();
}

}  // namespace detail

#ifdef NDEBUG
#define OSP_ASSERT(cond) ((void)0)
#else
#define OSP_ASSERT(cond)                                                    \
  ((cond) ? ((void)0) : ::osp::detail::AssertFail(#cond, __FILE__, __LINE__))
#endif

// ============================================================================
// Monotonic Clock Utilities
// ============================================================================

/**
 * @brief Return current monotonic time in nanoseconds (steady_clock).
 */
inline uint64_t SteadyNowNs() noexcept {
  const auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count());
}

/**
 * @brief Return current monotonic time in microseconds (steady_clock).
 */
inline uint64_t SteadyNowUs() noexcept {
  const auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(dur).count());
}

// ============================================================================
// ThreadHeartbeat - Lightweight liveness signal for thread monitoring
// ============================================================================

/**
 * @brief Minimal heartbeat primitive for thread liveness monitoring.
 *
 * Each monitored thread holds a pointer to a ThreadHeartbeat and calls Beat()
 * in its main loop. An external watchdog reads last_beat_us to detect timeouts.
 *
 * Design: lives in platform.hpp so all modules can use it without extra
 * dependencies. Only one atomic store per loop iteration (hot path).
 */
struct ThreadHeartbeat {
  std::atomic<uint64_t> last_beat_us{0};  ///< Last heartbeat timestamp (us).

  /** @brief Record a heartbeat (hot path, single relaxed store). */
  void Beat() noexcept {
    last_beat_us.store(SteadyNowUs(), std::memory_order_relaxed);
  }

  /** @brief Read last heartbeat timestamp. */
  uint64_t LastBeatUs() const noexcept {
    return last_beat_us.load(std::memory_order_acquire);
  }
};

// ============================================================================
// Macro Helpers
// ============================================================================

#define OSP_CONCAT_IMPL(a, b) a##b
#define OSP_CONCAT(a, b) OSP_CONCAT_IMPL(a, b)

}  // namespace osp

#endif  // OSP_PLATFORM_HPP_
