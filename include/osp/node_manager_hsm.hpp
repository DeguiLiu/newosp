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
 * @file node_manager_hsm.hpp
 * @brief HSM-driven node connection manager with per-node state machines.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Each node connection has its own HSM managing lifecycle:
 * Connected -> Suspect -> Disconnected
 */

#ifndef OSP_NODE_MANAGER_HSM_HPP_
#define OSP_NODE_MANAGER_HSM_HPP_

#include "osp/fault_collector.hpp"
#include "osp/hsm.hpp"
#include "osp/platform.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"

#include <cstring>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace osp {

// ============================================================================
// HSM Events
// ============================================================================

enum class NodeConnectionEvent : uint32_t {
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

  // Fault reporting (built-in, nullptr = disabled)
  FaultReporter fault_reporter;

  // Built-in fault indices for node connection events
  static constexpr uint16_t kFaultHeartbeatTimeout = 0U;
  static constexpr uint16_t kFaultDisconnected = 1U;

  // State indices for RequestTransition from within handlers
  StateMachine<NodeConnectionContext, 4>* sm;
  int32_t idx_connected;
  int32_t idx_suspect;
  int32_t idx_disconnected;

  NodeConnectionContext() noexcept
      : node_id(0),
        missed_heartbeats(0),
        max_missed(3),
        last_heartbeat_us(0),
        disconnect_requested(false),
        on_disconnect(nullptr),
        disconnect_ctx(nullptr),
        fault_reporter(),
        sm(nullptr),
        idx_connected(-1),
        idx_suspect(-1),
        idx_disconnected(-1) {}
};

// ============================================================================
// HSM State Handlers
// ============================================================================

namespace detail {

// Connected state: normal operation
inline TransitionResult StateConnected(NodeConnectionContext& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatReceived)) {
    ctx.missed_heartbeats = 0;
    if (event.data != nullptr) {
      ctx.last_heartbeat_us = *static_cast<const uint64_t*>(event.data);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatTimeout)) {
    ++ctx.missed_heartbeats;
    return ctx.sm->RequestTransition(ctx.idx_suspect);
  }
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtDisconnect)) {
    ctx.disconnect_requested = true;
    return ctx.sm->RequestTransition(ctx.idx_disconnected);
  }
  return TransitionResult::kUnhandled;
}

// Suspect state: missed some heartbeats
inline TransitionResult StateSuspect(NodeConnectionContext& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatReceived)) {
    ctx.missed_heartbeats = 0;
    if (event.data != nullptr) {
      ctx.last_heartbeat_us = *static_cast<const uint64_t*>(event.data);
    }
    return ctx.sm->RequestTransition(ctx.idx_connected);
  }
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatTimeout)) {
    ++ctx.missed_heartbeats;
    if (ctx.missed_heartbeats >= ctx.max_missed) {
      ctx.fault_reporter.Report(NodeConnectionContext::kFaultHeartbeatTimeout, ctx.node_id, FaultPriority::kHigh);
      return ctx.sm->RequestTransition(ctx.idx_disconnected);
    }
    return TransitionResult::kHandled;
  }
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtDisconnect)) {
    ctx.disconnect_requested = true;
    return ctx.sm->RequestTransition(ctx.idx_disconnected);
  }
  return TransitionResult::kUnhandled;
}

// Disconnected state: connection lost
inline TransitionResult StateDisconnected(NodeConnectionContext& ctx, const Event& event) {
  if (event.id == static_cast<uint32_t>(NodeConnectionEvent::kEvtReconnect)) {
    ctx.missed_heartbeats = 0;
    ctx.disconnect_requested = false;
    return ctx.sm->RequestTransition(ctx.idx_connected);
  }
  return TransitionResult::kUnhandled;
}

// Entry action for Disconnected state
inline void OnEnterDisconnected(NodeConnectionContext& ctx) {
  ctx.fault_reporter.Report(NodeConnectionContext::kFaultDisconnected, ctx.node_id, FaultPriority::kCritical);
  if (ctx.on_disconnect != nullptr) {
    ctx.on_disconnect(ctx.node_id, ctx.disconnect_ctx);
  }
}

}  // namespace detail

