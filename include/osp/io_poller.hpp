/**
 * @file io_poller.hpp
 * @brief Unified I/O event poller abstraction over epoll (Linux) and kqueue (macOS).
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_IO_POLLER_HPP_
#define OSP_IO_POLLER_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>
#include <array>
#include <unistd.h>

#if defined(OSP_PLATFORM_LINUX)
#include <sys/epoll.h>
#elif defined(OSP_PLATFORM_MACOS)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace osp {

// ============================================================================
// Error Enum
// ============================================================================

enum class PollerError : uint8_t {
  kCreateFailed,
  kAddFailed,
  kModifyFailed,
  kRemoveFailed,
  kWaitFailed
};

// ============================================================================
// Event Types
// ============================================================================

enum class IoEvent : uint8_t {
  kReadable = 0x01,
  kWritable = 0x02,
  kError    = 0x04,
  kHangup   = 0x08
};

inline constexpr uint8_t operator|(IoEvent a, IoEvent b) {
  return static_cast<uint8_t>(a) | static_cast<uint8_t>(b);
}

struct PollResult {
  int32_t fd;
  uint8_t events;  // bitmask of IoEvent
};

// ============================================================================
// IoPoller
// ============================================================================

#ifndef OSP_IO_POLLER_MAX_EVENTS
#define OSP_IO_POLLER_MAX_EVENTS 64U
#endif

class IoPoller {
 public:
  IoPoller() noexcept;
  ~IoPoller();

  // Non-copyable
  IoPoller(const IoPoller&) = delete;
  IoPoller& operator=(const IoPoller&) = delete;

  // Movable
  IoPoller(IoPoller&& other) noexcept;
  IoPoller& operator=(IoPoller&& other) noexcept;

  bool IsValid() const noexcept { return poller_fd_ >= 0; }
  int32_t Fd() const noexcept { return poller_fd_; }

  /** @brief Add an fd to monitor with given events (kReadable, kWritable). */
  expected<void, PollerError> Add(int32_t fd, uint8_t events);

  /** @brief Modify monitored events for an fd. */
  expected<void, PollerError> Modify(int32_t fd, uint8_t events);

  /** @brief Remove an fd from monitoring. */
  expected<void, PollerError> Remove(int32_t fd);

  /**
   * @brief Wait for events.
   * @param results  Output array for ready events.
   * @param max_results  Maximum number of events to return.
   * @param timeout_ms  -1 for infinite, 0 for non-blocking.
   * @return Number of ready events.
   */
  expected<uint32_t, PollerError> Wait(PollResult* results,
                                       uint32_t max_results,
                                       int32_t timeout_ms = -1);

  /** @brief Wait with internal buffer. */
  expected<uint32_t, PollerError> Wait(int32_t timeout_ms = -1);

  /** @brief Access results from the last Wait() call with internal buffer. */
  const PollResult* Results() const noexcept { return results_.data(); }

 private:
  int32_t poller_fd_;
  std::array<PollResult, OSP_IO_POLLER_MAX_EVENTS> results_;
  uint32_t result_count_;
};

// ============================================================================
// Inline Implementation
// ============================================================================

#if defined(OSP_PLATFORM_LINUX)

// ----------------------------------------------------------------------------
// Linux (epoll) helpers
// ----------------------------------------------------------------------------

namespace detail {

inline uint32_t IoEventToEpoll(uint8_t events) {
  uint32_t ep = EPOLLET;  // edge-triggered by default
  if (events & static_cast<uint8_t>(IoEvent::kReadable)) {
    ep |= EPOLLIN;
  }
  if (events & static_cast<uint8_t>(IoEvent::kWritable)) {
    ep |= EPOLLOUT;
  }
  return ep;
}

inline uint8_t EpollToIoEvent(uint32_t ep) {
  uint8_t ev = 0;
  if (ep & EPOLLIN) {
    ev |= static_cast<uint8_t>(IoEvent::kReadable);
  }
  if (ep & EPOLLOUT) {
    ev |= static_cast<uint8_t>(IoEvent::kWritable);
  }
  if (ep & EPOLLERR) {
    ev |= static_cast<uint8_t>(IoEvent::kError);
  }
  if (ep & EPOLLHUP) {
    ev |= static_cast<uint8_t>(IoEvent::kHangup);
  }
  return ev;
}

}  // namespace detail

