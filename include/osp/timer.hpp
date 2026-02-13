/**
 * @file timer.hpp
 * @brief Header-only periodic and one-shot timer task scheduler for OSP-CPP.
 *
 * Provides a thread-based scheduler that fires registered callbacks at
 * configurable periodic intervals or after a single delay (one-shot).
 * Uses std::chrono::steady_clock for monotonic timing and std::thread
 * for the scheduler loop.
 *
 * Callbacks are executed outside the internal mutex to prevent deadlocks
 * when callbacks acquire external locks (collect-release-execute pattern).
 *
 * Callback constraints:
 * - Callbacks run sequentially on the scheduler thread.
 * - A slow callback delays all other tasks.  Keep callbacks < 1ms.
 * - Blocking I/O or long computation should be offloaded to a worker thread.
 * - It is safe to call Add()/Remove() from within a callback.
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
 *
 * Callbacks are executed outside the scheduler's internal mutex.
 * It is safe to acquire external locks within callbacks.
 * Callbacks should return promptly (< 1ms recommended) to avoid
 * delaying other scheduled tasks.
 */
using TimerTaskFn = void (*)(void* ctx);

// ============================================================================
// TimerScheduler - Periodic timer task scheduler
// ============================================================================

/**
 * @brief Fixed-capacity periodic and one-shot timer scheduler driven by a
 *        background thread.
 *
 * Tasks are stored in an embedded array whose size is fixed at compile time
 * via the MaxTasks template parameter.  Zero heap allocation.
 *
 * The scheduler thread sleeps between ticks to keep CPU usage low while
 * still achieving reasonable timing accuracy.
 *
 * Typical usage:
 *
 *   osp::TimerScheduler<8> sched;
 *   sched.Add(1000, my_periodic_callback);
 *   sched.AddOneShot(500, my_oneshot_callback);
 *   sched.Start();
 *   // ... do other work ...
 *   sched.Stop();
 *
 * Non-copyable, non-movable.
 *
 * @tparam MaxTasks  Maximum concurrent timer tasks (compile-time capacity).
 */
template <uint32_t MaxTasks = 16>
class TimerScheduler final {
 public:
  // --------------------------------------------------------------------------
  // Construction / Destruction
  // --------------------------------------------------------------------------

  /**
   * @brief Construct a scheduler.  Zero heap allocation.
   */
  TimerScheduler() = default;

  /**
   * @brief Destructor.  Stops the scheduler thread if running.
   */
  ~TimerScheduler() { Stop(); }

  TimerScheduler(const TimerScheduler&) = delete;
  TimerScheduler& operator=(const TimerScheduler&) = delete;
  TimerScheduler(TimerScheduler&&) = delete;
  TimerScheduler& operator=(TimerScheduler&&) = delete;

  // --------------------------------------------------------------------------
  // Capacity Query
  // --------------------------------------------------------------------------

  /**
   * @brief Return the compile-time maximum number of task slots.
   */
  static constexpr uint32_t Capacity() noexcept { return MaxTasks; }

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
    if (fn == nullptr) {
      return expected<TimerTaskId, TimerError>::error(
          TimerError::kInvalidPeriod);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < MaxTasks; ++i) {
      if (!slots_[i].active) {
        const uint64_t period_ns =
            static_cast<uint64_t>(period_ms) * kNsPerMs;
        slots_[i].fn = fn;
        slots_[i].ctx = ctx;
        slots_[i].period_ns = period_ns;
        slots_[i].next_fire_ns = SteadyNowNs() + period_ns;
        slots_[i].id = next_id_++;
        slots_[i].active = true;
        slots_[i].one_shot = false;
        return expected<TimerTaskId, TimerError>::success(
            TimerTaskId(slots_[i].id));
      }
    }

