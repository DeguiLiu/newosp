/**
 * @file bt.hpp
 * @brief Lightweight header-only behavior tree library for embedded systems.
 *
 * Design principles:
 * - Template for type-safe user context (no void* casting)
 * - Flat array storage: all nodes pre-allocated, zero heap on tick path
 * - Index-based parent/child references (no pointers, cache-friendly)
 * - Configurable limits via macros (max children, max nodes)
 * - Compatible with -fno-exceptions -fno-rtti
 *
 * Configuration macros (define BEFORE including this header):
 * - OSP_BT_MAX_CHILDREN: Max children per composite node (default 8)
 * - OSP_BT_MAX_NODES: Max total nodes in a tree (default 32)
 *
 * Naming convention (Google C++ Style Guide):
 * - Public methods: PascalCase (e.g., Tick(), AddAction())
 * - Accessors: PascalCase (e.g., NodeCount(), Name())
 * - Members: snake_case_ with trailing underscore
 */

#ifndef OSP_BT_HPP_
#define OSP_BT_HPP_

#include "osp/platform.hpp"

#include <cstdint>

// ============================================================================
// Configuration
// ============================================================================

#ifndef OSP_BT_MAX_CHILDREN
#define OSP_BT_MAX_CHILDREN 8
#endif

#ifndef OSP_BT_MAX_NODES
#define OSP_BT_MAX_NODES 32
#endif

namespace osp {

// ============================================================================
// NodeStatus
// ============================================================================

/**
 * @brief Behavior tree node execution status.
 *
 * Small integer values optimize switch-statement jump table generation.
 */
enum class NodeStatus : uint8_t {
  kSuccess = 0,  ///< Node completed successfully
  kFailure,      ///< Node failed
  kRunning,      ///< Node still executing (async/multi-tick operation)
  kIdle          ///< Node has not been ticked yet
};

/**
 * @brief Convert NodeStatus to human-readable string.
 */
inline constexpr const char* NodeStatusToString(NodeStatus s) noexcept {
  return (s == NodeStatus::kSuccess) ? "SUCCESS"
       : (s == NodeStatus::kFailure) ? "FAILURE"
       : (s == NodeStatus::kRunning) ? "RUNNING"
       : (s == NodeStatus::kIdle)    ? "IDLE"
       : "UNKNOWN";
}

// ============================================================================
// NodeType
// ============================================================================

/**
 * @brief Behavior tree node type enumeration.
 *
 * - kAction:    Leaf node that performs an action
 * - kCondition: Leaf node that checks a condition (returns Success/Failure)
 * - kSequence:  Composite: all children must succeed (AND logic)
 * - kSelector:  Composite: first successful child wins (OR logic)
 * - kParallel:  Composite: tick all children, threshold-based success
 * - kInverter:  Decorator: inverts child result (SUCCESS <-> FAILURE)
 * - kRepeat:    Decorator: repeats child execution N times
 */
enum class NodeType : uint8_t {
  kAction = 0,
  kCondition,
  kSequence,
  kSelector,
  kParallel,
  kInverter,
  kRepeat
};

/**
 * @brief Convert NodeType to human-readable string.
 */
inline constexpr const char* NodeTypeToString(NodeType t) noexcept {
  return (t == NodeType::kAction)    ? "ACTION"
       : (t == NodeType::kCondition) ? "CONDITION"
       : (t == NodeType::kSequence)  ? "SEQUENCE"
       : (t == NodeType::kSelector)  ? "SELECTOR"
       : (t == NodeType::kParallel)  ? "PARALLEL"
       : (t == NodeType::kInverter)  ? "INVERTER"
       : (t == NodeType::kRepeat)    ? "REPEAT"
       : "UNKNOWN";
}

// ============================================================================
// BtNode - POD node stored in flat array
// ============================================================================

/**
 * @brief Behavior tree node, stored as POD in the tree's flat array.
 * @tparam Context User-defined context type for type-safe shared data.
 *
 * Uses index-based references instead of pointers for cache-friendly
 * traversal and deterministic memory layout.
 */
template <typename Context>
struct BtNode {
  /// Tick callback: returns execution status. Used by Action/Condition nodes.
  using TickFn = NodeStatus (*)(Context& ctx);

