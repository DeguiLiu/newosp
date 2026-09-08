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
 * @file node_manager.hpp
 * @brief TCP node connection manager with heartbeat detection.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 * Manages TCP connections to remote nodes, heartbeat detection,
 * and disconnect notification callbacks.
 */

#ifndef OSP_NODE_MANAGER_HPP_
#define OSP_NODE_MANAGER_HPP_

#include "osp/event_loop.hpp"
#include "osp/platform.hpp"
#include "osp/socket.hpp"
#include "osp/thread.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"

#if OSP_HAS_NETWORK

#include <cerrno>
#include <cstring>

#include <atomic>
#include <mutex>

namespace osp {

// ============================================================================
// Configuration Constants
// ============================================================================

// ============================================================================
// NodeManagerError
// ============================================================================

enum class NodeManagerError : uint8_t {
  kTableFull,
  kConnectionFailed,
  kBindFailed,
  kNotFound,
  kAlreadyRunning,
  kNotRunning,
  kInvalidId,
};

// ============================================================================
// Heartbeat Protocol
// ============================================================================

/**
 * Heartbeat frame wire format:
 * +--------+----------+------------+
 * | magic  | node_id  | timestamp  |
 * | 4 byte | 2 byte   | 8 byte     |
 * +--------+----------+------------+
 * Total: 14 bytes
 */
inline constexpr uint32_t kHeartbeatMagic = 0x4F534842;  // "OSHB"
inline constexpr uint32_t kHeartbeatFrameSize = 14;

// ============================================================================
// NodeManagerConfig
// ============================================================================

struct NodeManagerConfig {
  uint32_t max_nodes = OSP_NODE_MANAGER_MAX_NODES;
  uint32_t heartbeat_interval_ms = 1000;
  uint32_t heartbeat_timeout_count = 3;  // Disconnect after N missed heartbeats
};

// ============================================================================
// Disconnect Callback Type
// ============================================================================

using NodeDisconnectFn = void (*)(uint16_t node_id, void* ctx);

// ============================================================================
// NodeEntry - Internal Node Storage
// ============================================================================

struct NodeEntry {
  uint16_t node_id;
  TcpSocket socket;
  TcpListener listener;
  FixedString<63> remote_host;
  uint16_t remote_port;
  uint64_t last_heartbeat_us;  // Microsecond timestamp
  bool active;
  bool is_listener;  // true = we accepted this connection

  NodeEntry() noexcept
      : node_id(0),
        socket(),
        listener(),
        remote_host(),
        remote_port(0),
        last_heartbeat_us(0),
        active(false),
        is_listener(false) {}
};

// ============================================================================
// NodeManager
// ============================================================================

template <uint32_t MaxNodes = OSP_NODE_MANAGER_MAX_NODES>
class NodeManager : public EventLoop<NodeManager<MaxNodes>, MaxNodes + 1U, 2U> {
 public:
  explicit NodeManager(const NodeManagerConfig& cfg = {}, TimerScheduler<>* scheduler = nullptr) noexcept
      : EventLoop<NodeManager<MaxNodes>, MaxNodes + 1U, 2U>(),
        config_(cfg),
        running_(false),
        next_node_id_(1),
        node_count_(0),
        scheduler_(scheduler),
        timer_task_id_(0),
        timer_id_(0),
        disconnect_fn_(nullptr),
        disconnect_ctx_(nullptr) {}

  ~NodeManager() { Stop(); }

  NodeManager(const NodeManager&) = delete;
  NodeManager& operator=(const NodeManager&) = delete;

  // ==========================================================================
  // Connection Management
  // ==========================================================================

  /**
   * @brief Create a TCP listener on the given port.
   * @param port TCP port to bind to (0 for OS-assigned).
   * @return The assigned node_id for the listener, or NodeManagerError.
   */
  expected<uint16_t, NodeManagerError> CreateListener(uint16_t port) noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);

