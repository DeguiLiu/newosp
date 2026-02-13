/**
 * @file watchdog.hpp
 * @brief Thread watchdog component for monitoring thread liveness.
 *
 * Provides a lightweight watchdog mechanism to detect unresponsive threads.
 * Threads register with the watchdog and periodically "feed" it to signal
 * they are alive. If a thread fails to feed within its timeout period,
 * the watchdog invokes a timeout callback.
 *
 * Design:
 * - Zero heap allocation, fixed-capacity slot array.
 * - Feed() is lock-free (hot path) - just atomic store.
 * - Check() uses mutex for slot iteration (cold path, called from timer).
 * - Callbacks executed outside mutex (collect-release-execute pattern).
 * - Compatible with -fno-exceptions -fno-rtti.
 *
 * Typical usage:
 *
 *   osp::ThreadWatchdog<32> wd;
 *   wd.SetOnTimeout([](uint32_t id, const char* name, void* ctx) {
 *     fprintf(stderr, "Thread %s timed out!\n", name);
 *   }, nullptr);
 *
 *   // In worker thread:
 *   auto slot = wd.Register("WorkerThread", 5000);  // 5s timeout
 *   if (slot.has_value()) {
 *     while (running) {
 *       // ... do work ...
 *       wd.Feed(slot.value());  // Signal alive
 *     }
 *     wd.Unregister(slot.value());
 *   }
 *
 *   // In main thread or timer callback:
 *   wd.Check();  // Scan for timeouts
 *
 * Integration with TimerScheduler:
 *
 *   osp::TimerScheduler<8> timer;
 *   osp::ThreadWatchdog<32> wd;
 *   timer.Add(1000, &osp::ThreadWatchdog<32>::CheckTick, &wd);
 *   timer.Start();
 */

#ifndef OSP_WATCHDOG_HPP_
#define OSP_WATCHDOG_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace osp {

// ============================================================================
// WatchdogSlotId - Strong type for watchdog slot identifiers
// ============================================================================

struct WatchdogSlotIdTag {};
using WatchdogSlotId = NewType<uint32_t, WatchdogSlotIdTag>;

// ============================================================================
// WatchdogError - Error codes for watchdog operations
// ============================================================================

/**
 * @brief Error codes returned by watchdog operations.
 */
enum class WatchdogError : uint8_t {
  kSlotsFull = 0,         ///< All watchdog slots are occupied.
  kInvalidTimeout,        ///< Timeout value is zero or invalid.
  kNotRegistered,         ///< Slot ID not found or already unregistered.
  kAlreadyRegistered      ///< Thread already registered (reserved).
};

// ============================================================================
// ThreadWatchdog - Thread liveness monitor
// ============================================================================

/**
 * @brief Monitors thread liveness via periodic feed operations.
 *
 * Threads register with a timeout period and must call Feed() at least
 * once per period. Check() scans all registered threads and invokes
 * callbacks for timed-out or recovered threads.
 *
 * @tparam MaxThreads  Maximum number of concurrent monitored threads.
 */
template <uint32_t MaxThreads = 32>
class ThreadWatchdog final {
 public:
  // --------------------------------------------------------------------------
  // Callback Types
  // --------------------------------------------------------------------------

  /**
   * @brief Callback invoked when a thread times out.
   *
   * @param slot_id  The slot ID of the timed-out thread.
   * @param name     The thread name (null-terminated, max 31 chars).
   * @param ctx      User-supplied context pointer.
   */
  using TimeoutCallback = void (*)(uint32_t slot_id, const char* name, void* ctx);

  /**
   * @brief Callback invoked when a previously timed-out thread recovers.
   *
   * @param slot_id  The slot ID of the recovered thread.
   * @param name     The thread name (null-terminated, max 31 chars).
   * @param ctx      User-supplied context pointer.
   */
  using RecoverCallback = void (*)(uint32_t slot_id, const char* name, void* ctx);

