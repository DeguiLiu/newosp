// Single-threaded event loop unifying fd readiness, timers and a cross-thread
// wakeup pipe in one Run() loop (libev ev_io/ev_timer/ev_async equivalent,
// built on top of the IoPoller backend). Callbacks are CRTP hooks, not
// function pointers: the Derived type supplies OnFd/OnTimer.
// SPDX-License-Identifier: MIT

#ifndef OSP_EVENT_LOOP_HPP_
#define OSP_EVENT_LOOP_HPP_

#include "osp/io_poller.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

#include <array>
#include <atomic>
#include <mutex>

#if OSP_HAS_NETWORK

#include <fcntl.h>
#include <unistd.h>

namespace osp {

enum class LoopError : uint8_t {
  kNoFreeFdSlot,
  kNoFreeTimerSlot,
  kInvalidFd,
  kInvalidTimerId,
  kInvalidPeriod,
  kBackendFailed
};

/**
 * @brief Unified single-thread event loop, CRTP skeleton + hooks.
 *
 * The Derived type supplies two hooks, both invoked on the Run thread:
 *   void OnFd(int32_t fd, uint8_t events) noexcept;
 *   void OnTimer(uint32_t timer_id) noexcept;
 * Dispatch is compile-time bound through static_cast<Derived*>(this), so there
 * is no function-pointer erasure and no vtable. Distinguish fds/timers inside
 * the hooks via the fd / timer_id argument.
 *
 * Combines the IoPoller backend (epoll/kqueue/poll) with a fixed-capacity
 * linear-scan timer table and a self-pipe for cross-thread wakeup. Run() waits
 * for fd events and timer expiry with the poll timeout set to the nearest
 * timer expiry, so the loop sleeps until the next event is due instead of
 * polling at a fixed interval.
 *
 * Thread-safety boundaries:
 * - fd watchers are thread-safe: AddFd/ModifyFd/RemoveFd take an internal
 *   mutex so producers on other threads may register sockets.
 * - timers are thread-safe: Schedule/Cancel take the internal mutex; the
 *   OnTimer hook fires on the Run thread (collect-release-execute).
 * - Wake() is the only cross-thread entry point (libev ev_async equivalent).
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */
template <typename Derived, uint32_t MaxFds = OSP_IO_POLLER_MAX_EVENTS, uint32_t MaxTimers = 16U>
class EventLoop {
  static_assert(std::atomic<bool>::is_always_lock_free, "EventLoop requires a lock-free std::atomic<bool>");

 public:
  EventLoop() noexcept;
  ~EventLoop();

  // Non-copyable, non-movable (owns the backend poller and the wake pipe).
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  /** @brief True when the backend poller and the wake pipe are usable. */
  bool IsValid() const noexcept { return poller_.IsValid() && 0 <= wake_fds_[0]; }

  // -----------------------------------------------------------------------
  // fd watchers (Run-thread only, libev ev_io equivalent)
  // -----------------------------------------------------------------------

  /** @brief Monitor fd for readiness; readiness calls OnFd(fd, events). */
  [[nodiscard]] expected<void, LoopError> AddFd(int32_t fd, uint8_t events) noexcept;

  /** @brief Change the monitored event mask of an already-added fd. */
  [[nodiscard]] expected<void, LoopError> ModifyFd(int32_t fd, uint8_t events) noexcept;

  /** @brief Stop monitoring fd and free its slot. */
  [[nodiscard]] expected<void, LoopError> RemoveFd(int32_t fd) noexcept;

  // -----------------------------------------------------------------------
  // timers (thread-safe, libev ev_timer equivalent)
  // -----------------------------------------------------------------------

  /** @brief Register a periodic timer; expiry calls OnTimer(timer_id). */
  [[nodiscard]] expected<uint32_t, LoopError> Schedule(uint32_t period_ms) noexcept;

  /** @brief Register a one-shot timer fired once after delay_ms. */
  [[nodiscard]] expected<uint32_t, LoopError> ScheduleOnce(uint32_t delay_ms) noexcept;

