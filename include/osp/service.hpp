/**
 * @file service.hpp
 * @brief Request-response service pattern for OSP.
 *
 * Inspired by ROS2 Service/Client model. Provides synchronous request-response
 * communication over TCP with simple framing protocol.
 *
 * Header-only, C++17, compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_SERVICE_HPP_
#define OSP_SERVICE_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#if defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace osp {

// ============================================================================
// Service Error
// ============================================================================

enum class ServiceError : uint8_t {
  kBindFailed,
  kConnectFailed,
  kSendFailed,
  kRecvFailed,
  kTimeout,
  kSerializeFailed,
  kDeserializeFailed,
  kNotRunning,
};

// ============================================================================
// Service Frame Protocol
// ============================================================================

// Request frame: { magic(4B) 0x4F535052, req_size(4B), request_data }
// Response frame: { magic(4B) 0x4F535041, resp_size(4B), response_data }

inline constexpr uint32_t kServiceRequestMagic = 0x4F535052;   // "OSPR"
inline constexpr uint32_t kServiceResponseMagic = 0x4F535041;  // "OSPA"
inline constexpr uint32_t kServiceFrameHeaderSize = 8;

// Socket option values
inline constexpr int32_t kSocketOptEnable = 1;

// Async client polling interval
inline constexpr int32_t kAsyncPollIntervalMs = 10;

// ============================================================================
// Service - Server-side request handler
// ============================================================================

/**
 * @brief Request-response service (TCP-based).
 *
 * Listens on a TCP port and handles incoming requests by invoking a user-
 * provided handler function. Request and Response types must be trivially
 * copyable (POD).
 *
 * @tparam Request The request message type (must be trivially copyable).
 * @tparam Response The response message type (must be trivially copyable).
 */
template <typename Request, typename Response>
class Service {
 public:
  using Handler = Response (*)(const Request&, void*);

  struct Config {
    uint16_t port = 0;
    int32_t backlog = 8;
    uint32_t max_concurrent = 4;
  };

  explicit Service(const Config& cfg = {}) noexcept
      : config_(cfg),
        running_(false),
        handler_(nullptr),
        handler_ctx_(nullptr) {
    static_assert(std::is_trivially_copyable<Request>::value,
                  "Request must be trivially copyable");
    static_assert(std::is_trivially_copyable<Response>::value,
                  "Response must be trivially copyable");
    sockfd_.store(-1, std::memory_order_relaxed);
  }

  ~Service() { Stop(); }

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  Service(Service&&) = delete;
  Service& operator=(Service&&) = delete;

  /**
   * @brief Set the request handler function.
   * @param handler Function pointer to invoke for each request.
   * @param ctx User context pointer passed to handler.
   */
  void SetHandler(Handler handler, void* ctx = nullptr) noexcept {
    handler_ = handler;
    handler_ctx_ = ctx;
  }

