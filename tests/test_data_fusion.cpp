/**
 * @file test_data_fusion.cpp
 * @brief Tests for data_fusion.hpp (multi-message alignment and time sync)
 */

#include "osp/data_fusion.hpp"
#include "osp/bus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <variant>

// --- Test message types ---

struct SensorA {
  uint32_t value;
};

struct SensorB {
  float reading;
};

struct SensorC {
  int16_t status;
};

using TestPayload = std::variant<SensorA, SensorB, SensorC>;
using TestBus = osp::AsyncBus<TestPayload>;
using TestEnvelope = osp::MessageEnvelope<TestPayload>;

// ============================================================================
// FusedSubscription Tests
// ============================================================================

TEST_CASE("data_fusion - callback fires when all types received",
          "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static uint32_t captured_a = 0;
  static float captured_b = 0.0f;
  static int fire_count = 0;
  captured_a = 0;
  captured_b = 0.0f;
  fire_count = 0;

  osp::FusedSubscription<TestPayload, SensorA, SensorB> fusion;
  fusion.SetCallback([](const std::tuple<SensorA, SensorB>& data) {
    captured_a = std::get<0>(data).value;
    captured_b = std::get<1>(data).reading;
    ++fire_count;
  });

  REQUIRE(fusion.Activate(bus));

  // Publish both types
  bus.Publish(SensorA{42}, 1);
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);  // Only one type so far

  bus.Publish(SensorB{3.14f}, 2);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);
  REQUIRE(captured_a == 42);
  REQUIRE(captured_b == 3.14f);

  fusion.Deactivate(bus);
}

TEST_CASE("data_fusion - callback does not fire until all types received",
          "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::FusedSubscription<TestPayload, SensorA, SensorB, SensorC> fusion;
  fusion.SetCallback(
      [](const std::tuple<SensorA, SensorB, SensorC>&) { ++fire_count; });

  REQUIRE(fusion.Activate(bus));

  // Publish only two of three types
  bus.Publish(SensorA{1}, 1);
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);

  bus.Publish(SensorB{1.0f}, 2);
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);

  // Now publish the third
  bus.Publish(SensorC{99}, 3);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);

  fusion.Deactivate(bus);
}

TEST_CASE("data_fusion - auto-reset after firing", "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::FusedSubscription<TestPayload, SensorA, SensorB> fusion;
  fusion.SetCallback(
      [](const std::tuple<SensorA, SensorB>&) { ++fire_count; });

  REQUIRE(fusion.Activate(bus));

  // First cycle
  bus.Publish(SensorA{1}, 1);
  bus.Publish(SensorB{1.0f}, 2);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);

  // After auto-reset, one message should not fire
  bus.Publish(SensorA{2}, 1);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);  // Still 1, need SensorB again

  // Complete second cycle
  bus.Publish(SensorB{2.0f}, 2);
  bus.ProcessBatch();
  REQUIRE(fire_count == 2);

  fusion.Deactivate(bus);
}

TEST_CASE("data_fusion - activate/deactivate lifecycle", "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::FusedSubscription<TestPayload, SensorA, SensorB> fusion;
  fusion.SetCallback(
      [](const std::tuple<SensorA, SensorB>&) { ++fire_count; });

  // Can't deactivate before activate
  osp::DataFusionError err;
  REQUIRE(!fusion.Deactivate(bus, &err));
  REQUIRE(err == osp::DataFusionError::kNotActive);

  // Activate
  REQUIRE(fusion.Activate(bus));
  REQUIRE(fusion.IsActive());

  // Can't activate twice
  REQUIRE(!fusion.Activate(bus, &err));
  REQUIRE(err == osp::DataFusionError::kAlreadyActive);

  // Deactivate
  REQUIRE(fusion.Deactivate(bus));
  REQUIRE(!fusion.IsActive());

  // After deactivate, messages should not fire callback
  bus.Publish(SensorA{1}, 1);
  bus.Publish(SensorB{1.0f}, 2);
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);
}

TEST_CASE("data_fusion - callback not set returns error", "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  osp::FusedSubscription<TestPayload, SensorA, SensorB> fusion;
  // No SetCallback

  osp::DataFusionError err;
  REQUIRE(!fusion.Activate(bus, &err));
  REQUIRE(err == osp::DataFusionError::kCallbackNotSet);
}

// ============================================================================
// TimeSynchronizer Tests
// ============================================================================

