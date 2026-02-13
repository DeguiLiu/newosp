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
#include "osp/vocabulary.hpp"

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

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
  char name[64];           // Node name
  char address[64];        // IP address
  uint16_t port;           // Service port
  uint64_t last_seen_us;   // Last heartbeat time (microseconds)
  bool alive;              // Is alive

  DiscoveredNode() noexcept
      : name{}, address{}, port(0), last_seen_us(0), alive(false) {}
};

// ============================================================================
// TopicInfo - Topic advertisement information
// ============================================================================

struct TopicInfo {
  char name[64];        // Topic name
  char type_name[64];   // Type name (e.g. "SensorData")
  uint16_t publisher_port;
  bool is_publisher;    // true=publisher, false=subscriber

  TopicInfo() noexcept
      : name{}, type_name{}, publisher_port(0), is_publisher(false) {}
};

// ============================================================================
// ServiceInfo - Service advertisement information
// ============================================================================

struct ServiceInfo {
  char name[64];        // Service name
  char request_type[64];
  char response_type[64];
  uint16_t port;

  ServiceInfo() noexcept
      : name{}, request_type{}, response_type{}, port(0) {}
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
  expected<void, DiscoveryError> AddNode(const char* name, const char* address,
                                          uint16_t port) noexcept {
    if (count_ >= MaxNodes) {
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kSocketFailed);
    }

    DiscoveredNode& node = nodes_[count_];
    CopyString(node.name, name, 63);
    CopyString(node.address, address, 63);
    node.port = port;
    node.last_seen_us = GetTimestampUs();
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
      if (std::strcmp(nodes_[i].name, name) == 0) {
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
      if (std::strcmp(nodes_[i].name, name) == 0) {
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
  void ForEach(void (*callback)(const DiscoveredNode&, void*),
               void* ctx) const noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      callback(nodes_[i], ctx);
    }
  }

  /** @brief Get the number of nodes in the table. */
  uint32_t NodeCount() const noexcept { return count_; }

 private:
  static void CopyString(char* dst, const char* src, uint32_t max_len) noexcept {
    if (src == nullptr) {
      dst[0] = '\0';
      return;
    }
    uint32_t i = 0;
    for (; i < max_len && src[i] != '\0'; ++i) {
      dst[i] = src[i];
    }
    dst[i] = '\0';
  }