  /**
   * @brief Start the service (bind and listen).
   * @return Success or ServiceError.
   */
  expected<void, ServiceError> Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    // Create TCP socket
    int32_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    // Set SO_REUSEADDR
    int32_t opt = kSocketOptEnable;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                 static_cast<socklen_t>(sizeof(opt)));

    // Bind
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config_.port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
      ::close(fd);
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    // Listen
    if (::listen(fd, config_.backlog) < 0) {
      ::close(fd);
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    sockfd_.store(fd, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // Spawn accept thread
    accept_thread_ = std::thread([this]() { AcceptLoop(); });

    return expected<void, ServiceError>::success();
  }

  /**
   * @brief Stop the service and join threads.
   */
  void Stop() noexcept {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    int32_t fd = sockfd_.load(std::memory_order_acquire);
    if (fd >= 0) {
      sockfd_.store(-1, std::memory_order_release);
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
    }

    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }

    // Wait for worker threads to finish
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& entry : worker_entries_) {
      if (entry.thread.joinable()) {
        entry.thread.join();
      }
    }
    worker_entries_.clear();
  }

  /** @brief Check if the service is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Get the bound port (useful when using port 0 for OS-assigned). */
  uint16_t GetPort() const noexcept {
    int32_t fd = sockfd_.load(std::memory_order_acquire);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
  }

  /** @brief Set heartbeat for external watchdog monitoring (accept thread). */
  void SetHeartbeat(ThreadHeartbeat* hb) noexcept { heartbeat_ = hb; }

 private:
  struct WorkerEntry {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  class WorkerScope {
   public:
    explicit WorkerScope(std::atomic<uint32_t>& counter) noexcept
        : counter_(counter) {
      counter_.fetch_add(1, std::memory_order_relaxed);
    }
    ~WorkerScope() { counter_.fetch_sub(1, std::memory_order_relaxed); }
    WorkerScope(const WorkerScope&) = delete;
    WorkerScope& operator=(const WorkerScope&) = delete;

   private:
    std::atomic<uint32_t>& counter_;
  };

  void ReapFinishedWorkers() noexcept {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (uint32_t i = 0; i < worker_entries_.size();) {
      if (worker_entries_[i].finished->load(std::memory_order_acquire)) {
        if (worker_entries_[i].thread.joinable()) {
          worker_entries_[i].thread.join();
        }
        worker_entries_[i] = std::move(worker_entries_.back());
        worker_entries_.pop_back();
      } else {
        ++i;
      }
    }
  }

  void AcceptLoop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
      if (heartbeat_ != nullptr) { heartbeat_->Beat(); }
      // Reap finished workers
      ReapFinishedWorkers();

      sockaddr_in client_addr{};
      socklen_t addr_len = sizeof(client_addr);

      int32_t fd = sockfd_.load(std::memory_order_acquire);
      if (fd < 0) break;

      int32_t client_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&client_addr),
                               &addr_len);
      if (client_fd < 0) {
        if (!running_.load(std::memory_order_acquire)) break;
        continue;
      }

      // Check max_concurrent limit
      if (active_workers_.load(std::memory_order_relaxed) >= config_.max_concurrent) {
        ::close(client_fd);
        continue;
      }

      // Spawn worker thread to handle this connection
      auto finished = std::make_shared<std::atomic<bool>>(false);
      std::lock_guard<std::mutex> lock(threads_mutex_);
      worker_entries_.push_back(WorkerEntry{
          std::thread([this, client_fd, finished]() {
            WorkerScope scope(active_workers_);
            HandleConnection(client_fd);
            finished->store(true, std::memory_order_release);
          }),
          finished});
    }
  }

  void HandleConnection(int32_t client_fd) noexcept {
    while (running_.load(std::memory_order_acquire)) {
      // Receive request frame header
      uint8_t header[kServiceFrameHeaderSize];
      if (!RecvAll(client_fd, header, kServiceFrameHeaderSize)) {
        break;  // Client disconnected or error
      }

      // Validate magic
      uint32_t magic;
      std::memcpy(&magic, header, 4);
      if (magic != kServiceRequestMagic) break;

      // Extract request size
      uint32_t req_size;
      std::memcpy(&req_size, header + 4, 4);
      if (req_size != sizeof(Request)) break;

      // Receive request data
      Request req;
      if (!RecvAll(client_fd, &req, sizeof(Request))) break;

      // Invoke handler
      Response resp;
      if (handler_ != nullptr) {
        resp = handler_(req, handler_ctx_);
      } else {
        std::memset(&resp, 0, sizeof(Response));
      }

      // Send response frame
      uint8_t resp_header[kServiceFrameHeaderSize];
      uint32_t resp_magic = kServiceResponseMagic;
      uint32_t resp_size = static_cast<uint32_t>(sizeof(Response));
      std::memcpy(resp_header, &resp_magic, 4);
      std::memcpy(resp_header + 4, &resp_size, 4);

      if (!SendAll(client_fd, resp_header, kServiceFrameHeaderSize)) break;
      if (!SendAll(client_fd, &resp, sizeof(Response))) break;
    }
    ::close(client_fd);
  }

  static bool RecvAll(int32_t fd, void* buf, uint64_t len) noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    uint64_t remaining = len;
    while (remaining > 0) {
      int64_t n = ::recv(fd, ptr, remaining, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;  // EOF
      ptr += n;
      remaining -= static_cast<uint64_t>(n);
    }
    return true;
  }

  static bool SendAll(int32_t fd, const void* buf, uint64_t len) noexcept {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    uint64_t remaining = len;
    while (remaining > 0) {
      int64_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;  // EOF
      ptr += n;
      remaining -= static_cast<uint64_t>(n);
    }
    return true;
  }

  Config config_;
  std::atomic<int32_t> sockfd_;
  std::atomic<bool> running_;
  std::thread accept_thread_;
  std::vector<WorkerEntry> worker_entries_;
  std::mutex threads_mutex_;
  std::atomic<uint32_t> active_workers_{0};
  ThreadHeartbeat* heartbeat_{nullptr};

  Handler handler_;
  void* handler_ctx_;
};

