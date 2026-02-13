/**
 * @file lifecycle_node.hpp
 * @brief Lifecycle-managed node with state machine for controlled transitions.
 *
 * Extends Node with a state machine supporting:
 *   Unconfigured -> Inactive -> Active -> Inactive -> Unconfigured
 *   Any state -> Finalized (shutdown)
 *
 * Callbacks are function pointers (no heap allocation).
 */

#ifndef OSP_LIFECYCLE_NODE_HPP_
#define OSP_LIFECYCLE_NODE_HPP_

#include "osp/node.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

namespace osp {

// ============================================================================
// Lifecycle State
// ============================================================================

enum class LifecycleState : uint8_t {
  kUnconfigured,  // Initial state
  kInactive,      // Configured but not active
  kActive,        // Running
  kFinalized      // Terminal state
};

// ============================================================================
// Lifecycle Transition
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
// LifecycleNode<PayloadVariant>
// ============================================================================

/**
 * @brief Node with managed lifecycle state machine.
 *
 * Provides state transitions with user-defined callbacks. Callbacks are
 * function pointers (no heap allocation). The node automatically shuts down
 * on destruction if not already finalized.
 *
 * @tparam PayloadVariant The variant type used by the underlying Node.
 */
template <typename PayloadVariant>
class LifecycleNode : public Node<PayloadVariant> {
 public:
  /**
   * @brief Construct a lifecycle node.
   * @param name Node name (truncated to kNodeNameMaxLen chars).
   * @param id Unique sender ID for message tracing.
   */
  explicit LifecycleNode(const char* name, uint32_t id = 0) noexcept
      : Node<PayloadVariant>(name, id),
        state_(LifecycleState::kUnconfigured),
        on_configure_(nullptr),
        on_activate_(nullptr),
        on_deactivate_(nullptr),
        on_cleanup_(nullptr),
        on_shutdown_(nullptr) {}

  /**
   * @brief Destructor - triggers shutdown if not already finalized.
   */
  ~LifecycleNode() noexcept {
    if (state_ != LifecycleState::kFinalized) {
      (void)Trigger(LifecycleTransition::kShutdown);
    }
  }

  LifecycleNode(const LifecycleNode&) = delete;
  LifecycleNode& operator=(const LifecycleNode&) = delete;
  LifecycleNode(LifecycleNode&&) = delete;
  LifecycleNode& operator=(LifecycleNode&&) = delete;

  // ======================== State Query ========================

  /**
   * @brief Get the current lifecycle state.
   */
  LifecycleState GetState() const noexcept { return state_; }

  // ======================== Callback Registration ========================

  /**
   * @brief Register callback for Configure transition.
   * @param cb Function pointer returning true on success, false on failure.
   */
  void SetOnConfigure(LifecycleCallback cb) noexcept { on_configure_ = cb; }

  /**
   * @brief Register callback for Activate transition.
   * @param cb Function pointer returning true on success, false on failure.
   */
  void SetOnActivate(LifecycleCallback cb) noexcept { on_activate_ = cb; }

  /**
   * @brief Register callback for Deactivate transition.
   * @param cb Function pointer returning true on success, false on failure.
   */
  void SetOnDeactivate(LifecycleCallback cb) noexcept { on_deactivate_ = cb; }

  /**
   * @brief Register callback for Cleanup transition.
   * @param cb Function pointer returning true on success, false on failure.
   */
  void SetOnCleanup(LifecycleCallback cb) noexcept { on_cleanup_ = cb; }

  /**
   * @brief Register callback for Shutdown transition.
   * @param cb Function pointer (no return value, always succeeds).
   */
  void SetOnShutdown(LifecycleShutdownCallback cb) noexcept {
    on_shutdown_ = cb;
  }

  // ======================== State Transitions ========================

  /**
   * @brief Trigger a lifecycle state transition.
   *
   * Validates the transition from the current state, invokes the registered
   * callback (if any), and updates the state on success.
   *
   * @param transition The transition to trigger.
   * @return Success or error (kInvalidTransition, kCallbackFailed,
   *         kAlreadyFinalized).
   */
  expected<void, LifecycleError> Trigger(
      LifecycleTransition transition) noexcept {
    // Check if already finalized (except for shutdown, which is idempotent)
    if (state_ == LifecycleState::kFinalized &&
        transition != LifecycleTransition::kShutdown) {
      return expected<void, LifecycleError>::error(
          LifecycleError::kAlreadyFinalized);
    }

    switch (transition) {
      case LifecycleTransition::kConfigure:
        if (state_ != LifecycleState::kUnconfigured) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        if (on_configure_ != nullptr && !on_configure_()) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kCallbackFailed);
        }
        state_ = LifecycleState::kInactive;
        break;

      case LifecycleTransition::kActivate:
        if (state_ != LifecycleState::kInactive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        if (on_activate_ != nullptr && !on_activate_()) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kCallbackFailed);
        }
        state_ = LifecycleState::kActive;
        break;

      case LifecycleTransition::kDeactivate:
        if (state_ != LifecycleState::kActive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        if (on_deactivate_ != nullptr && !on_deactivate_()) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kCallbackFailed);
        }
        state_ = LifecycleState::kInactive;
        break;

      case LifecycleTransition::kCleanup:
        if (state_ != LifecycleState::kInactive) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kInvalidTransition);
        }
        if (on_cleanup_ != nullptr && !on_cleanup_()) {
          return expected<void, LifecycleError>::error(
              LifecycleError::kCallbackFailed);
        }
        state_ = LifecycleState::kUnconfigured;
        break;

      case LifecycleTransition::kShutdown:
        if (on_shutdown_ != nullptr) {
          on_shutdown_();
        }
        state_ = LifecycleState::kFinalized;
        break;
    }

    return expected<void, LifecycleError>::success();
  }

  // ======================== Convenience Methods ========================

  /**
   * @brief Trigger Configure transition (Unconfigured -> Inactive).
   */
  expected<void, LifecycleError> Configure() noexcept {
    return Trigger(LifecycleTransition::kConfigure);
  }

  /**
   * @brief Trigger Activate transition (Inactive -> Active).
   */
  expected<void, LifecycleError> Activate() noexcept {
    return Trigger(LifecycleTransition::kActivate);
  }

  /**
   * @brief Trigger Deactivate transition (Active -> Inactive).
   */
  expected<void, LifecycleError> Deactivate() noexcept {
    return Trigger(LifecycleTransition::kDeactivate);
  }

  /**
   * @brief Trigger Cleanup transition (Inactive -> Unconfigured).
   */
  expected<void, LifecycleError> Cleanup() noexcept {
    return Trigger(LifecycleTransition::kCleanup);
  }

  /**
   * @brief Trigger Shutdown transition (Any -> Finalized).
   */
  expected<void, LifecycleError> Shutdown() noexcept {
    return Trigger(LifecycleTransition::kShutdown);
  }

 private:
  LifecycleState state_;
  LifecycleCallback on_configure_;
  LifecycleCallback on_activate_;
  LifecycleCallback on_deactivate_;
  LifecycleCallback on_cleanup_;
  LifecycleShutdownCallback on_shutdown_;
};

}  // namespace osp

#endif  // OSP_LIFECYCLE_NODE_HPP_
