/**
 * @file shutdown.hpp
 * @brief Graceful shutdown manager for POSIX systems (Linux/macOS).
 *
 * Uses pipe(2) for async-signal-safe wakeup and sigaction(2) for
 * signal installation. Callbacks are executed in LIFO order on
 * shutdown. Header-only, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SHUTDOWN_HPP_
#define OSP_SHUTDOWN_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <unistd.h>

namespace osp {

// ============================================================================
// ShutdownError
// ============================================================================

enum class ShutdownError : uint8_t {
  kCallbacksFull = 0,
  kPipeCreationFailed,
  kSignalInstallFailed,
  kAlreadyInstantiated
};

// ============================================================================
// ShutdownFn - Cleanup callback type
// ============================================================================

/// @brief Shutdown cleanup callback. Receives the signal number (0 for manual).
using ShutdownFn = void (*)(int signo);

// ============================================================================
// Detail: Global instance pointer (function-local static for C++14)
// ============================================================================

// Forward declaration for detail namespace
class ShutdownManager;

namespace detail {

/**
 * @brief Returns a reference to the global ShutdownManager pointer.
 *
 * Uses a function-local static to avoid C++14 inline variable issues.
 * Exactly one ShutdownManager may exist per process.
 */
inline ShutdownManager*& GetShutdownInstance() {
  static ShutdownManager* ptr = nullptr;
  return ptr;
}

}  // namespace detail

// ============================================================================
// ShutdownManager
// ============================================================================

/**
 * @brief Manages graceful process shutdown via POSIX signals.
 *
 * Installs signal handlers for SIGINT and SIGTERM using sigaction(2).
 * The signal handler writes a single byte to an internal pipe to wake
 * the blocking WaitForShutdown() call in an async-signal-safe manner.
 * Registered cleanup callbacks are executed in LIFO order.
 *
 * Only one ShutdownManager instance may exist per process.
 *
 * Usage:
 * @code
 *   osp::ShutdownManager mgr;
 *   mgr.Register([](int) { cleanup_resources(); });
 *   mgr.InstallSignalHandlers();
 *   mgr.WaitForShutdown();
 * @endcode
 */
class ShutdownManager final {
 public:
  /**
   * @brief Constructs the shutdown manager and creates the wakeup pipe.
   * @param max_callbacks Maximum number of callbacks (clamped to kMaxCallbacks).
   *
   * Sets the global instance pointer. Only one instance per process is allowed;
   * a second construction will assert in debug mode.
   */
  explicit ShutdownManager(uint32_t max_callbacks = 16) noexcept
      : callback_count_(0),
        max_callbacks_((max_callbacks <= kMaxCallbacks) ? max_callbacks : kMaxCallbacks),
        shutdown_flag_(false),
        valid_(false) {
    pipe_fd_[0] = -1;
    pipe_fd_[1] = -1;

    // Enforce single-instance constraint at runtime (not just debug assert).
    // If another instance already exists, do NOT overwrite the global pointer.
    if (detail::GetShutdownInstance() != nullptr) {
      // Mark as invalid -- pipe remains {-1, -1}, all operations return error.
      return;
    }
    detail::GetShutdownInstance() = this;

    if (::pipe(pipe_fd_) != 0) {
      pipe_fd_[0] = -1;
      pipe_fd_[1] = -1;
      return;
    }
    valid_ = true;
  }

  /**
   * @brief Destroys the shutdown manager.
   *
   * Closes the wakeup pipe and clears the global instance pointer.
   */
  ~ShutdownManager() {
    if (pipe_fd_[0] >= 0) {
      ::close(pipe_fd_[0]);
    }
    if (pipe_fd_[1] >= 0) {
      ::close(pipe_fd_[1]);
    }
    // Only clear global pointer if we are the registered instance
    if (detail::GetShutdownInstance() == this) {
      detail::GetShutdownInstance() = nullptr;
    }
  }

  // Non-copyable, non-movable
  ShutdownManager(const ShutdownManager&) = delete;
  ShutdownManager& operator=(const ShutdownManager&) = delete;
  ShutdownManager(ShutdownManager&&) = delete;
  ShutdownManager& operator=(ShutdownManager&&) = delete;

  // ==========================================================================
  // Public API
  // ==========================================================================

  /**
   * @brief Check if this instance is the active (valid) shutdown manager.
   *
   * Returns false if another instance already existed at construction time
   * or if pipe creation failed.  All API calls on an invalid instance
   * return errors or are no-ops.
   */
  bool IsValid() const noexcept { return valid_; }