// ============================================================================
// Client - Client-side request sender
// ============================================================================

/**
 * @brief Service client for sending requests.
 *
 * Connects to a service endpoint and sends synchronous requests.
 * Request and Response types must be trivially copyable (POD).
 *
 * @tparam Request The request message type (must be trivially copyable).
 * @tparam Response The response message type (must be trivially copyable).
 */
template <typename Request, typename Response>
class Client {
 public:
  Client() noexcept : sockfd_(-1), connected_(false) {
    static_assert(std::is_trivially_copyable<Request>::value,
                  "Request must be trivially copyable");
    static_assert(std::is_trivially_copyable<Response>::value,
                  "Response must be trivially copyable");
  }

  ~Client() { Close(); }

  // Move-only
  Client(Client&& other) noexcept
      : sockfd_(other.sockfd_), connected_(other.connected_) {
    other.sockfd_ = -1;
    other.connected_ = false;
  }

  Client& operator=(Client&& other) noexcept {
    if (this != &other) {
      Close();
      sockfd_ = other.sockfd_;
      connected_ = other.connected_;
      other.sockfd_ = -1;
      other.connected_ = false;
    }
    return *this;
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /**
   * @brief Connect to a service endpoint.
   * @param host Host address (e.g. "127.0.0.1").
   * @param port Service port.
   * @param timeout_ms Connection timeout in milliseconds.
   * @return Client instance on success, ServiceError on failure.
   */
  static expected<Client, ServiceError> Connect(const char* host, uint16_t port,
                                                 int32_t timeout_ms = 5000) noexcept {
    Client client;

    // Create socket
    client.sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client.sockfd_ < 0) {
      return expected<Client, ServiceError>::error(ServiceError::kConnectFailed);
    }

    // Set socket to non-blocking for timeout support
    int32_t flags = ::fcntl(client.sockfd_, F_GETFL, 0);
    ::fcntl(client.sockfd_, F_SETFL, flags | O_NONBLOCK);

    // Connect
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
      ::close(client.sockfd_);
      client.sockfd_ = -1;
      return expected<Client, ServiceError>::error(ServiceError::kConnectFailed);
    }

    int32_t ret = ::connect(client.sockfd_, reinterpret_cast<sockaddr*>(&server_addr),
                        sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
      ::close(client.sockfd_);
      client.sockfd_ = -1;
      return expected<Client, ServiceError>::error(ServiceError::kConnectFailed);
    }

    // Wait for connection with timeout
    if (ret < 0) {
      fd_set write_fds;
      FD_ZERO(&write_fds);
      FD_SET(client.sockfd_, &write_fds);

      timeval tv;
      tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
      tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);

      ret = ::select(client.sockfd_ + 1, nullptr, &write_fds, nullptr, &tv);
      if (ret <= 0) {
        ::close(client.sockfd_);
        client.sockfd_ = -1;
        return expected<Client, ServiceError>::error(ServiceError::kTimeout);
      }

