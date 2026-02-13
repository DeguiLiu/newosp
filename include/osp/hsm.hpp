/**
 * @file hsm.hpp
 * @brief Lightweight header-only hierarchical state machine (HSM) for embedded
 *        systems.
 *
 * Design principles:
 * - Zero heap allocation: all state storage is stack-based fixed arrays
 * - C++17, compatible with -fno-exceptions -fno-rtti
 * - Template for type-safe user context
 * - Function pointers (not std::function) for minimal overhead
 * - LCA-based hierarchical transitions with correct entry/exit ordering
 *
 * Naming convention (Google C++ Style Guide):
 * - Public methods: PascalCase (AddState, Dispatch, Start)
 * - Member variables: snake_case_ trailing underscore
 * - Constants: kPascalCase
 */

#ifndef OSP_HSM_HPP_
#define OSP_HSM_HPP_

#include "osp/platform.hpp"

#ifndef OSP_HSM_MAX_DEPTH
#define OSP_HSM_MAX_DEPTH 32
#endif

namespace osp {

// ============================================================================
// Event
// ============================================================================

/**
 * @brief Lightweight event type passed to state handlers.
 *
 * Events are identified by a uint32_t ID and carry an optional opaque data
 * pointer. They are value types intended to be passed by const reference.
 */
struct Event {
  uint32_t id;
  const void* data;  ///< Optional payload, nullptr if unused.
};

// ============================================================================
// TransitionResult
// ============================================================================

/**
 * @brief Return value from state handler functions.
 */
enum class TransitionResult : uint8_t {
  kHandled,    ///< Event consumed by current state.
  kUnhandled,  ///< Event not handled, bubble up to parent.
  kTransition  ///< State transition requested via RequestTransition().
};

// ============================================================================
// StateConfig
// ============================================================================

/**
 * @brief Configuration for a single state in the hierarchy.
 *
 * @tparam Context User-defined context type for type-safe data access.
 *
 * States are defined declaratively via StateConfig structs and added to the
 * StateMachine via AddState(). The parent_index field establishes the
 * hierarchy (-1 means root / no parent).
 */
template <typename Context>
struct StateConfig {
  using HandlerFn = TransitionResult (*)(Context& ctx, const Event& event);
  using EntryFn = void (*)(Context& ctx);
  using ExitFn = void (*)(Context& ctx);
  using GuardFn = bool (*)(const Context& ctx, const Event& event);

  const char* name;       ///< State name for debugging (static lifetime).
  int32_t parent_index;   ///< Index of parent state, -1 for root.
  HandlerFn handler;      ///< Event handler function.
  EntryFn on_entry;       ///< Entry action, nullptr if none.
  ExitFn on_exit;         ///< Exit action, nullptr if none.
  GuardFn guard;          ///< Guard condition, nullptr = no guard.
};

// ============================================================================
// StateMachine
// ============================================================================

/**
 * @brief Hierarchical State Machine template.
 *
 * @tparam Context   User-defined context type (must outlive the state machine).
 * @tparam MaxStates Maximum number of states (fixed array, no heap).
 *
 * Usage pattern:
 * @code
 *   struct MyCtx { int value; };
 *   MyCtx ctx{};
 *   osp::StateMachine<MyCtx, 8> sm(ctx);
 *   auto s0 = sm.AddState({...});
 *   auto s1 = sm.AddState({...});
 *   sm.SetInitialState(s0);
 *   sm.Start();
 *   sm.Dispatch({EVENT_ID, nullptr});
 * @endcode
 *
 * The state machine provides:
 * - Build phase: AddState / SetInitialState / Start
 * - Runtime: Dispatch / RequestTransition
 * - Query: CurrentState / CurrentStateName / IsInState
 *
 * Event dispatch uses hierarchical bubbling: if the current state's handler
 * returns kUnhandled, the event propagates to the parent state, continuing
 * up to the root. Transitions use the LCA (Lowest Common Ancestor) algorithm
 * to determine exit and entry paths.
 */
template <typename Context, uint32_t MaxStates = 16>
class StateMachine final {
 public:
  /// @brief Sentinel value indicating no state / root state (no parent).
  static constexpr int32_t kNoState = -1;

  /**
   * @brief Construct a state machine bound to the given context.
   * @param ctx Reference to user context (must outlive the state machine).
   */
  explicit StateMachine(Context& ctx) noexcept
      : ctx_(ctx),
        current_state_(kNoState),
        initial_state_(kNoState),
        state_count_(0),
        started_(false),
        pending_target_(kNoState) {}

  // Non-copyable
  StateMachine(const StateMachine&) = delete;
  StateMachine& operator=(const StateMachine&) = delete;

