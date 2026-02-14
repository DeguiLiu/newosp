/**
 * @file lifecycle_node.hpp
 * @brief Lifecycle-managed node with HSM-driven hierarchical state machine.
 *
 * Rich state hierarchy driven by osp::StateMachine (hsm.hpp):
 *
 *   Alive (root)
 *   +-- Unconfigured
 *   |   +-- Initializing     (entry: load defaults)
 *   |   +-- WaitingConfig    (waiting for configure trigger)
 *   +-- Configured
 *   |   +-- Inactive
 *   |   |   +-- Standby      (ready to activate)
 *   |   |   +-- Paused       (was active, deactivated, can resume)
 *   |   +-- Active
 *   |       +-- Starting     (transitional: running on_activate)
 *   |       +-- Running      (normal operation)
 *   |       +-- Degraded     (running with warnings/errors)
 *   +-- Error
 *   |   +-- Recoverable     (can retry configure/activate)
 *   |   +-- Fatal           (must shutdown)
 *   +-- Finalized           (terminal)
 *
 * Backward-compatible public API: Configure(), Activate(), Deactivate(),
 * Cleanup(), Shutdown() still work. GetState() returns the original 4-state
 * LifecycleState enum. New API exposes the rich hierarchy.
 *
 * Callbacks are function pointers (no heap allocation).
 */

#ifndef OSP_LIFECYCLE_NODE_HPP_
#define OSP_LIFECYCLE_NODE_HPP_

#include "osp/fault_collector.hpp"
#include "osp/hsm.hpp"
#include "osp/node.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

namespace osp {

// ============================================================================
// Lifecycle State (backward-compatible, coarse-grained)
// ============================================================================

enum class LifecycleState : uint8_t {
  kUnconfigured,  // Initial state
  kInactive,      // Configured but not active
  kActive,        // Running
  kFinalized      // Terminal state
};

// ============================================================================
// Lifecycle Detailed State (rich hierarchy)
// ============================================================================

enum class LifecycleDetailedState : uint8_t {
  kAlive = 0,        // Root (abstract parent)
  kUnconfigured,     // Parent for Initializing/WaitingConfig
  kInitializing,     // Entry: loading defaults
  kWaitingConfig,    // Waiting for configure trigger
  kConfigured,       // Parent for Inactive/Active
  kInactive,         // Parent for Standby/Paused
  kStandby,          // Ready to activate
  kPaused,           // Was active, deactivated, can resume
  kActive,           // Parent for Starting/Running/Degraded
  kStarting,         // Transitional: running on_activate
  kRunning,          // Normal operation
  kDegraded,         // Running with warnings/errors
  kError,            // Parent for Recoverable/Fatal
  kRecoverable,      // Can retry configure/activate
  kFatal,            // Must shutdown
  kFinalized,        // Terminal state
  kCount             // Sentinel
};

// ============================================================================
// Lifecycle Transition (backward-compatible)
// ============================================================================

enum class LifecycleTransition : uint8_t {
  kConfigure,     // Unconfigured -> Inactive
  kActivate,      // Inactive -> Active
  kDeactivate,    // Active -> Inactive
  kCleanup,       // Inactive -> Unconfigured
  kShutdown       // Any -> Finalized
};

// ============================================================================
// Lifecycle Error
// ============================================================================

enum class LifecycleError : uint8_t {
  kInvalidTransition,  // Transition not allowed from current state
  kCallbackFailed,     // User callback returned false
  kAlreadyFinalized    // Node is already in Finalized state
};

// ============================================================================
// Lifecycle Callback Types
// ============================================================================

using LifecycleCallback = bool(*)();
using LifecycleShutdownCallback = void(*)();

// ============================================================================
// HSM Events for Lifecycle
// ============================================================================

enum class LifecycleHsmEvent : uint32_t {
  kLcEvtConfigure = 1,
  kLcEvtActivate = 2,
  kLcEvtDeactivate = 3,
  kLcEvtCleanup = 4,
  kLcEvtShutdown = 5,
  kLcEvtPause = 6,
  kLcEvtResume = 7,
  kLcEvtMarkDegraded = 8,
  kLcEvtClearDegraded = 9,
  kLcEvtError = 10,
  kLcEvtFatalError = 11,
  kLcEvtRecover = 12,
};

// Total HSM states in the lifecycle hierarchy
inline constexpr uint32_t kLifecycleHsmMaxStates = 16;

// ============================================================================
// LifecycleHsmContext
// ============================================================================

struct LifecycleHsmContext {
  // User callbacks
  LifecycleCallback on_configure;
  LifecycleCallback on_activate;
  LifecycleCallback on_deactivate;
  LifecycleCallback on_cleanup;
  LifecycleShutdownCallback on_shutdown;

