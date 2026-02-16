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
 * @file discovery.hpp
 * @brief Node discovery mechanisms for distributed OSP systems.
 *
 * Inspired by ROS2 DDS and CyberRT topology discovery patterns.
 * Provides both static configuration-based discovery and dynamic UDP multicast
 * discovery for automatic peer detection.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_DISCOVERY_HPP_
#define OSP_DISCOVERY_HPP_

#include "osp/platform.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"

#if OSP_HAS_NETWORK

#include <cstring>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace osp {

// ============================================================================
// Discovery Error
// ============================================================================

enum class DiscoveryError : uint8_t {
  kSocketFailed,
  kBindFailed,
  kMulticastJoinFailed,
  kSendFailed,
  kAlreadyRunning,
  kNotRunning,
};

// ============================================================================
// DiscoveredNode
// ============================================================================

struct DiscoveredNode {
  FixedString<63> name;     // Node name
  FixedString<63> address;  // IP address
  uint16_t port;            // Service port
  uint64_t last_seen_us;    // Last heartbeat time (microseconds)
  bool alive;               // Is alive

  DiscoveredNode() noexcept : name(), address(), port(0), last_seen_us(0), alive(false) {}
};

// ============================================================================
// TopicInfo - Topic advertisement information
// ============================================================================

struct TopicInfo {
  FixedString<63> name;       // Topic name
  FixedString<63> type_name;  // Type name (e.g. "SensorData")
  uint16_t publisher_port;
  bool is_publisher;  // true=publisher, false=subscriber

  TopicInfo() noexcept : name(), type_name(), publisher_port(0), is_publisher(false) {}
};

// ============================================================================
// ServiceInfo - Service advertisement information
// ============================================================================

struct ServiceInfo {
  FixedString<63> name;  // Service name
  FixedString<63> request_type;
  FixedString<63> response_type;
  uint16_t port;

  ServiceInfo() noexcept : name(), request_type(), response_type(), port(0) {}
};

// ============================================================================
// StaticDiscovery - Configuration-driven static endpoint table
// ============================================================================

template <uint32_t MaxNodes = 32>
class StaticDiscovery {
 public:
  StaticDiscovery() noexcept : count_(0) {}

  /**
   * @brief Add a known node to the static table.
   * @param name Node name (truncated to 63 chars).
   * @param address IP address (truncated to 63 chars).
   * @param port Service port.
   * @return Success or DiscoveryError.
   */
  expected<void, DiscoveryError> AddNode(const char* name, const char* address, uint16_t port) noexcept {
    if (count_ >= MaxNodes) {
      return expected<void, DiscoveryError>::error(DiscoveryError::kSocketFailed);
    }

    DiscoveredNode& node = nodes_[count_];
    node.name.assign(TruncateToCapacity, name);
    node.address.assign(TruncateToCapacity, address);
    node.port = port;
    node.last_seen_us = SteadyNowUs();
    node.alive = true;
    ++count_;

    return expected<void, DiscoveryError>::success();
  }

  /**
   * @brief Remove a node by name.
   * @param name Node name to remove.
   * @return true if found and removed, false otherwise.
   */
  bool RemoveNode(const char* name) noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(nodes_[i].name.c_str(), name) == 0) {
        // Shift remaining nodes down
        for (uint32_t j = i; j < count_ - 1; ++j) {
          nodes_[j] = nodes_[j + 1];
        }
        --count_;
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Find a node by name.
   * @param name Node name to search for.
   * @return Pointer to the node if found, nullptr otherwise.
   */
  const DiscoveredNode* FindNode(const char* name) const noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      if (std::strcmp(nodes_[i].name.c_str(), name) == 0) {
        return &nodes_[i];
      }
    }
    return nullptr;
  }

  /**
   * @brief Iterate over all nodes.
   * @param callback Function pointer to invoke for each node.
   * @param ctx User context pointer passed to callback.
   */
  void ForEach(void (*callback)(const DiscoveredNode&, void*), void* ctx) const noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      callback(nodes_[i], ctx);
    }
  }

  /** @brief Get the number of nodes in the table. */
  uint32_t NodeCount() const noexcept { return count_; }

 private:
  DiscoveredNode nodes_[MaxNodes];
  uint32_t count_;
};

// ============================================================================
// MulticastDiscovery - UDP multicast automatic discovery
// ============================================================================