// ============================================================================
// HsmNodeInfo - Diagnostic snapshot for a single node
// ============================================================================

/// POD snapshot of a managed node for diagnostic iteration.
struct HsmNodeInfo {
  uint16_t node_id;            ///< Node identifier
  const char* state_name;      ///< Current HSM state name
  uint64_t last_heartbeat_us;  ///< Last heartbeat timestamp (microseconds)
  uint32_t missed_count;       ///< Missed heartbeat count
};

// ============================================================================
// HsmNodeManager
// ============================================================================

template <uint32_t MaxNodes = 64>
class HsmNodeManager {
 public:
  explicit HsmNodeManager(TimerScheduler<>* scheduler = nullptr) noexcept
      : running_(false),
        node_count_(0),
        heartbeat_interval_ms_(1000),
        global_disconnect_fn_(nullptr),
        global_disconnect_ctx_(nullptr),
        scheduler_(scheduler),
        timer_task_id_(0) {}

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
    if (slot == nullptr)
      return false;

    // Initialize context
    slot->context.node_id = node_id;
    slot->context.missed_heartbeats = 0;
    slot->context.max_missed = max_missed;
    slot->context.last_heartbeat_us = SteadyNowUs();
    slot->context.disconnect_requested = false;
    slot->context.on_disconnect = global_disconnect_fn_;
    slot->context.disconnect_ctx = global_disconnect_ctx_;
    slot->context.fault_reporter = global_fault_reporter_;

    // Placement new to construct HSM
    auto* hsm = new (slot->hsm_storage) StateMachine<NodeConnectionContext, 4>(slot->context);
    slot->hsm_initialized = true;

    // Store SM pointer in context for handlers to call RequestTransition
    slot->context.sm = hsm;

    // Add states
    slot->state_connected = hsm->AddState({"Connected", -1, detail::StateConnected, nullptr, nullptr, nullptr});

    slot->state_suspect = hsm->AddState({"Suspect", -1, detail::StateSuspect, nullptr, nullptr, nullptr});

    slot->state_disconnected =
        hsm->AddState({"Disconnected", -1, detail::StateDisconnected, detail::OnEnterDisconnected, nullptr, nullptr});

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
    if (node == nullptr)
      return false;

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
    if (node == nullptr || !node->active)
      return;