  // Fault reporting (built-in, nullptr = disabled)
  FaultReporter fault_reporter;

  // Built-in fault indices for lifecycle transitions
  static constexpr uint16_t kFaultConfigureFailed  = 0U;
  static constexpr uint16_t kFaultActivateFailed   = 1U;
  static constexpr uint16_t kFaultDeactivateFailed = 2U;
  static constexpr uint16_t kFaultCleanupFailed    = 3U;

  // Transition result tracking (set by handlers before RequestTransition)
  LifecycleError last_error;
  bool transition_failed;

  // State machine pointer for RequestTransition from handlers
  StateMachine<LifecycleHsmContext, kLifecycleHsmMaxStates>* sm;

  // State indices (one per LifecycleDetailedState)
  int32_t idx[static_cast<uint8_t>(LifecycleDetailedState::kCount)];

  LifecycleHsmContext() noexcept
      : on_configure(nullptr), on_activate(nullptr),
        on_deactivate(nullptr), on_cleanup(nullptr),
        on_shutdown(nullptr),
        last_error(LifecycleError::kInvalidTransition),
        transition_failed(false), sm(nullptr) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(LifecycleDetailedState::kCount); ++i) {
      idx[i] = -1;
    }
  }

  int32_t I(LifecycleDetailedState s) const noexcept {
    return idx[static_cast<uint8_t>(s)];
  }
};

// ============================================================================
// HSM State Handlers (detail namespace)
// ============================================================================

namespace detail {
namespace lifecycle {

using Ctx = LifecycleHsmContext;
using DS = LifecycleDetailedState;

// --- Alive (root) ---
// Handles shutdown from any alive state (bubbles up from children).
inline TransitionResult HandleAlive(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtShutdown)) {
    if (ctx.on_shutdown != nullptr) {
      ctx.on_shutdown();
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kFinalized));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtFatalError)) {
    return ctx.sm->RequestTransition(ctx.I(DS::kFatal));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtError)) {
    return ctx.sm->RequestTransition(ctx.I(DS::kRecoverable));
  }
  return TransitionResult::kUnhandled;
}

// --- WaitingConfig (leaf under Unconfigured) ---
inline TransitionResult HandleWaitingConfig(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtConfigure)) {
    if (ctx.on_configure != nullptr && !ctx.on_configure()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultConfigureFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;  // Stay in current state
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kStandby));
  }
  return TransitionResult::kUnhandled;
}

// --- Standby (leaf under Inactive) ---
inline TransitionResult HandleStandby(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtActivate)) {
    if (ctx.on_activate != nullptr && !ctx.on_activate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultActivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kRunning));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtCleanup)) {
    if (ctx.on_cleanup != nullptr && !ctx.on_cleanup()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultCleanupFailed, 0U,
                                FaultPriority::kMedium);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kWaitingConfig));
  }
  return TransitionResult::kUnhandled;
}

// --- Paused (leaf under Inactive) ---
inline TransitionResult HandlePaused(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtResume) ||
      event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtActivate)) {
    if (ctx.on_activate != nullptr && !ctx.on_activate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultActivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kRunning));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtCleanup)) {
    if (ctx.on_cleanup != nullptr && !ctx.on_cleanup()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultCleanupFailed, 0U,
                                FaultPriority::kMedium);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kWaitingConfig));
  }
  return TransitionResult::kUnhandled;
}

// --- Running (leaf under Active) ---
inline TransitionResult HandleRunning(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtDeactivate)) {
    if (ctx.on_deactivate != nullptr && !ctx.on_deactivate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultDeactivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kStandby));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtPause)) {
    if (ctx.on_deactivate != nullptr && !ctx.on_deactivate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultDeactivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kPaused));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtMarkDegraded)) {
    return ctx.sm->RequestTransition(ctx.I(DS::kDegraded));
  }
  return TransitionResult::kUnhandled;
}

