/**
 * @file test_transport_factory.cpp
 * @brief Tests for transport_factory.hpp
 */

#include "osp/transport_factory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// ============================================================================
// TransportFactory::DetectBestTransport tests
// ============================================================================

TEST_CASE("transport_factory - DetectBestTransport local inproc",
          "[transport_factory]") {
  osp::TransportConfig cfg;
  std::strcpy(cfg.remote_host, "127.0.0.1");
  cfg.shm_channel_name[0] = '\0';  // Empty shm channel

  auto type = osp::TransportFactory::DetectBestTransport(cfg);
  REQUIRE(type == osp::TransportType::kInproc);
}

TEST_CASE("transport_factory - DetectBestTransport local shm",
          "[transport_factory]") {
  osp::TransportConfig cfg;
  std::strcpy(cfg.remote_host, "127.0.0.1");
  std::strcpy(cfg.shm_channel_name, "test_channel");

  auto type = osp::TransportFactory::DetectBestTransport(cfg);
  REQUIRE(type == osp::TransportType::kShm);
}

TEST_CASE("transport_factory - DetectBestTransport remote tcp",
          "[transport_factory]") {
  osp::TransportConfig cfg;
  std::strcpy(cfg.remote_host, "192.168.1.100");
  cfg.shm_channel_name[0] = '\0';

  auto type = osp::TransportFactory::DetectBestTransport(cfg);
  REQUIRE(type == osp::TransportType::kTcp);
}

TEST_CASE("transport_factory - DetectBestTransport auto resolution",
          "[transport_factory]") {
  // Test auto resolution with localhost
  osp::TransportConfig cfg1;
  std::strcpy(cfg1.remote_host, "localhost");
  cfg1.shm_channel_name[0] = '\0';
  cfg1.type = osp::TransportType::kAuto;

  auto type1 = osp::TransportFactory::DetectBestTransport(cfg1);
  REQUIRE(type1 == osp::TransportType::kInproc);

  // Test auto resolution with localhost and shm
  osp::TransportConfig cfg2;
  std::strcpy(cfg2.remote_host, "localhost");
  std::strcpy(cfg2.shm_channel_name, "my_shm");
  cfg2.type = osp::TransportType::kAuto;

  auto type2 = osp::TransportFactory::DetectBestTransport(cfg2);
  REQUIRE(type2 == osp::TransportType::kShm);

  // Test auto resolution with remote host
  osp::TransportConfig cfg3;
  std::strcpy(cfg3.remote_host, "10.0.0.1");
  cfg3.type = osp::TransportType::kAuto;

  auto type3 = osp::TransportFactory::DetectBestTransport(cfg3);
  REQUIRE(type3 == osp::TransportType::kTcp);
}

TEST_CASE("transport_factory - localhost detection", "[transport_factory]") {
  // Test with "localhost" string
  osp::TransportConfig cfg1;
  std::strcpy(cfg1.remote_host, "localhost");
  cfg1.shm_channel_name[0] = '\0';

  auto type1 = osp::TransportFactory::DetectBestTransport(cfg1);
  REQUIRE(type1 == osp::TransportType::kInproc);

  // Test with "127.0.0.1" string
  osp::TransportConfig cfg2;
  std::strcpy(cfg2.remote_host, "127.0.0.1");
  cfg2.shm_channel_name[0] = '\0';

  auto type2 = osp::TransportFactory::DetectBestTransport(cfg2);
  REQUIRE(type2 == osp::TransportType::kInproc);
}

TEST_CASE("transport_factory - empty host defaults to inproc",
          "[transport_factory]") {
  osp::TransportConfig cfg;
  cfg.remote_host[0] = '\0';  // Empty host
  cfg.shm_channel_name[0] = '\0';

  auto type = osp::TransportFactory::DetectBestTransport(cfg);
  REQUIRE(type == osp::TransportType::kInproc);
}

// ============================================================================
// TransportFactory::TransportTypeName tests
// ============================================================================

TEST_CASE("transport_factory - TransportTypeName strings",
          "[transport_factory]") {
  REQUIRE(std::strcmp(
              osp::TransportFactory::TransportTypeName(osp::TransportType::kInproc),
              "inproc") == 0);
  REQUIRE(std::strcmp(
              osp::TransportFactory::TransportTypeName(osp::TransportType::kShm),
              "shm") == 0);
  REQUIRE(std::strcmp(
              osp::TransportFactory::TransportTypeName(osp::TransportType::kTcp),
              "tcp") == 0);
  REQUIRE(std::strcmp(
              osp::TransportFactory::TransportTypeName(osp::TransportType::kAuto),
              "auto") == 0);
}

// ============================================================================
// TransportSelector tests
// ============================================================================

struct DummyPayload {};

TEST_CASE("transport_factory - TransportSelector configure auto",
          "[transport_factory]") {
  osp::TransportSelector<DummyPayload> selector;

  // Configure with auto and local host
  osp::TransportConfig cfg;
  cfg.type = osp::TransportType::kAuto;
  std::strcpy(cfg.remote_host, "127.0.0.1");
  cfg.shm_channel_name[0] = '\0';

  selector.Configure(cfg);
  REQUIRE(selector.ResolvedType() == osp::TransportType::kInproc);

  // Configure with auto and shm
  osp::TransportConfig cfg2;
  cfg2.type = osp::TransportType::kAuto;
  std::strcpy(cfg2.remote_host, "localhost");
  std::strcpy(cfg2.shm_channel_name, "test_shm");

  selector.Configure(cfg2);
  REQUIRE(selector.ResolvedType() == osp::TransportType::kShm);

  // Configure with explicit type (no auto resolution)
  osp::TransportConfig cfg3;
  cfg3.type = osp::TransportType::kTcp;
  std::strcpy(cfg3.remote_host, "127.0.0.1");  // Local, but explicit TCP

  selector.Configure(cfg3);
  REQUIRE(selector.ResolvedType() == osp::TransportType::kTcp);
}

TEST_CASE("transport_factory - TransportSelector IsLocal/IsRemote",
          "[transport_factory]") {
  osp::TransportSelector<DummyPayload> selector;

  // Test inproc (local)
  osp::TransportConfig cfg1;
  cfg1.type = osp::TransportType::kInproc;
  selector.Configure(cfg1);
  REQUIRE(selector.IsLocal());
  REQUIRE(!selector.IsRemote());

  // Test shm (local)
  osp::TransportConfig cfg2;
  cfg2.type = osp::TransportType::kShm;
  selector.Configure(cfg2);
  REQUIRE(selector.IsLocal());
  REQUIRE(!selector.IsRemote());

  // Test tcp (remote)
  osp::TransportConfig cfg3;
  cfg3.type = osp::TransportType::kTcp;
  selector.Configure(cfg3);
  REQUIRE(!selector.IsLocal());
  REQUIRE(selector.IsRemote());
}

TEST_CASE("transport_factory - TransportConfig defaults", "[transport_factory]") {
  osp::TransportConfig cfg;

  // Check default values
  REQUIRE(cfg.type == osp::TransportType::kAuto);
  REQUIRE(std::strcmp(cfg.remote_host, "127.0.0.1") == 0);
  REQUIRE(cfg.remote_port == 0);
  REQUIRE(cfg.shm_channel_name[0] == '\0');
  REQUIRE(cfg.shm_slot_size == 4096);
  REQUIRE(cfg.shm_slot_count == 256);
}