  NodeType type;                            ///< Node type
  const char* name;                         ///< Node name (static lifetime)
  TickFn tick_fn;                           ///< Tick function (leaf nodes only)
  int32_t parent_index;                     ///< Parent index (-1 for root)
  int32_t children[OSP_BT_MAX_CHILDREN];    ///< Child indices (-1 = unused)
  uint32_t child_count;                     ///< Number of active children
  uint32_t success_threshold;               ///< Parallel: required successes
  uint32_t repeat_count;                    ///< Repeat: iteration count (0 = infinite)
};

// ============================================================================
// BehaviorTree
// ============================================================================

/**
 * @brief Flat-array behavior tree with index-based node storage.
 * @tparam Context User-defined context type.
 * @tparam MaxNodes Maximum number of nodes (compile-time capacity).
 *
 * All nodes are stored in a contiguous array. Build the tree by calling
 * Add*() methods, then call SetRoot() and Tick() in your main loop.
 *
 * Typical usage:
 *   BtTestContext ctx;
 *   BehaviorTree<BtTestContext> tree(ctx, "my_tree");
 *   auto root = tree.AddSequence("root");
 *   tree.AddAction("act1", my_fn, root);
 *   tree.AddAction("act2", my_fn2, root);
 *   tree.SetRoot(root);
 *   NodeStatus s = tree.Tick();
 */
template <typename Context, uint32_t MaxNodes = OSP_BT_MAX_NODES>
class BehaviorTree final {
 public:
  /**
   * @brief Construct a behavior tree.
   * @param ctx  Reference to shared context (must outlive the tree).
   * @param name Tree name for debugging (must have static lifetime).
   */
  explicit BehaviorTree(Context& ctx, const char* name = "bt") noexcept
      : ctx_(ctx),
        name_(name),
        node_count_(0),
        root_index_(-1),
        last_status_(NodeStatus::kIdle) {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      repeat_counters_[i] = 0;
    }
  }

  // Non-copyable, non-movable (contains reference)
  BehaviorTree(const BehaviorTree&) = delete;
  BehaviorTree& operator=(const BehaviorTree&) = delete;
  BehaviorTree(BehaviorTree&&) = delete;
  BehaviorTree& operator=(BehaviorTree&&) = delete;

  // --- Build API ---

  /**
   * @brief Add an action leaf node.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddAction(const char* name, typename BtNode<Context>::TickFn fn,
                    int32_t parent = -1) {
    return AddNode(NodeType::kAction, name, fn, parent);
  }

  /**
   * @brief Add a condition leaf node.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddCondition(const char* name, typename BtNode<Context>::TickFn fn,
                       int32_t parent = -1) {
    return AddNode(NodeType::kCondition, name, fn, parent);
  }

  /**
   * @brief Add a sequence composite node.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddSequence(const char* name, int32_t parent = -1) {
    return AddNode(NodeType::kSequence, name, nullptr, parent);
  }

  /**
   * @brief Add a selector composite node.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddSelector(const char* name, int32_t parent = -1) {
    return AddNode(NodeType::kSelector, name, nullptr, parent);
  }

  /**
   * @brief Add a parallel composite node.
   * @param success_threshold Number of children that must succeed.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddParallel(const char* name, uint32_t success_threshold,
                      int32_t parent = -1) {
    int32_t idx = AddNode(NodeType::kParallel, name, nullptr, parent);
    if (idx >= 0) {
      nodes_[idx].success_threshold = success_threshold;
    }
    return idx;
  }

  /**
   * @brief Add an inverter decorator node.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddInverter(const char* name, int32_t parent = -1) {
    return AddNode(NodeType::kInverter, name, nullptr, parent);
  }

  /**
   * @brief Add a repeat decorator node.
   * @param count Repeat count (0 = infinite).
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddRepeat(const char* name, uint32_t count, int32_t parent = -1) {
    int32_t idx = AddNode(NodeType::kRepeat, name, nullptr, parent);
    if (idx >= 0) {
      nodes_[idx].repeat_count = count;
    }
    return idx;
  }

  /**
   * @brief Set the root node for Tick() traversal.
   * @param node_index Index returned by an Add*() call.
   */
  void SetRoot(int32_t node_index) {
    OSP_ASSERT(node_index >= 0 &&
               static_cast<uint32_t>(node_index) < node_count_);
    root_index_ = node_index;
  }

