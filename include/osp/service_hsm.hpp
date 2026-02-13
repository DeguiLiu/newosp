/**
 * @file service_hsm.hpp
 * @brief HSM-driven Service server connection lifecycle management.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Manages service server state transitions:
 * Idle -> Listening -> Active -> Error/ShuttingDown
 */

#ifndef OSP_SERVICE_HSM_HPP_
#define OSP_SERVICE_HSM_HPP_

#include "osp/hsm.hpp"
#include "osp/platform.hpp"

#include <cstdint>
#include <mutex>

namespace osp {

// ============================================================================
// HSM Events
// ============================================================================

enum ServiceHsmEvent : uint32_t {
  kSvcEvtStart = 1,
  kSvcEvtClientConnected = 2,
  kSvcEvtClientDisconnected = 3,
  kSvcEvtError = 4,
  kSvcEvtStop = 5,
  kSvcEvtRecover = 6,
};

// ============================================================================
// ServiceHsmContext
// ============================================================================

using ServiceErrorFn = void (*)(int32_t error_code, void* ctx);
using ServiceShutdownFn = void (*)(void* ctx);

struct ServiceHsmContext {
  uint32_t active_clients;
  int32_t error_code;
  ServiceErrorFn on_error;
  ServiceShutdownFn on_shutdown;
  void* callback_ctx;

  // State indices for RequestTransition from within handlers
  StateMachine<ServiceHsmContext, 8>* sm;
  int32_t idx_idle;
  int32_t idx_listening;
  int32_t idx_active;
  int32_t idx_error;
  int32_t idx_shutting_down;

  ServiceHsmContext() noexcept
      : active_clients(0), error_code(0),
        on_error(nullptr), on_shutdown(nullptr), callback_ctx(nullptr),
        sm(nullptr), idx_idle(-1), idx_listening(-1), idx_active(-1),
        idx_error(-1), idx_shutting_down(-1) {}
};

// ============================================================================
// HSM State Handlers
// ============================================================================

namespace detail {

// Idle state: service not started
inline TransitionResult StateIdle(ServiceHsmContext& ctx, const Event& event) {
  if (event.id == kSvcEvtStart) {
    return ctx.sm->RequestTransition(ctx.idx_listening);
  }
  if (event.id == kSvcEvtError) {
    if (event.data != nullptr) {
      ctx.error_code = *static_cast<const int32_t*>(event.data);
    }
    return ctx.sm->RequestTransition(ctx.idx_error);
  }
  return TransitionResult::kUnhandled;
}

// Listening state: bound to port, waiting for connections
inline TransitionResult StateListening(ServiceHsmContext& ctx,
                                       const Event& event) {
  if (event.id == kSvcEvtClientConnected) {
    ++ctx.active_clients;
    return ctx.sm->RequestTransition(ctx.idx_active);
  }
  if (event.id == kSvcEvtError) {
    if (event.data != nullptr) {
      ctx.error_code = *static_cast<const int32_t*>(event.data);
    }
    return ctx.sm->RequestTransition(ctx.idx_error);
  }
  if (event.id == kSvcEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_shutting_down);
  }
  return TransitionResult::kUnhandled;
}

// Active state: has active client connections
inline TransitionResult StateActive(ServiceHsmContext& ctx,
                                    const Event& event) {
  if (event.id == kSvcEvtClientConnected) {
    ++ctx.active_clients;
    return TransitionResult::kHandled;
  }
  if (event.id == kSvcEvtClientDisconnected) {
    if (ctx.active_clients > 0) {
      --ctx.active_clients;
    }
    if (ctx.active_clients == 0) {
      return ctx.sm->RequestTransition(ctx.idx_listening);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == kSvcEvtError) {
    if (event.data != nullptr) {
      ctx.error_code = *static_cast<const int32_t*>(event.data);
    }
    return ctx.sm->RequestTransition(ctx.idx_error);
  }
  if (event.id == kSvcEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_shutting_down);
  }
  return TransitionResult::kUnhandled;
}

// Error state: error occurred
inline TransitionResult StateError(ServiceHsmContext& ctx, const Event& event) {
  if (event.id == kSvcEvtRecover) {
    ctx.error_code = 0;
    ctx.active_clients = 0;
    return ctx.sm->RequestTransition(ctx.idx_idle);
  }
  if (event.id == kSvcEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_shutting_down);
  }
  return TransitionResult::kUnhandled;
}

// ShuttingDown state: service is shutting down
inline TransitionResult StateShuttingDown(ServiceHsmContext& ctx,
                                          const Event& event) {
  // Terminal state, no transitions out
  (void)ctx;
  (void)event;
  return TransitionResult::kHandled;
}

// Entry action for Error state
inline void OnEnterError(ServiceHsmContext& ctx) {
  if (ctx.on_error != nullptr) {
    ctx.on_error(ctx.error_code, ctx.callback_ctx);
  }
}

