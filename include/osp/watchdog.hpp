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
 * @file watchdog.hpp
 * @brief Thread watchdog component for monitoring thread liveness.
 *
 * Provides a lightweight watchdog mechanism to detect unresponsive threads.
 * Threads register with the watchdog and receive a ThreadHeartbeat* to
 * periodically signal liveness. If a thread fails to beat within its timeout,
 * the watchdog invokes a timeout callback.
 *
 * Design:
 * - Fixed-capacity slot array, no heap allocation in hot path.
 * - StartAutoCheck() allocates via std::thread (cold path only).
 * - Beat() is lock-free (hot path) - just atomic store via ThreadHeartbeat.
 * - Check() uses mutex for slot iteration (cold path, called from timer).
 * - Callbacks executed outside mutex (collect-release-execute pattern).
 * - Compatible with -fno-exceptions -fno-rtti.
 * - No circular dependency: modules only need platform.hpp for ThreadHeartbeat.
 *
 * Dependency direction:
 *   platform.hpp (ThreadHeartbeat)  <--  all modules (Beat only)
 *   platform.hpp (ThreadHeartbeat)  <--  watchdog.hpp (Check/Register)
 *   watchdog.hpp  <--  application layer (wiring)
 *
 * Typical usage:
 *
 *   // Application layer wires watchdog to modules:
 *   osp::ThreadWatchdog<32> wd;
 *   wd.SetOnTimeout([](uint32_t id, const char* name, void* ctx) {
 *     fprintf(stderr, "Thread %s timed out!\n", name);
 *   }, nullptr);
 *
 *   // Module thread function (only needs platform.hpp):
 *   void WorkerLoop(osp::ThreadHeartbeat* hb) {
 *     while (running) {
 *       if (hb) hb->Beat();  // single atomic store
 *       // ... do work ...
 *     }
 *   }
 *
 *   // Register and wire:
 *   auto [id, hb] = wd.Register("worker", 5000);
 *   // pass hb to module, start thread...
 *
 *   // Periodic check (from TimerScheduler or main loop):
 *   wd.Check();
 */

#ifndef OSP_WATCHDOG_HPP_
#define OSP_WATCHDOG_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace osp {

// ============================================================================
// WatchdogSlotId - Strong type for watchdog slot identifiers
// ============================================================================

struct WatchdogSlotIdTag {};
using WatchdogSlotId = NewType<uint32_t, WatchdogSlotIdTag>;

// ============================================================================
// WatchdogError - Error codes for watchdog operations
// ============================================================================

enum class WatchdogError : uint8_t {
  kSlotsFull = 0,   ///< All watchdog slots are occupied.
  kInvalidTimeout,  ///< Timeout value is zero or invalid.
  kNotRegistered,   ///< Slot ID not found or already unregistered.
};

// ============================================================================
// WatchdogSlotInfo - Diagnostic snapshot for a single slot
// ============================================================================

/// POD snapshot of a watchdog slot for diagnostic iteration.
struct WatchdogSlotInfo {
  uint32_t slot_id;       ///< Slot index
  const char* name;       ///< Thread name (pointer into slot, stable)
  uint64_t timeout_us;    ///< Configured timeout (microseconds)
  uint64_t last_beat_us;  ///< Last heartbeat timestamp (microseconds)
  bool timed_out;         ///< Currently timed out
};

// ============================================================================
// ThreadWatchdog - Thread liveness monitor
// ============================================================================

/**
 * @brief Monitors thread liveness via periodic heartbeat signals.
 *
 * Register() returns a ThreadHeartbeat* that the monitored thread calls
 * Beat() on. Check() scans all registered slots and invokes callbacks
 * for timed-out or recovered threads.
 *
 * ThreadHeartbeat lives in platform.hpp, so monitored modules do NOT need
 * to include watchdog.hpp -- they only need platform.hpp (already included
 * by all osp modules).
 *
 * @tparam MaxThreads  Maximum number of concurrent monitored threads.
 */
template <uint32_t MaxThreads = 32>
class ThreadWatchdog final {
  static_assert(MaxThreads > 0, "MaxThreads must be greater than 0");

 public:
  // --------------------------------------------------------------------------
  // Callback Types
  // --------------------------------------------------------------------------

