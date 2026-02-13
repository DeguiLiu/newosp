/**
 * @file timer.hpp
 * @brief Header-only periodic timer task scheduler for OSP-CPP.
 *
 * Provides a thread-based scheduler that fires registered callbacks at
 * configurable periodic intervals.  Uses std::chrono::steady_clock for
 * monotonic timing and std::thread for the scheduler loop.
 *
 * All public methods are thread-safe (guarded by internal mutex).
 * Compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_TIMER_HPP_
#define OSP_TIMER_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace osp {

// ============================================================================
// TimerTaskFn - Callback type for timer tasks
// ============================================================================

/**
 * @brief Plain function pointer invoked by the scheduler on each period tick.
 *
 * @param ctx  User-supplied opaque context pointer (may be nullptr).
 */
using TimerTaskFn = void (*)(void* ctx);

// ============================================================================
// TimerScheduler - Periodic timer task scheduler
// ============================================================================

/**
 * @brief Fixed-capacity periodic timer scheduler driven by a background thread.
 *
 * Tasks are stored in a pre-allocated slot array whose size is fixed at
 * construction time.  The scheduler thread sleeps between ticks to keep
 * CPU usage low while still achieving reasonable timing accuracy.
 *
 * Typical usage:
 *
 *   osp::TimerScheduler sched(8);
 *   sched.Add(1000, my_callback);
 *   sched.Start();
 *   // ... do other work ...
 *   sched.Stop();
 *
 * Non-copyable, non-movable.
 */
class TimerScheduler final {
 public:
  // --------------------------------------------------------------------------
  // Construction / Destruction
  // --------------------------------------------------------------------------

  /**
   * @brief Construct a scheduler with a fixed number of task slots.
   *
   * @param max_tasks  Maximum concurrent timer tasks (default 16).
   */
  explicit TimerScheduler(uint32_t max_tasks = 16)
      : slots_(new TaskSlot[max_tasks]),
        max_tasks_(max_tasks) {}

  /**
   * @brief Destructor.  Stops the scheduler thread if running, then frees
   *        the slot array.
   */
  ~TimerScheduler() {
    Stop();
    delete[] slots_;
  }

  TimerScheduler(const TimerScheduler&) = delete;
  TimerScheduler& operator=(const TimerScheduler&) = delete;
  TimerScheduler(TimerScheduler&&) = delete;
  TimerScheduler& operator=(TimerScheduler&&) = delete;

  // --------------------------------------------------------------------------
  // Task Management
  // --------------------------------------------------------------------------

  /**
   * @brief Register a periodic timer task.
   *
   * @param period_ms  Firing period in milliseconds (must be > 0).
   * @param fn         Callback invoked every period_ms milliseconds.
   * @param ctx        Opaque context pointer forwarded to fn (default nullptr).
   *
   * @return TimerTaskId on success, or TimerError on failure:
   *         - kInvalidPeriod if period_ms == 0.
   *         - kSlotsFull     if all task slots are occupied.
   */
  expected<TimerTaskId, TimerError> Add(uint32_t period_ms,
                                        TimerTaskFn fn,
                                        void* ctx = nullptr) {
    if (period_ms == 0U) {
      return expected<TimerTaskId, TimerError>::error(
          TimerError::kInvalidPeriod);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < max_tasks_; ++i) {
      if (!slots_[i].active) {
        const uint64_t period_ns =
            static_cast<uint64_t>(period_ms) * 1000000ULL;
        slots_[i].fn = fn;
        slots_[i].ctx = ctx;
        slots_[i].period_ns = period_ns;
        slots_[i].next_fire_ns = NowNs() + period_ns;
        slots_[i].id = next_id_++;
        slots_[i].active = true;
        return expected<TimerTaskId, TimerError>::success(
            TimerTaskId(slots_[i].id));
      }
    }

    return expected<TimerTaskId, TimerError>::error(TimerError::kSlotsFull);
  }

  /**
   * @brief Remove a previously registered timer task by its ID.
   *
   * The slot is marked inactive and will be skipped on the next scheduler
   * round.  The slot becomes available for re-use by subsequent Add() calls.
   *
   * @param task_id  The ID returned by a prior successful Add() call.
   *
   * @return Success, or TimerError::kNotRunning if the task_id is not found.
   */
  expected<void, TimerError> Remove(TimerTaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < max_tasks_; ++i) {
      if (slots_[i].active && slots_[i].id == task_id.value()) {
        slots_[i].active = false;
        return expected<void, TimerError>::success();
      }
    }

