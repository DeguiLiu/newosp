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

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"
#include "osp/socket.hpp"

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace osp {

// ============================================================================
// Configuration Constants
// ============================================================================

#ifndef OSP_NODE_MANAGER_MAX_NODES
#define OSP_NODE_MANAGER_MAX_NODES 64U
#endif

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
static constexpr uint32_t kHeartbeatMagic = 0x4F534842;  // "OSHB"
static constexpr uint32_t kHeartbeatFrameSize = 14;

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
  char remote_host[64];
  uint16_t remote_port;
  uint64_t last_heartbeat_us;  // Microsecond timestamp
  bool active;
  bool is_listener;  // true = we accepted this connection

  NodeEntry() noexcept
      : node_id(0), socket(), listener(), remote_host{}, remote_port(0),
        last_heartbeat_us(0), active(false), is_listener(false) {}
};

// ============================================================================
// NodeManager
// ============================================================================

template <uint32_t MaxNodes = OSP_NODE_MANAGER_MAX_NODES>
class NodeManager {
 public:
  explicit NodeManager(const NodeManagerConfig& cfg = {}) noexcept
      : config_(cfg), running_(false), next_node_id_(1),
        node_count_(0), disconnect_fn_(nullptr), disconnect_ctx_(nullptr) {}

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
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* slot = FindSlot();
    if (slot == nullptr) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kTableFull);
    }

    auto listener_r = TcpListener::Create();
    if (!listener_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kBindFailed);
    }

    // Set SO_REUSEADDR
    int opt = 1;
    ::setsockopt(listener_r.value().Fd(), SOL_SOCKET, SO_REUSEADDR, &opt,
                 static_cast<socklen_t>(sizeof(opt)));

    auto addr_r = SocketAddress::FromIpv4("0.0.0.0", port);
    if (!addr_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kBindFailed);
    }

    auto bind_r = listener_r.value().Bind(addr_r.value());
    if (!bind_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kBindFailed);
    }

    auto listen_r = listener_r.value().Listen(8);
    if (!listen_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kBindFailed);
    }

    // Store listener in the node entry
    slot->node_id = AllocNodeId();
    slot->listener = static_cast<TcpListener&&>(listener_r.value());
    std::memset(slot->remote_host, 0, sizeof(slot->remote_host));
    std::memcpy(slot->remote_host, "0.0.0.0", 7);
    slot->remote_port = port;
    slot->last_heartbeat_us = GetTimestampUs();
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
  expected<uint16_t, NodeManagerError> Connect(const char* host,
                                                uint16_t port) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* slot = FindSlot();
    if (slot == nullptr) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kTableFull);
    }

    auto sock_r = TcpSocket::Create();
    if (!sock_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kConnectionFailed);
    }
    TcpSocket sock = static_cast<TcpSocket&&>(sock_r.value());

    auto addr_r = SocketAddress::FromIpv4(host, port);
    if (!addr_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kConnectionFailed);
    }

    auto conn_r = sock.Connect(addr_r.value());
    if (!conn_r.has_value()) {
      return expected<uint16_t, NodeManagerError>::error(
          NodeManagerError::kConnectionFailed);
    }

    // Disable Nagle's algorithm for low-latency heartbeats
    (void)sock.SetNoDelay(true);

    slot->node_id = AllocNodeId();
    slot->socket = static_cast<TcpSocket&&>(sock);
    std::memset(slot->remote_host, 0, sizeof(slot->remote_host));
    if (host != nullptr) {
      uint32_t i = 0;
      for (; i < 63 && host[i] != '\0'; ++i) {
        slot->remote_host[i] = host[i];
      }
      slot->remote_host[i] = '\0';
    }
    slot->remote_port = port;
    slot->last_heartbeat_us = GetTimestampUs();
    slot->active = true;
    slot->is_listener = false;
    ++node_count_;

    return expected<uint16_t, NodeManagerError>::success(slot->node_id);
  }

  /**
   * @brief Disconnect a node.
   * @param node_id The node identifier to disconnect.
   * @return Success or NodeManagerError::kNotFound.
   */
  expected<void, NodeManagerError> Disconnect(uint16_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    NodeEntry* node = FindNode(node_id);
    if (node == nullptr) {
      return expected<void, NodeManagerError>::error(
          NodeManagerError::kNotFound);
    }

    if (node->is_listener) {
      node->listener.Close();
    } else {
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    const NodeEntry* node = const_cast<NodeManager*>(this)->FindNode(node_id);
    return node != nullptr && node->active;
  }

  /**
   * @brief Get the number of active nodes.
   * @return The count of active nodes.
   */
  uint32_t NodeCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load()) {
      return expected<void, NodeManagerError>::error(
          NodeManagerError::kAlreadyRunning);
    }

    running_.store(true);
    heartbeat_thread_ = std::thread([this]() { HeartbeatLoop(); });

    return expected<void, NodeManagerError>::success();
  }

  /**
   * @brief Stop the heartbeat thread.
   */
  void Stop() noexcept {
    running_.store(false);
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
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
  bool IsRunning() const noexcept {
    return running_.load();
  }

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
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active) {
        fn(nodes_[i]);
      }
    }
  }

 private:
  NodeManagerConfig config_;
  std::atomic<bool> running_;
  uint16_t next_node_id_;
  uint32_t node_count_;
  NodeEntry nodes_[MaxNodes];
  mutable std::mutex mutex_;
  std::thread heartbeat_thread_;

  NodeDisconnectFn disconnect_fn_;
  void* disconnect_ctx_;

  // ==========================================================================
  // Heartbeat Implementation
  // ==========================================================================

  void HeartbeatLoop() noexcept {
    while (running_.load()) {
      auto start = std::chrono::steady_clock::now();

      {
        std::lock_guard<std::mutex> lock(mutex_);

        // Send heartbeats to all active connected nodes
        for (uint32_t i = 0; i < MaxNodes; ++i) {
          if (nodes_[i].active && !nodes_[i].is_listener) {
            SendHeartbeat(nodes_[i]);
          }
        }

        // Check for timeouts
        CheckTimeouts();
      }

      // Sleep for the configured interval
      auto end = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          end - start);
      auto sleep_time = std::chrono::milliseconds(config_.heartbeat_interval_ms) - elapsed;
      if (sleep_time.count() > 0) {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }

  void SendHeartbeat(NodeEntry& node) noexcept {
    uint8_t frame[kHeartbeatFrameSize];
    uint64_t timestamp = GetTimestampUs();

    // Encode: magic(4B) + node_id(2B) + timestamp(8B)
    std::memcpy(frame + 0, &kHeartbeatMagic, 4);
    std::memcpy(frame + 4, &node.node_id, 2);
    std::memcpy(frame + 6, &timestamp, 8);

    auto r = node.socket.Send(frame, kHeartbeatFrameSize);
    // Only update timestamp if send was successful
    if (r.has_value() && r.value() == static_cast<int32_t>(kHeartbeatFrameSize)) {
      node.last_heartbeat_us = timestamp;
    }
    // If send fails, don't update timestamp - this will trigger timeout detection
  }

  void CheckTimeouts() noexcept {
    uint64_t now = GetTimestampUs();
    uint64_t timeout_us = static_cast<uint64_t>(config_.heartbeat_interval_ms) *
                          config_.heartbeat_timeout_count * 1000;

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active && !nodes_[i].is_listener) {
        if ((now - nodes_[i].last_heartbeat_us) > timeout_us) {
          // Timeout detected
          uint16_t node_id = nodes_[i].node_id;
          nodes_[i].socket.Close();
          nodes_[i].active = false;
          --node_count_;

          // Invoke disconnect callback
          if (disconnect_fn_ != nullptr) {
            disconnect_fn_(node_id, disconnect_ctx_);
          }
        }
      }
    }
  }

  // ==========================================================================
  // Utilities
  // ==========================================================================

  static uint64_t GetTimestampUs() noexcept {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());
  }

  uint16_t AllocNodeId() noexcept { return next_node_id_++; }

  NodeEntry* FindSlot() noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (!nodes_[i].active) return &nodes_[i];
    }
    return nullptr;
  }

  NodeEntry* FindNode(uint16_t id) noexcept {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].active && nodes_[i].node_id == id) return &nodes_[i];
    }
    return nullptr;
  }
};

}  // namespace osp

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#endif  // OSP_NODE_MANAGER_HPP_
