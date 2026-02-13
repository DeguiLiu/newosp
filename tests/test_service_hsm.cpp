/**
 * @file test_service_hsm.cpp
 * @brief Unit tests for HsmService.
 */

#include "osp/service_hsm.hpp"
#include "osp/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("service_hsm - Initial state is Idle", "[HsmService]") {
  osp::HsmService<32> service;

  CHECK(std::strcmp(service.GetState(), "NotStarted") == 0);
  CHECK(service.IsIdle());
  CHECK_FALSE(service.IsListening());
  CHECK_FALSE(service.IsActive());
  CHECK_FALSE(service.IsError());
  CHECK_FALSE(service.IsShuttingDown());
}

TEST_CASE("service_hsm - Start transitions to Listening", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();

  CHECK(std::strcmp(service.GetState(), "Listening") == 0);
  CHECK(service.IsListening());
  CHECK_FALSE(service.IsIdle());
  CHECK_FALSE(service.IsActive());
  CHECK(service.GetActiveClients() == 0U);
}

TEST_CASE("service_hsm - Client connect transitions to Active", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  CHECK(service.IsListening());

  service.OnClientConnect();

  CHECK(std::strcmp(service.GetState(), "Active") == 0);
  CHECK(service.IsActive());
  CHECK_FALSE(service.IsListening());
  CHECK(service.GetActiveClients() == 1U);
}

TEST_CASE("service_hsm - Client disconnect returns to Listening", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  service.OnClientConnect();
  CHECK(service.IsActive());
  CHECK(service.GetActiveClients() == 1U);

  service.OnClientDisconnect();

  CHECK(std::strcmp(service.GetState(), "Listening") == 0);
  CHECK(service.IsListening());
  CHECK_FALSE(service.IsActive());
  CHECK(service.GetActiveClients() == 0U);
}

TEST_CASE("service_hsm - Multiple clients stay Active", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  service.OnClientConnect();
  CHECK(service.GetActiveClients() == 1U);

  service.OnClientConnect();
  CHECK(service.GetActiveClients() == 2U);
  CHECK(service.IsActive());

  service.OnClientConnect();
  CHECK(service.GetActiveClients() == 3U);
  CHECK(service.IsActive());

  // Disconnect one client, should stay Active
  service.OnClientDisconnect();
  CHECK(service.GetActiveClients() == 2U);
  CHECK(service.IsActive());

  // Disconnect another, still Active
  service.OnClientDisconnect();
  CHECK(service.GetActiveClients() == 1U);
  CHECK(service.IsActive());

  // Disconnect last client, return to Listening
  service.OnClientDisconnect();
  CHECK(service.GetActiveClients() == 0U);
  CHECK(service.IsListening());
}

TEST_CASE("service_hsm - Error transitions to Error state", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  CHECK(service.IsListening());

  int32_t error_code = 42;
  service.OnError(error_code);

  CHECK(std::strcmp(service.GetState(), "Error") == 0);
  CHECK(service.IsError());
  CHECK_FALSE(service.IsListening());
  CHECK(service.GetErrorCode() == 42);
}

TEST_CASE("service_hsm - Recover from Error returns to Idle", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  service.OnClientConnect();
  service.OnClientConnect();
  CHECK(service.GetActiveClients() == 2U);

  service.OnError(99);
  CHECK(service.IsError());
  CHECK(service.GetErrorCode() == 99);

  service.Recover();

  CHECK(std::strcmp(service.GetState(), "Idle") == 0);
  CHECK(service.IsIdle());
  CHECK_FALSE(service.IsError());
  CHECK(service.GetErrorCode() == 0);
  CHECK(service.GetActiveClients() == 0U);  // Reset on recovery
}

TEST_CASE("service_hsm - Stop transitions to ShuttingDown", "[HsmService]") {
  osp::HsmService<32> service;

  service.Start();
  service.OnClientConnect();
  CHECK(service.IsActive());

  service.Stop();

  CHECK(std::strcmp(service.GetState(), "ShuttingDown") == 0);
  CHECK(service.IsShuttingDown());
  CHECK_FALSE(service.IsActive());
}

TEST_CASE("service_hsm - Error callback fires", "[HsmService]") {
  osp::HsmService<32> service;

  std::atomic<bool> callback_fired{false};
  std::atomic<int32_t> received_error_code{0};

  struct Context {
    std::atomic<bool>* fired;
    std::atomic<int32_t>* error_code;
  };
  Context context{&callback_fired, &received_error_code};

  service.SetErrorCallback(
      [](int32_t error_code, void* ctx) {
        auto* context = static_cast<Context*>(ctx);
        context->fired->store(true);
        context->error_code->store(error_code);
      },
      &context);

  service.Start();

  int32_t test_error = 123;
  service.OnError(test_error);

  CHECK(callback_fired.load());
  CHECK(received_error_code.load() == 123);
  CHECK(service.IsError());
}

TEST_CASE("service_hsm - Shutdown callback fires", "[HsmService]") {
  osp::HsmService<32> service;

  std::atomic<bool> callback_fired{false};

  service.SetShutdownCallback(
      [](void* ctx) {
        auto* fired = static_cast<std::atomic<bool>*>(ctx);
        fired->store(true);
      },
      &callback_fired);

  service.Start();
  service.OnClientConnect();
  service.OnClientConnect();
  CHECK(service.GetActiveClients() == 2U);

  service.Stop();

  CHECK(callback_fired.load());
  CHECK(service.IsShuttingDown());
  CHECK(service.GetActiveClients() == 0U);  // Reset on shutdown
}
