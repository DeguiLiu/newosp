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
  std::atomic<uint32_t> success_count{0};

  for (uint32_t i = 0; i < kNumClients; ++i) {
    clients[i] = std::thread([i, port, &success_count]() {
      auto client_r = osp::Client<AddRequest, AddResponse>::Connect("127.0.0.1", port);
      if (!client_r.has_value()) return;

      auto client = std::move(client_r.value());

      for (int j = 0; j < 5; ++j) {
        AddRequest req{static_cast<int32_t>(i), static_cast<int32_t>(j)};
        auto resp_r = client.Call(req);
        if (resp_r.has_value() &&
            resp_r.value().sum == static_cast<int32_t>(i + j)) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      }

      client.Close();
    });
  }

  for (uint32_t i = 0; i < kNumClients; ++i) {
    clients[i].join();
  }

  // Verify all requests were processed correctly
  REQUIRE(success_count.load(std::memory_order_relaxed) == kNumClients * 5);

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

// ============================================================================
// 10. ServiceRegistry - Register and lookup
// ============================================================================

TEST_CASE("service - ServiceRegistry register and lookup", "[service][registry]") {
  osp::ServiceRegistry<32> registry;

  auto r1 = registry.Register("add_service", "127.0.0.1", 8080);
  REQUIRE(r1.has_value());
  REQUIRE(registry.Count() == 1);

  auto r2 = registry.Register("echo_service", "192.168.1.100", 9090);
  REQUIRE(r2.has_value());
  REQUIRE(registry.Count() == 2);

  // Lookup existing service
  const auto* entry1 = registry.Lookup("add_service");
  REQUIRE(entry1 != nullptr);
  REQUIRE(entry1->active);
  REQUIRE(std::strcmp(entry1->name, "add_service") == 0);
  REQUIRE(std::strcmp(entry1->host, "127.0.0.1") == 0);
  REQUIRE(entry1->port == 8080);

  const auto* entry2 = registry.Lookup("echo_service");
  REQUIRE(entry2 != nullptr);
  REQUIRE(entry2->active);
  REQUIRE(std::strcmp(entry2->name, "echo_service") == 0);
  REQUIRE(std::strcmp(entry2->host, "192.168.1.100") == 0);
  REQUIRE(entry2->port == 9090);

  // Lookup non-existent service
  const auto* entry3 = registry.Lookup("nonexistent");
  REQUIRE(entry3 == nullptr);
}

// ============================================================================
// 11. ServiceRegistry - Unregister
// ============================================================================

TEST_CASE("service - ServiceRegistry unregister", "[service][registry]") {
  osp::ServiceRegistry<32> registry;

  registry.Register("service1", "127.0.0.1", 8080);
  registry.Register("service2", "127.0.0.1", 8081);
  registry.Register("service3", "127.0.0.1", 8082);
  REQUIRE(registry.Count() == 3);

  // Unregister existing service
  bool removed = registry.Unregister("service2");
  REQUIRE(removed);
  REQUIRE(registry.Count() == 2);

  // Verify it's gone
  const auto* entry = registry.Lookup("service2");
  REQUIRE(entry == nullptr);

  // Other services still exist
  REQUIRE(registry.Lookup("service1") != nullptr);
  REQUIRE(registry.Lookup("service3") != nullptr);

  // Unregister non-existent service
  bool removed2 = registry.Unregister("nonexistent");
  REQUIRE(!removed2);
  REQUIRE(registry.Count() == 2);
}

// ============================================================================
// 12. ServiceRegistry - Duplicate name
// ============================================================================

TEST_CASE("service - ServiceRegistry duplicate name", "[service][registry]") {
  osp::ServiceRegistry<32> registry;

  auto r1 = registry.Register("my_service", "127.0.0.1", 8080);
  REQUIRE(r1.has_value());

  // Try to register with same name
  auto r2 = registry.Register("my_service", "127.0.0.1", 9090);
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::ServiceError::kBindFailed);

  // Original entry unchanged
  const auto* entry = registry.Lookup("my_service");
  REQUIRE(entry != nullptr);
  REQUIRE(entry->port == 8080);
}