    return expected<void, TimerError>::error(TimerError::kNotRunning);
  }

  // --------------------------------------------------------------------------
  // Scheduler Lifecycle
  // --------------------------------------------------------------------------

  /**
   * @brief Start the background scheduler thread.
   *
   * @return Success, or TimerError::kAlreadyRunning if already started.
   */
  expected<void, TimerError> Start() {
    if (running_.load(std::memory_order_acquire)) {
      return expected<void, TimerError>::error(TimerError::kAlreadyRunning);
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&TimerScheduler::ScheduleLoop, this);

    return expected<void, TimerError>::success();
  }

  /**
   * @brief Stop the scheduler thread (blocks until the thread exits).
   *
   * Safe to call even if the scheduler is not running.
   */
  void Stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  /**
   * @brief Query whether the scheduler thread is currently running.
   */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /**
   * @brief Count the number of currently active timer tasks.
   *
   * Thread-safe (acquires internal mutex).
   */
  uint32_t TaskCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < max_tasks_; ++i) {
      if (slots_[i].active) {
        ++count;
      }
    }
    return count;
  }

 private:
  // --------------------------------------------------------------------------
  // Internal Types
  // --------------------------------------------------------------------------

  /**
   * @brief Represents a single timer task slot.
   */
  struct TaskSlot {
    TimerTaskFn fn = nullptr;   ///< Callback function pointer.
    void* ctx = nullptr;        ///< User context forwarded to fn.
    uint64_t period_ns = 0;     ///< Firing period in nanoseconds.
    uint64_t next_fire_ns = 0;  ///< Next absolute fire time (monotonic ns).
    uint32_t id = 0;            ///< Unique task identifier.
    bool active = false;        ///< Whether this slot is in use.
  };

  // --------------------------------------------------------------------------
  // Data Members
  // --------------------------------------------------------------------------

  TaskSlot* slots_;                       ///< Pre-allocated task slot array.
  uint32_t max_tasks_;                    ///< Capacity of slots_ array.
  uint32_t next_id_ = 1;                 ///< Monotonically increasing ID.
  std::atomic<bool> running_{false};      ///< Scheduler thread active flag.
  std::thread worker_;                    ///< Background scheduler thread.
  mutable std::mutex mutex_;              ///< Guards slots_ and next_id_.

  // --------------------------------------------------------------------------
  // Scheduler Loop
  // --------------------------------------------------------------------------

  /**
   * @brief Main loop executed by the background scheduler thread.
   *
   * On each iteration the loop scans all active slots, fires any tasks
   * whose deadline has elapsed, advances their next-fire times (handling
   * multiple missed periods), and then sleeps for a fraction of the
   * shortest remaining deadline.
   */
  void ScheduleLoop() {
    while (running_.load(std::memory_order_acquire)) {
      uint64_t now = NowNs();
      uint64_t min_remaining = UINT64_MAX;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0U; i < max_tasks_; ++i) {
          if (!slots_[i].active) {
            continue;
          }

          if (now >= slots_[i].next_fire_ns) {
            slots_[i].fn(slots_[i].ctx);
            slots_[i].next_fire_ns += slots_[i].period_ns;

            // Handle multiple missed periods: advance until ahead of now.
            while (slots_[i].next_fire_ns <= now) {
              slots_[i].next_fire_ns += slots_[i].period_ns;
            }
          }

          const uint64_t after = NowNs();
          const uint64_t remaining =
              (slots_[i].next_fire_ns > after)
                  ? (slots_[i].next_fire_ns - after)
                  : 0U;
          if (remaining < min_remaining) {
            min_remaining = remaining;
          }
        }
      }

      // Sleep for half the minimum remaining time, clamped to [1ms, 10ms].
      uint64_t sleep_ns = (min_remaining == UINT64_MAX)
                              ? 10000000ULL        // 10 ms when no tasks
                              : (min_remaining / 2);
      if (sleep_ns < 1000000ULL) {
        sleep_ns = 1000000ULL;  // min 1 ms
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  }

  // --------------------------------------------------------------------------
  // Monotonic Clock Helper
  // --------------------------------------------------------------------------

  /**
   * @brief Return the current monotonic time in nanoseconds.
   */
  static uint64_t NowNs() noexcept {
    const auto dur = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count());
  }
};

}  // namespace osp

#endif  // OSP_TIMER_HPP_