  /** @brief Deactivate a timer by id (the id is freed and may be reused). */
  [[nodiscard]] expected<void, LoopError> Cancel(uint32_t timer_id) noexcept;

  // -----------------------------------------------------------------------
  // wakeup (thread-safe, libev ev_async equivalent)
  // -----------------------------------------------------------------------

  /** @brief Interrupt a blocking Run() from any thread. */
  void Wake() noexcept;

  // -----------------------------------------------------------------------
  // loop driver
  // -----------------------------------------------------------------------

  /** @brief Run the loop until Stop() is called. */
  void Run() noexcept;

  /** @brief Request loop exit and wake it if it is blocked in Wait. */
  void Stop() noexcept;

  /**
   * @brief Arm the loop for a new Run() (only when not running).
   *
   * Run() no longer clears stop_ itself: a concurrent Stop() between thread
   * spawn and loop entry would otherwise be wiped by the entry store and
   * the loop would run forever. Start() clears the flag before spawning
   * the Run thread; a Stop() racing after that is honored by Run().
   */
  void ClearStop() noexcept;

  /**
   * @brief Wake hook (libev ev_async equivalent). The default is a no-op;
   *        Derived overrides it to run work after a cross-thread Wake().
   */
  void OnWake() noexcept {}

  /**
   * @brief fd readiness hook (libev ev_io equivalent). The default is a
   *        no-op; Derived overrides it when it registers fd watchers.
   */
  void OnFd(int32_t /*fd*/, uint8_t /*events*/) noexcept {}

  /**
   * @brief timer expiry hook (libev ev_timer equivalent). The default is a
   *        no-op; Derived overrides it when it schedules timers.
   */
  void OnTimer(uint32_t /*timer_id*/) noexcept {}

 private:
  Derived& self() noexcept { return *static_cast<Derived*>(this); }

  struct FdSlot {
    int32_t fd = -1;
    bool active = false;
  };

  struct TimerSlot {
    uint32_t id = 0U;
    uint64_t period_ns = 0U;
    uint64_t next_fire_ns = 0U;
    bool active = false;
    bool one_shot = false;
  };

  int32_t NextTimeoutMs() noexcept;
  bool DrainWake() noexcept;
  void FireExpiredTimers() noexcept;