  /**
   * @brief Register a cleanup callback to be invoked on shutdown (LIFO order).
   * @param fn Callback function pointer. Must not be nullptr.
   * @return success on registration, or ShutdownError on failure:
   *         - kAlreadyInstantiated if this instance is invalid (duplicate).
   *         - kCallbacksFull if fn is nullptr or capacity reached.
   */
  expected<void, ShutdownError> Register(ShutdownFn fn) noexcept {
    if (!valid_) {
      return expected<void, ShutdownError>::error(
          ShutdownError::kAlreadyInstantiated);
    }
    if (fn == nullptr) {
      return expected<void, ShutdownError>::error(ShutdownError::kCallbacksFull);
    }
    if (callback_count_ >= max_callbacks_) {
      return expected<void, ShutdownError>::error(ShutdownError::kCallbacksFull);
    }
    callbacks_[callback_count_] = fn;
    ++callback_count_;
    return expected<void, ShutdownError>::success();
  }

  /**
   * @brief Install signal handlers for SIGINT and SIGTERM via sigaction(2).
   * @return success on installation, or ShutdownError::kSignalInstallFailed.
   *
   * Uses sigaction(2) (not signal(2)) with SA_RESTART flag.
   */
  expected<void, ShutdownError> InstallSignalHandlers() noexcept {
    if (!valid_) {
      return expected<void, ShutdownError>::error(
          ShutdownError::kAlreadyInstantiated);
    }
    if (pipe_fd_[1] < 0) {
      return expected<void, ShutdownError>::error(
          ShutdownError::kPipeCreationFailed);
    }

    struct sigaction sa;
    sa.sa_handler = &ShutdownManager::SignalHandler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (::sigaction(SIGINT, &sa, nullptr) != 0) {
      return expected<void, ShutdownError>::error(
          ShutdownError::kSignalInstallFailed);
    }
    if (::sigaction(SIGTERM, &sa, nullptr) != 0) {
      return expected<void, ShutdownError>::error(
          ShutdownError::kSignalInstallFailed);
    }

    return expected<void, ShutdownError>::success();
  }

  /**
   * @brief Manually trigger shutdown.
   * @param signo Signal number to pass to callbacks (0 for manual trigger).
   *
   * Sets the shutdown flag and writes to the pipe to unblock WaitForShutdown.
   */
  void Quit(int signo = 0) noexcept {
    bool expected_val = false;
    if (shutdown_flag_.compare_exchange_strong(expected_val, true)) {
      signo_.store(signo, std::memory_order_relaxed);
      if (pipe_fd_[1] >= 0) {
        const uint8_t byte = 1;
        // write(2) is async-signal-safe; ignore return value intentionally
        (void)::write(pipe_fd_[1], &byte, 1);
      }
    }
  }

  /**
   * @brief Block until a shutdown signal is received, then run callbacks LIFO.
   *
   * Blocks on read(2) from the internal pipe. Once unblocked (by signal
   * handler or Quit()), executes all registered callbacks in reverse
   * registration order (LIFO).
   */
  void WaitForShutdown() noexcept {
    if (pipe_fd_[0] >= 0 && !shutdown_flag_.load()) {
      uint8_t buf = 0;
      // Block until signal handler or Quit() writes to the pipe
      (void)::read(pipe_fd_[0], &buf, 1);
    }

    // Execute callbacks in LIFO order
    const int signo = signo_.load(std::memory_order_relaxed);
    for (uint32_t i = callback_count_; i > 0U; --i) {
      if (callbacks_[i - 1U] != nullptr) {
        callbacks_[i - 1U](signo);
      }
    }
  }

  /**
   * @brief Check if shutdown has been requested.
   * @return true if shutdown was signaled or Quit() was called.
   */
  bool IsShutdownRequested() const noexcept {
    return shutdown_flag_.load();
  }

 private:
  // ==========================================================================
  // Constants
  // ==========================================================================

  static constexpr uint32_t kMaxCallbacks = 16;

  // ==========================================================================
  // Signal handler (static, async-signal-safe)
  // ==========================================================================

  /**
   * @brief Static signal handler installed via sigaction(2).
   *
   * Only performs async-signal-safe operations: sets an atomic flag
   * and writes a single byte to the wakeup pipe.
   */
  static void SignalHandler(int signo) {
    ShutdownManager* self = detail::GetShutdownInstance();
    if (self != nullptr) {
      self->shutdown_flag_.store(true);
      self->signo_.store(signo, std::memory_order_relaxed);
      if (self->pipe_fd_[1] >= 0) {
        const uint8_t byte = 1;
        (void)::write(self->pipe_fd_[1], &byte, 1);
      }
    }
  }

  // ==========================================================================
  // Data members
  // ==========================================================================

  ShutdownFn callbacks_[kMaxCallbacks] = {};
  uint32_t callback_count_;
  uint32_t max_callbacks_;
  std::atomic<bool> shutdown_flag_;
  int pipe_fd_[2];
  std::atomic<int> signo_{0};  ///< Signal number, written from signal handler (lock-free)
  bool valid_;                 ///< True if this is the active (valid) instance.
};

}  // namespace osp

#endif  // OSP_SHUTDOWN_HPP_