#ifndef OSP_DISCOVERY_PORT
#define OSP_DISCOVERY_PORT 9999
#endif
#ifndef OSP_DISCOVERY_INTERVAL_MS
#define OSP_DISCOVERY_INTERVAL_MS 1000
#endif
#ifndef OSP_DISCOVERY_TIMEOUT_MS
#define OSP_DISCOVERY_TIMEOUT_MS 3000
#endif
#ifndef OSP_DISCOVERY_MULTICAST_GROUP
#define OSP_DISCOVERY_MULTICAST_GROUP "239.255.0.1"
#endif

template <uint32_t MaxNodes = 32>
class MulticastDiscovery {
 public:
  using NodeCallback = void (*)(const DiscoveredNode&, void*);

  struct Config {
    const char* multicast_group = OSP_DISCOVERY_MULTICAST_GROUP;
    uint16_t port = OSP_DISCOVERY_PORT;
    uint32_t announce_interval_ms = OSP_DISCOVERY_INTERVAL_MS;
    uint32_t timeout_ms = OSP_DISCOVERY_TIMEOUT_MS;
  };

  explicit MulticastDiscovery(const Config& cfg = {}, TimerScheduler<>* scheduler = nullptr) noexcept
      : config_(cfg),
        sockfd_(-1),
        running_(false),
        local_port_(0),
        on_node_join_(nullptr),
        on_node_leave_(nullptr),
        join_ctx_(nullptr),
        leave_ctx_(nullptr),
        node_count_(0),
        scheduler_(scheduler),
        announce_task_id_(0),
        timeout_task_id_(0) {
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      nodes_[i].alive = false;
    }
  }

  ~MulticastDiscovery() { Stop(); }

  MulticastDiscovery(const MulticastDiscovery&) = delete;
  MulticastDiscovery& operator=(const MulticastDiscovery&) = delete;
  MulticastDiscovery(MulticastDiscovery&&) = delete;
  MulticastDiscovery& operator=(MulticastDiscovery&&) = delete;

  /**
   * @brief Set local node information.
   * @param name Local node name (truncated to 63 chars).
   * @param service_port Local service port.
   */
  void SetLocalNode(const char* name, uint16_t service_port) noexcept {
    local_name_.assign(TruncateToCapacity, name);
    local_port_ = service_port;
  }

  /**
   * @brief Set callback for node join events.
   * @param cb Function pointer to invoke when a new node is discovered.
   * @param ctx User context pointer.
   */
  void SetOnNodeJoin(NodeCallback cb, void* ctx = nullptr) noexcept {
    on_node_join_ = cb;
    join_ctx_ = ctx;
  }

  /**
   * @brief Set callback for node leave events.
   * @param cb Function pointer to invoke when a node times out.
   * @param ctx User context pointer.
   */
  void SetOnNodeLeave(NodeCallback cb, void* ctx = nullptr) noexcept {
    on_node_leave_ = cb;
    leave_ctx_ = ctx;
  }

  /**
   * @brief Start discovery (spawn background threads).
   * @return Success or DiscoveryError.
   */
  expected<void, DiscoveryError> Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return expected<void, DiscoveryError>::error(DiscoveryError::kAlreadyRunning);
    }

    // Create UDP socket
    sockfd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
      return expected<void, DiscoveryError>::error(DiscoveryError::kSocketFailed);
    }

    // Set SO_REUSEADDR
    constexpr int32_t kSoReuseAddrValue = 1;
    int32_t opt = kSoReuseAddrValue;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof(opt)));

    // Bind to multicast port
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config_.port);

    // MISRA C++ Rule 5-2-4 deviation: reinterpret_cast required by POSIX
    // socket API (bind/sendto/recvfrom expect sockaddr*).
    if (::bind(sockfd_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(DiscoveryError::kBindFailed);
    }

    // Join multicast group
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, config_.multicast_group, &mreq.imr_multiaddr) != 1) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(DiscoveryError::kMulticastJoinFailed);
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (::setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(DiscoveryError::kMulticastJoinFailed);
    }

    // Set socket to non-blocking for receive thread
    int32_t flags = ::fcntl(sockfd_, F_GETFL, 0);
    ::fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    running_.store(true, std::memory_order_release);

    if (scheduler_ != nullptr) {
      // Register periodic tasks with external scheduler
      auto ann_r = scheduler_->Add(config_.announce_interval_ms, AnnounceTick, this);
      if (ann_r.has_value()) {
        announce_task_id_ = ann_r.value();
      }

      // Check timeouts ~3x per timeout period
      uint32_t timeout_check_ms = config_.timeout_ms / 3U;
      if (timeout_check_ms == 0U)
        timeout_check_ms = 1U;
      auto to_r = scheduler_->Add(timeout_check_ms, TimeoutTick, this);
      if (to_r.has_value()) {
        timeout_task_id_ = to_r.value();
      }
    } else {
      // Spawn announce thread (legacy mode)
      announce_thread_ = std::thread([this]() { AnnounceLoop(); });
    }

    // Receive thread is always needed (blocking I/O)
    receive_thread_ = std::thread([this]() { ReceiveLoop(); });

    return expected<void, DiscoveryError>::success();
  }

  /**
   * @brief Stop discovery and join threads.
   */
  void Stop() noexcept {
    if (!running_.load(std::memory_order_acquire))
      return;

    running_.store(false, std::memory_order_release);

    // Close socket first to unblock recvfrom/sendto in threads
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
      }
    }

    if (scheduler_ != nullptr) {
      static_cast<void>(scheduler_->Remove(announce_task_id_));
      static_cast<void>(scheduler_->Remove(timeout_task_id_));
    } else {
      if (announce_thread_.joinable()) {
        announce_thread_.join();
      }
    }

    if (receive_thread_.joinable()) {
      receive_thread_.join();
    }
  }

  /** @brief Check if discovery is running. */
  bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

  /**
   * @brief Find a node by name.
   * @param name Node name to search for.
   * @return Pointer to the node if found and alive, nullptr otherwise.
   */
  const DiscoveredNode* FindNode(const char* name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].alive && std::strcmp(nodes_[i].name.c_str(), name) == 0) {
        return &nodes_[i];
      }
    }
    return nullptr;
  }

  /** @brief Get the number of alive nodes. */
  uint32_t NodeCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_count_;
  }

  /**
   * @brief Iterate over all alive nodes.
   * @param cb Function pointer to invoke for each node.
   * @param ctx User context pointer.
   */
  void ForEach(NodeCallback cb, void* ctx) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].alive) {
        cb(nodes_[i], ctx);
      }
    }
  }

  /** @brief Set heartbeat for external watchdog monitoring (announce thread). */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  static constexpr uint32_t kAnnounceMagic = 0x4F535044;  // "OSPD"
  static constexpr uint32_t kAnnounceSize = 4 + 64 + 2;

  static void AnnounceTick(void* ctx) noexcept {
    auto* self = static_cast<MulticastDiscovery*>(ctx);

    sockaddr_in mcast_addr{};
    mcast_addr.sin_family = AF_INET;
    ::inet_pton(AF_INET, self->config_.multicast_group, &mcast_addr.sin_addr);
    mcast_addr.sin_port = htons(self->config_.port);

    uint8_t packet[kAnnounceSize];
    std::memcpy(packet, &kAnnounceMagic, 4);
    std::memset(packet + 4, 0, 64);
    std::memcpy(packet + 4, self->local_name_.c_str(), self->local_name_.size());
    std::memcpy(packet + 68, &self->local_port_, 2);

    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (self->sockfd_ >= 0) {
        ::sendto(self->sockfd_, packet, kAnnounceSize, 0, reinterpret_cast<sockaddr*>(&mcast_addr), sizeof(mcast_addr));
      }
    }
  }

  static void TimeoutTick(void* ctx) noexcept {
    auto* self = static_cast<MulticastDiscovery*>(ctx);
    self->CheckTimeouts();
  }

  void AnnounceLoop() noexcept {
    sockaddr_in mcast_addr{};
    mcast_addr.sin_family = AF_INET;
    ::inet_pton(AF_INET, config_.multicast_group, &mcast_addr.sin_addr);
    mcast_addr.sin_port = htons(config_.port);

    while (running_.load(std::memory_order_acquire)) {
      if (heartbeat_ != nullptr) {
        heartbeat_->Beat();
      }
      // Build announce packet
      uint8_t packet[kAnnounceSize];
      std::memcpy(packet, &kAnnounceMagic, 4);
      // Copy local_name_ into packet (64 bytes, zero-padded)
      std::memset(packet + 4, 0, 64);
      std::memcpy(packet + 4, local_name_.c_str(), local_name_.size());
      std::memcpy(packet + 68, &local_port_, 2);

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sockfd_ >= 0) {
          ::sendto(sockfd_, packet, kAnnounceSize, 0, reinterpret_cast<sockaddr*>(&mcast_addr), sizeof(mcast_addr));
        }
      }

      // Sleep for announce interval
      std::this_thread::sleep_for(std::chrono::milliseconds(config_.announce_interval_ms));
    }
  }

  void ReceiveLoop() noexcept {
    uint8_t packet[kAnnounceSize];
    constexpr uint32_t kSleepIntervalMs = 100;

    while (running_.load(std::memory_order_acquire)) {
      if (heartbeat_ != nullptr) {
        heartbeat_->Beat();
      }
      sockaddr_in sender_addr{};
      socklen_t addr_len = sizeof(sender_addr);

      ssize_t n = -1;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sockfd_ >= 0) {
          n = ::recvfrom(sockfd_, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&sender_addr), &addr_len);
        }
      }

      if (n == static_cast<ssize_t>(kAnnounceSize)) {
        ProcessAnnounce(packet, sender_addr);
      }

      // Check for timeouts (only in legacy mode; scheduler handles this)
      if (scheduler_ == nullptr) {
        CheckTimeouts();
      }

      // Sleep briefly to avoid busy-wait
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepIntervalMs));
    }
  }

  void ProcessAnnounce(const uint8_t* packet, const sockaddr_in& sender) noexcept {
    // Validate magic
    uint32_t magic;
    std::memcpy(&magic, packet, 4);
    if (magic != kAnnounceMagic)
      return;

    // Extract name and port
    char name_buf[64];
    std::memcpy(name_buf, packet + 4, 64);
    name_buf[63] = '\0';  // Ensure null-terminated

    uint16_t port;
    std::memcpy(&port, packet + 68, 2);

    // Get sender IP
    char address_buf[64];
    ::inet_ntop(AF_INET, &sender.sin_addr, address_buf, sizeof(address_buf));

    uint64_t now = SteadyNowUs();

    // Collect-release-execute: update state under lock, call callback outside
    bool should_notify_join = false;
    DiscoveredNode join_node_copy;
    NodeCallback join_cb = nullptr;
    void* join_cb_ctx = nullptr;

    {
      std::lock_guard<std::mutex> lock(mutex_);

      // Find or create node entry
      uint32_t slot = MaxNodes;
      for (uint32_t i = 0; i < MaxNodes; ++i) {
        if (nodes_[i].alive && std::strcmp(nodes_[i].name.c_str(), name_buf) == 0) {
          slot = i;
          break;
        }
      }

      if (slot == MaxNodes) {
        // New node - find free slot
        for (uint32_t i = 0; i < MaxNodes; ++i) {
          if (!nodes_[i].alive) {
            slot = i;
            break;
          }
        }
      }

      if (slot == MaxNodes)
        return;  // No free slots

      bool is_new = !nodes_[slot].alive;

      nodes_[slot].name.assign(TruncateToCapacity, name_buf);
      nodes_[slot].address.assign(TruncateToCapacity, address_buf);
      nodes_[slot].port = port;
      nodes_[slot].last_seen_us = now;
      nodes_[slot].alive = true;

      if (is_new) {
        ++node_count_;
        if (on_node_join_ != nullptr) {
          should_notify_join = true;
          join_node_copy = nodes_[slot];
          join_cb = on_node_join_;
          join_cb_ctx = join_ctx_;
        }
      }
    }
    // mutex_ released

    // Execute callback outside lock — safe for callback to call discovery API
    if (should_notify_join) {
      join_cb(join_node_copy, join_cb_ctx);
    }
  }

  void CheckTimeouts() noexcept {
    uint64_t now = SteadyNowUs();
    uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

    // Collect-release-execute: collect timed-out nodes under lock,
    // invoke leave callbacks outside lock to prevent deadlock.
    DiscoveredNode leave_copies[MaxNodes];
    uint32_t leave_count = 0;
    NodeCallback leave_cb = nullptr;
    void* leave_cb_ctx = nullptr;

    {
      std::lock_guard<std::mutex> lock(mutex_);

      leave_cb = on_node_leave_;
      leave_cb_ctx = leave_ctx_;

      for (uint32_t i = 0; i < MaxNodes; ++i) {
        if (nodes_[i].alive) {
          if (now > nodes_[i].last_seen_us && (now - nodes_[i].last_seen_us) > timeout_us) {
            nodes_[i].alive = false;
            --node_count_;
            if (leave_cb != nullptr) {
              leave_copies[leave_count] = nodes_[i];
              ++leave_count;
            }
          }
        }
      }
    }
    // mutex_ released

    // Execute leave callbacks outside lock
    for (uint32_t i = 0; i < leave_count; ++i) {
      leave_cb(leave_copies[i], leave_cb_ctx);
    }
  }

  Config config_;
  int32_t sockfd_;
  std::atomic<bool> running_;
  std::thread announce_thread_;
  std::thread receive_thread_;
  ThreadHeartbeat* heartbeat_{nullptr};

  FixedString<63> local_name_;
  uint16_t local_port_;

  NodeCallback on_node_join_;
  NodeCallback on_node_leave_;
  void* join_ctx_;
  void* leave_ctx_;

  mutable std::mutex mutex_;
  DiscoveredNode nodes_[MaxNodes];
  uint32_t node_count_;

  TimerScheduler<>* scheduler_;
  TimerTaskId announce_task_id_{0};
  TimerTaskId timeout_task_id_{0};
};