  // --------------------------------------------------------------------------
  // Construction / Destruction
  // --------------------------------------------------------------------------

  /**
   * @brief Construct a watchdog. Zero heap allocation.
   */
  ThreadWatchdog() noexcept = default;

  /**
   * @brief Destructor. Does not automatically unregister threads.
   */
  ~ThreadWatchdog() = default;

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
   * Finds a free slot, marks it active, and records the timeout period.
   * The thread must call Feed() at least once per timeout_ms milliseconds.
   *
   * Thread-safe (acquires internal mutex).
   *
   * @param name        Thread name (max 31 chars, truncated if longer).
   * @param timeout_ms  Timeout period in milliseconds (must be > 0).
   *
   * @return WatchdogSlotId on success, or WatchdogError on failure:
   *         - kInvalidTimeout if timeout_ms == 0.
   *         - kSlotsFull      if all slots are occupied.
   */
  expected<WatchdogSlotId, WatchdogError> Register(const char* name,
                                                     uint32_t timeout_ms) noexcept {
    if (timeout_ms == 0U) {
      return expected<WatchdogSlotId, WatchdogError>::error(
          WatchdogError::kInvalidTimeout);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (!slots_[i].active.load(std::memory_order_relaxed)) {
        slots_[i].name.assign(TruncateToCapacity, name);
        slots_[i].timeout_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;
        slots_[i].last_feed_us.store(SteadyNowUs(), std::memory_order_relaxed);
        slots_[i].timed_out = false;
        slots_[i].active.store(true, std::memory_order_release);
        return expected<WatchdogSlotId, WatchdogError>::success(
            WatchdogSlotId(i));
      }
    }

    return expected<WatchdogSlotId, WatchdogError>::error(
        WatchdogError::kSlotsFull);
  }

  /**
   * @brief Unregister a thread from watchdog monitoring.
   *
   * Marks the slot inactive and makes it available for reuse.
   *
   * Thread-safe (acquires internal mutex).
   *
   * @param id  The slot ID returned by Register().
   *
   * @return Success, or WatchdogError::kNotRegistered if id is invalid.
   */
  expected<void, WatchdogError> Unregister(WatchdogSlotId id) noexcept {
    const uint32_t idx = id.value();
    if (idx >= MaxThreads) {
      return expected<void, WatchdogError>::error(
          WatchdogError::kNotRegistered);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_[idx].active.load(std::memory_order_relaxed)) {
      return expected<void, WatchdogError>::error(
          WatchdogError::kNotRegistered);
    }

    slots_[idx].active.store(false, std::memory_order_release);
    return expected<void, WatchdogError>::success();
  }

  // --------------------------------------------------------------------------
  // Feed (HOT PATH - lock-free)
  // --------------------------------------------------------------------------

  /**
   * @brief Signal that the thread is alive.
   *
   * Updates the last feed timestamp for the given slot. This is the hot path
   * and must be as fast as possible - just an atomic store with relaxed
   * ordering.
   *
   * Lock-free, no mutex, no branches (except bounds check).
   *
   * @param id  The slot ID returned by Register().
   */
  void Feed(WatchdogSlotId id) noexcept {
    const uint32_t idx = id.value();
    if (OSP_LIKELY(idx < MaxThreads)) {
      slots_[idx].last_feed_us.store(SteadyNowUs(), std::memory_order_relaxed);
    }
  }

  // --------------------------------------------------------------------------
  // Check (COLD PATH - called from timer)
  // --------------------------------------------------------------------------

