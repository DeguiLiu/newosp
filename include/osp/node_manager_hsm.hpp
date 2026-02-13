/**
 * @file node_manager_hsm.hpp
 * @brief HSM-driven node connection manager with per-node state machines.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Each node connection has its own HSM managing lifecycle:
 * Connected -> Suspect -> Disconnected
 */

#ifndef OSP_NODE_MANAGER_HSM_HPP_
#define OSP_NODE_MANAGER_HSM_HPP_

#include "osp/hsm.hpp"
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace osp {

// ============================================================================
// HSM Events
// ============================================================================

enum NodeConnectionEvent : uint32_t {
  kEvtHeartbeatReceived = 1,
  kEvtHeartbeatTimeout = 2,
  kEvtDisconnect = 3,
  kEvtReconnect = 4,
};

// ============================================================================
// NodeConnectionContext
// ============================================================================

using NodeDisconnectFn = void (*)(uint16_t node_id, void* ctx);

struct NodeConnectionContext {
  uint16_t node_id;
  uint32_t missed_heartbeats;
  uint32_t max_missed;
  uint64_t last_heartbeat_us;
  bool disconnect_requested;
  NodeDisconnectFn on_disconnect;
  void* disconnect_ctx;

  // State indices for RequestTransition from within handlers
  StateMachine<NodeConnectionContext, 4>* sm;
  int32_t idx_connected;
  int32_t idx_suspect;
  int32_t idx_disconnected;

  NodeConnectionContext() noexcept
      : node_id(0), missed_heartbeats(0), max_missed(3),
        last_heartbeat_us(0), disconnect_requested(false),
        on_disconnect(nullptr), disconnect_ctx(nullptr),
        sm(nullptr), idx_connected(-1), idx_suspect(-1),
        idx_disconnected(-1) {}
};

// ============================================================================
// HSM State Handlers
// ============================================================================

namespace detail {

// Connected state: normal operation
inline TransitionResult StateConnected(NodeConnectionContext& ctx,
                                       const Event& event) {
  if (event.id == kEvtHeartbeatReceived) {
    ctx.missed_heartbeats = 0;
    if (event.data != nullptr) {
      ctx.last_heartbeat_us = *static_cast<const uint64_t*>(event.data);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtHeartbeatTimeout) {
    ++ctx.missed_heartbeats;
    return ctx.sm->RequestTransition(ctx.idx_suspect);
  }
  if (event.id == kEvtDisconnect) {
    ctx.disconnect_requested = true;
    return ctx.sm->RequestTransition(ctx.idx_disconnected);
  }
  return TransitionResult::kUnhandled;
}

// Suspect state: missed some heartbeats
inline TransitionResult StateSuspect(NodeConnectionContext& ctx,
                                     const Event& event) {
  if (event.id == kEvtHeartbeatReceived) {
    ctx.missed_heartbeats = 0;
    if (event.data != nullptr) {
      ctx.last_heartbeat_us = *static_cast<const uint64_t*>(event.data);
    }
    return ctx.sm->RequestTransition(ctx.idx_connected);
  }
  if (event.id == kEvtHeartbeatTimeout) {
    ++ctx.missed_heartbeats;
    if (ctx.missed_heartbeats >= ctx.max_missed) {
      return ctx.sm->RequestTransition(ctx.idx_disconnected);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == kEvtDisconnect) {
    ctx.disconnect_requested = true;
    return ctx.sm->RequestTransition(ctx.idx_disconnected);
  }
  return TransitionResult::kUnhandled;
}

// Disconnected state: connection lost
inline TransitionResult StateDisconnected(NodeConnectionContext& ctx,
                                          const Event& event) {
  if (event.id == kEvtReconnect) {
    ctx.missed_heartbeats = 0;
    ctx.disconnect_requested = false;
    return ctx.sm->RequestTransition(ctx.idx_connected);
  }
  return TransitionResult::kUnhandled;
}

// Entry action for Disconnected state
inline void OnEnterDisconnected(NodeConnectionContext& ctx) {
  if (ctx.on_disconnect != nullptr) {
    ctx.on_disconnect(ctx.node_id, ctx.disconnect_ctx);
  }
}

}  // namespace detail

// ============================================================================
// HsmNodeManager
// ============================================================================

template <uint32_t MaxNodes = 64>
class HsmNodeManager {
 public:
  HsmNodeManager() noexcept
      : running_(false), node_count_(0), heartbeat_interval_ms_(1000),
        global_disconnect_fn_(nullptr), global_disconnect_ctx_(nullptr) {}

  ~HsmNodeManager() { Stop(); }

  HsmNodeManager(const HsmNodeManager&) = delete;
  HsmNodeManager& operator=(const HsmNodeManager&) = delete;

  // ==========================================================================
  // Configuration
  // ==========================================================================

  void SetHeartbeatInterval(uint32_t interval_ms) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    heartbeat_interval_ms_ = interval_ms;
  }

  void SetMaxMissedHeartbeats(uint32_t max_missed) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (entries_[i].active) {
        entries_[i].context.max_missed = max_missed;
      }
    }
  }

  // ==========================================================================
  // Node Management
  // ==========================================================================

  bool AddNode(uint16_t node_id, uint32_t max_missed = 3) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* slot = FindSlot();
    if (slot == nullptr) return false;

    // Initialize context
    slot->context.node_id = node_id;
    slot->context.missed_heartbeats = 0;
    slot->context.max_missed = max_missed;
    slot->context.last_heartbeat_us = GetTimestampUs();
    slot->context.disconnect_requested = false;
    slot->context.on_disconnect = global_disconnect_fn_;
    slot->context.disconnect_ctx = global_disconnect_ctx_;

    // Placement new to construct HSM
    auto* hsm = new (slot->hsm_storage)
        StateMachine<NodeConnectionContext, 4>(slot->context);
    slot->hsm_initialized = true;

    // Store SM pointer in context for handlers to call RequestTransition
    slot->context.sm = hsm;

    // Add states
    slot->state_connected = hsm->AddState({
        "Connected", -1,
        detail::StateConnected,
        nullptr, nullptr, nullptr
    });

    slot->state_suspect = hsm->AddState({
        "Suspect", -1,
        detail::StateSuspect,
        nullptr, nullptr, nullptr
    });

    slot->state_disconnected = hsm->AddState({
        "Disconnected", -1,
        detail::StateDisconnected,
        detail::OnEnterDisconnected,
        nullptr, nullptr
    });

    // Store indices in context for handlers
    slot->context.idx_connected = slot->state_connected;
    slot->context.idx_suspect = slot->state_suspect;
    slot->context.idx_disconnected = slot->state_disconnected;

    hsm->SetInitialState(slot->state_connected);
    hsm->Start();

    slot->active = true;
    ++node_count_;
    return true;
  }

  bool RemoveNode(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr) return false;

    if (node->hsm_initialized) {
      node->GetHsm()->~StateMachine();
      node->hsm_initialized = false;
    }

    node->active = false;
    --node_count_;
    return true;
  }

  // ==========================================================================
  // Heartbeat Events
  // ==========================================================================

  void OnHeartbeat(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr || !node->active) return;

    uint64_t timestamp = GetTimestampUs();
    Event evt{kEvtHeartbeatReceived, &timestamp};
    node->GetHsm()->Dispatch(evt);
  }

  void CheckTimeouts() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t now = GetTimestampUs();
    uint64_t timeout_us = static_cast<uint64_t>(heartbeat_interval_ms_) * 1000;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!entries_[i].active) continue;

      NodeEntry& entry = entries_[i];
      if ((now - entry.context.last_heartbeat_us) > timeout_us) {
        Event evt{kEvtHeartbeatTimeout, nullptr};
        entry.GetHsm()->Dispatch(evt);
      }
    }
  }

  void RequestDisconnect(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr || !node->active) return;

    Event evt{kEvtDisconnect, nullptr};
    node->GetHsm()->Dispatch(evt);
  }

  void RequestReconnect(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr || !node->active) return;

    Event evt{kEvtReconnect, nullptr};
    node->GetHsm()->Dispatch(evt);
  }

  // ==========================================================================
  // Callback Registration
  // ==========================================================================

  void OnDisconnect(NodeDisconnectFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    global_disconnect_fn_ = fn;
    global_disconnect_ctx_ = ctx;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (entries_[i].active) {
        entries_[i].context.on_disconnect = fn;
        entries_[i].context.disconnect_ctx = ctx;
      }
    }
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  const char* GetNodeState(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active) return "";
    return node->GetHsm()->CurrentStateName();
  }

  bool IsConnected(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active) return false;
    return node->GetHsm()->IsInState(node->state_connected);
  }

  uint32_t NodeCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_count_;
  }

  uint32_t GetMissedHeartbeats(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active) return 0;
    return node->context.missed_heartbeats;
  }

  // ==========================================================================
  // Thread Control
  // ==========================================================================

  bool Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return false;
    running_.store(true);
    monitor_thread_ = std::thread([this]() { MonitorLoop(); });
    return true;
  }

  void Stop() noexcept {
    running_.store(false);
    if (monitor_thread_.joinable()) {
      monitor_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (entries_[i].active && entries_[i].hsm_initialized) {
        entries_[i].GetHsm()->~StateMachine();
        entries_[i].hsm_initialized = false;
      }
      entries_[i].active = false;
    }
    node_count_ = 0;
  }

  bool IsRunning() const noexcept { return running_.load(); }

 private:
  struct NodeEntry {
    NodeConnectionContext context;
    int32_t state_connected;
    int32_t state_suspect;
    int32_t state_disconnected;
    bool active;
    bool hsm_initialized;
    alignas(StateMachine<NodeConnectionContext, 4>)
        uint8_t hsm_storage[sizeof(StateMachine<NodeConnectionContext, 4>)];

    NodeEntry() noexcept
        : context(), state_connected(-1), state_suspect(-1),
          state_disconnected(-1), active(false), hsm_initialized(false) {}

    ~NodeEntry() noexcept {
      if (hsm_initialized) {
        GetHsm()->~StateMachine();
      }
    }

    StateMachine<NodeConnectionContext, 4>* GetHsm() noexcept {
      return reinterpret_cast<StateMachine<NodeConnectionContext, 4>*>(
          hsm_storage);
    }

    const StateMachine<NodeConnectionContext, 4>* GetHsm() const noexcept {
      return reinterpret_cast<
          const StateMachine<NodeConnectionContext, 4>*>(hsm_storage);
    }
  };

  NodeEntry entries_[MaxNodes];
  uint32_t node_count_;
  uint32_t heartbeat_interval_ms_;
  std::atomic<bool> running_;
  std::thread monitor_thread_;
  mutable std::mutex mutex_;

  NodeDisconnectFn global_disconnect_fn_;
  void* global_disconnect_ctx_;

  void MonitorLoop() noexcept {
    while (running_.load()) {
      auto start = std::chrono::steady_clock::now();
      CheckTimeouts();
      auto end = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          end - start);
      auto sleep_time =
          std::chrono::milliseconds(heartbeat_interval_ms_) - elapsed;
      if (sleep_time.count() > 0) {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }

  static uint64_t GetTimestampUs() noexcept {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
  }

  NodeEntry* FindSlot() noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!entries_[i].active) return &entries_[i];
    }
    return nullptr;
  }

  NodeEntry* FindNode(uint16_t node_id) noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (entries_[i].active && entries_[i].context.node_id == node_id)
        return &entries_[i];
    }
    return nullptr;
  }
};

}  // namespace osp

#endif  // OSP_NODE_MANAGER_HSM_HPP_