  // --- Runtime API ---

  /**
   * @brief Execute one tick of the behavior tree from the root.
   * @return Status of the root node after this tick.
   */
  NodeStatus Tick() {
    if (OSP_UNLIKELY(root_index_ < 0)) {
      last_status_ = NodeStatus::kFailure;
      return last_status_;
    }
    last_status_ = TickNode(root_index_);
    return last_status_;
  }

  // --- Query API ---

  /** @brief Get the number of nodes currently in the tree. */
  uint32_t NodeCount() const noexcept { return node_count_; }

  /** @brief Get the tree name. */
  const char* Name() const noexcept { return name_; }

  /** @brief Get the status from the last Tick() call. */
  NodeStatus LastStatus() const noexcept { return last_status_; }

 private:
  // --- Tick dispatch ---

  /**
   * @brief Tick a single node by index, dispatching by type.
   */
  NodeStatus TickNode(int32_t index) {
    OSP_ASSERT(index >= 0 && static_cast<uint32_t>(index) < node_count_);
    BtNode<Context>& node = nodes_[index];

    switch (node.type) {
      case NodeType::kAction:
      case NodeType::kCondition:
        OSP_ASSERT(node.tick_fn != nullptr);
        return node.tick_fn(ctx_);

      case NodeType::kSequence:
        return TickSequence(index);

      case NodeType::kSelector:
        return TickSelector(index);

      case NodeType::kParallel:
        return TickParallel(index);

      case NodeType::kInverter:
        return TickInverter(index);

      case NodeType::kRepeat:
        return TickRepeat(index);

      default:
        return NodeStatus::kFailure;
    }
  }

  /**
   * @brief Tick a sequence node.
   *
   * Tick children left-to-right. On kFailure, return kFailure immediately.
   * On kRunning, return kRunning. If all succeed, return kSuccess.
   */
  NodeStatus TickSequence(int32_t index) {
    BtNode<Context>& node = nodes_[index];

    for (uint32_t i = 0; i < node.child_count; ++i) {
      NodeStatus child_status = TickNode(node.children[i]);

      if (child_status == NodeStatus::kFailure) {
        return NodeStatus::kFailure;
      }
      if (child_status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
      }
    }

    return NodeStatus::kSuccess;
  }

  /**
   * @brief Tick a selector node.
   *
   * Tick children left-to-right. On kSuccess, return kSuccess immediately.
   * On kRunning, return kRunning. If all fail, return kFailure.
   */
  NodeStatus TickSelector(int32_t index) {
    BtNode<Context>& node = nodes_[index];

    for (uint32_t i = 0; i < node.child_count; ++i) {
      NodeStatus child_status = TickNode(node.children[i]);

      if (child_status == NodeStatus::kSuccess) {
        return NodeStatus::kSuccess;
      }
      if (child_status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
      }
    }

    return NodeStatus::kFailure;
  }