      // Check for connection error
      int32_t error = 0;
      socklen_t len = sizeof(error);
      ::getsockopt(client.sockfd_, SOL_SOCKET, SO_ERROR, &error, &len);
      if (error != 0) {
        ::close(client.sockfd_);
        client.sockfd_ = -1;
        return expected<Client, ServiceError>::error(ServiceError::kConnectFailed);
      }
    }

    // Set socket back to blocking
    ::fcntl(client.sockfd_, F_SETFL, flags);

    // Disable Nagle's algorithm
    int32_t nodelay = kSocketOptEnable;
    ::setsockopt(client.sockfd_, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                 static_cast<socklen_t>(sizeof(nodelay)));

    client.connected_ = true;
    return expected<Client, ServiceError>::success(std::move(client));
  }

  /**
   * @brief Send a request and receive a response.
   * @param req The request message.
   * @param timeout_ms Timeout in milliseconds.
   * @return Response on success, ServiceError on failure.
   */
  expected<Response, ServiceError> Call(const Request& req,
                                         int32_t timeout_ms = 2000) noexcept {
    if (!connected_) {
      return expected<Response, ServiceError>::error(ServiceError::kNotRunning);
    }

    // Send request frame
    uint8_t header[kServiceFrameHeaderSize];
    uint32_t magic = kServiceRequestMagic;
    uint32_t req_size = static_cast<uint32_t>(sizeof(Request));
    std::memcpy(header, &magic, 4);
    std::memcpy(header + 4, &req_size, 4);

    if (!SendAll(sockfd_, header, kServiceFrameHeaderSize)) {
      connected_ = false;
      return expected<Response, ServiceError>::error(ServiceError::kSendFailed);
    }

    if (!SendAll(sockfd_, &req, sizeof(Request))) {
      connected_ = false;
      return expected<Response, ServiceError>::error(ServiceError::kSendFailed);
    }

    // Set receive timeout
    timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    ::setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv,
                 static_cast<socklen_t>(sizeof(tv)));

    // Receive response frame
    uint8_t resp_header[kServiceFrameHeaderSize];
    if (!RecvAll(sockfd_, resp_header, kServiceFrameHeaderSize)) {
      connected_ = false;
      return expected<Response, ServiceError>::error(ServiceError::kRecvFailed);
    }

    // Validate magic
    uint32_t resp_magic;
    std::memcpy(&resp_magic, resp_header, 4);
    if (resp_magic != kServiceResponseMagic) {
      connected_ = false;
      return expected<Response, ServiceError>::error(
          ServiceError::kDeserializeFailed);
    }

    // Extract response size
    uint32_t resp_size;
    std::memcpy(&resp_size, resp_header + 4, 4);
    if (resp_size != sizeof(Response)) {
      connected_ = false;
      return expected<Response, ServiceError>::error(
          ServiceError::kDeserializeFailed);
    }

    // Receive response data
    Response resp;
    if (!RecvAll(sockfd_, &resp, sizeof(Response))) {
      connected_ = false;
      return expected<Response, ServiceError>::error(ServiceError::kRecvFailed);
    }

    return expected<Response, ServiceError>::success(resp);
  }

  /** @brief Close the connection. */
  void Close() noexcept {
    if (sockfd_ >= 0) {
      ::close(sockfd_);
      sockfd_ = -1;
    }
    connected_ = false;
  }

  /** @brief Check if connected. */
  bool IsConnected() const noexcept { return connected_; }

 private:
  static bool RecvAll(int32_t fd, void* buf, uint64_t len) noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    uint64_t remaining = len;
    while (remaining > 0) {
      int64_t n = ::recv(fd, ptr, remaining, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;  // EOF
      ptr += n;
      remaining -= static_cast<uint64_t>(n);
    }
    return true;
  }

  static bool SendAll(int32_t fd, const void* buf, uint64_t len) noexcept {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    uint64_t remaining = len;
    while (remaining > 0) {
      int64_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;  // EOF
      ptr += n;
      remaining -= static_cast<uint64_t>(n);
    }
    return true;
  }

  int32_t sockfd_;
  bool connected_;
};

// ============================================================================
// AsyncCallResult - Result container for async calls
// ============================================================================

