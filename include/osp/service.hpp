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
#include <cstring>
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

static constexpr uint32_t kServiceRequestMagic = 0x4F535052;   // "OSPR"
static constexpr uint32_t kServiceResponseMagic = 0x4F535041;  // "OSPA"
static constexpr uint32_t kServiceFrameHeaderSize = 8;

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
    int backlog = 8;
    uint32_t max_concurrent = 4;
  };

  explicit Service(const Config& cfg = {}) noexcept
      : config_(cfg),
        sockfd_(-1),
        running_(false),
        handler_(nullptr),
        handler_ctx_(nullptr) {
    static_assert(std::is_trivially_copyable<Request>::value,
                  "Request must be trivially copyable");
    static_assert(std::is_trivially_copyable<Response>::value,
                  "Response must be trivially copyable");
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
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    // Set SO_REUSEADDR
    int opt = 1;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                 static_cast<socklen_t>(sizeof(opt)));

    // Bind
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config_.port);

    if (::bind(sockfd_, reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

    // Listen
    if (::listen(sockfd_, config_.backlog) < 0) {
      ::close(sockfd_);
      sockfd_ = -1;
      return expected<void, ServiceError>::error(ServiceError::kBindFailed);
    }

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

    if (sockfd_ >= 0) {
      ::shutdown(sockfd_, SHUT_RDWR);
      ::close(sockfd_);
      sockfd_ = -1;
    }

    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }

    // Wait for worker threads to finish
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& t : worker_threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    worker_threads_.clear();
  }

  /** @brief Check if the service is running. */
  bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /** @brief Get the bound port (useful when using port 0 for OS-assigned). */
  uint16_t GetPort() const noexcept {
    if (sockfd_ < 0) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(sockfd_, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
  }

 private:
  void AcceptLoop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
      sockaddr_in client_addr{};
      socklen_t addr_len = sizeof(client_addr);

      int client_fd = ::accept(sockfd_, reinterpret_cast<sockaddr*>(&client_addr),
                               &addr_len);
      if (client_fd < 0) {
        if (!running_.load(std::memory_order_acquire)) break;
        continue;
      }

      // Spawn worker thread to handle this connection
      std::lock_guard<std::mutex> lock(threads_mutex_);
      worker_threads_.emplace_back([this, client_fd]() {
        HandleConnection(client_fd);
      });
    }
  }

  void HandleConnection(int client_fd) noexcept {
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

  static bool RecvAll(int fd, void* buf, size_t len) noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
      ssize_t n = ::recv(fd, ptr, remaining, 0);
      if (n <= 0) return false;
      ptr += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  static bool SendAll(int fd, const void* buf, size_t len) noexcept {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
      ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
      if (n <= 0) return false;
      ptr += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  Config config_;
  int sockfd_;
  std::atomic<bool> running_;
  std::thread accept_thread_;
  std::vector<std::thread> worker_threads_;
  std::mutex threads_mutex_;

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
                                                 int timeout_ms = 5000) noexcept {
    Client client;

    // Create socket
    client.sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client.sockfd_ < 0) {
      return expected<Client, ServiceError>::error(ServiceError::kConnectFailed);
    }

    // Set socket to non-blocking for timeout support
    int flags = ::fcntl(client.sockfd_, F_GETFL, 0);
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

    int ret = ::connect(client.sockfd_, reinterpret_cast<sockaddr*>(&server_addr),
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
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;

      ret = ::select(client.sockfd_ + 1, nullptr, &write_fds, nullptr, &tv);
      if (ret <= 0) {
        ::close(client.sockfd_);
        client.sockfd_ = -1;
        return expected<Client, ServiceError>::error(ServiceError::kTimeout);
      }

      // Check for connection error
      int error = 0;
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
    int nodelay = 1;
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
                                         int timeout_ms = 2000) noexcept {
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
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
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
  static bool RecvAll(int fd, void* buf, size_t len) noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
      ssize_t n = ::recv(fd, ptr, remaining, 0);
      if (n <= 0) return false;
      ptr += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  static bool SendAll(int fd, const void* buf, size_t len) noexcept {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
      ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
      if (n <= 0) return false;
      ptr += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  int sockfd_;
  bool connected_;
};

#endif  // defined(OSP_PLATFORM_LINUX) || defined(OSP_PLATFORM_MACOS)

}  // namespace osp

#endif  // OSP_SERVICE_HPP_