    uint64_t timestamp = SteadyNowUs();
    Event evt{static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatReceived), &timestamp};
    node->GetHsm()->Dispatch(evt);
  }

  void CheckTimeouts() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t now = SteadyNowUs();
    uint64_t timeout_us = static_cast<uint64_t>(heartbeat_interval_ms_) * 1000;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!entries_[i].active)
        continue;

      NodeEntry& entry = entries_[i];
      if ((now - entry.context.last_heartbeat_us) > timeout_us) {
        Event evt{static_cast<uint32_t>(NodeConnectionEvent::kEvtHeartbeatTimeout), nullptr};
        entry.GetHsm()->Dispatch(evt);
      }
    }
  }

  void RequestDisconnect(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr || !node->active)
      return;

    Event evt{static_cast<uint32_t>(NodeConnectionEvent::kEvtDisconnect), nullptr};
    node->GetHsm()->Dispatch(evt);
  }

  void RequestReconnect(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr || !node->active)
      return;

    Event evt{static_cast<uint32_t>(NodeConnectionEvent::kEvtReconnect), nullptr};
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

  /// @brief Wire fault reporter for automatic heartbeat/disconnect reporting.
  /// @param reporter FaultReporter with fn + ctx (nullptr fn = disabled).
  void SetFaultReporter(FaultReporter reporter) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    global_fault_reporter_ = reporter;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (entries_[i].active) {
        entries_[i].context.fault_reporter = reporter;
      }
    }
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  const char* GetNodeState(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active)
      return "";
    return node->GetHsm()->CurrentStateName();
  }

  bool IsConnected(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active)
      return false;
    return node->GetHsm()->IsInState(node->state_connected);
  }

  uint32_t NodeCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_count_;
  }

  uint32_t GetMissedHeartbeats(uint16_t node_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<HsmNodeManager*>(this)->FindNode(node_id);
    if (node == nullptr || !node->active)
      return 0;
    return node->context.missed_heartbeats;
  }

  /// Iterate active nodes for diagnostic purposes.
  /// Callback signature: void(const HsmNodeInfo&).
  template <typename Fn>
  void ForEachNode(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0U; i < MaxNodes; ++i) {
      if (entries_[i].active && entries_[i].hsm_initialized) {
        HsmNodeInfo info{};
        info.node_id = entries_[i].context.node_id;
        info.state_name = entries_[i].GetHsm()->CurrentStateName();
        info.last_heartbeat_us = entries_[i].context.last_heartbeat_us;
        info.missed_count = entries_[i].context.missed_heartbeats;
        fn(info);
      }
    }
  }

  // ==========================================================================
  // Thread Control
  // ==========================================================================

  bool Start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load())
      return false;
    running_.store(true);

    if (scheduler_ != nullptr) {
      auto r = scheduler_->Add(heartbeat_interval_ms_, MonitorTick, this);
      if (r.has_value()) {
        timer_task_id_ = r.value();
      } else {
        running_.store(false);
        return false;
      }
    } else {
      monitor_thread_ = std::thread([this]() { MonitorLoop(); });
    }

    return true;
  }

  void Stop() noexcept {
    running_.store(false);

    if (scheduler_ != nullptr) {
      static_cast<void>(scheduler_->Remove(timer_task_id_));
    } else {
      if (monitor_thread_.joinable()) {
        monitor_thread_.join();
      }
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

  /** @brief Set heartbeat for external watchdog monitoring. */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  struct NodeEntry {
    NodeConnectionContext context;
    int32_t state_connected;
    int32_t state_suspect;
    int32_t state_disconnected;
    bool active;
    bool hsm_initialized;
    alignas(StateMachine<NodeConnectionContext, 4>) uint8_t hsm_storage[sizeof(StateMachine<NodeConnectionContext, 4>)];

    NodeEntry() noexcept
        : context(),
          state_connected(-1),
          state_suspect(-1),
          state_disconnected(-1),
          active(false),
          hsm_initialized(false) {}

    ~NodeEntry() noexcept {
      if (hsm_initialized) {
        GetHsm()->~StateMachine();
      }
    }

    StateMachine<NodeConnectionContext, 4>* GetHsm() noexcept {
      return reinterpret_cast<StateMachine<NodeConnectionContext, 4>*>(hsm_storage);
    }

    const StateMachine<NodeConnectionContext, 4>* GetHsm() const noexcept {
      return reinterpret_cast<const StateMachine<NodeConnectionContext, 4>*>(hsm_storage);
    }
  };

  NodeEntry entries_[MaxNodes];
  uint32_t node_count_;
  uint32_t heartbeat_interval_ms_;
  std::atomic<bool> running_;
  std::thread monitor_thread_;
  mutable std::mutex mutex_;
  ThreadHeartbeat* heartbeat_{nullptr};

  NodeDisconnectFn global_disconnect_fn_;
  void* global_disconnect_ctx_;
  FaultReporter global_fault_reporter_;

  TimerScheduler<>* scheduler_;
  TimerTaskId timer_task_id_{0};

  static void MonitorTick(void* ctx) noexcept {
    auto* self = static_cast<HsmNodeManager*>(ctx);
    self->CheckTimeouts();
  }

  void MonitorLoop() noexcept {
    while (running_.load()) {
      if (heartbeat_ != nullptr) {
        heartbeat_->Beat();
      }
      const uint64_t start_us = SteadyNowUs();
      CheckTimeouts();
      const uint64_t elapsed_us = SteadyNowUs() - start_us;
      const uint64_t interval_us = static_cast<uint64_t>(heartbeat_interval_ms_) * 1000U;
      if (elapsed_us < interval_us) {
        std::this_thread::sleep_for(std::chrono::microseconds(interval_us - elapsed_us));
      }
    }
  }

  NodeEntry* FindSlot() noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!entries_[i].active)
        return &entries_[i];
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