// Entry action for ShuttingDown state
inline void OnEnterShuttingDown(ServiceHsmContext& ctx) {
  if (ctx.on_shutdown != nullptr) {
    ctx.on_shutdown(ctx.callback_ctx);
  }
  ctx.active_clients = 0;
}

}  // namespace detail

// ============================================================================
// HsmService
// ============================================================================

/**
 * @brief HSM-driven service server connection lifecycle manager.
 *
 * @tparam MaxClients Maximum number of concurrent clients (unused in HSM,
 *                    kept for API compatibility).
 *
 * Thread-safe: all public methods are protected by mutex.
 */
template <uint32_t MaxClients = 32>
class HsmService {
 public:
  HsmService() noexcept : hsm_(context_), started_(false) {
    context_.sm = &hsm_;

    // Add states
    context_.idx_idle = hsm_.AddState({
        "Idle", -1,
        detail::StateIdle,
        nullptr, nullptr, nullptr
    });

    context_.idx_listening = hsm_.AddState({
        "Listening", -1,
        detail::StateListening,
        nullptr, nullptr, nullptr
    });

    context_.idx_active = hsm_.AddState({
        "Active", -1,
        detail::StateActive,
        nullptr, nullptr, nullptr
    });

    context_.idx_error = hsm_.AddState({
        "Error", -1,
        detail::StateError,
        detail::OnEnterError,
        nullptr, nullptr
    });

    context_.idx_shutting_down = hsm_.AddState({
        "ShuttingDown", -1,
        detail::StateShuttingDown,
        detail::OnEnterShuttingDown,
        nullptr, nullptr
    });

    hsm_.SetInitialState(context_.idx_idle);
  }

  ~HsmService() = default;

  HsmService(const HsmService&) = delete;
  HsmService& operator=(const HsmService&) = delete;

  // ==========================================================================
  // Lifecycle Control
  // ==========================================================================

  /**
   * @brief Start the service (transition from Idle to Listening).
   */
  void Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      hsm_.Start();
      started_ = true;
    }
    Event evt{kSvcEvtStart, nullptr};
    hsm_.Dispatch(evt);
  }

  /**
   * @brief Stop the service (transition to ShuttingDown).
   */
  void Stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{kSvcEvtStop, nullptr};
    hsm_.Dispatch(evt);
  }

  /**
   * @brief Recover from error state (transition to Idle).
   */
  void Recover() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{kSvcEvtRecover, nullptr};
    hsm_.Dispatch(evt);
  }

  // ==========================================================================
  // Event Triggers
  // ==========================================================================

  /**
   * @brief Notify that a client has connected.
   */
  void OnClientConnect() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{kSvcEvtClientConnected, nullptr};
    hsm_.Dispatch(evt);
  }

  /**
   * @brief Notify that a client has disconnected.
   */
  void OnClientDisconnect() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{kSvcEvtClientDisconnected, nullptr};
    hsm_.Dispatch(evt);
  }

  /**
   * @brief Notify that an error has occurred.
   * @param error_code Error code to store in context.
   */
  void OnError(int32_t error_code) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{kSvcEvtError, &error_code};
    hsm_.Dispatch(evt);
  }

  // ==========================================================================
  // Callback Registration
  // ==========================================================================

  /**
   * @brief Register error callback.
   * @param fn Function to call when entering Error state.
   * @param ctx User context pointer passed to callback.
   */
  void SetErrorCallback(ServiceErrorFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_error = fn;
    context_.callback_ctx = ctx;
  }

  /**
   * @brief Register shutdown callback.
   * @param fn Function to call when entering ShuttingDown state.
   * @param ctx User context pointer passed to callback.
   */
  void SetShutdownCallback(ServiceShutdownFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_shutdown = fn;
    context_.callback_ctx = ctx;
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  /**
   * @brief Get the current state name.
   * @return State name string (static lifetime).
   */
  const char* GetState() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return "NotStarted";
    return hsm_.CurrentStateName();
  }

  /**
   * @brief Check if service is in Active state.
   */
  bool IsActive() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_active);
  }

  /**
   * @brief Check if service is in Listening state.
   */
  bool IsListening() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_listening);
  }

  /**
   * @brief Check if service is in Error state.
   */
  bool IsError() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_error);
  }

  /**
   * @brief Check if service is in Idle state.
   */
  bool IsIdle() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return true;
    return hsm_.IsInState(context_.idx_idle);
  }

  /**
   * @brief Check if service is in ShuttingDown state.
   */
  bool IsShuttingDown() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_shutting_down);
  }

  /**
   * @brief Get the number of active clients.
   */
  uint32_t GetActiveClients() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.active_clients;
  }

  /**
   * @brief Get the last error code.
   */
  int32_t GetErrorCode() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.error_code;
  }

 private:
  ServiceHsmContext context_;
  StateMachine<ServiceHsmContext, 8> hsm_;
  bool started_;
  mutable std::mutex mutex_;
};

}  // namespace osp

#endif  // OSP_SERVICE_HSM_HPP_