template <typename Response>
struct AsyncCallResult {
  Response response{};
  ServiceError error{ServiceError::kNotRunning};
  bool completed{false};
  bool success{false};
};

// ============================================================================
// ServiceRegistry - Local service name to endpoint mapping
// ============================================================================

/**
 * @brief Local registry that maps service names to endpoints.
 *
 * Provides a simple in-memory registry for service discovery.
 * Thread-safe for concurrent access.
 *
 * @tparam MaxServices Maximum number of services that can be registered.
 */
template <uint32_t MaxServices = 32>
class ServiceRegistry {
 public:
  struct Entry {
    FixedString<63> name;
    FixedString<63> host;
    uint16_t port;
  };

  ServiceRegistry() noexcept = default;

  /**
   * @brief Register a service endpoint.
   * @param name Service name (must be unique).
   * @param host Host address.
   * @param port Port number.
   * @return Success or ServiceError.
   */
  expected<void, ServiceError> Register(const char* name, const char* host,
                                         uint16_t port) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for duplicate name
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (std::strcmp(entries_[i].name.c_str(), name) == 0) {
        return expected<void, ServiceError>::error(ServiceError::kBindFailed);
      }
    }

    if (entries_.full()) {
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    Entry entry;
    entry.name.assign(TruncateToCapacity, name);
    entry.host.assign(TruncateToCapacity, host);
    entry.port = port;
    static_cast<void>(entries_.push_back(entry));
    return expected<void, ServiceError>::success();
  }

  /**
   * @brief Unregister a service by name.
   * @param name Service name.
   * @return true if found and removed, false otherwise.
   */
  bool Unregister(const char* name) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (std::strcmp(entries_[i].name.c_str(), name) == 0) {
        static_cast<void>(entries_.erase_unordered(i));
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Lookup a service by name.
   * @param name Service name.
   * @return Entry value if found, empty optional otherwise.
   */
  optional<Entry> Lookup(const char* name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (std::strcmp(entries_[i].name.c_str(), name) == 0) {
        return optional<Entry>(entries_[i]);
      }
    }
    return optional<Entry>();
  }

  /**
   * @brief Count active entries.
   * @return Number of registered services.
   */
  uint32_t Count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  /**
   * @brief Reset all entries.
   */
  void Reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
  }

 private:
  FixedVector<Entry, MaxServices> entries_;
  mutable std::mutex mutex_;
};

// ============================================================================
// AsyncClient - Asynchronous client with background thread
// ============================================================================

/**
 * @brief Asynchronous service client.
 *
 * Wraps Client with background thread for async calls. Allows non-blocking
 * request-response pattern.
 *
 * @tparam Request The request message type (must be trivially copyable).
 * @tparam Response The response message type (must be trivially copyable).
 */
template <typename Request, typename Response>
class AsyncClient {
 public:
  AsyncClient() noexcept
      : connected_(false),
        call_in_progress_(false),
        result_ready_(false) {
    static_assert(std::is_trivially_copyable<Request>::value,
                  "Request must be trivially copyable");
    static_assert(std::is_trivially_copyable<Response>::value,
                  "Response must be trivially copyable");
    std::memset(&result_.response, 0, sizeof(Response));
    result_.error = ServiceError::kNotRunning;
    result_.completed = false;
    result_.success = false;
  }

  ~AsyncClient() {
    Close();
  }

  AsyncClient(const AsyncClient&) = delete;
  AsyncClient& operator=(const AsyncClient&) = delete;

  AsyncClient(AsyncClient&& other) noexcept
      : client_(std::move(other.client_)),
        connected_(other.connected_),
        call_in_progress_(other.call_in_progress_.load()),
        result_ready_(other.result_ready_.load()),
        result_(other.result_) {
    other.connected_ = false;
    other.call_in_progress_.store(false);
    other.result_ready_.store(false);
  }

