/**
 * @file hsm_table.hpp
 * @brief Table-driven hierarchical state machine (HSM).
 *
 * Static transition table form of the HSM: states and transitions are POD
 * tables supplied at construction (typically constexpr), Dispatch() looks up
 * (state, event) in the table instead of interpreting an enum+if-else chain
 * inside per-state handler functions. Coexists with the handler-based
 * osp::StateMachine in hsm.hpp and can be migrated to incrementally.
 *
 * Design:
 * - StateDef carries name, parent (hierarchy) and optional on_entry/on_exit.
 * - TransitionDef carries from, event, to, TransitionKind and an optional
 *   side-effect action. The action must NOT make transition decisions; the
 *   target state is data, not a return value.
 * - Dispatch bubbles up the parent chain: it tries the current state, then
 *   its ancestors, for a matching transition. Root with no match is the
 *   explicit reject arc (silent stay, no action, no exit/entry).
 * - LCA-based external transitions reuse the same ordering as StateMachine:
 *   exit source up to the LCA, then enter target down from the LCA.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * SPDX-License-Identifier: MIT
 */

#ifndef OSP_HSM_TABLE_HPP_
#define OSP_HSM_TABLE_HPP_

#include "osp/hsm.hpp"

#include <cstdint>

namespace osp {

/**
 * @brief Semantics of a single static transition row.
 */
enum class TransitionKind : uint8_t {
  kExternal,  // Full LCA transition: exit source, enter target.
  kInternal,  // Run the action, stay in the current state (no exit/enter).
  kSelf       // Exit and re-enter the current (source) state.
};

/**
 * @brief Per-state definition (no handler; decisions live in TransitionDef).
 *
 * @tparam Context User-defined context type.
 */
template <typename Context>
struct StateDef {
  const char* name;            ///< Debug name (static lifetime).
  int32_t parent;              ///< Parent state index, -1 for root.
  void (*on_entry)(Context&);  ///< Entry action, nullptr if none.
  void (*on_exit)(Context&);   ///< Exit action, nullptr if none.
};

/**
 * @brief Per-transition definition: one row per (from, event).
 *
 * @tparam Context User-defined context type.
 */
template <typename Context>
struct TransitionDef {
  int32_t from;                                    ///< Source state index.
  uint32_t event;                                  ///< Event id.
  int32_t to;                                      ///< Target state index.
  TransitionKind kind;                             ///< kExternal / kInternal / kSelf.
  void (*action)(Context& ctx, const void* data);  ///< Side effect, may be null.
  /// Guard predicate; when non-null, the row matches only if it returns
  /// true. Rows for the same (from, event) are scanned in order: the first
  /// row whose guard passes (or that has no guard) wins. May be null.
  bool (*guard)(Context& ctx, const void* data);
};

/**
 * @brief Table-driven hierarchical state machine.
 *
 * @tparam Context       User-context type (must outlive the HSM).
 * @tparam MaxStates     Compile-time cap on state depth/path buffers.
 * @tparam MaxTransitions Compile-time cap on the transition-table size.
 *
 * Holds pointers to caller-owned tables (no copy, no heap). The tables are
 * typically `constexpr` so the machine is fully statically specifiable.
 */
template <typename Context, uint32_t MaxStates = 16, uint32_t MaxTransitions = 64>
class TableHsm {
 public:
  /// @brief Sentinel meaning "no state / root has no parent".
  static constexpr int32_t kNoState = -1;

  /**
   * @brief Construct bound to a context and caller-owned static tables.
   */
  TableHsm(Context& ctx, const StateDef<Context>* states, uint32_t state_count,
           const TransitionDef<Context>* transitions, uint32_t trans_count) noexcept
      : ctx_(ctx),
        states_(states),
        state_count_(state_count),
        transitions_(transitions),
        trans_count_(trans_count),
        current_state_(kNoState),
        initial_state_(kNoState),
        started_(false) {}

  // Non-copyable, non-movable (holds references).
  TableHsm(const TableHsm&) = delete;
  TableHsm& operator=(const TableHsm&) = delete;

  /**
   * @brief Designate the state entered on Start().
   */
  void SetInitialState(int32_t state_index) noexcept { initial_state_ = state_index; }

  /**
   * @brief Enter the initial state, executing on_entry top-down from root.
   */
  void Start() noexcept {
    if (initial_state_ < 0) {
      return;
    }
    started_ = true;
    current_state_ = initial_state_;
    RunEntryPath(initial_state_);
  }

  /**
   * @brief Dispatch an event, bubbling up the hierarchy and applying a
   *        matching transition (or the silent reject arc at the root).
   */
  void Dispatch(const Event& event) noexcept {
    if (!started_) {
      return;
    }
    int32_t state = current_state_;
    while (state >= 0) {
      const TransitionDef<Context>* t = FindTransition(state, event.id, event.data);
      if (t != nullptr) {
        Apply(t, event.data);
        return;
      }
      state = states_[state].parent;
    }
    // Reject arc: no matching row in the whole hierarchy. Silent stay.
  }