// ============================================================================
// 13. ServiceRegistry - Overflow
// ============================================================================

TEST_CASE("service - ServiceRegistry overflow", "[service][registry]") {
  osp::ServiceRegistry<4> registry;  // Small capacity

  // Fill registry
  REQUIRE(registry.Register("service1", "127.0.0.1", 8081).has_value());
  REQUIRE(registry.Register("service2", "127.0.0.1", 8082).has_value());
  REQUIRE(registry.Register("service3", "127.0.0.1", 8083).has_value());
  REQUIRE(registry.Register("service4", "127.0.0.1", 8084).has_value());
  REQUIRE(registry.Count() == 4);

  // Try to add one more - should fail
  auto r = registry.Register("service5", "127.0.0.1", 8085);
  REQUIRE(!r.has_value());
  REQUIRE(r.get_error() == osp::ServiceError::kBindFailed);
  REQUIRE(registry.Count() == 4);

  // After unregister, should be able to add again
  registry.Unregister("service2");
  REQUIRE(registry.Count() == 3);

  auto r2 = registry.Register("service5", "127.0.0.1", 8085);
  REQUIRE(r2.has_value());
  REQUIRE(registry.Count() == 4);
}

// ============================================================================
// 14. AsyncClient - Connect and call
// ============================================================================

TEST_CASE("service - AsyncClient connect and call", "[service][async]") {
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

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect async client
  auto client_r = osp::AsyncClient<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());

  auto client = std::move(client_r.value());
  REQUIRE(client.IsConnected());

  // Send async request
  AddRequest req{15, 25};
  bool initiated = client.CallAsync(req);
  REQUIRE(initiated);

  // Wait for result
  auto resp_r = client.GetResult(5000);
  REQUIRE(resp_r.has_value());
  REQUIRE(resp_r.value().sum == 40);

  client.Close();
  service.Stop();
}

// ============================================================================
// 15. AsyncClient - IsReady check
// ============================================================================

TEST_CASE("service - AsyncClient IsReady check", "[service][async]") {
  // Start service with slow handler
  osp::Service<AddRequest, AddResponse>::Config cfg;
  cfg.port = 0;  // OS-assigned
  osp::Service<AddRequest, AddResponse> service(cfg);

  service.SetHandler(
      [](const AddRequest& req, void* /*ctx*/) -> AddResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        AddResponse resp;
        resp.sum = req.a + req.b;
        return resp;
      });

  auto start_r = service.Start();
  REQUIRE(start_r.has_value());

  uint16_t port = service.GetPort();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto client_r = osp::AsyncClient<AddRequest, AddResponse>::Connect("127.0.0.1", port);
  REQUIRE(client_r.has_value());
  auto client = std::move(client_r.value());

  // Send async request
  AddRequest req{10, 20};
  bool initiated = client.CallAsync(req);
  REQUIRE(initiated);

  // Check IsReady immediately - should be false
  REQUIRE(!client.IsReady());

  // Wait a bit and check again
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(client.IsReady());

  // Get result
  auto resp_r = client.GetResult(100);
  REQUIRE(resp_r.has_value());
  REQUIRE(resp_r.value().sum == 30);

  client.Close();
  service.Stop();
}

// ============================================================================
// 16. AsyncCallResult - Default state
// ============================================================================

TEST_CASE("service - AsyncCallResult default state", "[service][async]") {
  osp::AsyncCallResult<AddResponse> result;

  // Check default state
  REQUIRE(result.completed == false);
  REQUIRE(result.success == false);

  // Manually set values
  result.response.sum = 42;
  result.error = osp::ServiceError::kTimeout;
  result.completed = true;
  result.success = false;

  REQUIRE(result.response.sum == 42);
  REQUIRE(result.error == osp::ServiceError::kTimeout);
  REQUIRE(result.completed == true);
  REQUIRE(result.success == false);
}