  using TimeoutCallback = void (*)(uint32_t slot_id, const char* name, void* ctx);
  using RecoverCallback = void (*)(uint32_t slot_id, const char* name, void* ctx);

  // --------------------------------------------------------------------------
  // Registration Result
  // --------------------------------------------------------------------------

  /** @brief Successful registration result: slot ID + heartbeat pointer. */
  struct RegResult {
    WatchdogSlotId id;
    ThreadHeartbeat* heartbeat;  ///< Pointer to slot's heartbeat (stable).
  };

  // --------------------------------------------------------------------------
  // Construction / Destruction
  // --------------------------------------------------------------------------

  ThreadWatchdog() noexcept = default;
  ~ThreadWatchdog() noexcept { StopAutoCheck(); }

  ThreadWatchdog(const ThreadWatchdog&) = delete;
  ThreadWatchdog& operator=(const ThreadWatchdog&) = delete;
  ThreadWatchdog(ThreadWatchdog&&) = delete;
  ThreadWatchdog& operator=(ThreadWatchdog&&) = delete;

  // --------------------------------------------------------------------------
  // Registration
  // --------------------------------------------------------------------------

  /**
   * @brief Register a thread for watchdog monitoring.
   *
   * Returns a RegResult containing the slot ID and a stable ThreadHeartbeat*
   * that the monitored thread should call Beat() on in its main loop.
   *
   * Thread-safe (acquires internal mutex).
   *
   * @param name        Thread name (max 31 chars, truncated if longer).
   * @param timeout_ms  Timeout period in milliseconds (must be > 0).
   * @return RegResult on success, or WatchdogError on failure.
   */
  expected<RegResult, WatchdogError> Register(const char* name, uint32_t timeout_ms) noexcept {
    if (timeout_ms == 0U) {
      return expected<RegResult, WatchdogError>::error(WatchdogError::kInvalidTimeout);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (!slots_[i].active.load(std::memory_order_relaxed)) {
        slots_[i].name.assign(TruncateToCapacity, name);
        slots_[i].timeout_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;
        slots_[i].heartbeat.Beat();  // Initialize with current time
        slots_[i].timed_out = false;
        slots_[i].active.store(true, std::memory_order_release);
        return expected<RegResult, WatchdogError>::success(RegResult{WatchdogSlotId(i), &slots_[i].heartbeat});
      }
    }

    return expected<RegResult, WatchdogError>::error(WatchdogError::kSlotsFull);
  }

  /**
   * @brief Unregister a thread from watchdog monitoring.
   *
   * Thread-safe (acquires internal mutex).
   *
   * @param id  The slot ID from Register().
   * @return Success, or WatchdogError::kNotRegistered if invalid.
   */
  expected<void, WatchdogError> Unregister(WatchdogSlotId id) noexcept {
    const uint32_t idx = id.value();
    if (idx >= MaxThreads) {
      return expected<void, WatchdogError>::error(WatchdogError::kNotRegistered);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_[idx].active.load(std::memory_order_relaxed)) {
      return expected<void, WatchdogError>::error(WatchdogError::kNotRegistered);
    }

    slots_[idx].active.store(false, std::memory_order_release);
    return expected<void, WatchdogError>::success();
  }

  /**
   * @brief Feed a slot by ID (convenience, for callers that have the ID).
   *
   * Equivalent to calling Beat() on the slot's ThreadHeartbeat.
   * Lock-free hot path.
   */
  void Feed(WatchdogSlotId id) noexcept {
    const uint32_t idx = id.value();
    if (OSP_LIKELY(idx < MaxThreads)) {
      slots_[idx].heartbeat.Beat();
    }
  }

  // --------------------------------------------------------------------------
  // Check (COLD PATH - called from timer or main loop)
  // --------------------------------------------------------------------------