// --- Degraded (leaf under Active) ---
inline TransitionResult HandleDegraded(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtClearDegraded)) {
    return ctx.sm->RequestTransition(ctx.I(DS::kRunning));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtDeactivate)) {
    if (ctx.on_deactivate != nullptr && !ctx.on_deactivate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultDeactivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kStandby));
  }
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtPause)) {
    if (ctx.on_deactivate != nullptr && !ctx.on_deactivate()) {
      ctx.transition_failed = true;
      ctx.last_error = LifecycleError::kCallbackFailed;
      ctx.fault_reporter.Report(Ctx::kFaultDeactivateFailed, 0U,
                                FaultPriority::kHigh);
      return TransitionResult::kHandled;
    }
    return ctx.sm->RequestTransition(ctx.I(DS::kPaused));
  }
  return TransitionResult::kUnhandled;
}

// --- Starting (transitional leaf under Active) ---
inline TransitionResult HandleStarting(Ctx& /*ctx*/, const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

// --- Recoverable (leaf under Error) ---
inline TransitionResult HandleRecoverable(Ctx& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtRecover)) {
    return ctx.sm->RequestTransition(ctx.I(DS::kWaitingConfig));
  }
  return TransitionResult::kUnhandled;
}

// --- Fatal (leaf under Error) ---
// Only shutdown is allowed (handled by Alive root).
inline TransitionResult HandleFatal(Ctx& /*ctx*/, const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

// --- Finalized (terminal) ---
inline TransitionResult HandleFinalized(Ctx& ctx, const Event& event) {
  // Idempotent shutdown
  if (event.id == static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtShutdown)) {
    if (ctx.on_shutdown != nullptr) {
      ctx.on_shutdown();
    }
    return TransitionResult::kHandled;
  }
  return TransitionResult::kHandled;  // Absorb all events
}

// --- Null handlers for abstract parent states ---
inline TransitionResult HandleNoop(Ctx& /*ctx*/, const Event& /*event*/) {
  return TransitionResult::kUnhandled;
}

}  // namespace lifecycle
}  // namespace detail

// ============================================================================
// LifecycleNode<PayloadVariant>
// ============================================================================

/**
 * @brief Node with HSM-driven hierarchical lifecycle state machine.
 *
 * Provides backward-compatible state transitions (Configure/Activate/
 * Deactivate/Cleanup/Shutdown) plus rich hierarchical states (Pause/Resume/
 * MarkDegraded/Recover/Error). The node automatically shuts down on
 * destruction if not already finalized.
 *
 * @tparam PayloadVariant The variant type used by the underlying Node.
 */
template <typename PayloadVariant>
class LifecycleNode : public Node<PayloadVariant> {
  using HsmType = StateMachine<LifecycleHsmContext, kLifecycleHsmMaxStates>;
  using DS = LifecycleDetailedState;

 public:
  /**
   * @brief Construct a lifecycle node.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   */
  explicit LifecycleNode(const char* name, uint32_t id = 0) noexcept
      : Node<PayloadVariant>(name, id), ctx_(), hsm_initialized_(false) {
    InitHsm();
  }

  /**
   * @brief Destructor - triggers shutdown if not already finalized.
   */
  ~LifecycleNode() noexcept {
    if (GetDetailedState() != DS::kFinalized) {
      static_cast<void>(Trigger(LifecycleTransition::kShutdown));
    }
    if (hsm_initialized_) {
      GetHsm()->~HsmType();
      hsm_initialized_ = false;
    }
  }

  LifecycleNode(const LifecycleNode&) = delete;
  LifecycleNode& operator=(const LifecycleNode&) = delete;
  LifecycleNode(LifecycleNode&&) = delete;
  LifecycleNode& operator=(LifecycleNode&&) = delete;

  // ======================== State Query (backward-compatible) ========================

  /**
   * @brief Get the coarse-grained lifecycle state (backward-compatible).
   */
  LifecycleState GetState() const noexcept {
    return MapToCoarseState(GetDetailedState());
  }