  // --- Build Phase ---

  /**
   * @brief Add a state to the machine.
   * @param config State configuration.
   * @return Index of the newly added state, or kNoState if full.
   */
  int32_t AddState(const StateConfig<Context>& config) noexcept {
    OSP_ASSERT(!started_);
    if (state_count_ >= MaxStates) {
      return kNoState;
    }
    int32_t index = static_cast<int32_t>(state_count_);
    states_[state_count_] = config;
    ++state_count_;
    return index;
  }

  /**
   * @brief Designate which state to enter when Start() is called.
   * @param state_index Index returned by AddState().
   */
  void SetInitialState(int32_t state_index) noexcept {
    OSP_ASSERT(!started_);
    OSP_ASSERT(state_index >= 0 &&
               static_cast<uint32_t>(state_index) < state_count_);
    initial_state_ = state_index;
  }

  /**
   * @brief Start the state machine.
   *
   * Enters the initial state (and all its ancestors from root downward),
   * executing on_entry actions in top-down order.
   */
  void Start() noexcept {
    OSP_ASSERT(!started_);
    OSP_ASSERT(initial_state_ >= 0);

    started_ = true;
    current_state_ = initial_state_;

    // Build entry path from root down to initial state
    int32_t path[OSP_HSM_MAX_DEPTH];
    uint32_t len = 0;
    BuildEntryPath(kNoState, initial_state_, path, len);

    // Execute entry actions top-down (path is stored bottom-up, so reverse)
    for (uint32_t i = len; i > 0; --i) {
      int32_t idx = path[i - 1];
      if (states_[idx].on_entry != nullptr) {
        states_[idx].on_entry(ctx_);
      }
    }
  }

  // --- Runtime ---

  /**
   * @brief Dispatch an event to the state machine.
   *
   * The event is first offered to the current state's handler. If the handler
   * returns kUnhandled, the event bubbles up to the parent state, and so on
   * up to the root.
   *
   * If a handler returns kTransition (via RequestTransition), the LCA-based
   * transition is executed after the handler returns.
   *
   * @param event The event to process.
   */
  void Dispatch(const Event& event) noexcept {
    OSP_ASSERT(started_);

    int32_t state = current_state_;
    while (state >= 0) {
      const auto& sc = states_[state];

      // Check guard: if guard exists and returns false, skip this state
      if (sc.guard != nullptr && !sc.guard(ctx_, event)) {
        state = sc.parent_index;
        continue;
      }

      if (sc.handler != nullptr) {
        TransitionResult result = sc.handler(ctx_, event);
        if (result == TransitionResult::kHandled) {
          return;
        }
        if (result == TransitionResult::kTransition) {
          OSP_ASSERT(pending_target_ >= 0);
          int32_t target = pending_target_;
          pending_target_ = kNoState;
          TransitionTo(target);
          return;
        }
        // kUnhandled: bubble up to parent
      }
      state = sc.parent_index;
    }
    // Event unhandled by all states in the hierarchy -- silently drop
  }

  /**
   * @brief Request a transition to the target state.
   *
   * Must be called from within a state handler. The handler should return the
   * value returned by this function (TransitionResult::kTransition).
   *
   * @param target Index of the target state.
   * @return TransitionResult::kTransition (return this from your handler).
   */
  TransitionResult RequestTransition(int32_t target) noexcept {
    pending_target_ = target;
    return TransitionResult::kTransition;
  }

  // --- Query ---

  /** @brief Get index of the current state. */
  int32_t CurrentState() const noexcept { return current_state_; }

  /** @brief Get name of the current state, or "" if not started. */
  const char* CurrentStateName() const noexcept {
    if (current_state_ < 0) {
      return "";
    }
    return states_[static_cast<uint32_t>(current_state_)].name;
  }

  /**
   * @brief Check if the machine is currently in the given state or one of
   *        its descendants.
   * @param state_index State index to check.
   * @return true if current state equals state_index or is a descendant of it.
   */
  bool IsInState(int32_t state_index) const noexcept {
    int32_t s = current_state_;
    while (s >= 0) {
      if (s == state_index) {
        return true;
      }
      s = states_[static_cast<uint32_t>(s)].parent_index;
    }
    return false;
  }

  /** @brief Check if the state machine has been started. */
  bool IsStarted() const noexcept { return started_; }

  /** @brief Get the total number of states added. */
  uint32_t StateCount() const noexcept { return state_count_; }