  /**
   * @brief Scan all registered threads and invoke callbacks for timeouts.
   *
   * Callbacks are executed outside the mutex (collect-release-execute).
   *
   * @return Number of currently timed-out threads.
   */
  uint32_t Check() noexcept {
    struct PendingCallback {
      TimeoutCallback fn;
      uint32_t slot_id;
      FixedString<32> name;
      void* ctx;
    };

    PendingCallback timeout_pending[MaxThreads];
    PendingCallback recover_pending[MaxThreads];
    uint32_t timeout_count = 0U;
    uint32_t recover_count = 0U;
    uint32_t total_timed_out = 0U;

    const uint64_t now = SteadyNowUs();

    // Phase 1: Collect under lock
    {
      std::lock_guard<std::mutex> lock(mutex_);

      for (uint32_t i = 0U; i < MaxThreads; ++i) {
        if (!slots_[i].active.load(std::memory_order_acquire)) {
          continue;
        }

        const uint64_t last_beat = slots_[i].heartbeat.LastBeatUs();
        const bool is_timed_out = (now > last_beat) && ((now - last_beat) > slots_[i].timeout_us);

        if (is_timed_out && !slots_[i].timed_out) {
          slots_[i].timed_out = true;
          if (on_timeout_ != nullptr) {
            timeout_pending[timeout_count].fn = on_timeout_;
            timeout_pending[timeout_count].slot_id = i;
            timeout_pending[timeout_count].name = slots_[i].name;
            timeout_pending[timeout_count].ctx = timeout_ctx_;
            ++timeout_count;
          }
        } else if (!is_timed_out && slots_[i].timed_out) {
          slots_[i].timed_out = false;
          if (on_recovered_ != nullptr) {
            recover_pending[recover_count].fn = on_recovered_;
            recover_pending[recover_count].slot_id = i;
            recover_pending[recover_count].name = slots_[i].name;
            recover_pending[recover_count].ctx = recover_ctx_;
            ++recover_count;
          }
        }

        if (slots_[i].timed_out) {
          ++total_timed_out;
        }
      }
    }

    // Phase 2: Execute callbacks outside lock
    for (uint32_t i = 0U; i < timeout_count; ++i) {
      timeout_pending[i].fn(timeout_pending[i].slot_id, timeout_pending[i].name.c_str(), timeout_pending[i].ctx);
    }
    for (uint32_t i = 0U; i < recover_count; ++i) {
      recover_pending[i].fn(recover_pending[i].slot_id, recover_pending[i].name.c_str(), recover_pending[i].ctx);
    }

    return total_timed_out;
  }

  // --------------------------------------------------------------------------
  // Callback Configuration
  // --------------------------------------------------------------------------

  /// @pre Must be called before StartAutoCheck() or any concurrent Check().
  void SetOnTimeout(TimeoutCallback fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    on_timeout_ = fn;
    timeout_ctx_ = ctx;
  }

  /// @pre Must be called before StartAutoCheck() or any concurrent Check().
  void SetOnRecovered(RecoverCallback fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    on_recovered_ = fn;
    recover_ctx_ = ctx;
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  bool IsTimedOut(WatchdogSlotId id) const noexcept {
    const uint32_t idx = id.value();
    if (idx >= MaxThreads) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!slots_[idx].active.load(std::memory_order_acquire)) {
      return false;
    }
    return slots_[idx].timed_out;
  }