  // ======================== Rich State Query ========================

  /**
   * @brief Get the detailed hierarchical state.
   */
  LifecycleDetailedState GetDetailedState() const noexcept {
    if (!hsm_initialized_) return DS::kFinalized;
    int32_t current = GetHsm()->CurrentState();
    // Map HSM state index back to LifecycleDetailedState
    for (uint8_t i = 0; i < static_cast<uint8_t>(DS::kCount); ++i) {
      if (ctx_.idx[i] == current) {
        return static_cast<DS>(i);
      }
    }
    return DS::kFinalized;
  }

  /**
   * @brief Get the HSM state name string.
   */
  const char* DetailedStateName() const noexcept {
    if (!hsm_initialized_) return "Finalized";
    return GetHsm()->CurrentStateName();
  }

  /**
   * @brief Check if currently in a given detailed state or any descendant.
   */
  bool IsInDetailedState(LifecycleDetailedState state) const noexcept {
    if (!hsm_initialized_) return state == DS::kFinalized;
    return GetHsm()->IsInState(ctx_.I(state));
  }

  // ======================== Callback Registration ========================

  void SetOnConfigure(LifecycleCallback cb) noexcept { ctx_.on_configure = cb; }
  void SetOnActivate(LifecycleCallback cb) noexcept { ctx_.on_activate = cb; }
  void SetOnDeactivate(LifecycleCallback cb) noexcept { ctx_.on_deactivate = cb; }
  void SetOnCleanup(LifecycleCallback cb) noexcept { ctx_.on_cleanup = cb; }
  void SetOnShutdown(LifecycleShutdownCallback cb) noexcept { ctx_.on_shutdown = cb; }

  /// @brief Wire fault reporter for automatic transition failure reporting.
  /// @param reporter FaultReporter with fn + ctx (nullptr fn = disabled).
  void SetFaultReporter(FaultReporter reporter) noexcept {
    ctx_.fault_reporter = reporter;
  }

  // ======================== State Transitions (backward-compatible) ========================