    NodeEntry* slot = FindSlot();
    if (slot == nullptr) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kTableFull);
    }

    auto listener_r = TcpListener::Create();
    if (!listener_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kBindFailed);
    }

    // Set SO_REUSEADDR
    int32_t opt = 1;
    (void)socket_api::SetSockOpt(listener_r.value().Fd(), SOL_SOCKET, SO_REUSEADDR, &opt,
                                 static_cast<socklen_t>(sizeof(opt)));

    auto addr_r = SocketAddress::FromIpv4("0.0.0.0", port);
    if (!addr_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kBindFailed);
    }

    auto bind_r = listener_r.value().Bind(addr_r.value());
    if (!bind_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kBindFailed);
    }

    auto listen_r = listener_r.value().Listen(8);
    if (!listen_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kBindFailed);
    }

    // Store listener in the node entry
    slot->node_id = AllocNodeId();
    slot->listener = static_cast<TcpListener&&>(listener_r.value());
    slot->remote_host = "0.0.0.0";
    slot->remote_port = port;
    slot->last_heartbeat_us = SteadyNowUs();
    slot->active = true;
    slot->is_listener = true;
    ++node_count_;

    return expected<uint16_t, NodeManagerError>::success(slot->node_id);
  }

  /**
   * @brief Connect to a remote node.
   * @param host Remote host address (e.g. "127.0.0.1").
   * @param port Remote port number.
   * @return The assigned node_id for the connection, or NodeManagerError.
   */
  expected<uint16_t, NodeManagerError> Connect(const char* host, uint16_t port) noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);

    NodeEntry* slot = FindSlot();
    if (slot == nullptr) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kTableFull);
    }

    auto sock_r = TcpSocket::Create();
    if (!sock_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kConnectionFailed);
    }
    TcpSocket sock = static_cast<TcpSocket&&>(sock_r.value());

    auto addr_r = SocketAddress::FromIpv4(host, port);
    if (!addr_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kConnectionFailed);
    }

    auto conn_r = sock.Connect(addr_r.value());
    if (!conn_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(NodeManagerError::kConnectionFailed);
    }

    // Disable Nagle's algorithm for low-latency heartbeats
    static_cast<void>(sock.SetNoDelay(true));
    // Non-blocking: OnFd recv must never block the loop thread.
    static_cast<void>(sock.SetNonBlocking(true));

    slot->node_id = AllocNodeId();
    slot->socket = static_cast<TcpSocket&&>(sock);
    slot->remote_host.assign(TruncateToCapacity, host);
    slot->remote_port = port;
    slot->last_heartbeat_us = SteadyNowUs();
    slot->active = true;
    slot->is_listener = false;
    ++node_count_;

    // Register the socket for event-driven disconnect detection (ev_io).
    static_cast<void>(this->AddFd(slot->socket.Fd(), static_cast<uint8_t>(IoEvent::kReadable),
                                  static_cast<uintptr_t>(slot->node_id)));

    return expected<uint16_t, NodeManagerError>::success(slot->node_id);
  }

  /**
   * @brief Disconnect a node.
   * @param node_id The node identifier to disconnect.
   * @return Success or NodeManagerError::kNotFound.
   */
  expected<void, NodeManagerError> Disconnect(uint16_t node_id) noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr) {
      return expected<void, NodeManagerError>::error(NodeManagerError::kNotFound);
    }

    if (node->is_listener) {
      node->listener.Close();
    } else {
      static_cast<void>(this->RemoveFd(node->socket.Fd()));
      node->socket.Close();
    }
    node->active = false;
    --node_count_;

    return expected<void, NodeManagerError>::success();
  }

  // ==========================================================================
  // Callback Registration
  // ==========================================================================

  /**
   * @brief Register a disconnect callback.
   * @param fn Callback function pointer.
   * @param ctx User context pointer passed to the callback.
   */
  void OnDisconnect(NodeDisconnectFn fn, void* ctx = nullptr) noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    disconnect_fn_ = fn;
    disconnect_ctx_ = ctx;
  }

  // ==========================================================================
  // Query
  // ==========================================================================

  /**
   * @brief Check if a node is connected.
   * @param node_id The node identifier to check.
   * @return true if the node is active, false otherwise.
   */
  bool IsConnected(uint16_t node_id) const noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    const NodeEntry* node = const_cast<NodeManager*>(this)->FindNode(node_id);
    return node != nullptr && node->active;
  }

  /**
   * @brief Get the number of active nodes.
   * @return The count of active nodes.
   */
  uint32_t NodeCount() const noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    return node_count_;
  }

  // ==========================================================================
  // Heartbeat Thread Control
  // ==========================================================================

  /**
   * @brief Start the heartbeat thread.
   * @return Success or NodeManagerError::kAlreadyRunning.
   */
  expected<void, NodeManagerError> Start() noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);

    if (running_.load()) {
      return expected<void, NodeManagerError>::error(NodeManagerError::kAlreadyRunning);
    }

    running_.store(true);

    if (scheduler_ != nullptr) {
      auto r = scheduler_->Add(config_.heartbeat_interval_ms, HeartbeatTick, this);
      if (r.has_value()) {
        timer_task_id_ = r.value();
      } else {
        running_.store(false);
        return expected<void, NodeManagerError>::error(NodeManagerError::kNotRunning);
      }
    } else {
      // EventLoop-driven heartbeat: a periodic timer wakes the unified loop;
      // Run() blocks on the poller with timeout = next heartbeat deadline.
      auto r = this->Schedule(config_.heartbeat_interval_ms);
      if (!r.has_value()) {
        running_.store(false);
        return expected<void, NodeManagerError>::error(NodeManagerError::kNotRunning);
      }
      timer_id_ = r.value();
      this->ClearStop();
      if (!run_thread_.Start(ThreadOptions{"nm-loop"}, [this]() { this->Run(); })) {
        running_.store(false);
        return expected<void, NodeManagerError>::error(NodeManagerError::kNotRunning);
      }
    }

    return expected<void, NodeManagerError>::success();
  }

  /**
   * @brief Stop the heartbeat thread.
   */
  void Stop() noexcept {
    running_.store(false);

    if (scheduler_ != nullptr) {
      static_cast<void>(scheduler_->Remove(timer_task_id_));
    } else {
      EventLoop<NodeManager<MaxNodes>, MaxNodes + 1U, 2U>::Stop();
      if (run_thread_.joinable()) {
        run_thread_.join();
      }
      static_cast<void>(this->Cancel(timer_id_));
    }

    std::lock_guard<osp::Mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active) {
        if (nodes_[i].is_listener) {
          nodes_[i].listener.Close();
        } else {
          nodes_[i].socket.Close();
        }
        nodes_[i].active = false;
      }
    }
    node_count_ = 0;
  }

  /**
   * @brief Check if the heartbeat thread is running.
   * @return true if running, false otherwise.
   */
  bool IsRunning() const noexcept { return running_.load(); }

  // ==========================================================================
  // Iteration
  // ==========================================================================

  /**
   * @brief Iterate over all active nodes.
   * @tparam Fn Callable with signature void(const NodeEntry&).
   * @param fn The callable to invoke for each active node.
   */
  template <typename Fn>
  void ForEach(Fn&& fn) const noexcept {
    std::lock_guard<osp::Mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active) {
        fn(nodes_[i]);
      }
    }
  }

  /** @brief Set heartbeat for external watchdog monitoring. */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  NodeManagerConfig config_;
  std::atomic<bool> running_;
  uint16_t next_node_id_;
  uint32_t node_count_;
  NodeEntry nodes_[MaxNodes];
  mutable osp::Mutex mutex_;
  osp::Thread run_thread_;
  ThreadHeartbeat* heartbeat_{nullptr};

  TimerScheduler<>* scheduler_;
  TimerTaskId timer_task_id_{0};
  uint32_t timer_id_{0};

  NodeDisconnectFn disconnect_fn_;
  void* disconnect_ctx_;

  // ==========================================================================
  // Heartbeat Implementation
  // ==========================================================================

 public:
  // EventLoop hooks (CRTP): OnTimer fires on each heartbeat tick; OnFd
  // handles event-driven disconnect detection on registered node sockets.
  void OnTimer(uint32_t timer_id) noexcept {
    (void)timer_id;
    HeartbeatOnce();
  }

  void OnFd(int32_t fd, uint8_t events, uintptr_t user_data) noexcept {
    const uint16_t node_id = static_cast<uint16_t>(user_data);
    if (0 == (events & static_cast<uint8_t>(IoEvent::kReadable))) {
      return;
    }
    // Non-blocking recv: 0 = FIN, >0 = peer alive, <0 EAGAIN = no data,
    // <0 otherwise = connection error.
    uint8_t buf[64];
    const int32_t n = socket_api::Recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      std::lock_guard<osp::Mutex> lock(mutex_);
      // Re-validate the fd so a recycled descriptor cannot touch a new node.
      NodeEntry* node = FindNode(node_id);
      if (node != nullptr && node->active && node->socket.Fd() == fd) {
        node->last_heartbeat_us = SteadyNowUs();
      }
      return;
    }
    if (0 == n) {
      DisconnectByNodeId(node_id, fd);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;  // No data, connection healthy.
    }
    DisconnectByNodeId(node_id, fd);
  }

 private:
  /// @brief Disconnect a node by stable id, re-validating the fd, then fire
  ///        the disconnect callback.
  void DisconnectByNodeId(uint16_t node_id, int32_t fd) noexcept {
    NodeDisconnectFn fn = nullptr;
    void* fn_ctx = nullptr;
    {
      std::lock_guard<osp::Mutex> lock(mutex_);
      NodeEntry* node = FindNode(node_id);
      if (node == nullptr || !node->active || node->socket.Fd() != fd) {
        return;
      }
      static_cast<void>(this->RemoveFd(fd));
      node->socket.Close();
      node->active = false;
      --node_count_;
      fn = disconnect_fn_;
      fn_ctx = disconnect_ctx_;
    }
    if (fn != nullptr) {
      fn(node_id, fn_ctx);
    }
  }

  void HeartbeatOnce() noexcept {
    if (heartbeat_ != nullptr) {
      heartbeat_->Beat();
    }

    // Snapshot heartbeat targets as (node_id, fd) under the lock, then send
    // outside it so a blocked TCP send does not hold mutex_.
    uint16_t target_ids[MaxNodes];
    int32_t target_fds[MaxNodes];
    uint32_t target_count = 0U;
    {
      std::lock_guard<osp::Mutex> lock(mutex_);
      for (uint32_t i = 0; i < MaxNodes; ++i) {
        if (nodes_[i].active && !nodes_[i].is_listener) {
          target_ids[target_count] = nodes_[i].node_id;
          target_fds[target_count] = nodes_[i].socket.Fd();
          ++target_count;
        }
      }
    }
    for (uint32_t i = 0U; i < target_count; ++i) {
      SendHeartbeatByFd(target_ids[i], target_fds[i]);
    }

    // Collect pending callbacks under lock
    uint16_t disconnected_ids[MaxNodes];
    uint32_t disconnect_count = 0U;
    NodeDisconnectFn fn = nullptr;
    void* fn_ctx = nullptr;
    {
      std::lock_guard<osp::Mutex> lock(mutex_);
      disconnect_count = CollectTimeouts(disconnected_ids);
      fn = disconnect_fn_;
      fn_ctx = disconnect_ctx_;
    }

    // Execute callbacks outside lock
    if (fn != nullptr) {
      for (uint32_t i = 0; i < disconnect_count; ++i) {
        fn(disconnected_ids[i], fn_ctx);
      }
    }
  }

  static void HeartbeatTick(void* ctx) noexcept { static_cast<NodeManager*>(ctx)->HeartbeatOnce(); }

  void SendHeartbeatByFd(uint16_t node_id, int32_t fd) noexcept {
    if (fd < 0) {
      return;
    }
    uint8_t frame[kHeartbeatFrameSize];
    uint64_t timestamp = SteadyNowUs();

    // Encode: magic(4B) + node_id(2B) + timestamp(8B)
    std::memcpy(frame + 0, &kHeartbeatMagic, 4);
    std::memcpy(frame + 4, &node_id, 2);
    std::memcpy(frame + 6, &timestamp, 8);

    const int32_t sent = socket_api::Send(fd, frame, kHeartbeatFrameSize, kSendNoSignal);
    if (sent != static_cast<int32_t>(kHeartbeatFrameSize)) {
      return;  // Send failed: leave last_heartbeat_us stale to trigger timeout.
    }
    // Write back the timestamp under the lock, re-validating that the node
    // still owns this fd so a concurrent Disconnect/Connect fd reuse is not
    // mis-written to a different node.
    std::lock_guard<osp::Mutex> lock(mutex_);
    NodeEntry* node = FindNode(node_id);
    if (node != nullptr && node->active && node->socket.Fd() == fd) {
      node->last_heartbeat_us = timestamp;
    }
  }

  /// @brief Collect timed-out nodes for deferred callback execution.
  /// @pre Must be called with mutex_ held.
  /// @param out_ids  Array to store timed-out node IDs.
  /// @return Number of timed-out nodes.
  uint32_t CollectTimeouts(uint16_t* out_ids) noexcept {
    uint32_t count = 0U;
    const uint64_t now = SteadyNowUs();
    const uint64_t timeout_us =
        static_cast<uint64_t>(config_.heartbeat_interval_ms) * config_.heartbeat_timeout_count * 1000U;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active && !nodes_[i].is_listener) {
        if ((now - nodes_[i].last_heartbeat_us) > timeout_us) {
          out_ids[count] = nodes_[i].node_id;
          ++count;
          nodes_[i].socket.Close();
          nodes_[i].active = false;
          --node_count_;
        }
      }
    }
    return count;
  }

  // ==========================================================================
  // Utilities
  // ==========================================================================

  uint16_t AllocNodeId() noexcept {
    uint16_t id = next_node_id_++;
    // Wrap around: skip 0 (invalid sentinel)
    if (0U == next_node_id_) {
      next_node_id_ = 1U;
    }
    return id;
  }

  NodeEntry* FindSlot() noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!nodes_[i].active)
        return &nodes_[i];
    }
    return nullptr;
  }

  NodeEntry* FindNode(uint16_t id) noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active && nodes_[i].node_id == id)
        return &nodes_[i];
    }
    return nullptr;
  }

};

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_NODE_MANAGER_HPP_