  /**
   * @brief Scan all registered threads and invoke callbacks for timeouts.
   *
   * Iterates active slots, compares current time vs last feed time + timeout.
   * If a thread has timed out and wasn't already marked timed_out, invokes
   * on_timeout_. If a thread was timed_out but has now fed, invokes
   * on_recovered_.
   *
   * Callbacks are executed outside the mutex (collect-release-execute pattern).
   *
   * Thread-safe (acquires internal mutex for slot iteration).
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

    // Phase 1: Collect callbacks under lock
    {
      std::lock_guard<std::mutex> lock(mutex_);

      for (uint32_t i = 0U; i < MaxThreads; ++i) {
        if (!slots_[i].active.load(std::memory_order_acquire)) {
          continue;
        }

        const uint64_t last_feed = slots_[i].last_feed_us.load(
            std::memory_order_relaxed);
        const uint64_t deadline = last_feed + slots_[i].timeout_us;
        const bool is_timed_out = (now > deadline);

        if (is_timed_out && !slots_[i].timed_out) {
          // Newly timed out
          slots_[i].timed_out = true;
          if (on_timeout_ != nullptr) {
            timeout_pending[timeout_count].fn = on_timeout_;
            timeout_pending[timeout_count].slot_id = i;
            timeout_pending[timeout_count].name = slots_[i].name;
            timeout_pending[timeout_count].ctx = timeout_ctx_;
            ++timeout_count;
          }
        } else if (!is_timed_out && slots_[i].timed_out) {
          // Recovered
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
    // mutex_ released here

    // Phase 2: Execute callbacks outside the lock
    for (uint32_t i = 0U; i < timeout_count; ++i) {
      timeout_pending[i].fn(timeout_pending[i].slot_id,
                            timeout_pending[i].name.c_str(),
                            timeout_pending[i].ctx);
    }

    for (uint32_t i = 0U; i < recover_count; ++i) {
      recover_pending[i].fn(recover_pending[i].slot_id,
                            recover_pending[i].name.c_str(),
                            recover_pending[i].ctx);
    }

    return total_timed_out;
  }

  // --------------------------------------------------------------------------
  // Callback Configuration
  // --------------------------------------------------------------------------

  /**
   * @brief Set the timeout callback.
   *
   * @param fn   Callback function pointer (may be nullptr to disable).
   * @param ctx  User context forwarded to fn.
   */
  void SetOnTimeout(TimeoutCallback fn, void* ctx = nullptr) noexcept {
    on_timeout_ = fn;
    timeout_ctx_ = ctx;
  }

  /**
   * @brief Set the recovery callback.
   *
   * @param fn   Callback function pointer (may be nullptr to disable).
   * @param ctx  User context forwarded to fn.
   */
  void SetOnRecovered(RecoverCallback fn, void* ctx = nullptr) noexcept {
    on_recovered_ = fn;
    recover_ctx_ = ctx;
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /**
   * @brief Check if a specific slot is currently timed out.
   *
   * Thread-safe (acquires internal mutex).
   *
   * @param id  The slot ID returned by Register().
   *
   * @return true if the slot is active and timed out, false otherwise.
   */
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

  /**
   * @brief Count the number of currently active (registered) threads.
   *
   * Thread-safe (acquires internal mutex).
   */
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

  /**
   * @brief Count the number of currently timed-out threads.
   *
   * Thread-safe (acquires internal mutex).
   */
  uint32_t TimedOutCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < MaxThreads; ++i) {
      if (slots_[i].active.load(std::memory_order_acquire) &&
          slots_[i].timed_out) {
        ++count;
      }
    }
    return count;
  }

  // --------------------------------------------------------------------------
  // TimerScheduler Integration
  // --------------------------------------------------------------------------

  /**
   * @brief Static callback for TimerScheduler integration.
   *
   * Usage:
   *   osp::TimerScheduler<8> timer;
   *   osp::ThreadWatchdog<32> wd;
   *   timer.Add(1000, &osp::ThreadWatchdog<32>::CheckTick, &wd);
   *
   * @param ctx  Pointer to ThreadWatchdog instance.
   */
  static void CheckTick(void* ctx) noexcept {
    OSP_ASSERT(ctx != nullptr);
    static_cast<ThreadWatchdog*>(ctx)->Check();
  }

 private:
  // --------------------------------------------------------------------------
  // Internal Types
  // --------------------------------------------------------------------------

  /**
   * @brief Represents a single watchdog slot.
   */
  struct Slot {
    std::atomic<bool> active{false};           ///< Slot is in use.
    std::atomic<uint64_t> last_feed_us{0};     ///< Last feed timestamp (us).
    uint64_t timeout_us{0};                    ///< Timeout period (us).
    bool timed_out{false};                     ///< Currently timed out flag.
    FixedString<32> name;                      ///< Thread name.
  };

  // --------------------------------------------------------------------------
  // Data Members
  // --------------------------------------------------------------------------

  Slot slots_[MaxThreads]{};                   ///< Embedded slot array.
  mutable std::mutex mutex_;                   ///< Guards slot iteration.
  TimeoutCallback on_timeout_ = nullptr;       ///< Timeout callback.
  void* timeout_ctx_ = nullptr;                ///< Timeout callback context.
  RecoverCallback on_recovered_ = nullptr;     ///< Recovery callback.
  void* recover_ctx_ = nullptr;                ///< Recovery callback context.
};

