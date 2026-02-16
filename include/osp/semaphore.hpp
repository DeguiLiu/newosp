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
 * @file semaphore.hpp
 * @brief Counting and binary semaphores, plus POSIX sem_t wrapper.
 *
 * Provides LightSemaphore (mutex+condvar based), BinarySemaphore
 * (max count 1), and PosixSemaphore (sem_t RAII wrapper for Linux/macOS).
 * Header-only, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SEMAPHORE_HPP_
#define OSP_SEMAPHORE_HPP_

#include "osp/platform.hpp"

#include <cstdint>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)
#include <cerrno>

#include <semaphore.h>
#include <time.h>
#endif

namespace osp {

// ============================================================================
// SemaphoreError
// ============================================================================

enum class SemaphoreError : uint8_t { kTimeout = 0, kInterrupted, kInvalid };

// ============================================================================
// LightSemaphore
// ============================================================================

/**
 * @brief Lightweight counting semaphore using mutex + condition_variable.
 *
 * Thread-safe, no heap allocation, -fno-exceptions -fno-rtti compatible.
 * Suitable for embedded systems where POSIX semaphores are unavailable
 * or where a portable C++17 implementation is preferred.
 *
 * Usage:
 * @code
 *   osp::LightSemaphore sem(0);
 *   // Producer thread:
 *   sem.Signal();
 *   // Consumer thread:
 *   sem.Wait();
 * @endcode
 */
class LightSemaphore final {
 public:
  /**
   * @brief Construct with an initial count.
   * @param initial_count Starting value of the semaphore counter.
   */
  explicit LightSemaphore(uint32_t initial_count = 0) noexcept : count_(initial_count) {}

  ~LightSemaphore() = default;

  // Non-copyable, non-movable
  LightSemaphore(const LightSemaphore&) = delete;
  LightSemaphore& operator=(const LightSemaphore&) = delete;
  LightSemaphore(LightSemaphore&&) = delete;
  LightSemaphore& operator=(LightSemaphore&&) = delete;

  // ==========================================================================
  // Public API
  // ==========================================================================

  /**
   * @brief Increment the count and wake one waiting thread.
   */
  void Signal() noexcept {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      ++count_;
    }
    cv_.notify_one();
  }

  /**
   * @brief Alias for Signal().
   */
  void Post() noexcept { Signal(); }

  /**
   * @brief Decrement the count, blocking if it is zero.
   */
  void Wait() noexcept {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return count_ > 0U; });
    --count_;
  }

  /**
   * @brief Non-blocking try-decrement.
   * @return true if the count was decremented, false if it was zero.
   */
  bool TryWait() noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    if (count_ > 0U) {
      --count_;
      return true;
    }
    return false;
  }

  /**
   * @brief Timed wait with microsecond timeout.
   * @param timeout_us Maximum time to wait in microseconds.
   * @return true if the count was decremented before timeout, false otherwise.
   */
  bool WaitFor(uint64_t timeout_us) noexcept {
    std::unique_lock<std::mutex> lk(mtx_);
    bool result = cv_.wait_for(lk, std::chrono::microseconds(timeout_us), [this] { return count_ > 0U; });
    if (result) {
      --count_;
    }
    return result;
  }

  /**
   * @brief Current count (approximate, relaxed ordering).
   * @return The current semaphore count.
   */
  uint32_t Count() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return count_;
  }

 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  uint32_t count_;
};

// ============================================================================
// BinarySemaphore
// ============================================================================

/**
 * @brief Binary semaphore (max count = 1).
 *
 * Signal() sets the count to 1 (never increments beyond 1).
 * Otherwise behaves identically to LightSemaphore.
 *
 * Usage:
 * @code
 *   osp::BinarySemaphore bsem;
 *   bsem.Signal();   // count becomes 1
 *   bsem.Signal();   // count stays 1 (clamped)
 *   bsem.Wait();     // count becomes 0
 * @endcode
 */
class BinarySemaphore final {
 public:
  /**
   * @brief Construct with an initial count (clamped to 0 or 1).
   * @param initial_count Starting value, clamped to max 1.
   */
  explicit BinarySemaphore(uint32_t initial_count = 0) noexcept : count_(initial_count > 0U ? 1U : 0U) {}

  ~BinarySemaphore() = default;

  // Non-copyable, non-movable
  BinarySemaphore(const BinarySemaphore&) = delete;
  BinarySemaphore& operator=(const BinarySemaphore&) = delete;
  BinarySemaphore(BinarySemaphore&&) = delete;
  BinarySemaphore& operator=(BinarySemaphore&&) = delete;

  // ==========================================================================
  // Public API
  // ==========================================================================

  /**
   * @brief Set count to 1 (clamped, never exceeds 1) and wake one waiter.
   */
  void Signal() noexcept {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      count_ = 1U;
    }
    cv_.notify_one();
  }

  /**
   * @brief Alias for Signal().
   */
  void Post() noexcept { Signal(); }

  /**
   * @brief Decrement the count, blocking if it is zero.
   */
  void Wait() noexcept {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return count_ > 0U; });
    count_ = 0U;
  }

  /**
   * @brief Non-blocking try-decrement.
   * @return true if the count was decremented, false if it was zero.
   */
  bool TryWait() noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    if (count_ > 0U) {
      count_ = 0U;
      return true;
    }
    return false;
  }

  /**
   * @brief Timed wait with microsecond timeout.
   * @param timeout_us Maximum time to wait in microseconds.
   * @return true if the count was decremented before timeout, false otherwise.
   */
  bool WaitFor(uint64_t timeout_us) noexcept {
    std::unique_lock<std::mutex> lk(mtx_);
    bool result = cv_.wait_for(lk, std::chrono::microseconds(timeout_us), [this] { return count_ > 0U; });
    if (result) {
      count_ = 0U;
    }
    return result;
  }

  /**
   * @brief Current count (0 or 1).
   * @return The current semaphore count.
   */
  uint32_t Count() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return count_;
  }

 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  uint32_t count_;
};

