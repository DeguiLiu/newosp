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

#include "osp/fault_collector.hpp"
#include "osp/hsm.hpp"
#include "osp/platform.hpp"

#include <cstdint>
#include <mutex>

namespace osp {

// ============================================================================
// HSM Events
// ============================================================================

enum class DiscoveryHsmEvent : uint32_t {
  kDiscEvtStart = 1,
  kDiscEvtNodeFound = 2,
  kDiscEvtNodeLost = 3,
  kDiscEvtNetworkStable = 4,
  kDiscEvtNetworkDegraded = 5,
  kDiscEvtStop = 6,
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

  // Optional fault reporting (nullptr = disabled)
  FaultReporter fault_reporter;

  // Fault index for discovery degradation
  static constexpr uint16_t kFaultNetworkDegraded = 0U;

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
        fault_reporter(),
        sm(nullptr), idx_idle(-1), idx_announcing(-1), idx_discovering(-1),
        idx_stable(-1), idx_degraded(-1), idx_stopped(-1) {}
};

// ============================================================================
// HSM State Handlers
// ============================================================================

namespace detail {

// Idle state: discovery service not started
inline TransitionResult DiscStateIdle(DiscoveryHsmContext& ctx,
                                      const Event& event) {
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStart)) {
    return ctx.sm->RequestTransition(ctx.idx_announcing);
  }
  return TransitionResult::kUnhandled;
}

// Announcing state: broadcasting self existence
inline TransitionResult DiscStateAnnouncing(DiscoveryHsmContext& ctx,
                                            const Event& event) {
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeFound)) {
    ++ctx.discovered_count;
    return ctx.sm->RequestTransition(ctx.idx_discovering);
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStop)) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Discovering state: receiving other node broadcasts
inline TransitionResult DiscStateDiscovering(DiscoveryHsmContext& ctx,
                                             const Event& event) {
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeFound)) {
    ++ctx.discovered_count;
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNetworkStable)) {
    if (ctx.discovered_count >= ctx.stable_threshold) {
      return ctx.sm->RequestTransition(ctx.idx_stable);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeLost)) {
    ++ctx.lost_count;
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStop)) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Stable state: network topology stable
inline TransitionResult DiscStateStable(DiscoveryHsmContext& ctx,
                                        const Event& event) {
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeFound)) {
    ++ctx.discovered_count;
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeLost)) {
    ++ctx.lost_count;
    return ctx.sm->RequestTransition(ctx.idx_degraded);
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStop)) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Degraded state: partial node timeout, network unstable
inline TransitionResult DiscStateDegraded(DiscoveryHsmContext& ctx,
                                          const Event& event) {
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeFound)) {
    ++ctx.discovered_count;
    return ctx.sm->RequestTransition(ctx.idx_discovering);
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNetworkStable)) {
    if (ctx.discovered_count >= ctx.stable_threshold) {
      return ctx.sm->RequestTransition(ctx.idx_stable);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeLost)) {
    ++ctx.lost_count;
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStop)) {
    return ctx.sm->RequestTransition(ctx.idx_stopped);
  }
  return TransitionResult::kUnhandled;
}

// Stopped state: discovery stopped
inline TransitionResult DiscStateStopped(DiscoveryHsmContext& /*ctx*/,
                                         const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

// Entry action for Stable state
inline void OnEnterDiscStable(DiscoveryHsmContext& ctx) {
  if (ctx.on_stable != nullptr) {
    ctx.on_stable(ctx.callback_ctx);
  }
}

// Entry action for Degraded state
inline void OnEnterDiscDegraded(DiscoveryHsmContext& ctx) {
  ctx.fault_reporter.Report(
      DiscoveryHsmContext::kFaultNetworkDegraded,
      ctx.lost_count, FaultPriority::kMedium);
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
  HsmDiscovery() noexcept : hsm_(context_), started_(false) {
    context_.sm = &hsm_;

    // Add states
    context_.idx_idle = hsm_.AddState({
        "Idle", -1,
        detail::DiscStateIdle,
        nullptr, nullptr, nullptr
    });

    context_.idx_announcing = hsm_.AddState({
        "Announcing", -1,
        detail::DiscStateAnnouncing,
        nullptr, nullptr, nullptr
    });

    context_.idx_discovering = hsm_.AddState({
        "Discovering", -1,
        detail::DiscStateDiscovering,
        nullptr, nullptr, nullptr
    });

    context_.idx_stable = hsm_.AddState({
        "Stable", -1,
        detail::DiscStateStable,
        detail::OnEnterDiscStable,
        nullptr, nullptr
    });

    context_.idx_degraded = hsm_.AddState({
        "Degraded", -1,
        detail::DiscStateDegraded,
        detail::OnEnterDiscDegraded,
        nullptr, nullptr
    });

    context_.idx_stopped = hsm_.AddState({
        "Stopped", -1,
        detail::DiscStateStopped,
        nullptr, nullptr, nullptr
    });

    hsm_.SetInitialState(context_.idx_idle);
  }

  ~HsmDiscovery() = default;

  HsmDiscovery(const HsmDiscovery&) = delete;
  HsmDiscovery& operator=(const HsmDiscovery&) = delete;

  // ==========================================================================
  // Configuration
  // ==========================================================================

  void SetStableThreshold(uint32_t threshold) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.stable_threshold = threshold;
  }

  void OnStable(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_stable = fn;
    context_.callback_ctx = ctx;
  }

  void OnDegraded(DiscoveryCallbackFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.on_degraded = fn;
    context_.callback_ctx = ctx;
  }

  /// @brief Wire optional fault reporter for automatic degradation reporting.
  /// @param reporter FaultReporter with fn + ctx (nullptr fn = disabled).
  void SetFaultReporter(FaultReporter reporter) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.fault_reporter = reporter;
  }

  // ==========================================================================
  // Lifecycle Control
  // ==========================================================================

  void Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      hsm_.Start();
      started_ = true;
    }
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStart), nullptr};
    hsm_.Dispatch(evt);
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtStop), nullptr};
    hsm_.Dispatch(evt);
  }

  // ==========================================================================
  // Event Triggers
  // ==========================================================================

  void OnNodeFound() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeFound), nullptr};
    hsm_.Dispatch(evt);
  }

  void OnNodeLost() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNodeLost), nullptr};
    hsm_.Dispatch(evt);
  }

  void CheckStability() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNetworkStable), nullptr};
    hsm_.Dispatch(evt);
  }

  void TriggerDegraded() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    Event evt{static_cast<uint32_t>(DiscoveryHsmEvent::kDiscEvtNetworkDegraded), nullptr};
    hsm_.Dispatch(evt);
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  const char* GetState() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return "NotStarted";
    return hsm_.CurrentStateName();
  }

  bool IsStable() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_stable);
  }

  bool IsDegraded() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_degraded);
  }

  bool IsDiscovering() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_discovering);
  }

  bool IsAnnouncing() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_announcing);
  }

  bool IsIdle() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return true;
    return hsm_.IsInState(context_.idx_idle);
  }

  bool IsStopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return false;
    return hsm_.IsInState(context_.idx_stopped);
  }

  uint32_t GetDiscoveredCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.discovered_count;
  }

  uint32_t GetLostCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_.lost_count;
  }

  void ResetCounters() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    context_.discovered_count = 0;
    context_.lost_count = 0;
  }

 private:
  DiscoveryHsmContext context_;
  StateMachine<DiscoveryHsmContext, 8> hsm_;
  bool started_;
  mutable std::mutex mutex_;
};

}  // namespace osp

#endif  // OSP_DISCOVERY_HSM_HPP_