// ============================================================================
// WatchdogGuard - RAII watchdog registration
// ============================================================================

/**
 * @brief RAII wrapper for automatic watchdog registration/unregistration.
 *
 * Registers with the watchdog on construction and unregisters on destruction.
 * Provides a Feed() method for periodic liveness signaling.
 *
 * Non-copyable, non-movable.
 *
 * Typical usage:
 *
 *   osp::ThreadWatchdog<32> wd;
 *   {
 *     osp::WatchdogGuard<32> guard(&wd, "WorkerThread", 5000);
 *     while (running) {
 *       // ... do work ...
 *       guard.Feed();
 *     }
 *   }  // Automatic unregister
 *
 * @tparam MaxThreads  Must match the ThreadWatchdog template parameter.
 */
template <uint32_t MaxThreads = 32>
class WatchdogGuard final {
 public:
  /**
   * @brief Construct and register with the watchdog.
   *
   * @param wd          Non-owning pointer to ThreadWatchdog instance.
   * @param name        Thread name (max 31 chars).
   * @param timeout_ms  Timeout period in milliseconds.
   */
  WatchdogGuard(ThreadWatchdog<MaxThreads>* wd, const char* name,
                uint32_t timeout_ms) noexcept
      : wd_(wd), id_(WatchdogSlotId(0)), valid_(false) {
    OSP_ASSERT(wd_ != nullptr);
    auto result = wd_->Register(name, timeout_ms);
    if (result.has_value()) {
      id_ = result.value();
      valid_ = true;
    }
  }

  /**
   * @brief Destructor. Unregisters from the watchdog if valid.
   */
  ~WatchdogGuard() {
    if (valid_) {
      (void)wd_->Unregister(id_);
    }
  }

  WatchdogGuard(const WatchdogGuard&) = delete;
  WatchdogGuard& operator=(const WatchdogGuard&) = delete;
  WatchdogGuard(WatchdogGuard&&) = delete;
  WatchdogGuard& operator=(WatchdogGuard&&) = delete;

  /**
   * @brief Signal that the thread is alive.
   *
   * Forwards to ThreadWatchdog::Feed() if the guard is valid.
   */
  void Feed() noexcept {
    if (valid_) {
      wd_->Feed(id_);
    }
  }

  /**
   * @brief Check if the guard successfully registered.
   *
   * @return true if registration succeeded, false otherwise.
   */
  bool IsValid() const noexcept { return valid_; }

 private:
  ThreadWatchdog<MaxThreads>* wd_;  ///< Non-owning watchdog pointer.
  WatchdogSlotId id_;               ///< Assigned slot ID.
  bool valid_;                      ///< Registration succeeded flag.
};

}  // namespace osp

#endif  // OSP_WATCHDOG_HPP_
