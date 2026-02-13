/**
 * @file discovery_hsm.hpp
 * @brief HSM-driven node discovery flow management.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Manages discovery lifecycle: Idle -> Announcing -> Discovering -> Stable/Degraded
 *
 * This is an independent enhancement module that does not modify discovery.hpp.
 */

#ifndef OSP_DISCOVERY_HSM_HPP_
#define OSP_DISCOVERY_HSM_HPP_

#include "osp/hsm.hpp"
#include "osp/platform.hpp"

#include <cstdint>
#include <mutex>

namespace osp {

// ============================================================================
// HSM Events
// ============================================================================

enum DiscoveryEvent : uint32_t {
  kEvtStart = 1,
  kEvtNodeFound = 2,
  kEvtNodeLost = 3,
  kEvtNetworkStable = 4,
  kEvtNetworkDegraded = 5,
  kEvtStop = 6,
};

// ============================================================================
// DiscoveryHsmContext
// ============================================================================

using DiscoveryCallbackFn = void (*)(void* ctx);

struct DiscoveryHsmContext {
  uint32_t discovered_count;
  uint32_t lost_count;
  uint32_t stable_threshold;  ///< Min nodes for stable state
  DiscoveryCallbackFn on_stable;
  DiscoveryCallbackFn on_degraded;
  void* callback_ctx;

  // State indices for RequestTransition from within handlers
  StateMachine<DiscoveryHsmContext, 8>* sm;
  int32_t idx_idle;
  int32_t idx_announcing;
  int32_t idx_discovering;
  int32_t idx_stable;
  int32_t idx_degraded;
  int32_t idx_stopped;

  DiscoveryHsmContext() noexcept
      : discovered_count(0), lost_count(0), stable_threshold(1),
        on_stable(nullptr), on_degraded(nullptr), callback_ctx(nullptr),
        sm(nullptr), idx_idle(-1), idx_announcing(-1), idx_discovering(-1),
        idx_stable(-1), idx_degraded(-1), idx_stopped(-1) {}
};

// ============================================================================
// HSM State Handlers
// ============================================================================

namespace detail {

// Idle state: discovery service not started
inline TransitionResult StateIdle(DiscoveryHsmContext& ctx,
                                  const Event& event) {
  if (event.id == kEvtStart) {
    return ctx.sm->RequestTransition(ctx.idx_announcing);
  }
  return TransitionResult::kUnhandled;
}

// Announcing state: broadcasting self existence
inline TransitionResult StateAnnouncing(DiscoveryHsmContext& ctx,
                                        const Event& event) {
  if (event.id == kEvtNodeFound) {
    ++ctx.discovered_count;
    return ctx.sm->RequestTransition(ctx.idx_discovering);
  }
  if (event.id == kEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Discovering state: receiving other node broadcasts
inline TransitionResult StateDiscovering(DiscoveryHsmContext& ctx,
                                         const Event& event) {
  if (event.id == kEvtNodeFound) {
    ++ctx.discovered_count;
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtNetworkStable) {
    if (ctx.discovered_count >= ctx.stable_threshold) {
      return ctx.sm->RequestTransition(ctx.idx_stable);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtNodeLost) {
    ++ctx.lost_count;
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Stable state: network topology stable
inline TransitionResult StateStable(DiscoveryHsmContext& ctx,
                                    const Event& event) {
  if (event.id == kEvtNodeFound) {
    ++ctx.discovered_count;
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtNodeLost) {
    ++ctx.lost_count;
    return ctx.sm->RequestTransition(ctx.idx_degraded);
  }
  if (event.id == kEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Degraded state: partial node timeout, network unstable
inline TransitionResult StateDegraded(DiscoveryHsmContext& ctx,
                                      const Event& event) {
  if (event.id == kEvtNodeFound) {
    ++ctx.discovered_count;
    return ctx.sm->RequestTransition(ctx.idx_discovering);
  }
  if (event.id == kEvtNetworkStable) {
    if (ctx.discovered_count >= ctx.stable_threshold) {
      return ctx.sm->RequestTransition(ctx.idx_stable);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtNodeLost) {
    ++ctx.lost_count;
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtStop) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Stopped state: discovery stopped
inline TransitionResult StateStopped(DiscoveryHsmContext& ctx,
                                     const Event& event) {
  (void)ctx;
  (void)event;
  return TransitionResult::kUnhandled;
}

// Entry action for Stable state
inline void OnEnterStable(DiscoveryHsmContext& ctx) {
  if (ctx.on_stable != nullptr) {
    ctx.on_stable(ctx.callback_ctx);
  }
}

// Entry action for Degraded state
inline void OnEnterDegraded(DiscoveryHsmContext& ctx) {
  if (ctx.on_degraded != nullptr) {
    ctx.on_degraded(ctx.callback_ctx);
  }
}

}  // namespace detail

// ============================================================================
// HsmDiscovery
// ============================================================================

/**
 * @brief HSM-driven node discovery flow manager.
 *
 * @tparam MaxNodes Maximum number of nodes (unused in HSM, for API consistency)
 *
 * Usage:
 * @code
 *   HsmDiscovery<64> discovery;
 *   discovery.SetStableThreshold(3);
 *   discovery.OnStable([](void*) { printf("Network stable\n"); }, nullptr);
 *   discovery.Start();
 *   discovery.OnNodeFound();
 *   discovery.CheckStability();
 * @endcode
 */
template <uint32_t MaxNodes = 64>
class HsmDiscovery {
 public:
  HsmDiscovery() noexcept : hsm_initialized_(false) {
    InitializeHsm();
  }

  ~HsmDiscovery() {
    if (hsm_initialized_) {
      GetHsm()->~StateMachine();
      hsm_initialized_ = false;
    }
  }

  HsmDiscovery(const HsmDiscovery&) = delete;
  HsmDiscovery& operator=(const HsmDiscovery&) = delete;

  // ==========================================================================
  // Configuration
  // ==========================================================================

  /**
   * @brief Set minimum node count for stable state.
   * @param threshold Minimum discovered nodes to consider network stable.
   */
  void SetStableThreshold(uint32_t threshold) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.stable_threshold = threshold;
  }

  /**
   * @brief Register callback for stable state entry.
   * @param fn Callback function.
   * @param ctx User context pointer.
   */
  void OnStable(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_stable = fn;
    context_.callback_ctx = ctx;
  }

  /**
   * @brief Register callback for degraded state entry.
   * @param fn Callback function.
   * @param ctx User context pointer.
   */
  void OnDegraded(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_degraded = fn;
    context_.callback_ctx = ctx;
  }

  // ==========================================================================
  // Lifecycle Control
  // ==========================================================================

  /**
   * @brief Start discovery service (Idle -> Announcing).
   */
  void Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtStart, nullptr};
    GetHsm()->Dispatch(evt);
  }

  /**
   * @brief Stop discovery service (Any -> Stopped).
   */
  void Stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtStop, nullptr};
    GetHsm()->Dispatch(evt);
  }

  // ==========================================================================
  // Event Triggers
  // ==========================================================================

  /**
   * @brief Notify that a new node was discovered.
   */
  void OnNodeFound() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtNodeFound, nullptr};
    GetHsm()->Dispatch(evt);
  }

  /**
   * @brief Notify that a node was lost (timeout).
   */
  void OnNodeLost() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtNodeLost, nullptr};
    GetHsm()->Dispatch(evt);
  }

