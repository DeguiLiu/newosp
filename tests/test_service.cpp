/**
 * @file test_service.cpp
 * @brief Tests for service.hpp: Service and Client.
 */

#include <catch2/catch_test_macros.hpp>
#include "osp/service.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// ============================================================================
// Test message types
// ============================================================================

struct AddRequest {
  int32_t a;
  int32_t b;
};

struct AddResponse {
  int32_t sum;
};

struct EchoRequest {
  char message[64];
};

struct EchoResponse {
  char message[64];
};

// ============================================================================
// Helper: Get OS-assigned port from a bound socket
// ============================================================================

static uint16_t GetBoundPort(int sockfd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  ::getsockname(sockfd, reinterpret_cast<sockaddr*>(&addr), &len);
  return ntohs(addr.sin_port);
}

// ============================================================================
// 1. Service - Start/Stop
// ============================================================================

TEST_CASE("service - Service start and stop", "[service]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned port
  osp::Service<AddRequest, AddResponse> service(cfg);

  auto r = service.Start();
  REQUIRE(r.has_value());
  REQUIRE(service.IsRunning());

  service.Stop();
  REQUIRE(!service.IsRunning());
}

// ============================================================================
// 2. Client - Connect and single Call
// ============================================================================

TEST_CASE("service - Client connect and single call", "[service][client]") {
  // Start service
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  service.SetHandler(
      [](const AddRequest& req, void* /*ctx*/) -> AddResponse {
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  // Give service time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect client
  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());

  auto client = std::move(client_r.value());
  REQUIRE(client.IsConnected());

  // Send request
  AddRequest req{10, 20};
  auto resp_r = client.Call(req);
  REQUIRE(resp_r.has_value());
  REQUIRE(resp_r.value().sum == 30);

  client.Close();
  service.Stop();
}

// ============================================================================
// 3. Client - Multiple calls
// ============================================================================

TEST_CASE("service - Client multiple calls", "[service][client]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  service.SetHandler(
      [](const AddRequest& req, void* /*ctx*/) -> AddResponse {
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  // Multiple calls
  for (int i = 0; i < 10; ++i) {
    AddRequest req{i, i * 2};
    auto resp_r = client.Call(req);
    REQUIRE(resp_r.has_value());
    REQUIRE(resp_r.value().sum == i + i * 2);
  }

  client.Close();
  service.Stop();
}

// ============================================================================
// 4. Client - Connection failure
// ============================================================================

TEST_CASE("service - Client connect to non-existent service", "[service][client]") {
  // Try to connect to a port with no service
  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", 19999, 500);
  REQUIRE(!client_r.has_value());
  REQUIRE((client_r.get_error() == osp::ServiceError::kTimeout ||
           client_r.get_error() == osp::ServiceError::kConnectFailed));
}

// ============================================================================
// 5. Client - Call timeout
// ============================================================================

TEST_CASE("service - Client call timeout", "[service][client]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  // Handler that sleeps longer than timeout
  service.SetHandler(
      [](const AddRequest& req, void* /*ctx*/) -> AddResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  // Call with short timeout
  AddRequest req{10, 20};
  auto resp_r = client.Call(req, 500);
  REQUIRE(!resp_r.has_value());
  REQUIRE(resp_r.get_error() == osp::ServiceError::kRecvFailed);

  client.Close();
  service.Stop();
}

// ============================================================================
// 6. Multiple concurrent clients
// ============================================================================

TEST_CASE("service - Multiple concurrent clients", "[service][client]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  cfg.max_concurrent = 8;
  osp::Service<AddRequest, AddResponse> service(cfg);

  std::atomic<uint32_t> request_count{0};

  service.SetHandler(
      [](const AddRequest& req, void* ctx) -> AddResponse {
        auto* count = static_cast<std::atomic<uint32_t>*>(ctx);
        count->fetch_add(1, std::memory_order_relaxed);
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      },
      &request_count);

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Spawn multiple client threads
  constexpr uint32_t kNumClients = 4;
  std::thread clients[kNumClients];

  for (uint32_t i = 0; i < kNumClients; ++i) {
    clients[i] = std::thread([i, port]() {
      auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
      if (!client_r.has_value()) return;

      auto client = std::move(client_r.value());

      for (int j = 0; j < 5; ++j) {
        AddRequest req{static_cast<int32_t>(i), static_cast<int32_t>(j)};
        auto resp_r = client.Call(req);
        if (resp_r.has_value()) {
          REQUIRE(resp_r.value().sum == static_cast<int32_t>(i + j));
        }
      }

      client.Close();
    });
  }

  for (uint32_t i = 0; i < kNumClients; ++i) {
    clients[i].join();
  }

  // Verify all requests were processed
  REQUIRE(request_count.load(std::memory_order_relaxed) == kNumClients * 5);

  service.Stop();
}

// ============================================================================
// 7. Echo service test
// ============================================================================

TEST_CASE("service - Echo service", "[service]") {
  osp::Service<EchoRequest, EchoResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<EchoRequest, EchoResponse> service(cfg);

  service.SetHandler(
      [](const EchoRequest& req, void* /*ctx*/) -> EchoResponse {
        EchoResponse resp;
        std::strcpy(resp.message, req.message);
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::Client<EchoRequest, EchoResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  EchoRequest req;
  std::strcpy(req.message, "Hello, OSP!");
  auto resp_r = client.Call(req);
  REQUIRE(resp_r.has_value());
  REQUIRE(std::strcmp(resp_r.value().message, "Hello, OSP!") == 0);

  client.Close();
  service.Stop();
}

// ============================================================================
// 8. Service without handler
// ============================================================================

TEST_CASE("service - Service without handler returns zero response", "[service]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  // No handler set

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  AddRequest req{10, 20};
  auto resp_r = client.Call(req);
  REQUIRE(resp_r.has_value());
  REQUIRE(resp_r.value().sum == 0);  // Default zero-initialized response

  client.Close();
  service.Stop();
}

// ============================================================================
// 9. Client move semantics
// ============================================================================

TEST_CASE("service - Client move semantics", "[service][client]") {
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  service.SetHandler(
      [](const AddRequest& req, void* /*ctx*/) -> AddResponse {
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  REQUIRE(port > 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());

  // Move construct
  auto client1 = std::move(client_r.value());
  REQUIRE(client1.IsConnected());

  // Move assign
  osp::Client<AddRequest, AddResponse> client2;
  client2 = std::move(client1);
  REQUIRE(client2.IsConnected());
  REQUIRE(!client1.IsConnected());

  // Use moved client
  AddRequest req{5, 7};
  auto resp_r = client2.Call(req);
  REQUIRE(resp_r.has_value());
  REQUIRE(resp_r.value().sum == 12);

  client2.Close();
  service.Stop();
}