  IoPoller poller_;
  int32_t wake_fds_[2] = {-1, -1};
  std::array<FdSlot, MaxFds> fds_{};
  std::array<TimerSlot, MaxTimers> timers_{};
  uint32_t next_timer_id_ = 1U;
  std::mutex fd_mutex_;
  std::mutex timer_mutex_;
  std::atomic<bool> stop_{false};
};

// ============================================================================
// Inline Implementation
// ============================================================================

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
EventLoop<Derived, MaxFds, MaxTimers>::EventLoop() noexcept {
  if (0 != ::pipe(wake_fds_)) {
    return;
  }
  // Non-blocking read: DrainWake() loops until EAGAIN instead of blocking.
  // Non-blocking write: a full pipe means we are already pending-woken.
  if (0 != ::fcntl(wake_fds_[0], F_SETFL, O_NONBLOCK) || 0 != ::fcntl(wake_fds_[1], F_SETFL, O_NONBLOCK)) {
    ::close(wake_fds_[0]);
    ::close(wake_fds_[1]);
    wake_fds_[0] = -1;
    wake_fds_[1] = -1;
    return;
  }
  // Registering the wake fd on the lwIP poll fallback may fail; that is
  // non-fatal: Run() still works via timer timeouts, only Wake() no-ops.
  (void)poller_.Add(wake_fds_[0], static_cast<uint8_t>(IoEvent::kReadable));
}

// The loop does not own the Run thread: the caller MUST Stop() and join the
// Run thread before destruction. Destroying while Run() still polls the wake
// fd would close a descriptor the loop is actively reading, so the kernel may
// reuse its number (use-after-close) and the Run thread would leak.
template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
EventLoop<Derived, MaxFds, MaxTimers>::~EventLoop() {
  if (0 <= wake_fds_[0]) {
    ::close(wake_fds_[0]);
  }
  if (0 <= wake_fds_[1]) {
    ::close(wake_fds_[1]);
  }
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<void, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::AddFd(int32_t fd, uint8_t events) noexcept {
  std::lock_guard<std::mutex> lock(fd_mutex_);
  uint32_t slot = MaxFds;
  for (uint32_t i = 0U; i < MaxFds; ++i) {
    if (!fds_[i].active) {
      slot = i;
      break;
    }
  }
  if (MaxFds == slot) {
    return expected<void, LoopError>::error(LoopError::kNoFreeFdSlot);
  }
  auto r = poller_.Add(fd, events);
  if (!r.has_value()) {
    return expected<void, LoopError>::error(LoopError::kBackendFailed);
  }
  fds_[slot].fd = fd;
  fds_[slot].active = true;
  return expected<void, LoopError>::success();
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<void, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::ModifyFd(int32_t fd, uint8_t events) noexcept {
  std::lock_guard<std::mutex> lock(fd_mutex_);
  auto r = poller_.Modify(fd, events);
  if (!r.has_value()) {
    return expected<void, LoopError>::error(LoopError::kBackendFailed);
  }
  return expected<void, LoopError>::success();
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<void, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::RemoveFd(int32_t fd) noexcept {
  std::lock_guard<std::mutex> lock(fd_mutex_);
  auto r = poller_.Remove(fd);
  if (!r.has_value()) {
    return expected<void, LoopError>::error(LoopError::kBackendFailed);
  }
  for (uint32_t i = 0U; i < MaxFds; ++i) {
    if (fds_[i].active && fds_[i].fd == fd) {
      fds_[i].fd = -1;
      fds_[i].active = false;
      break;
    }
  }
  return expected<void, LoopError>::success();
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<uint32_t, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::Schedule(uint32_t period_ms) noexcept {
  if (0U == period_ms) {
    return expected<uint32_t, LoopError>::error(LoopError::kInvalidPeriod);
  }
  std::lock_guard<std::mutex> lock(timer_mutex_);
  for (uint32_t i = 0U; i < MaxTimers; ++i) {
    if (!timers_[i].active) {
      const uint64_t period_ns = static_cast<uint64_t>(period_ms) * 1000000ULL;
      timers_[i].id = next_timer_id_++;
      timers_[i].period_ns = period_ns;
      timers_[i].next_fire_ns = SteadyNowNs() + period_ns;
      timers_[i].active = true;
      timers_[i].one_shot = false;
      return expected<uint32_t, LoopError>::success(timers_[i].id);
    }
  }
  return expected<uint32_t, LoopError>::error(LoopError::kNoFreeTimerSlot);
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<uint32_t, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::ScheduleOnce(uint32_t delay_ms) noexcept {
  if (0U == delay_ms) {
    return expected<uint32_t, LoopError>::error(LoopError::kInvalidPeriod);
  }
  std::lock_guard<std::mutex> lock(timer_mutex_);
  for (uint32_t i = 0U; i < MaxTimers; ++i) {
    if (!timers_[i].active) {
      const uint64_t delay_ns = static_cast<uint64_t>(delay_ms) * 1000000ULL;
      timers_[i].id = next_timer_id_++;
      timers_[i].period_ns = delay_ns;
      timers_[i].next_fire_ns = SteadyNowNs() + delay_ns;
      timers_[i].active = true;
      timers_[i].one_shot = true;
      return expected<uint32_t, LoopError>::success(timers_[i].id);
    }
  }
  return expected<uint32_t, LoopError>::error(LoopError::kNoFreeTimerSlot);
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
expected<void, LoopError> EventLoop<Derived, MaxFds, MaxTimers>::Cancel(uint32_t timer_id) noexcept {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  for (uint32_t i = 0U; i < MaxTimers; ++i) {
    if (timers_[i].active && timers_[i].id == timer_id) {
      timers_[i].active = false;
      return expected<void, LoopError>::success();
    }
  }
  return expected<void, LoopError>::error(LoopError::kInvalidTimerId);
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
void EventLoop<Derived, MaxFds, MaxTimers>::Wake() noexcept {
  const uint8_t c = static_cast<uint8_t>('x');
  if (0 <= wake_fds_[1]) {
    // A short write is atomic below PIPE_BUF; EAGAIN means already woken.
    (void)::write(wake_fds_[1], &c, 1);
  }
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
void EventLoop<Derived, MaxFds, MaxTimers>::Run() noexcept {
  while (!stop_.load(std::memory_order_acquire)) {
    const int32_t timeout_ms = NextTimeoutMs();
    const auto wr = poller_.Wait(timeout_ms);
    if (DrainWake()) {
      // Drain the wake hook before re-checking stop, so a final Stop()
      // event posted concurrently is still dispatched before exit.
      self().OnWake();
    }
    if (stop_.load(std::memory_order_acquire)) {
      break;
    }
    if (wr.has_value()) {
      const PollResult* results = poller_.Results();
      for (uint32_t i = 0U; i < wr.value(); ++i) {
        if (results[i].fd != wake_fds_[0]) {
          self().OnFd(results[i].fd, results[i].events);
        }
      }
    }
    FireExpiredTimers();
  }
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
void EventLoop<Derived, MaxFds, MaxTimers>::Stop() noexcept {
  stop_.store(true, std::memory_order_release);
  Wake();
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
void EventLoop<Derived, MaxFds, MaxTimers>::ClearStop() noexcept {
  stop_.store(false, std::memory_order_release);
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
int32_t EventLoop<Derived, MaxFds, MaxTimers>::NextTimeoutMs() noexcept {
  std::lock_guard<std::mutex> lock(timer_mutex_);
  const uint64_t now = SteadyNowNs();
  uint64_t min_next = UINT64_MAX;
  for (uint32_t i = 0U; i < MaxTimers; ++i) {
    if (timers_[i].active && timers_[i].next_fire_ns < min_next) {
      min_next = timers_[i].next_fire_ns;
    }
  }
  if (UINT64_MAX == min_next) {
    return -1;  // No timers: wait indefinitely; Wake() interrupts.
  }
  if (min_next <= now) {
    return 0;  // Already due: do not block.
  }
  const uint64_t delta_ns = min_next - now;
  const uint64_t delta_ms = (delta_ns + 999999ULL) / 1000000ULL;
  return 0x7FFFFFFFULL < delta_ms ? 0x7FFFFFFF : static_cast<int32_t>(delta_ms);
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
bool EventLoop<Derived, MaxFds, MaxTimers>::DrainWake() noexcept {
  uint8_t buf[64];
  bool drained = false;
  while (0 < ::read(wake_fds_[0], buf, sizeof(buf))) {
    drained = true;
  }
  return drained;
}

template <typename Derived, uint32_t MaxFds, uint32_t MaxTimers>
void EventLoop<Derived, MaxFds, MaxTimers>::FireExpiredTimers() noexcept {
  uint32_t due[MaxTimers];
  uint32_t count = 0U;

  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    const uint64_t now = SteadyNowNs();
    for (uint32_t i = 0U; i < MaxTimers; ++i) {
      if (!timers_[i].active || now < timers_[i].next_fire_ns) {
        continue;
      }
      due[count] = timers_[i].id;
      ++count;

      if (timers_[i].one_shot) {
        timers_[i].active = false;
      } else {
        // Catch-up: advance past every missed period so a late loop
        // fires exactly once, never a burst.
        timers_[i].next_fire_ns += timers_[i].period_ns;
        while (timers_[i].next_fire_ns <= now) {
          timers_[i].next_fire_ns += timers_[i].period_ns;
        }
      }
    }
  }

  // Hooks run outside the mutex (collect-release-execute).
  for (uint32_t i = 0U; i < count; ++i) {
    self().OnTimer(due[i]);
  }
}

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_EVENT_LOOP_HPP_