  AsyncClient& operator=(AsyncClient&& other) noexcept {
    if (this != &other) {
      Close();
      client_ = std::move(other.client_);
      connected_ = other.connected_;
      call_in_progress_.store(other.call_in_progress_.load());
      result_ready_.store(other.result_ready_.load());
      result_ = other.result_;
      other.connected_ = false;
      other.call_in_progress_.store(false);
      other.result_ready_.store(false);
    }
    return *this;
  }

  /**
   * @brief Connect to a service endpoint.
   * @param host Host address (e.g. "127.0.0.1").
   * @param port Service port.
   * @param timeout_ms Connection timeout in milliseconds.
   * @return AsyncClient instance on success, ServiceError on failure.
   */
  static expected<AsyncClient, ServiceError> Connect(
      const char* host, uint16_t port, int32_t timeout_ms = 5000) noexcept {
    AsyncClient async_client;

    auto client_r = Client<Request, Response>::Connect(host, port, timeout_ms);
    if (!client_r.has_value()) {
      return expected<AsyncClient, ServiceError>::error(client_r.get_error());
    }

    async_client.client_ = std::move(client_r.value());
    async_client.connected_ = true;

    return expected<AsyncClient, ServiceError>::success(
        std::move(async_client));
  }

  /**
   * @brief Async call - spawns a thread, stores result.
   * @param req The request message.
   * @param timeout_ms Timeout in milliseconds.
   * @return true if call was initiated, false if already in progress.
   */
  bool CallAsync(const Request& req, int32_t timeout_ms = 2000) noexcept {
    if (!connected_) return false;

    bool expected_false = false;
    if (!call_in_progress_.compare_exchange_strong(expected_false, true,
                                                    std::memory_order_acquire)) {
      return false;  // Call already in progress
    }

    result_ready_.store(false, std::memory_order_release);

    // Join previous thread if exists
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }

    // Spawn worker thread
    worker_thread_ = std::thread([this, req, timeout_ms]() {
      auto resp_r = client_.Call(req, timeout_ms);

      std::lock_guard<std::mutex> lock(result_mutex_);
      if (resp_r.has_value()) {
        result_.response = resp_r.value();
        result_.error = ServiceError::kNotRunning;  // No error
        result_.success = true;
      } else {
        std::memset(&result_.response, 0, sizeof(Response));
        result_.error = resp_r.get_error();
        result_.success = false;
      }
      result_.completed = true;

      result_ready_.store(true, std::memory_order_release);
      call_in_progress_.store(false, std::memory_order_release);
    });

    return true;
  }

  /**
   * @brief Check if async result is ready.
   * @return true if result is available.
   */
  bool IsReady() const noexcept {
    return result_ready_.load(std::memory_order_acquire);
  }

  /**
   * @brief Get result (blocks if not ready, with timeout).
   * @param timeout_ms Timeout in milliseconds.
   * @return Response on success, ServiceError on failure.
   */
  expected<Response, ServiceError> GetResult(int32_t timeout_ms = 5000) noexcept {
    const uint64_t start_us = SteadyNowUs();
    const uint64_t timeout_us = static_cast<uint64_t>(timeout_ms) * 1000U;

    while (!result_ready_.load(std::memory_order_acquire)) {
      if (SteadyNowUs() - start_us >= timeout_us) {
        return expected<Response, ServiceError>::error(ServiceError::kTimeout);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kAsyncPollIntervalMs));
    }

    std::lock_guard<std::mutex> lock(result_mutex_);
    if (result_.success) {
      return expected<Response, ServiceError>::success(result_.response);
    } else {
      return expected<Response, ServiceError>::error(result_.error);
    }
  }

  /**
   * @brief Check if connected.
   * @return true if connected.
   */
  bool IsConnected() const noexcept { return connected_; }

  /**
   * @brief Close the connection.
   */
  void Close() noexcept {
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
    client_.Close();
    connected_ = false;
  }

 private:
  Client<Request, Response> client_;
  bool connected_;
  std::atomic<bool> call_in_progress_;
  std::atomic<bool> result_ready_;
  AsyncCallResult<Response> result_;
  std::mutex result_mutex_;
  std::thread worker_thread_;
};

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

}  // namespace osp

#endif  // OSP_SERVICE_HPP_
