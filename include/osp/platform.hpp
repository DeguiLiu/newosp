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

// ============================================================================
// RT-Thread Detection
// ============================================================================
// Must precede any #include so the conditional include below can pull in
// <rtthread.h>. Primary signal: <rtthread.h> present on the include path
// (every RT-Thread build ships it). Fallback: rtconfig.h macros (RT_VERSION /
// RT_USING_*) that RT-Thread builds define. Users may force it via
// -DOSP_PLATFORM_RTTHREAD=1.
#ifndef OSP_PLATFORM_RTTHREAD
#if defined(__has_include)
#if __has_include(<rtthread.h>)
#define OSP_PLATFORM_RTTHREAD 1
#endif
#endif
#if !defined(OSP_PLATFORM_RTTHREAD)
#if defined(RT_VERSION) || defined(RT_USING_HOOK) || defined(RT_USING_NEWLIB) || defined(RT_USING_LIBC)
#define OSP_PLATFORM_RTTHREAD 1
#endif
#endif
#endif  // ifndef OSP_PLATFORM_RTTHREAD

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <atomic>

#if defined(OSP_PLATFORM_RTTHREAD)
#include <rtthread.h>
#else
#include <ctime>

#include <chrono>
#include <thread>
#endif

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
// Network Stack Detection
// ============================================================================

// OSP_HAS_NETWORK: set to 1 when the target has a BSD-socket API available.
// Users may override via CMake (-DOSP_HAS_NETWORK=0) for bare-metal or
// RTOS configurations that do not enable a network stack.
#ifndef OSP_HAS_NETWORK
#if defined(__has_include)
#if __has_include(<sys/socket.h>)
#define OSP_HAS_NETWORK 1
#else
#define OSP_HAS_NETWORK 0
#endif
#else
// Conservative default: assume network is available on known desktop/server OS.
#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#define OSP_HAS_NETWORK 1
#else
#define OSP_HAS_NETWORK 0
#endif
#endif
#endif  // ifndef OSP_HAS_NETWORK

// ============================================================================
// Network Backend Selection
// ============================================================================

// OSP_NET_BACKEND: underlying network API used by socket.hpp / io_poller.hpp.
//   0 = POSIX BSD socket (Linux host / ARM-Linux)
//   1 = lwIP native socket API (lwip_* direct calls; used for host unixsim
//       tests and as the adapter target for RT-Thread + lwIP via SAL/netdev)
//   2 = network disabled
// Detect RT-Thread via rtconfig macros; users may override via compile defs.
#ifndef OSP_NET_BACKEND
#if defined(OSP_PLATFORM_RTTHREAD)
#define OSP_NET_BACKEND 1
#elif !OSP_HAS_NETWORK
#define OSP_NET_BACKEND 2
#else
#define OSP_NET_BACKEND 0
#endif
#endif  // ifndef OSP_NET_BACKEND

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
#define OSP_PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define OSP_LIKELY(x) (x)
#define OSP_UNLIKELY(x) (x)
#define OSP_UNUSED
#define OSP_PRINTF_FMT(a, b)
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
#if defined(OSP_PLATFORM_RTTHREAD)
  // Requires RT_USING_CONSOLE. Halt with an observable side effect (tick
  // delay): an empty infinite loop has no observable behavior and is UB under
  // [intro.progress], so the optimizer may assume it terminates.
  rt_kprintf("OSP_ASSERT failed: %s at %s:%d\n", cond, file, line);
  for (;;) {
    rt_thread_delay(1);
  }
#else
  (void)std::fprintf(stderr, "OSP_ASSERT failed: %s at %s:%d\n", cond, file, line);
  std::abort();
#endif
}

}  // namespace detail

#ifdef NDEBUG
#define OSP_ASSERT(cond) ((void)0)
#else
#define OSP_ASSERT(cond) ((cond) ? ((void)0) : ::osp::detail::AssertFail(#cond, __FILE__, __LINE__))
#endif

// ============================================================================
// Monotonic Clock Utilities
// ============================================================================

/**
 * @brief Return current monotonic time in nanoseconds.
 *
 * On RT-Thread this is tick-based: 1 tick = 1e9 / RT_TICK_PER_SECOND ns.
 * rt_tick_get() is a 32-bit counter, so the returned value wraps (~49.7 days
 * at 1000 Hz) and is only valid for short-window relative comparisons, not as
 * an absolute timestamp.
 */
inline uint64_t SteadyNowNs() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  return static_cast<uint64_t>(rt_tick_get()) * (1000000000ULL / RT_TICK_PER_SECOND);
#else
  const auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count());
#endif
}

/**
 * @brief Return coarse monotonic time in nanoseconds.
 *
 * Uses CLOCK_MONOTONIC_COARSE (~4ms resolution) which on Linux is
 * vDSO-backed and avoids syscall even when the kernel disables vDSO
 * for CLOCK_MONOTONIC. Falls back to SteadyNowNs() on non-Linux.
 */