  /**
   * @brief Trigger a lifecycle state transition.
   */
  expected<void, LifecycleError> Trigger(
      LifecycleTransition transition) noexcept {
    DS current = GetDetailedState();

    // Check if already finalized (except for shutdown, which is idempotent)
    if (current == DS::kFinalized &&
        transition != LifecycleTransition::kShutdown) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kAlreadyFinalized);
    }

    // Error states block all transitions except shutdown
    if (IsInDetailedState(DS::kError) &&
        transition != LifecycleTransition::kShutdown) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }

    // Validate transition is allowed from current coarse state
    LifecycleState coarse = MapToCoarseState(current);
    uint32_t evt_id = 0;

    switch (transition) {
      case LifecycleTransition::kConfigure:
        if (coarse != LifecycleState::kUnconfigured) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        evt_id = static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtConfigure);
        break;
      case LifecycleTransition::kActivate:
        if (coarse != LifecycleState::kInactive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        evt_id = static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtActivate);
        break;
      case LifecycleTransition::kDeactivate:
        if (coarse != LifecycleState::kActive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        evt_id = static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtDeactivate);
        break;
      case LifecycleTransition::kCleanup:
        if (coarse != LifecycleState::kInactive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        evt_id = static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtCleanup);
        break;
      case LifecycleTransition::kShutdown:
        evt_id = static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtShutdown);
        break;
    }

    return DispatchAndCheck(evt_id);
  }

  // ======================== Convenience Methods (backward-compatible) ========================

  expected<void, LifecycleError> Configure() noexcept {
    return Trigger(LifecycleTransition::kConfigure);
  }
  expected<void, LifecycleError> Activate() noexcept {
    return Trigger(LifecycleTransition::kActivate);
  }
  expected<void, LifecycleError> Deactivate() noexcept {
    return Trigger(LifecycleTransition::kDeactivate);
  }
  expected<void, LifecycleError> Cleanup() noexcept {
    return Trigger(LifecycleTransition::kCleanup);
  }
  expected<void, LifecycleError> Shutdown() noexcept {
    return Trigger(LifecycleTransition::kShutdown);
  }

  // ======================== Rich Transitions (new API) ========================

  /**
   * @brief Pause: Active/Running -> Inactive/Paused (calls on_deactivate).
   */
  expected<void, LifecycleError> Pause() noexcept {
    if (!IsInDetailedState(DS::kActive)) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtPause));
  }

  /**
   * @brief Resume: Inactive/Paused -> Active/Running (calls on_activate).
   */
  expected<void, LifecycleError> Resume() noexcept {
    DS current = GetDetailedState();
    if (current != DS::kPaused) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtResume));
  }

  /**
   * @brief Mark the active node as degraded.
   */
  expected<void, LifecycleError> MarkDegraded() noexcept {
    DS current = GetDetailedState();
    if (current != DS::kRunning) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtMarkDegraded));
  }

  /**
   * @brief Clear degraded status, return to Running.
   */
  expected<void, LifecycleError> ClearDegraded() noexcept {
    DS current = GetDetailedState();
    if (current != DS::kDegraded) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtClearDegraded));
  }

  /**
   * @brief Trigger a recoverable error from any alive state.
   */
  expected<void, LifecycleError> TriggerError() noexcept {
    DS current = GetDetailedState();
    if (current == DS::kFinalized || IsInDetailedState(DS::kError)) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtError));
  }

  /**
   * @brief Trigger a fatal error from any alive state.
   */
  expected<void, LifecycleError> TriggerFatalError() noexcept {
    DS current = GetDetailedState();
    if (current == DS::kFinalized || IsInDetailedState(DS::kError)) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtFatalError));
  }

  /**
   * @brief Recover from Recoverable error -> WaitingConfig.
   */
  expected<void, LifecycleError> Recover() noexcept {
    DS current = GetDetailedState();
    if (current != DS::kRecoverable) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kInvalidTransition);
    }
    return DispatchAndCheck(static_cast<uint32_t>(LifecycleHsmEvent::kLcEvtRecover));
  }

 private:
  // ======================== HSM Storage ========================

  LifecycleHsmContext ctx_;
  bool hsm_initialized_;
  alignas(HsmType) uint8_t hsm_storage_[sizeof(HsmType)];

  HsmType* GetHsm() noexcept {
    return reinterpret_cast<HsmType*>(hsm_storage_);
  }
  const HsmType* GetHsm() const noexcept {
    return reinterpret_cast<const HsmType*>(hsm_storage_);
  }

  // ======================== HSM Initialization ========================

  void InitHsm() noexcept {
    namespace lc = detail::lifecycle;

    auto* hsm = new (hsm_storage_) HsmType(ctx_);
    hsm_initialized_ = true;
    ctx_.sm = hsm;

    // Add states matching the hierarchy.
    // Parent indices reference previously added states.
    // Alive (root, parent=-1)
    ctx_.idx[static_cast<uint8_t>(DS::kAlive)] =
        hsm->AddState({"Alive", -1, lc::HandleAlive, nullptr, nullptr, nullptr});

    int32_t alive = ctx_.I(DS::kAlive);

    // Unconfigured (parent=Alive)
    ctx_.idx[static_cast<uint8_t>(DS::kUnconfigured)] =
        hsm->AddState({"Unconfigured", alive, lc::HandleNoop, nullptr, nullptr, nullptr});

    int32_t uncfg = ctx_.I(DS::kUnconfigured);

    // Initializing (parent=Unconfigured)
    ctx_.idx[static_cast<uint8_t>(DS::kInitializing)] =
        hsm->AddState({"Initializing", uncfg, lc::HandleNoop, nullptr, nullptr, nullptr});

    // WaitingConfig (parent=Unconfigured)
    ctx_.idx[static_cast<uint8_t>(DS::kWaitingConfig)] =
        hsm->AddState({"WaitingConfig", uncfg, lc::HandleWaitingConfig, nullptr, nullptr, nullptr});

    // Configured (parent=Alive)
    ctx_.idx[static_cast<uint8_t>(DS::kConfigured)] =
        hsm->AddState({"Configured", alive, lc::HandleNoop, nullptr, nullptr, nullptr});

    int32_t configured = ctx_.I(DS::kConfigured);

    // Inactive (parent=Configured)
    ctx_.idx[static_cast<uint8_t>(DS::kInactive)] =
        hsm->AddState({"Inactive", configured, lc::HandleNoop, nullptr, nullptr, nullptr});

    int32_t inactive = ctx_.I(DS::kInactive);

    // Standby (parent=Inactive)
    ctx_.idx[static_cast<uint8_t>(DS::kStandby)] =
        hsm->AddState({"Standby", inactive, lc::HandleStandby, nullptr, nullptr, nullptr});

    // Paused (parent=Inactive)
    ctx_.idx[static_cast<uint8_t>(DS::kPaused)] =
        hsm->AddState({"Paused", inactive, lc::HandlePaused, nullptr, nullptr, nullptr});

    // Active (parent=Configured)
    ctx_.idx[static_cast<uint8_t>(DS::kActive)] =
        hsm->AddState({"Active", configured, lc::HandleNoop, nullptr, nullptr, nullptr});

    int32_t active = ctx_.I(DS::kActive);

    // Starting (parent=Active)
    ctx_.idx[static_cast<uint8_t>(DS::kStarting)] =
        hsm->AddState({"Starting", active, lc::HandleStarting, nullptr, nullptr, nullptr});

    // Running (parent=Active)
    ctx_.idx[static_cast<uint8_t>(DS::kRunning)] =
        hsm->AddState({"Running", active, lc::HandleRunning, nullptr, nullptr, nullptr});

    // Degraded (parent=Active)
    ctx_.idx[static_cast<uint8_t>(DS::kDegraded)] =
        hsm->AddState({"Degraded", active, lc::HandleDegraded, nullptr, nullptr, nullptr});

    // Error (parent=Alive)
    ctx_.idx[static_cast<uint8_t>(DS::kError)] =
        hsm->AddState({"Error", alive, lc::HandleNoop, nullptr, nullptr, nullptr});

    int32_t error = ctx_.I(DS::kError);

    // Recoverable (parent=Error)
    ctx_.idx[static_cast<uint8_t>(DS::kRecoverable)] =
        hsm->AddState({"Recoverable", error, lc::HandleRecoverable, nullptr, nullptr, nullptr});

    // Fatal (parent=Error)
    ctx_.idx[static_cast<uint8_t>(DS::kFatal)] =
        hsm->AddState({"Fatal", error, lc::HandleFatal, nullptr, nullptr, nullptr});

    // Finalized (parent=-1, independent root -- terminal)
    ctx_.idx[static_cast<uint8_t>(DS::kFinalized)] =
        hsm->AddState({"Finalized", -1, lc::HandleFinalized, nullptr, nullptr, nullptr});

    // Initial state: WaitingConfig (under Unconfigured)
    hsm->SetInitialState(ctx_.I(DS::kWaitingConfig));
    hsm->Start();
  }

  // ======================== Dispatch Helper ========================

  expected<void, LifecycleError> DispatchAndCheck(uint32_t evt_id) noexcept {
    ctx_.transition_failed = false;
    Event evt{evt_id, nullptr};
    GetHsm()->Dispatch(evt);
    if (ctx_.transition_failed) {
      return expected<void, LifecycleError>::error(ctx_.last_error);
    }
    return expected<void, LifecycleError>::success();
  }

  // ======================== State Mapping ========================

  static LifecycleState MapToCoarseState(DS detailed) noexcept {
    switch (detailed) {
      case DS::kAlive:
      case DS::kUnconfigured:
      case DS::kInitializing:
      case DS::kWaitingConfig:
        return LifecycleState::kUnconfigured;

      case DS::kConfigured:
      case DS::kInactive:
      case DS::kStandby:
      case DS::kPaused:
        return LifecycleState::kInactive;

      case DS::kActive:
      case DS::kStarting:
      case DS::kRunning:
      case DS::kDegraded:
        return LifecycleState::kActive;

      case DS::kError:
      case DS::kRecoverable:
      case DS::kFatal:
        // Error states map to kUnconfigured for backward compat
        // (node needs reconfiguration after error recovery)
        return LifecycleState::kUnconfigured;

      case DS::kFinalized:
      case DS::kCount:
      default:
        return LifecycleState::kFinalized;
    }
  }
};

}  // namespace osp

#endif  // OSP_LIFECYCLE_NODE_HPP_