// ============================================================================
// TopicAwareDiscovery - Topic and service aware discovery registry
// ============================================================================

template <uint32_t MaxNodes = 32, uint32_t MaxTopicsPerNode = 16>
class TopicAwareDiscovery {
 public:
  static constexpr uint32_t kMaxServicesPerNode = 8;

  struct DiscoveryEntry {
    DiscoveredNode node;
    TopicInfo topics[MaxTopicsPerNode];
    uint32_t topic_count;
    ServiceInfo services[kMaxServicesPerNode];
    uint32_t service_count;

    DiscoveryEntry() noexcept : node(), topics{}, topic_count(0), services{}, service_count(0) {}
  };

  TopicAwareDiscovery() noexcept : local_topic_count_(0), local_service_count_(0) {}

  /**
   * @brief Add a local topic advertisement.
   * @param topic Topic information to advertise.
   * @return Success or DiscoveryError.
   */
  expected<void, DiscoveryError> AddLocalTopic(const TopicInfo& topic) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_topic_count_ >= MaxTopicsPerNode) {
      return expected<void, DiscoveryError>::error(DiscoveryError::kSocketFailed);
    }
    local_topics_[local_topic_count_++] = topic;
    return expected<void, DiscoveryError>::success();
  }

  /**
   * @brief Add a local service advertisement.
   * @param svc Service information to advertise.
   * @return Success or DiscoveryError.
   */
  expected<void, DiscoveryError> AddLocalService(const ServiceInfo& svc) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_service_count_ >= kMaxServicesPerNode) {
      return expected<void, DiscoveryError>::error(DiscoveryError::kSocketFailed);
    }
    local_services_[local_service_count_++] = svc;
    return expected<void, DiscoveryError>::success();
  }

  /**
   * @brief Find publishers for a given topic.
   * @param topic_name Topic name to search for.
   * @param out Output array for matching topics.
   * @param max_results Maximum number of results to return.
   * @return Number of publishers found.
   */
  uint32_t FindPublishers(const char* topic_name, TopicInfo* out, uint32_t max_results) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (uint32_t i = 0; i < local_topic_count_ && count < max_results; ++i) {
      if (local_topics_[i].is_publisher && std::strcmp(local_topics_[i].name.c_str(), topic_name) == 0) {
        out[count++] = local_topics_[i];
      }
    }
    return count;
  }

  /**
   * @brief Find subscribers for a given topic.
   * @param topic_name Topic name to search for.
   * @param out Output array for matching topics.
   * @param max_results Maximum number of results to return.
   * @return Number of subscribers found.
   */
  uint32_t FindSubscribers(const char* topic_name, TopicInfo* out, uint32_t max_results) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (uint32_t i = 0; i < local_topic_count_ && count < max_results; ++i) {
      if (!local_topics_[i].is_publisher && std::strcmp(local_topics_[i].name.c_str(), topic_name) == 0) {
        out[count++] = local_topics_[i];
      }
    }
    return count;
  }

  /**
   * @brief Find a service by name.
   * @param service_name Service name to search for.
   * @return Pointer to the service if found, nullptr otherwise.
   */
  const ServiceInfo* FindService(const char* service_name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < local_service_count_; ++i) {
      if (std::strcmp(local_services_[i].name.c_str(), service_name) == 0) {
        return &local_services_[i];
      }
    }
    return nullptr;
  }

  /** @brief Get the number of local topics. */
  uint32_t TopicCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_topic_count_;
  }

  /** @brief Get the number of local services. */
  uint32_t ServiceCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_service_count_;
  }

 private:
  mutable std::mutex mutex_;
  TopicInfo local_topics_[MaxTopicsPerNode];
  uint32_t local_topic_count_;
  ServiceInfo local_services_[kMaxServicesPerNode];
  uint32_t local_service_count_;
};

}  // namespace osp

#endif  // OSP_HAS_NETWORK

#endif  // OSP_DISCOVERY_HPP_