inline uint64_t CoarseNowNs() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  // Tick is already a coarse clock; no separate coarse source on RT-Thread.
  return SteadyNowNs();
#elif defined(CLOCK_MONOTONIC_COARSE)
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
#else
  return SteadyNowNs();
#endif
}

/**
 * @brief Return current monotonic time in microseconds.
 *
 * RT-Thread: 1 tick = 1e6 / RT_TICK_PER_SECOND us. Same 32-bit wrap caveat
 * as SteadyNowNs().
 */
inline uint64_t SteadyNowUs() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  return static_cast<uint64_t>(rt_tick_get()) * (1000000ULL / RT_TICK_PER_SECOND);
#else
  const auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(dur).count());
#endif
}

/**
 * @brief Return coarse monotonic time in microseconds.
 *
 * Uses CLOCK_MONOTONIC_COARSE (typically ~4ms resolution, vDSO-backed,
 * no syscall even when the kernel disables vDSO for MONOTONIC).
 * Suitable for message timestamps and heartbeat where microsecond precision
 * is unnecessary but overhead matters on low-end CPUs (100 MHz).
 */
inline uint64_t CoarseNowUs() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  return SteadyNowUs();
#elif defined(CLOCK_MONOTONIC_COARSE)
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
#else
  return SteadyNowUs();
#endif
}

// ============================================================================
// CpuRelax - Architecture-specific pause hint for spin loops
// ============================================================================

/**
 * @brief Yield the current thread's CPU slice.
 *
 * RT-Thread: rt_thread_yield() (cooperative switch, returns to the scheduler).
 * Host: std::this_thread::yield(). Modules should call this instead of
 * std::this_thread::yield() directly so the RT-Thread port stays linkable
 * without a pthread/SAL layer.
 */
inline void ThreadYield() noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  rt_thread_yield();
#else
  std::this_thread::yield();
#endif
}

/**
 * @brief Sleep for at least us microseconds.
 *
 * RT-Thread resolves to a millisecond delay (rt_thread_mdelay), so sub-tick
 * sleeps are clamped to 1 ms. Host: std::this_thread::sleep_for.
 */
inline void ThreadSleepUs(uint64_t us) noexcept {
#if defined(OSP_PLATFORM_RTTHREAD)
  const uint64_t ms = (us + 999) / 1000;
  rt_thread_mdelay(static_cast<rt_uint32_t>(ms < 1 ? 1 : ms));
#else
  std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}

/**
 * @brief Issue a CPU relax/pause hint for spin-wait loops.
 *
 * Reduces power consumption and avoids pipeline stalls on x86 (PAUSE) and
 * ARM (YIELD). Falls back to ThreadYield() on unknown architectures.
 */
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  ThreadYield();
#endif
}

// ============================================================================
// AdaptiveBackoff - Three-phase backoff: spin -> yield -> sleep
// ============================================================================

/**
 * @brief Adaptive backoff strategy for busy-wait loops.
 *
 * Three phases:
 *   1. Spin with CPU relax hint (exponential: 1..64 iterations)
 *   2. Thread yield (kYieldLimit times)
 *   3. Sleep (50us, coarse wait)
 *
 * Stack-only, no heap allocation. -fno-exceptions -fno-rtti safe.
 */
class AdaptiveBackoff {
 public:
  void Reset() noexcept { spin_count_ = 0U; }

  void Wait() noexcept {
    if (spin_count_ < kSpinLimit) {
      const uint32_t iters = 1U << spin_count_;
      for (uint32_t i = 0U; i < iters; ++i) {
        CpuRelax();
      }
      ++spin_count_;
    } else if (spin_count_ < kSpinLimit + kYieldLimit) {
      ThreadYield();
      ++spin_count_;
    } else {
      ThreadSleepUs(50);
    }
  }

  /**
   * @brief Check if still in the spin phase (before yield/sleep).
   *
   * Useful for worker loops that want to spin briefly before falling
   * through to a condition_variable wait.
   */
  bool InSpinPhase() const noexcept { return spin_count_ < kSpinLimit; }

 private:
  static constexpr uint32_t kSpinLimit = 6U;   ///< ~1-64 spins
  static constexpr uint32_t kYieldLimit = 4U;  ///< 4 yields before sleep
  uint32_t spin_count_{0U};
};

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
  void Beat() noexcept { last_beat_us.store(SteadyNowUs(), std::memory_order_relaxed); }

  /** @brief Read last heartbeat timestamp (relaxed: only carries timestamp, no data dependency). */
  [[nodiscard]] uint64_t LastBeatUs() const noexcept { return last_beat_us.load(std::memory_order_relaxed); }
};

// ============================================================================
// Macro Helpers
// ============================================================================

#define OSP_CONCAT_IMPL(a, b) a##b
#define OSP_CONCAT(a, b) OSP_CONCAT_IMPL(a, b)

}  // namespace osp

#endif  // OSP_PLATFORM_HPP_