  static uint64_t GetTimestampUs() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(us.count());
  }

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

  explicit MulticastDiscovery(const Config& cfg = {}) noexcept
      : config_(cfg),
        sockfd_(-1),
        running_(false),
        local_port_(0),
        on_node_join_(nullptr),
        on_node_leave_(nullptr),
        join_ctx_(nullptr),
        leave_ctx_(nullptr),
        node_count_(0) {
    local_name_[0] = '\0';
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
    CopyString(local_name_, name, 63);
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
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kAlreadyRunning);
    }

    // Create UDP socket
    sockfd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kSocketFailed);
    }

    // Set SO_REUSEADDR
    int opt = 1;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                 static_cast<socklen_t>(sizeof(opt)));

    // Bind to multicast port
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config_.port);

    if (::bind(sockfd_, reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(DiscoveryError::kBindFailed);
    }

    // Join multicast group
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, config_.multicast_group, &mreq.imr_multiaddr) !=
        1) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kMulticastJoinFailed);
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (::setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                     sizeof(mreq)) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kMulticastJoinFailed);
    }

    // Set socket to non-blocking for receive thread
    int flags = ::fcntl(sockfd_, F_GETFL, 0);
    ::fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    running_.store(true, std::memory_order_release);

    // Spawn announce thread
    announce_thread_ = std::thread([this]() { AnnounceLoop(); });

    // Spawn receive thread
    receive_thread_ = std::thread([this]() { ReceiveLoop(); });

    return expected<void, DiscoveryError>::success();
  }

  /**
   * @brief Stop discovery and join threads.
   */
  void Stop() noexcept {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    if (announce_thread_.joinable()) {
      announce_thread_.join();
    }
    if (receive_thread_.joinable()) {
      receive_thread_.join();
    }

    if (sockfd_ >= 0) {
      ::close(sockfd_);
      sockfd_ = -1;
    }
  }

  /** @brief Check if discovery is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /**
   * @brief Find a node by name.
   * @param name Node name to search for.
   * @return Pointer to the node if found and alive, nullptr otherwise.
   */
  const DiscoveredNode* FindNode(const char* name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].alive && std::strcmp(nodes_[i].name, name) == 0) {
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

 private:
  // Announce packet format: { magic(4B), name(64B), port(2B) }
  static constexpr uint32_t kAnnounceMagic = 0x4F535044;  // "OSPD"
  static constexpr uint32_t kAnnounceSize = 4 + 64 + 2;

  void AnnounceLoop() noexcept {
    sockaddr_in mcast_addr{};
    mcast_addr.sin_family = AF_INET;
    ::inet_pton(AF_INET, config_.multicast_group, &mcast_addr.sin_addr);
    mcast_addr.sin_port = htons(config_.port);

    while (running_.load(std::memory_order_acquire)) {
      // Build announce packet
      uint8_t packet[kAnnounceSize];
      std::memcpy(packet, &kAnnounceMagic, 4);
      std::memcpy(packet + 4, local_name_, 64);
      std::memcpy(packet + 68, &local_port_, 2);

      ::sendto(sockfd_, packet, kAnnounceSize, 0,
               reinterpret_cast<sockaddr*>(&mcast_addr), sizeof(mcast_addr));

      // Sleep for announce interval
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.announce_interval_ms));
    }
  }

  void ReceiveLoop() noexcept {
    uint8_t packet[kAnnounceSize];

    while (running_.load(std::memory_order_acquire)) {
      sockaddr_in sender_addr{};
      socklen_t addr_len = sizeof(sender_addr);

      ssize_t n = ::recvfrom(sockfd_, packet, sizeof(packet), 0,
                             reinterpret_cast<sockaddr*>(&sender_addr),
                             &addr_len);

      if (n == static_cast<ssize_t>(kAnnounceSize)) {
        ProcessAnnounce(packet, sender_addr);
      }

      // Check for timeouts
      CheckTimeouts();

      // Sleep briefly to avoid busy-wait
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void ProcessAnnounce(const uint8_t* packet, const sockaddr_in& sender) noexcept {
    // Validate magic
    uint32_t magic;
    std::memcpy(&magic, packet, 4);
    if (magic != kAnnounceMagic) return;

    // Extract name and port
    char name[64];
    std::memcpy(name, packet + 4, 64);
    name[63] = '\0';  // Ensure null-terminated

    uint16_t port;
    std::memcpy(&port, packet + 68, 2);

    // Get sender IP
    char address[64];
    ::inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));

    uint64_t now = GetTimestampUs();

    std::lock_guard<std::mutex> lock(mutex_);

    // Find or create node entry
    uint32_t slot = MaxNodes;
    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].alive && std::strcmp(nodes_[i].name, name) == 0) {
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

    if (slot == MaxNodes) return;  // No free slots

    bool is_new = !nodes_[slot].alive;

    CopyString(nodes_[slot].name, name, 63);
    CopyString(nodes_[slot].address, address, 63);
    nodes_[slot].port = port;
    nodes_[slot].last_seen_us = now;
    nodes_[slot].alive = true;

    if (is_new) {
      ++node_count_;
      if (on_node_join_ != nullptr) {
        on_node_join_(nodes_[slot], join_ctx_);
      }
    }
  }

  void CheckTimeouts() noexcept {
    uint64_t now = GetTimestampUs();
    uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0; i < MaxNodes; ++i) {
      if (nodes_[i].alive) {
        if (now - nodes_[i].last_seen_us > timeout_us) {
          nodes_[i].alive = false;
          --node_count_;
          if (on_node_leave_ != nullptr) {
            on_node_leave_(nodes_[i], leave_ctx_);
          }
        }
      }
    }
  }

  static void CopyString(char* dst, const char* src, uint32_t max_len) noexcept {
    if (src == nullptr) {
      dst[0] = '\0';
      return;
    }
    uint32_t i = 0;
    for (; i < max_len && src[i] != '\0'; ++i) {
      dst[i] = src[i];
    }
    dst[i] = '\0';
  }

  static uint64_t GetTimestampUs() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(us.count());
  }

  Config config_;
  int sockfd_;
  std::atomic<bool> running_;
  std::thread announce_thread_;
  std::thread receive_thread_;

  char local_name_[64];
  uint16_t local_port_;

  NodeCallback on_node_join_;
  NodeCallback on_node_leave_;
  void* join_ctx_;
  void* leave_ctx_;

  mutable std::mutex mutex_;
  DiscoveredNode nodes_[MaxNodes];
  uint32_t node_count_;
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

    DiscoveryEntry() noexcept
        : node(), topics{}, topic_count(0), services{}, service_count(0) {}
  };

  TopicAwareDiscovery() noexcept
      : local_topic_count_(0), local_service_count_(0) {}

  /**
   * @brief Add a local topic advertisement.
   * @param topic Topic information to advertise.
   * @return Success or DiscoveryError.
   */
  expected<void, DiscoveryError> AddLocalTopic(const TopicInfo& topic) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_topic_count_ >= MaxTopicsPerNode) {
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kSocketFailed);
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
      return expected<void, DiscoveryError>::error(
          DiscoveryError::kSocketFailed);
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
  uint32_t FindPublishers(const char* topic_name, TopicInfo* out,
                          uint32_t max_results) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (uint32_t i = 0; i < local_topic_count_ && count < max_results; ++i) {
      if (local_topics_[i].is_publisher &&
          std::strcmp(local_topics_[i].name, topic_name) == 0) {
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
  uint32_t FindSubscribers(const char* topic_name, TopicInfo* out,
                           uint32_t max_results) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (uint32_t i = 0; i < local_topic_count_ && count < max_results; ++i) {
      if (!local_topics_[i].is_publisher &&
          std::strcmp(local_topics_[i].name, topic_name) == 0) {
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
      if (std::strcmp(local_services_[i].name, service_name) == 0) {
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

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

}  // namespace osp

#endif  // OSP_DISCOVERY_HPP_