// ============================================================================
// PosixSemaphore (Linux / macOS only)
// ============================================================================

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

/**
 * @brief RAII wrapper around POSIX sem_t.
 *
 * Uses sem_init / sem_destroy for lifecycle management.
 * On Linux, WaitFor() uses sem_timedwait for precise timing.
 * On macOS, WaitFor() uses a poll loop with sem_trywait since
 * macOS does not support sem_timedwait.
 *
 * Non-copyable, non-movable.
 */
class PosixSemaphore final {
 public:
  /**
   * @brief Construct and initialize the POSIX semaphore.
   * @param initial_count Initial semaphore value.
   */
  explicit PosixSemaphore(uint32_t initial_count = 0) noexcept : valid_(false) {
    if (::sem_init(&sem_, 0, initial_count) == 0) {
      valid_ = true;
    }
  }

  /**
   * @brief Destroy the POSIX semaphore.
   */
  ~PosixSemaphore() {
    if (valid_) {
      ::sem_destroy(&sem_);
    }
  }

  // Non-copyable, non-movable
  PosixSemaphore(const PosixSemaphore&) = delete;
  PosixSemaphore& operator=(const PosixSemaphore&) = delete;
  PosixSemaphore(PosixSemaphore&&) = delete;
  PosixSemaphore& operator=(PosixSemaphore&&) = delete;

  // ==========================================================================
  // Public API
  // ==========================================================================

  /**
   * @brief Increment the semaphore (sem_post).
   */
  void Post() noexcept {
    OSP_ASSERT(valid_);
    ::sem_post(&sem_);
  }

  /**
   * @brief Decrement the semaphore, blocking if zero (sem_wait).
   */
  void Wait() noexcept {
    OSP_ASSERT(valid_);
    while (::sem_wait(&sem_) != 0) {
      // Retry on EINTR (interrupted by signal)
      if (errno != EINTR) {
        break;
      }
    }
  }

  /**
   * @brief Non-blocking try-decrement (sem_trywait).
   * @return true if decremented, false if the semaphore was zero.
   */
  bool TryWait() noexcept {
    OSP_ASSERT(valid_);
    return ::sem_trywait(&sem_) == 0;
  }

  /**
   * @brief Timed wait with microsecond timeout.
   * @param timeout_us Maximum time to wait in microseconds.
   * @return true if decremented before timeout, false otherwise.
   *
   * On Linux, uses sem_timedwait for efficient kernel-level waiting.
   * On macOS, falls back to a poll loop with short sleeps since
   * macOS does not provide sem_timedwait for unnamed semaphores.
   */
  bool WaitFor(uint64_t timeout_us) noexcept {
    OSP_ASSERT(valid_);
#if defined(OSP_PLATFORM_LINUX)
    return TimedWaitLinux(timeout_us);
#elif defined(OSP_PLATFORM_MACOS)
    return TimedWaitPoll(timeout_us);
#else
    return TimedWaitPoll(timeout_us);
#endif
  }

  /**
   * @brief Check if the underlying sem_t was initialized successfully.
   * @return true if valid.
   */
  bool IsValid() const noexcept { return valid_; }

 private:
#if defined(OSP_PLATFORM_LINUX)
  /**
   * @brief Linux implementation using sem_timedwait with absolute deadline.
   */
  bool TimedWaitLinux(uint64_t timeout_us) noexcept {
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      return false;
    }

    // Add timeout to current time
    uint64_t nsec = static_cast<uint64_t>(ts.tv_nsec) + (timeout_us % 1000000ULL) * 1000ULL;
    ts.tv_sec += static_cast<time_t>(timeout_us / 1000000ULL + nsec / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(nsec % 1000000000ULL);

    while (true) {
      int ret = ::sem_timedwait(&sem_, &ts);
      if (ret == 0) {
        return true;
      }
      if (errno == EINTR) {
        continue;  // Retry on signal interruption
      }
      return false;  // ETIMEDOUT or other error
    }
  }
#endif

  /**
   * @brief Poll-based timed wait (used on macOS or as fallback).
   *
   * Polls sem_trywait in a loop with short sleeps until the timeout
   * expires. Less efficient than sem_timedwait but portable.
   */
  bool TimedWaitPoll(uint64_t timeout_us) noexcept {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);

    // Try immediately first
    if (::sem_trywait(&sem_) == 0) {
      return true;
    }

    // Poll with backoff
    constexpr uint64_t kInitialSleepUs = 50;
    constexpr uint64_t kMaxSleepUs = 1000;
    uint64_t sleep_us = kInitialSleepUs;

    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
      if (::sem_trywait(&sem_) == 0) {
        return true;
      }
      // Simple backoff: double sleep up to max
      if (sleep_us < kMaxSleepUs) {
        sleep_us *= 2U;
        if (sleep_us > kMaxSleepUs) {
          sleep_us = kMaxSleepUs;
        }
      }
    }

    // Final attempt at deadline
    return ::sem_trywait(&sem_) == 0;
  }

  sem_t sem_;
  bool valid_;
};

#endif  // OSP_PLATFORM_LINUX || OSP_PLATFORM_MACOS

}  // namespace osp

#endif  // OSP_SEMAPHORE_HPP_