TEST_CASE("data_fusion - time sync fires within time window",
          "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static uint32_t captured_a = 0;
  static float captured_b = 0.0f;
  static int fire_count = 0;
  captured_a = 0;
  captured_b = 0.0f;
  fire_count = 0;

  osp::TimeSynchronizer<TestPayload, SensorA, SensorB> sync;
  sync.SetCallback([](const std::tuple<SensorA, SensorB>& data) {
    captured_a = std::get<0>(data).value;
    captured_b = std::get<1>(data).reading;
    ++fire_count;
  });
  sync.SetTimeWindow(1000);  // 1ms window

  REQUIRE(sync.Activate(bus));

  // Publish with timestamps within window
  bus.PublishFast(SensorA{10}, 1, 5000);
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);

  bus.PublishFast(SensorB{2.5f}, 2, 5500);  // 500us apart, within 1000us
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);
  REQUIRE(captured_a == 10);
  REQUIRE(captured_b == 2.5f);

  sync.Deactivate(bus);
}

TEST_CASE("data_fusion - time sync does not fire when window expired",
          "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::TimeSynchronizer<TestPayload, SensorA, SensorB> sync;
  sync.SetCallback(
      [](const std::tuple<SensorA, SensorB>&) { ++fire_count; });
  sync.SetTimeWindow(100);  // 100us window

  REQUIRE(sync.Activate(bus));

  // Publish with timestamps far apart (exceeds 100us window)
  bus.PublishFast(SensorA{1}, 1, 1000);
  bus.ProcessBatch();

  bus.PublishFast(SensorB{1.0f}, 2, 2000);  // 1000us apart, window is 100us
  bus.ProcessBatch();
  REQUIRE(fire_count == 0);

  // The stale SensorA should have been dropped.
  // Now publish SensorA close to SensorB's timestamp
  bus.PublishFast(SensorA{2}, 1, 1950);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);  // Now within window of SensorB (2000-1950=50 < 100)

  sync.Deactivate(bus);
}

TEST_CASE("data_fusion - time sync resets after firing", "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::TimeSynchronizer<TestPayload, SensorA, SensorB> sync;
  sync.SetCallback(
      [](const std::tuple<SensorA, SensorB>&) { ++fire_count; });
  sync.SetTimeWindow(500);

  REQUIRE(sync.Activate(bus));

  // First cycle
  bus.PublishFast(SensorA{1}, 1, 1000);
  bus.PublishFast(SensorB{1.0f}, 2, 1200);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);

  // After reset, need both types again
  bus.PublishFast(SensorA{2}, 1, 2000);
  bus.ProcessBatch();
  REQUIRE(fire_count == 1);  // Only one type

  bus.PublishFast(SensorB{2.0f}, 2, 2100);
  bus.ProcessBatch();
  REQUIRE(fire_count == 2);

  sync.Deactivate(bus);
}

TEST_CASE("data_fusion - time sync handles rapid sequential messages",
          "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  fire_count = 0;

  osp::TimeSynchronizer<TestPayload, SensorA, SensorB> sync;
  sync.SetCallback(
      [](const std::tuple<SensorA, SensorB>&) { ++fire_count; });
  sync.SetTimeWindow(200);

  REQUIRE(sync.Activate(bus));

  // Rapid sequence of messages, all within window
  for (uint32_t i = 0; i < 5; ++i) {
    uint64_t base_ts = static_cast<uint64_t>(i) * 1000 + 10000;
    bus.PublishFast(SensorA{i}, 1, base_ts);
    bus.PublishFast(SensorB{static_cast<float>(i)}, 2, base_ts + 50);
    bus.ProcessBatch();
  }

  REQUIRE(fire_count == 5);

  sync.Deactivate(bus);
}

TEST_CASE("data_fusion - multiple fire cycles", "[data_fusion]") {
  TestBus::Instance().Reset();
  auto& bus = TestBus::Instance();

  static int fire_count = 0;
  static uint32_t last_a = 0;
  static float last_b = 0.0f;
  fire_count = 0;
  last_a = 0;
  last_b = 0.0f;

  osp::FusedSubscription<TestPayload, SensorA, SensorB> fusion;
  fusion.SetCallback([](const std::tuple<SensorA, SensorB>& data) {
    last_a = std::get<0>(data).value;
    last_b = std::get<1>(data).reading;
    ++fire_count;
  });

  REQUIRE(fusion.Activate(bus));

  // Run 10 fire cycles
  for (uint32_t i = 0; i < 10; ++i) {
    bus.Publish(SensorA{i * 10}, 1);
    bus.Publish(SensorB{static_cast<float>(i) * 0.5f}, 2);
    bus.ProcessBatch();
  }

  REQUIRE(fire_count == 10);
  REQUIRE(last_a == 90);
  REQUIRE(last_b == 4.5f);
  REQUIRE(fusion.FireCount() == 10);

  fusion.Deactivate(bus);
}