  uint32_t ActiveCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire)) {
        ++count;
      }
    }
    return count;
  }

  uint32_t TimedOutCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire) && slots_[i].timed_out) {
        ++count;
      }
    }
    return count;
  }

  /** @brief Compile-time capacity. */
  static constexpr uint32_t Capacity() noexcept { return MaxThreads; }

  /// Iterate active watchdog slots for diagnostic purposes.
  /// Callback signature: void(const WatchdogSlotInfo&).
  template <typename Fn>
  void ForEachSlot(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire)) {
        WatchdogSlotInfo info{};
        info.slot_id = i;
        info.name = slots_[i].name.c_str();
        info.timeout_us = slots_[i].timeout_us;
        info.last_beat_us = slots_[i].heartbeat.LastBeatUs();
        info.timed_out = slots_[i].timed_out;
        fn(info);
      }
    }
  }

  // --------------------------------------------------------------------------
  // TimerScheduler Integration
  // --------------------------------------------------------------------------

  /**
   * @brief Static callback for TimerScheduler integration.
   *
   * Usage:
   *   timer.Add(1000, &osp::ThreadWatchdog<32>::CheckTick, &wd);
   */
  static void CheckTick(void* ctx) noexcept {
    OSP_ASSERT(ctx != nullptr);
    static_cast<ThreadWatchdog*>(ctx)->Check();
  }

  // --------------------------------------------------------------------------
  // Self-Driven Check (optional fallback for TimerScheduler single-point failure)
  // --------------------------------------------------------------------------

  /**
   * @brief Start an internal thread that calls Check() periodically.
   *
   * Provides a fallback when TimerScheduler (the primary Check() driver)
   * may itself become unresponsive. Only one auto-check thread is created;
   * calling StartAutoCheck() again is a no-op.
   *
   * The auto-check thread is intentionally simple (sleep + Check loop)
   * and does NOT register itself with the watchdog to avoid circular
   * dependency.
   *
   * @param interval_ms  Check interval in milliseconds (must be > 0).
   */
  void StartAutoCheck(uint32_t interval_ms) noexcept {
    if (interval_ms == 0U) {
      return;
    }
    bool expected = false;
    if (!auto_check_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;  // Already running
    }
    auto_check_thread_ = std::thread([this, interval_ms]() {
      while (auto_check_running_.load(std::memory_order_acquire)) {
        Check();
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      }
    });
  }

  /**
   * @brief Stop the internal auto-check thread.
   *
   * Blocks until the thread exits. Safe to call if StartAutoCheck()
   * was never called (no-op).
   */
  void StopAutoCheck() noexcept {
    bool expected = true;
    if (!auto_check_running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      return;  // Another thread is already stopping or not running
    }
    if (auto_check_thread_.joinable()) {
      auto_check_thread_.join();
    }
  }

 private:
  struct Slot {
    std::atomic<bool> active{false};
    ThreadHeartbeat heartbeat;  ///< Heartbeat signal (in platform.hpp).
    uint64_t timeout_us{0};
    bool timed_out{false};  ///< Protected by mutex_.
    FixedString<32> name;
  };

  Slot slots_[MaxThreads]{};
  mutable std::mutex mutex_;
  TimeoutCallback on_timeout_{nullptr};
  void* timeout_ctx_{nullptr};
  RecoverCallback on_recovered_{nullptr};
  void* recover_ctx_{nullptr};
  std::atomic<bool> auto_check_running_{false};
  std::thread auto_check_thread_;
};

// ============================================================================
// WatchdogGuard - RAII watchdog registration (for application layer)
// ============================================================================

/**
 * @brief RAII wrapper for automatic watchdog registration/unregistration.
 *
 * Handles nullptr watchdog gracefully (no-op).
 *
 * @tparam MaxThreads  Must match the ThreadWatchdog template parameter.
 */
template <uint32_t MaxThreads = 32>
class WatchdogGuard final {
  static_assert(MaxThreads > 0, "MaxThreads must be greater than 0");

 public:
  /**
   * @brief Construct and register with the watchdog.
   *
   * If wd is nullptr, the guard is a no-op (IsValid() returns false).
   */
  explicit WatchdogGuard(ThreadWatchdog<MaxThreads>* wd, const char* name, uint32_t timeout_ms) noexcept
      : wd_(wd), hb_(nullptr), id_(WatchdogSlotId(0)), valid_(false) {
    if (wd_ == nullptr)
      return;
    auto result = wd_->Register(name, timeout_ms);
    if (result.has_value()) {
      id_ = result.value().id;
      hb_ = result.value().heartbeat;
      valid_ = true;
    }
  }

  ~WatchdogGuard() noexcept {
    if (valid_) {
      (void)wd_->Unregister(id_);
    }
  }

  WatchdogGuard(const WatchdogGuard&) = delete;
  WatchdogGuard& operator=(const WatchdogGuard&) = delete;
  WatchdogGuard(WatchdogGuard&&) = delete;
  WatchdogGuard& operator=(WatchdogGuard&&) = delete;

  /** @brief Signal liveness (forwards to ThreadHeartbeat::Beat). */
  void Feed() noexcept {
    if (hb_ != nullptr) {
      hb_->Beat();
    }
  }

  bool IsValid() const noexcept { return valid_; }

 private:
  ThreadWatchdog<MaxThreads>* wd_;
  ThreadHeartbeat* hb_;
  WatchdogSlotId id_;
  bool valid_;
};

}  // namespace osp

#endif  // OSP_WATCHDOG_HPP_