  /**
   * @brief Check if network is stable and trigger state transition if needed.
   *
   * Should be called periodically after a period of no topology changes.
   */
  void CheckStability() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtNetworkStable, nullptr};
    GetHsm()->Dispatch(evt);
  }

  /**
   * @brief Trigger network degraded event (multiple nodes lost).
   */
  void TriggerDegraded() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Event evt{kEvtNetworkDegraded, nullptr};
    GetHsm()->Dispatch(evt);
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  /**
   * @brief Get current state name.
   * @return State name string (static lifetime).
   */
  const char* GetState() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->CurrentStateName();
  }

  /**
   * @brief Check if in Stable state.
   */
  bool IsStable() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_stable);
  }

  /**
   * @brief Check if in Degraded state.
   */
  bool IsDegraded() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_degraded);
  }

  /**
   * @brief Check if in Discovering state.
   */
  bool IsDiscovering() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_discovering);
  }

  /**
   * @brief Check if in Announcing state.
   */
  bool IsAnnouncing() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_announcing);
  }

  /**
   * @brief Check if in Idle state.
   */
  bool IsIdle() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_idle);
  }

  /**
   * @brief Check if stopped.
   */
  bool IsStopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetHsm()->IsInState(context_.idx_stopped);
  }

  /**
   * @brief Get discovered node count.
   */
  uint32_t GetDiscoveredCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.discovered_count;
  }

  /**
   * @brief Get lost node count.
   */
  uint32_t GetLostCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.lost_count;
  }

  /**
   * @brief Reset counters (discovered_count, lost_count).
   */
  void ResetCounters() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.discovered_count = 0;
    context_.lost_count = 0;
  }

 private:
  DiscoveryHsmContext context_;
  bool hsm_initialized_;
  alignas(StateMachine<DiscoveryHsmContext, 8>)
      uint8_t hsm_storage_[sizeof(StateMachine<DiscoveryHsmContext, 8>)];
  mutable std::mutex mutex_;

  void InitializeHsm() noexcept {
    auto* hsm = new (hsm_storage_)
        StateMachine<DiscoveryHsmContext, 8>(context_);
    hsm_initialized_ = true;

    // Store SM pointer in context for handlers to call RequestTransition
    context_.sm = hsm;

    // Add states
    context_.idx_idle = hsm->AddState({
        "Idle", -1,
        detail::StateIdle,
        nullptr, nullptr, nullptr
    });

    context_.idx_announcing = hsm->AddState({
        "Announcing", -1,
        detail::StateAnnouncing,
        nullptr, nullptr, nullptr
    });

    context_.idx_discovering = hsm->AddState({
        "Discovering", -1,
        detail::StateDiscovering,
        nullptr, nullptr, nullptr
    });

    context_.idx_stable = hsm->AddState({
        "Stable", -1,
        detail::StateStable,
        detail::OnEnterStable,
        nullptr, nullptr
    });

    context_.idx_degraded = hsm->AddState({
        "Degraded", -1,
        detail::StateDegraded,
        detail::OnEnterDegraded,
        nullptr, nullptr
    });

    context_.idx_stopped = hsm->AddState({
        "Stopped", -1,
        detail::StateStopped,
        nullptr, nullptr, nullptr
    });

    hsm->SetInitialState(context_.idx_idle);
    hsm->Start();
  }

  StateMachine<DiscoveryHsmContext, 8>* GetHsm() noexcept {
    return reinterpret_cast<StateMachine<DiscoveryHsmContext, 8>*>(
        hsm_storage_);
  }

  const StateMachine<DiscoveryHsmContext, 8>* GetHsm() const noexcept {
    return reinterpret_cast<const StateMachine<DiscoveryHsmContext, 8>*>(
        hsm_storage_);
  }
};

}  // namespace osp

#endif  // OSP_DISCOVERY_HSM_HPP_