inline IoPoller::IoPoller() noexcept
    : poller_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      results_{},
      result_count_(0) {}

inline IoPoller::~IoPoller() {
  if (poller_fd_ >= 0) {
    ::close(poller_fd_);
  }
}

inline IoPoller::IoPoller(IoPoller&& other) noexcept
    : poller_fd_(other.poller_fd_), results_{}, result_count_(0) {
  other.poller_fd_ = -1;
  other.result_count_ = 0;
}

inline IoPoller& IoPoller::operator=(IoPoller&& other) noexcept {
  if (this != &other) {
    if (poller_fd_ >= 0) {
      ::close(poller_fd_);
    }
    poller_fd_ = other.poller_fd_;
    result_count_ = 0;
    other.poller_fd_ = -1;
    other.result_count_ = 0;
  }
  return *this;
}

inline expected<void, PollerError> IoPoller::Add(int fd, uint8_t events) {
  struct epoll_event ev {};
  ev.events = detail::IoEventToEpoll(events);
  ev.data.fd = fd;
  if (::epoll_ctl(poller_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    return expected<void, PollerError>::error(PollerError::kAddFailed);
  }
  return expected<void, PollerError>::success();
}

inline expected<void, PollerError> IoPoller::Modify(int fd, uint8_t events) {
  struct epoll_event ev {};
  ev.events = detail::IoEventToEpoll(events);
  ev.data.fd = fd;
  if (::epoll_ctl(poller_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
    return expected<void, PollerError>::error(PollerError::kModifyFailed);
  }
  return expected<void, PollerError>::success();
}

inline expected<void, PollerError> IoPoller::Remove(int32_t fd) {
  if (::epoll_ctl(poller_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
    return expected<void, PollerError>::error(PollerError::kRemoveFailed);
  }
  return expected<void, PollerError>::success();
}

inline expected<uint32_t, PollerError> IoPoller::Wait(PollResult* results,
                                                      uint32_t max_results,
                                                      int32_t timeout_ms) {
  struct epoll_event raw_events[OSP_IO_POLLER_MAX_EVENTS];
  const int32_t max_ev = static_cast<int32_t>(
      max_results < OSP_IO_POLLER_MAX_EVENTS ? max_results
                                             : OSP_IO_POLLER_MAX_EVENTS);

  int32_t n = ::epoll_wait(poller_fd_, raw_events, max_ev, timeout_ms);
  if (n < 0) {
    return expected<uint32_t, PollerError>::error(PollerError::kWaitFailed);
  }

  auto count = static_cast<uint32_t>(n);
  for (uint32_t i = 0; i < count; ++i) {
    results[i].fd = raw_events[i].data.fd;
    results[i].events = detail::EpollToIoEvent(raw_events[i].events);
  }
  return expected<uint32_t, PollerError>::success(count);
}

inline expected<uint32_t, PollerError> IoPoller::Wait(int timeout_ms) {
  auto r = Wait(results_.data(), OSP_IO_POLLER_MAX_EVENTS, timeout_ms);
  if (r.has_value()) {
    result_count_ = r.value();
  }
  return r;
}

#elif defined(OSP_PLATFORM_MACOS)

// ----------------------------------------------------------------------------
// macOS (kqueue) helpers
// ----------------------------------------------------------------------------

inline IoPoller::IoPoller() noexcept
    : poller_fd_(::kqueue()), results_{}, result_count_(0) {}

inline IoPoller::~IoPoller() {
  if (poller_fd_ >= 0) {
    ::close(poller_fd_);
  }
}

inline IoPoller::IoPoller(IoPoller&& other) noexcept
    : poller_fd_(other.poller_fd_), results_{}, result_count_(0) {
  other.poller_fd_ = -1;
  other.result_count_ = 0;
}

inline IoPoller& IoPoller::operator=(IoPoller&& other) noexcept {
  if (this != &other) {
    if (poller_fd_ >= 0) {
      ::close(poller_fd_);
    }
    poller_fd_ = other.poller_fd_;
    result_count_ = 0;
    other.poller_fd_ = -1;
    other.result_count_ = 0;
  }
  return *this;
}

inline expected<void, PollerError> IoPoller::Add(int fd, uint8_t events) {
  std::array<struct kevent, 2> changes{};
  int32_t nchanges = 0;
  if (events & static_cast<uint8_t>(IoEvent::kReadable)) {
    EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
    ++nchanges;
  }
  if (events & static_cast<uint8_t>(IoEvent::kWritable)) {
    EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
    ++nchanges;
  }
  if (nchanges == 0) {
    return expected<void, PollerError>::error(PollerError::kAddFailed);
  }
  struct timespec ts = {0, 0};
  if (::kevent(poller_fd_, changes, nchanges, nullptr, 0, &ts) < 0) {
    return expected<void, PollerError>::error(PollerError::kAddFailed);
  }
  return expected<void, PollerError>::success();
}

inline expected<void, PollerError> IoPoller::Modify(int fd, uint8_t events) {
  // kqueue: re-add with new filters (idempotent) and delete unwanted
  std::array<struct kevent, 4> changes{};
  int32_t nchanges = 0;

  if (events & static_cast<uint8_t>(IoEvent::kReadable)) {
    EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
  } else {
    EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  }
  ++nchanges;

  if (events & static_cast<uint8_t>(IoEvent::kWritable)) {
    EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
           nullptr);
  } else {
    EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  }
  ++nchanges;

  struct timespec ts = {0, 0};
  // Ignore individual filter errors (e.g., deleting a filter not added)
  (void)::kevent(poller_fd_, changes, nchanges, nullptr, 0, &ts);
  return expected<void, PollerError>::success();
}

inline expected<void, PollerError> IoPoller::Remove(int32_t fd) {
  std::array<struct kevent, 2> changes{};
  // Attempt to remove both filters; ignore errors from filters not registered
  EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  struct timespec ts = {0, 0};
  (void)::kevent(poller_fd_, changes, 2, nullptr, 0, &ts);
  return expected<void, PollerError>::success();
}

inline expected<uint32_t, PollerError> IoPoller::Wait(PollResult* results,
                                                      uint32_t max_results,
                                                      int32_t timeout_ms) {
  struct kevent raw_events[OSP_IO_POLLER_MAX_EVENTS];
  const int32_t max_ev = static_cast<int32_t>(
      max_results < OSP_IO_POLLER_MAX_EVENTS ? max_results
                                             : OSP_IO_POLLER_MAX_EVENTS);

  struct timespec ts;
  struct timespec* ts_ptr = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
    ts_ptr = &ts;
  }

  int32_t n = ::kevent(poller_fd_, nullptr, 0, raw_events, max_ev, ts_ptr);
  if (n < 0) {
    return expected<uint32_t, PollerError>::error(PollerError::kWaitFailed);
  }

  // Merge multiple filter events for the same fd into a single PollResult
  uint32_t count = 0;
  for (int32_t i = 0; i < n; ++i) {
    int32_t fd = static_cast<int32_t>(raw_events[i].ident);
    uint8_t ev = 0;
    if (raw_events[i].filter == EVFILT_READ) {
      ev |= static_cast<uint8_t>(IoEvent::kReadable);
    }
    if (raw_events[i].filter == EVFILT_WRITE) {
      ev |= static_cast<uint8_t>(IoEvent::kWritable);
    }
    if (raw_events[i].flags & EV_ERROR) {
      ev |= static_cast<uint8_t>(IoEvent::kError);
    }
    if (raw_events[i].flags & EV_EOF) {
      ev |= static_cast<uint8_t>(IoEvent::kHangup);
    }

    // Try to merge with existing entry for this fd
    bool merged = false;
    for (uint32_t j = 0; j < count; ++j) {
      if (results[j].fd == fd) {
        results[j].events |= ev;
        merged = true;
        break;
      }
    }
    if (!merged && count < max_results) {
      results[count].fd = fd;
      results[count].events = ev;
      ++count;
    }
  }
  return expected<uint32_t, PollerError>::success(count);
}

inline expected<uint32_t, PollerError> IoPoller::Wait(int timeout_ms) {
  auto r = Wait(results_.data(), OSP_IO_POLLER_MAX_EVENTS, timeout_ms);
  if (r.has_value()) {
    result_count_ = r.value();
  }
  return r;
}

#else
#error "IoPoller: unsupported platform (requires Linux epoll or macOS kqueue)"
#endif

}  // namespace osp

#endif  // OSP_IO_POLLER_HPP_