    return expected<TimerTaskId, TimerError>::error(TimerError::kSlotsFull);
  }

  /**
   * @brief Register a one-shot timer task that fires once after delay_ms.
   *
   * After the callback fires, the slot is automatically deactivated.
   * Calling Remove() on an already-fired one-shot returns kNotRunning
   * (same as removing a non-existent task).
   *
   * @param delay_ms  Delay before firing in milliseconds (must be > 0).
   * @param fn        Callback invoked once after delay_ms milliseconds.
   * @param ctx       Opaque context pointer forwarded to fn (default nullptr).
   *
   * @return TimerTaskId on success, or TimerError on failure:
   *         - kInvalidPeriod if delay_ms == 0.
   *         - kSlotsFull     if all task slots are occupied.
   */
  expected<TimerTaskId, TimerError> AddOneShot(uint32_t delay_ms,
                                                TimerTaskFn fn,
                                                void* ctx = nullptr) {
    if (delay_ms == 0U) {
      return expected<TimerTaskId, TimerError>::error(
          TimerError::kInvalidPeriod);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0U; i < MaxTasks; ++i) {
      if (!slots_[i].active) {
        const uint64_t delay_ns =
            static_cast<uint64_t>(delay_ms) * kNsPerMs;
        slots_[i].fn = fn;
        slots_[i].ctx = ctx;
        slots_[i].period_ns = delay_ns;
        slots_[i].next_fire_ns = SteadyNowNs() + delay_ns;
        slots_[i].id = next_id_++;
        slots_[i].active = true;
        slots_[i].one_shot = true;
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

    for (uint32_t i = 0U; i < MaxTasks; ++i) {
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
    for (uint32_t i = 0U; i < MaxTasks; ++i) {
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
    bool one_shot = false;      ///< If true, auto-deactivate after first fire.
  };

  /**
   * @brief Snapshot of a pending callback collected during the scan phase.
   */
  struct PendingTask {
    TimerTaskFn fn = nullptr;
    void* ctx = nullptr;
  };

  // --------------------------------------------------------------------------
  // Constants
  // --------------------------------------------------------------------------

  static constexpr uint64_t kNsPerMs = 1000000ULL;           ///< ns per ms
  static constexpr uint64_t kDefaultSleepNs = 10000000ULL;  ///< 10ms default sleep
  static constexpr uint64_t kMinSleepNs = 1000000ULL;       ///< 1ms minimum sleep

  // --------------------------------------------------------------------------
  // Data Members
  // --------------------------------------------------------------------------

  TaskSlot slots_[MaxTasks]{};              ///< Embedded task slot array.
  uint32_t next_id_ = 1;                   ///< Monotonically increasing ID.
  std::atomic<bool> running_{false};        ///< Scheduler thread active flag.
  std::thread worker_;                      ///< Background scheduler thread.
  mutable std::mutex mutex_;                ///< Guards slots_ and next_id_.

  // --------------------------------------------------------------------------
  // Scheduler Loop (collect-release-execute pattern)
  // --------------------------------------------------------------------------

  /**
   * @brief Main loop executed by the background scheduler thread.
   *
   * Uses a collect-release-execute pattern:
   * 1. Lock mutex, scan slots, collect expired tasks into stack array.
   * 2. Release mutex.
   * 3. Execute collected callbacks outside the lock.
   *
   * This prevents deadlocks when callbacks acquire external mutexes
   * and allows Add()/Remove() to proceed during callback execution.
   */
  void ScheduleLoop() {
    while (running_.load(std::memory_order_acquire)) {
      PendingTask pending[MaxTasks];
      uint32_t pending_count = 0U;
      uint64_t min_remaining = UINT64_MAX;

      // Phase 1: Collect expired tasks under lock
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t now = SteadyNowNs();

        for (uint32_t i = 0U; i < MaxTasks; ++i) {
          if (!slots_[i].active) {
            continue;
          }

          if (now >= slots_[i].next_fire_ns) {
            // Collect callback for deferred execution
            pending[pending_count].fn = slots_[i].fn;
            pending[pending_count].ctx = slots_[i].ctx;
            ++pending_count;

            if (slots_[i].one_shot) {
              // One-shot: deactivate immediately, no reschedule
              slots_[i].active = false;
            } else {
              // Periodic: advance next fire time
              slots_[i].next_fire_ns += slots_[i].period_ns;

              // Handle multiple missed periods: advance until ahead of now.
              while (slots_[i].next_fire_ns <= now) {
                slots_[i].next_fire_ns += slots_[i].period_ns;
              }
            }
          }

          // Compute remaining time for sleep calculation
          const uint64_t after = SteadyNowNs();
          const uint64_t remaining =
              (slots_[i].next_fire_ns > after)
                  ? (slots_[i].next_fire_ns - after)
                  : 0U;
          if (remaining < min_remaining) {
            min_remaining = remaining;
          }
        }
      }
      // mutex_ released here

      // Phase 2: Execute callbacks outside the lock
      for (uint32_t i = 0U; i < pending_count; ++i) {
        pending[i].fn(pending[i].ctx);
      }

      // Sleep for half the minimum remaining time, clamped to [1ms, 10ms].
      uint64_t sleep_ns = (min_remaining == UINT64_MAX)
                              ? kDefaultSleepNs
                              : (min_remaining / 2);
      if (sleep_ns < kMinSleepNs) {
        sleep_ns = kMinSleepNs;
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  }
};

}  // namespace osp

#endif  // OSP_TIMER_HPP_