  /**
   * @brief Imperatively move to a state (exit/enter via LCA), outside a
   *        Dispatch. Returns false when not started or the target is invalid.
   */
  bool ForceTransition(int32_t target) noexcept {
    if (!started_) {
      return false;
    }
    if (target < 0 || static_cast<uint32_t>(target) >= state_count_) {
      return false;
    }
    TransitionTo(target);
    return true;
  }

  // --- Query -------------------------------------------------------------

  /// @brief Index of the current leaf state, or kNoState before Start().
  int32_t CurrentState() const noexcept { return current_state_; }

  /// @brief Name of the current state, or "" before Start().
  const char* CurrentStateName() const noexcept {
    if (current_state_ < 0) {
      return "";
    }
    return states_[current_state_].name;
  }

  /// @brief True when in the given state or one of its descendants.
  bool IsInState(int32_t state_index) const noexcept {
    for (int32_t s = current_state_; s >= 0; s = states_[s].parent) {
      if (s == state_index) {
        return true;
      }
    }
    return false;
  }

  bool IsStarted() const noexcept { return started_; }

 private:
  const TransitionDef<Context>* FindTransition(int32_t state, uint32_t event, const void* data) const noexcept {
    for (uint32_t i = 0; i < trans_count_; ++i) {
      if (transitions_[i].from == state && transitions_[i].event == event) {
        if (transitions_[i].guard == nullptr || transitions_[i].guard(ctx_, data)) {
          return &transitions_[i];
        }
      }
    }
    return nullptr;
  }

  void Apply(const TransitionDef<Context>* t, const void* data) noexcept {
    if (t->action != nullptr) {
      t->action(ctx_, data);
    }
    if (TransitionKind::kInternal == t->kind) {
      return;
    }
    // kSelf and kExternal both move through TransitionTo; a kSelf row's `to`
    // equals its source, so TransitionTo runs the self exit+re-enter branch.
    TransitionTo(t->to);
  }

  void RunEntryPath(int32_t leaf) noexcept {
    int32_t path[MaxStates];
    uint32_t len = 0;
    for (int32_t s = leaf; s >= 0 && len < MaxStates; s = states_[s].parent) {
      path[len] = s;
      ++len;
    }
    // path is [leaf, ..., root]; run entry root->...->leaf (top-down).
    for (uint32_t i = len; i > 0; --i) {
      const int32_t idx = path[i - 1];
      if (states_[idx].on_entry != nullptr) {
        states_[idx].on_entry(ctx_);
      }
    }
  }

  void TransitionTo(int32_t target) noexcept {
    const int32_t source = current_state_;
    if (source == target) {
      // Self / reject: exit then re-enter the same state.
      if (states_[source].on_exit != nullptr) {
        states_[source].on_exit(ctx_);
      }
      if (states_[source].on_entry != nullptr) {
        states_[source].on_entry(ctx_);
      }
      return;
    }
    const int32_t lca = FindLca(source, target);

    // Exit source up to (not including) LCA, bottom-up.
    for (int32_t s = source; s >= 0 && s != lca; s = states_[s].parent) {
      if (states_[s].on_exit != nullptr) {
        states_[s].on_exit(ctx_);
      }
    }
    // Enter target down from LCA (not including LCA), top-down.
    RunEntryPathTo(target, lca);

    current_state_ = target;
  }

  void RunEntryPathTo(int32_t node, int32_t below) noexcept {
    int32_t path[MaxStates];
    uint32_t len = 0;
    for (int32_t s = node; s >= 0 && s != below && len < MaxStates; s = states_[s].parent) {
      path[len] = s;
      ++len;
    }
    for (uint32_t i = len; i > 0; --i) {
      const int32_t idx = path[i - 1];
      if (states_[idx].on_entry != nullptr) {
        states_[idx].on_entry(ctx_);
      }
    }
  }

  int32_t Depth(int32_t state) const noexcept {
    int32_t depth = 0;
    for (int32_t s = state; s >= 0; s = states_[s].parent) {
      ++depth;
    }
    return depth;
  }

  int32_t FindLca(int32_t s1, int32_t s2) const noexcept {
    int32_t d1 = Depth(s1);
    int32_t d2 = Depth(s2);
    int32_t a = s1;
    int32_t b = s2;
    while (d1 > d2) {
      a = (a >= 0) ? states_[a].parent : kNoState;
      --d1;
    }
    while (d2 > d1) {
      b = (b >= 0) ? states_[b].parent : kNoState;
      --d2;
    }
    while (a != b) {
      a = (a >= 0) ? states_[a].parent : kNoState;
      b = (b >= 0) ? states_[b].parent : kNoState;
    }
    return a;
  }

  Context& ctx_;
  const StateDef<Context>* states_;
  uint32_t state_count_;
  const TransitionDef<Context>* transitions_;
  uint32_t trans_count_;
  int32_t current_state_;
  int32_t initial_state_;
  bool started_;
};

}  // namespace osp

#endif  // OSP_HSM_TABLE_HPP_