 private:
  /**
   * @brief Execute a full LCA-based transition from current_state_ to target.
   *
   * 1. Find LCA of current state and target.
   * 2. Exit states from current up to (not including) LCA.
   * 3. Enter states from LCA down to target.
   */
  void TransitionTo(int32_t target) noexcept {
    int32_t source = current_state_;

    // Self-transition: exit and re-enter same state
    if (source == target) {
      if (states_[static_cast<uint32_t>(source)].on_exit != nullptr) {
        states_[static_cast<uint32_t>(source)].on_exit(ctx_);
      }
      if (states_[static_cast<uint32_t>(source)].on_entry != nullptr) {
        states_[static_cast<uint32_t>(source)].on_entry(ctx_);
      }
      return;
    }

    int32_t lca = FindLCA(source, target);

    // Exit from source up to (but not including) LCA
    int32_t exit_path[OSP_HSM_MAX_DEPTH];
    uint32_t exit_len = 0;
    BuildExitPath(source, lca, exit_path, exit_len);
    for (uint32_t i = 0; i < exit_len; ++i) {
      int32_t idx = exit_path[i];
      if (states_[static_cast<uint32_t>(idx)].on_exit != nullptr) {
        states_[static_cast<uint32_t>(idx)].on_exit(ctx_);
      }
    }

    // Enter from LCA down to target
    int32_t entry_path[OSP_HSM_MAX_DEPTH];
    uint32_t entry_len = 0;
    BuildEntryPath(lca, target, entry_path, entry_len);
    for (uint32_t i = entry_len; i > 0; --i) {
      int32_t idx = entry_path[i - 1];
      if (states_[static_cast<uint32_t>(idx)].on_entry != nullptr) {
        states_[static_cast<uint32_t>(idx)].on_entry(ctx_);
      }
    }

    current_state_ = target;
  }

  /**
   * @brief Find the Lowest Common Ancestor of two states.
   *
   * Uses the depth-normalization approach:
   * 1. Compute depth of both states.
   * 2. Walk the deeper state up until both are at the same depth.
   * 3. Walk both up in lockstep until they meet.
   *
   * @return Index of the LCA state, or -1 if they share no common ancestor
   *         (both are independent roots).
   */
  int32_t FindLCA(int32_t s1, int32_t s2) const noexcept {
    // Compute depths
    int32_t d1 = Depth(s1);
    int32_t d2 = Depth(s2);

    int32_t p1 = s1;
    int32_t p2 = s2;

    // Normalize depths
    while (d1 > d2) {
      p1 = states_[static_cast<uint32_t>(p1)].parent_index;
      --d1;
    }
    while (d2 > d1) {
      p2 = states_[static_cast<uint32_t>(p2)].parent_index;
      --d2;
    }

    // Walk up until common ancestor
    while (p1 != p2) {
      p1 = (p1 >= 0) ? states_[static_cast<uint32_t>(p1)].parent_index : -1;
      p2 = (p2 >= 0) ? states_[static_cast<uint32_t>(p2)].parent_index : -1;
    }
    return p1;
  }

  /**
   * @brief Build the exit path from 'from' up to (not including) 'lca'.
   *
   * The path is stored in order from 'from' outward (bottom-up).
   */
  void BuildExitPath(int32_t from, int32_t lca,
                     int32_t* path, uint32_t& len) const noexcept {
    len = 0;
    int32_t s = from;
    while (s >= 0 && s != lca) {
      OSP_ASSERT(len < OSP_HSM_MAX_DEPTH);
      path[len] = s;
      ++len;
      s = states_[static_cast<uint32_t>(s)].parent_index;
    }
  }

  /**
   * @brief Build the entry path from 'to' up to (not including) 'from_lca'.
   *
   * The path is stored bottom-up (target first). Caller should iterate in
   * reverse to enter top-down.
   */
  void BuildEntryPath(int32_t from_lca, int32_t to,
                      int32_t* path, uint32_t& len) const noexcept {
    len = 0;
    int32_t s = to;
    while (s >= 0 && s != from_lca) {
      OSP_ASSERT(len < OSP_HSM_MAX_DEPTH);
      path[len] = s;
      ++len;
      s = states_[static_cast<uint32_t>(s)].parent_index;
    }
  }

  /** @brief Compute depth of a state in the hierarchy (0 for root). */
  int32_t Depth(int32_t state_index) const noexcept {
    int32_t depth = 0;
    int32_t s = state_index;
    while (s >= 0) {
      ++depth;
      s = states_[static_cast<uint32_t>(s)].parent_index;
    }
    return depth;
  }

  Context& ctx_;
  int32_t current_state_;
  int32_t initial_state_;
  uint32_t state_count_;
  bool started_;
  int32_t pending_target_;
  StateConfig<Context> states_[MaxStates];
};

}  // namespace osp

#endif  // OSP_HSM_HPP_