  /**
   * @brief Tick a parallel node.
   *
   * Tick ALL children. Count successes/failures.
   * If successes >= success_threshold, return kSuccess.
   * If failures > (child_count - success_threshold), return kFailure.
   * Otherwise return kRunning.
   */
  NodeStatus TickParallel(int32_t index) {
    BtNode<Context>& node = nodes_[index];

    uint32_t success_count = 0;
    uint32_t failure_count = 0;

    for (uint32_t i = 0; i < node.child_count; ++i) {
      NodeStatus child_status = TickNode(node.children[i]);

      if (child_status == NodeStatus::kSuccess) {
        ++success_count;
      } else if (child_status == NodeStatus::kFailure) {
        ++failure_count;
      }
    }

    if (success_count >= node.success_threshold) {
      return NodeStatus::kSuccess;
    }

    uint32_t max_allowed_failures =
        node.child_count - node.success_threshold;
    if (failure_count > max_allowed_failures) {
      return NodeStatus::kFailure;
    }

    return NodeStatus::kRunning;
  }

  /**
   * @brief Tick an inverter decorator node.
   *
   * Inverts child result: kSuccess <-> kFailure. kRunning passes through.
   */
  NodeStatus TickInverter(int32_t index) {
    BtNode<Context>& node = nodes_[index];
    OSP_ASSERT(node.child_count == 1);

    NodeStatus child_status = TickNode(node.children[0]);

    if (child_status == NodeStatus::kSuccess) {
      return NodeStatus::kFailure;
    }
    if (child_status == NodeStatus::kFailure) {
      return NodeStatus::kSuccess;
    }

    return child_status;  // kRunning passes through
  }

  /**
   * @brief Tick a repeat decorator node.
   *
   * Tick child up to repeat_count times. On kFailure, return kFailure
   * immediately. After repeat_count successes, return kSuccess.
   * repeat_count=0 means infinite (always return kRunning after kSuccess).
   */
  NodeStatus TickRepeat(int32_t index) {
    BtNode<Context>& node = nodes_[index];
    OSP_ASSERT(node.child_count == 1);

    NodeStatus child_status = TickNode(node.children[0]);

    if (child_status == NodeStatus::kFailure) {
      repeat_counters_[index] = 0;
      return NodeStatus::kFailure;
    }

    if (child_status == NodeStatus::kRunning) {
      return NodeStatus::kRunning;
    }

    // child_status == kSuccess
    if (node.repeat_count == 0) {
      // Infinite repeat: always running after success
      return NodeStatus::kRunning;
    }

    ++repeat_counters_[index];

    if (repeat_counters_[index] >= node.repeat_count) {
      repeat_counters_[index] = 0;
      return NodeStatus::kSuccess;
    }

    return NodeStatus::kRunning;
  }

  // --- Internal node construction ---

  /**
   * @brief Allocate a new node in the flat array.
   * @return Node index, or -1 if the tree is full.
   */
  int32_t AddNode(NodeType type, const char* name,
                  typename BtNode<Context>::TickFn fn, int32_t parent) {
    if (OSP_UNLIKELY(node_count_ >= MaxNodes)) {
      return -1;
    }

    int32_t idx = static_cast<int32_t>(node_count_);
    BtNode<Context>& node = nodes_[node_count_];
    ++node_count_;

    node.type = type;
    node.name = name;
    node.tick_fn = fn;
    node.parent_index = parent;
    node.child_count = 0;
    node.success_threshold = 0;
    node.repeat_count = 0;

    for (uint32_t i = 0; i < OSP_BT_MAX_CHILDREN; ++i) {
      node.children[i] = -1;
    }

    // Register as child of parent
    if (parent >= 0 && static_cast<uint32_t>(parent) < node_count_) {
      BtNode<Context>& parent_node = nodes_[parent];
      OSP_ASSERT(parent_node.child_count < OSP_BT_MAX_CHILDREN);
      parent_node.children[parent_node.child_count] = idx;
      ++parent_node.child_count;
    }

    return idx;
  }

  // --- Data members ---

  Context& ctx_;
  const char* name_;
  BtNode<Context> nodes_[MaxNodes];
  uint32_t node_count_;
  int32_t root_index_;
  NodeStatus last_status_;
  uint32_t repeat_counters_[MaxNodes];  ///< Per-node state for Repeat
};

}  // namespace osp

#endif  // OSP_BT_HPP_
