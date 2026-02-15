/**
 * @file test_static_node.cpp
 * @brief Tests for static_node.hpp (compile-time handler binding node)
 */

#include "osp/static_node.hpp"
#include "osp/node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <variant>

// --- Test message types ---
// NOTE: structs must be >= 8 bytes to avoid GCC 14 wide-read optimization
// triggering ASan stack-buffer-overflow (8-byte memcpy on 4-byte struct).

struct SensorData {
  float temp;
  uint32_t seq = 0;
};

struct MotorCmd {
  int32_t speed;
  uint32_t flags = 0;
};

struct StatusMsg {
  uint8_t code;
  uint8_t reserved[7] = {};
};

using TestPayload = std::variant<SensorData, MotorCmd, StatusMsg>;
using TestBus = osp::AsyncBus<TestPayload>;

// Helper to reset bus between tests
struct StaticNodeBusFixture {
  StaticNodeBusFixture() { TestBus::Instance().Reset(); }
  ~StaticNodeBusFixture() { TestBus::Instance().Reset(); }
};

// ============================================================================
// Test Handlers
// ============================================================================

// Handler that tracks all received messages
struct CountingHandler {
  int32_t sensor_count = 0;
  int32_t motor_count = 0;
  int32_t status_count = 0;
  float last_temp = 0.0f;
  int32_t last_speed = 0;
  uint8_t last_code = 0;
  uint32_t last_sender_id = 0;

  void operator()(const SensorData& d, const osp::MessageHeader& h) noexcept {
    ++sensor_count;
    last_temp = d.temp;
    last_sender_id = h.sender_id;
  }

  void operator()(const MotorCmd& c, const osp::MessageHeader& h) noexcept {
    ++motor_count;
    last_speed = c.speed;
    last_sender_id = h.sender_id;
  }

  void operator()(const StatusMsg& s, const osp::MessageHeader& h) noexcept {
    ++status_count;
    last_code = s.code;
    last_sender_id = h.sender_id;
  }
};

// Handler that only handles a subset of types (still must declare all)
struct SensorOnlyHandler {
  int32_t sensor_count = 0;
  float last_temp = 0.0f;

  void operator()(const SensorData& d, const osp::MessageHeader&) noexcept {
    ++sensor_count;
    last_temp = d.temp;
  }

  // No-op for other types
  void operator()(const MotorCmd&, const osp::MessageHeader&) noexcept {}
  void operator()(const StatusMsg&, const osp::MessageHeader&) noexcept {}
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("StaticNode - basic creation and accessors", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "test_static", 42, CountingHandler{});

  REQUIRE(std::strcmp(node.Name(), "test_static") == 0);
  REQUIRE(node.Id() == 42);
  REQUIRE(!node.IsStarted());
  REQUIRE(node.SubscriptionCount() == 0);
}

TEST_CASE("StaticNode - name truncation", "[static_node]") {
  StaticNodeBusFixture fix;

  const char* long_name = "this_is_a_very_long_static_node_name_exceeding_max";
  osp::StaticNode<TestPayload, CountingHandler> node(
      long_name, 1, CountingHandler{});

  REQUIRE(std::strlen(node.Name()) == osp::kNodeNameMaxLen);
}

TEST_CASE("StaticNode - null name", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      nullptr, 0, CountingHandler{});

  REQUIRE(std::strcmp(node.Name(), "") == 0);
}

TEST_CASE("StaticNode - Start subscribes to all types", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "starter", 1, CountingHandler{});

  auto result = node.Start();
  REQUIRE(result.has_value());
  REQUIRE(node.IsStarted());
  // 3 types in variant -> 3 subscriptions
  REQUIRE(node.SubscriptionCount() == 3);
}

TEST_CASE("StaticNode - double Start returns error", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "double_start", 1, CountingHandler{});

  auto r1 = node.Start();
  REQUIRE(r1.has_value());

  auto r2 = node.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::NodeError::kAlreadyStarted);
}

TEST_CASE("StaticNode - basic dispatch all types", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "dispatch", 10, CountingHandler{});

  auto result = node.Start();
  REQUIRE(result.has_value());

  // Publish one of each type
  node.Publish(SensorData{25.5f});
  node.Publish(MotorCmd{100});
  node.Publish(StatusMsg{42});
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.status_count == 1);
  REQUIRE(handler.last_temp == 25.5f);
  REQUIRE(handler.last_speed == 100);
  REQUIRE(handler.last_code == 42);
  REQUIRE(handler.last_sender_id == 10);
}

TEST_CASE("StaticNode - multiple messages of same type", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "multi_msg", 1, CountingHandler{});

  node.Start();

  node.Publish(SensorData{1.0f});
  node.Publish(SensorData{2.0f});
  node.Publish(SensorData{3.0f});
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 3);
  REQUIRE(handler.last_temp == 3.0f);
}

TEST_CASE("StaticNode - topic filtering", "[static_node]") {
  StaticNodeBusFixture fix;

  // Node with topic filter "sensor/imu"
  osp::StaticNode<TestPayload, CountingHandler> node(
      "topic_filter", 1, CountingHandler{}, "sensor/imu");

  node.Start();

  // Publish with matching topic - should be received
  TestBus::Instance().PublishTopic(TestPayload(SensorData{10.0f}), 2, "sensor/imu");
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);
  REQUIRE(handler.last_temp == 10.0f);

  // Publish with non-matching topic - should be filtered out
  TestBus::Instance().PublishTopic(TestPayload(SensorData{20.0f}), 2, "sensor/gps");
  node.SpinOnce();

  REQUIRE(handler.sensor_count == 1);  // No change

  // Publish without topic - should be filtered out (topic_hash 0 != filter)
  TestBus::Instance().Publish(TestPayload(SensorData{30.0f}), 2);
  node.SpinOnce();

  REQUIRE(handler.sensor_count == 1);  // No change
}

TEST_CASE("StaticNode - no topic filter receives all", "[static_node]") {
  StaticNodeBusFixture fix;

  // Node without topic filter
  osp::StaticNode<TestPayload, CountingHandler> node(
      "no_filter", 1, CountingHandler{});

  node.Start();

  // Publish without topic
  TestBus::Instance().Publish(TestPayload(SensorData{1.0f}), 2);
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);

  // Publish with a topic - still received (no filter set)
  TestBus::Instance().PublishTopic(TestPayload(SensorData{2.0f}), 2, "any_topic");
  node.SpinOnce();

  REQUIRE(handler.sensor_count == 2);
}

TEST_CASE("StaticNode - bus injection", "[static_node]") {
  // Use a separate variant type to get a distinct bus singleton
  struct InjSensor { float val; uint32_t pad_ = 0; };
  struct InjMotor { int32_t spd; uint32_t pad_ = 0; };
  using InjPayload = std::variant<InjSensor, InjMotor>;
  using InjBus = osp::AsyncBus<InjPayload>;

  InjBus::Instance().Reset();

  struct InjHandler {
    int32_t motor_count = 0;
    int32_t last_speed = 0;
    uint32_t last_sender_id = 0;
    void operator()(const InjSensor&, const osp::MessageHeader&) noexcept {}
    void operator()(const InjMotor& m, const osp::MessageHeader& h) noexcept {
      ++motor_count;
      last_speed = m.spd;
      last_sender_id = h.sender_id;
    }
  };

  // Inject the global singleton explicitly
  osp::StaticNode<InjPayload, InjHandler> node(
      "injected", 5, InjHandler{}, InjBus::Instance());

  node.Start();

  // Publish through the injected bus reference
  InjBus::Instance().Publish(InjPayload(InjMotor{77}), 3);
  InjBus::Instance().ProcessBatch();

  auto& handler = node.GetHandler();
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.last_speed == 77);
  REQUIRE(handler.last_sender_id == 3);

  InjBus::Instance().Reset();
}

TEST_CASE("StaticNode - bus injection with topic", "[static_node]") {
  // Use a separate variant type to get a distinct bus singleton
  struct InjSensor2 { float val; uint32_t pad_ = 0; };
  struct InjMotor2 { int32_t spd; uint32_t pad_ = 0; };
  using InjPayload2 = std::variant<InjSensor2, InjMotor2>;
  using InjBus2 = osp::AsyncBus<InjPayload2>;

  InjBus2::Instance().Reset();

  struct InjHandler2 {
    int32_t motor_count = 0;
    int32_t last_speed = 0;
    void operator()(const InjSensor2&, const osp::MessageHeader&) noexcept {}
    void operator()(const InjMotor2& m, const osp::MessageHeader&) noexcept {
      ++motor_count;
      last_speed = m.spd;
    }
  };

  osp::StaticNode<InjPayload2, InjHandler2> node(
      "injected_topic", 5, InjHandler2{}, "ctrl/motor", InjBus2::Instance());

  node.Start();

  // Publish with matching topic
  InjBus2::Instance().PublishTopic(InjPayload2(InjMotor2{55}), 3, "ctrl/motor");
  InjBus2::Instance().ProcessBatch();

  auto& handler = node.GetHandler();
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.last_speed == 55);

  // Publish with wrong topic - filtered
  InjBus2::Instance().PublishTopic(InjPayload2(InjMotor2{66}), 3, "ctrl/servo");
  InjBus2::Instance().ProcessBatch();

  REQUIRE(handler.motor_count == 1);  // No change

  InjBus2::Instance().Reset();
}

TEST_CASE("StaticNode - Stop clears subscriptions", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "stoppable", 1, CountingHandler{});

  node.Start();
  REQUIRE(node.SubscriptionCount() == 3);

  node.Publish(SensorData{1.0f});
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);

  node.Stop();
  REQUIRE(!node.IsStarted());
  REQUIRE(node.SubscriptionCount() == 0);

  // Re-publish should not be received
  TestBus::Instance().Publish(TestPayload(SensorData{2.0f}), 0);
  TestBus::Instance().ProcessBatch();

  REQUIRE(handler.sensor_count == 1);  // No change
}

TEST_CASE("StaticNode - Stop and restart", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "restartable", 1, CountingHandler{});

  // First start
  auto r1 = node.Start();
  REQUIRE(r1.has_value());
  REQUIRE(node.SubscriptionCount() == 3);

  node.Publish(SensorData{1.0f});
  node.SpinOnce();
  REQUIRE(node.GetHandler().sensor_count == 1);

  // Stop
  node.Stop();
  REQUIRE(node.SubscriptionCount() == 0);

  // Restart
  auto restart_result = node.Start();
  REQUIRE(restart_result.has_value());
  REQUIRE(node.SubscriptionCount() == 3);

  node.Publish(SensorData{2.0f});
  node.SpinOnce();
  REQUIRE(node.GetHandler().sensor_count == 2);
}

TEST_CASE("StaticNode - RAII unsubscribe on destroy", "[static_node]") {
  StaticNodeBusFixture fix;

  std::atomic<int32_t> external_count{0};

  // Handler that writes to an external counter
  struct ExternalHandler {
    std::atomic<int32_t>* counter;

    void operator()(const SensorData&, const osp::MessageHeader&) noexcept {
      counter->fetch_add(1);
    }
    void operator()(const MotorCmd&, const osp::MessageHeader&) noexcept {}
    void operator()(const StatusMsg&, const osp::MessageHeader&) noexcept {}
  };

  {
    osp::StaticNode<TestPayload, ExternalHandler> node(
        "raii_test", 1, ExternalHandler{&external_count});

    node.Start();

    TestBus::Instance().Publish(TestPayload(SensorData{1.0f}), 0);
    TestBus::Instance().ProcessBatch();
    REQUIRE(external_count.load() == 1);
  }
  // Node destroyed - subscriptions cleaned up

  TestBus::Instance().Publish(TestPayload(SensorData{2.0f}), 0);
  TestBus::Instance().ProcessBatch();
  REQUIRE(external_count.load() == 1);  // No change
}

TEST_CASE("StaticNode - multiple nodes on same bus", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node_a(
      "node_a", 1, CountingHandler{});
  osp::StaticNode<TestPayload, CountingHandler> node_b(
      "node_b", 2, CountingHandler{});

  node_a.Start();
  node_b.Start();

  // Both should have 3 subscriptions each
  REQUIRE(node_a.SubscriptionCount() == 3);
  REQUIRE(node_b.SubscriptionCount() == 3);

  // Publish one message - both nodes should receive it
  TestBus::Instance().Publish(TestPayload(SensorData{42.0f}), 99);
  TestBus::Instance().ProcessBatch();

  REQUIRE(node_a.GetHandler().sensor_count == 1);
  REQUIRE(node_a.GetHandler().last_temp == 42.0f);
  REQUIRE(node_b.GetHandler().sensor_count == 1);
  REQUIRE(node_b.GetHandler().last_temp == 42.0f);
}

TEST_CASE("StaticNode - mixed with regular Node", "[static_node]") {
  StaticNodeBusFixture fix;

  // StaticNode and regular Node on same bus
  osp::StaticNode<TestPayload, CountingHandler> static_node(
      "static", 1, CountingHandler{});
  osp::Node<TestPayload> regular_node("regular", 2);

  static_node.Start();

  int32_t regular_count = 0;
  regular_node.Subscribe<SensorData>(
      [&regular_count](const SensorData&, const osp::MessageHeader&) {
        ++regular_count;
      });

  // Publish via static node
  static_node.Publish(SensorData{10.0f});
  static_node.SpinOnce();

  // Both should receive it
  REQUIRE(static_node.GetHandler().sensor_count == 1);
  REQUIRE(regular_count == 1);
}

TEST_CASE("StaticNode - selective handler", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, SensorOnlyHandler> node(
      "selective", 1, SensorOnlyHandler{});

  node.Start();

  // Publish all types
  node.Publish(SensorData{99.9f});
  node.Publish(MotorCmd{50});
  node.Publish(StatusMsg{7});
  node.SpinOnce();

  // Only SensorData handler counts
  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);
  REQUIRE(handler.last_temp == 99.9f);
}

TEST_CASE("StaticNode - Publish and CreatePublisher", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "publisher", 5, CountingHandler{});

  node.Start();

  auto pub = node.CreatePublisher<MotorCmd>();
  REQUIRE(pub.IsValid());

  REQUIRE(pub.Publish(MotorCmd{200}));
  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.last_speed == 200);
  REQUIRE(handler.last_sender_id == 5);
}

TEST_CASE("StaticNode - PublishWithPriority", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "prio_pub", 1, CountingHandler{});

  // Subscribe a regular Node to check priority
  osp::Node<TestPayload> checker("checker", 2);
  osp::MessagePriority captured_prio = osp::MessagePriority::kMedium;
  checker.Subscribe<SensorData>(
      [&captured_prio](const SensorData&, const osp::MessageHeader& h) {
        captured_prio = h.priority;
      });

  node.Start();

  node.PublishWithPriority(SensorData{1.0f}, osp::MessagePriority::kHigh);
  node.SpinOnce();

  REQUIRE(captured_prio == osp::MessagePriority::kHigh);
}

TEST_CASE("StaticNode - topic publish", "[static_node]") {
  StaticNodeBusFixture fix;

  // Subscriber with topic filter
  osp::StaticNode<TestPayload, CountingHandler> sub_node(
      "topic_sub", 1, CountingHandler{}, "data/stream");

  sub_node.Start();

  // Publisher node (no topic filter needed for publishing)
  osp::StaticNode<TestPayload, CountingHandler> pub_node(
      "topic_pub", 2, CountingHandler{});

  pub_node.Start();

  // Publish with matching topic
  pub_node.Publish(SensorData{50.0f}, "data/stream");
  pub_node.SpinOnce();

  REQUIRE(sub_node.GetHandler().sensor_count == 1);
  REQUIRE(sub_node.GetHandler().last_temp == 50.0f);

  // Publish with wrong topic
  pub_node.Publish(SensorData{60.0f}, "data/other");
  pub_node.SpinOnce();

  REQUIRE(sub_node.GetHandler().sensor_count == 1);  // No change
}

TEST_CASE("StaticNode - sender ID tracking", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> receiver(
      "receiver", 100, CountingHandler{});

  receiver.Start();

  // Publish from different sender IDs
  TestBus::Instance().Publish(TestPayload(SensorData{1.0f}), 10);
  TestBus::Instance().ProcessBatch();
  REQUIRE(receiver.GetHandler().last_sender_id == 10);

  TestBus::Instance().Publish(TestPayload(SensorData{2.0f}), 20);
  TestBus::Instance().ProcessBatch();
  REQUIRE(receiver.GetHandler().last_sender_id == 20);
}

// ============================================================================
// Direct Dispatch (ProcessBatchWith) Tests
// ============================================================================

TEST_CASE("StaticNode direct dispatch without Start", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "direct_no_start", 7, CountingHandler{});

  // Do NOT call Start() - node should use direct dispatch mode
  REQUIRE(!node.IsStarted());
  REQUIRE(node.SubscriptionCount() == 0);

  // Publish messages via bus directly (not via node.Publish)
  TestBus::Instance().Publish(TestPayload(SensorData{11.0f}), 3);
  TestBus::Instance().Publish(TestPayload(MotorCmd{200}), 4);

  // SpinOnce should use ProcessBatchWith (direct dispatch)
  uint32_t processed = node.SpinOnce();
  REQUIRE(processed == 2);

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.last_temp == 11.0f);
  REQUIRE(handler.last_speed == 200);

  // Confirm node state unchanged - still not started
  REQUIRE(!node.IsStarted());
  REQUIRE(node.SubscriptionCount() == 0);
}

TEST_CASE("StaticNode direct dispatch message delivery", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "direct_delivery", 10, CountingHandler{});

  // No Start() - direct dispatch mode

  // Publish one of each type via bus
  TestBus::Instance().Publish(TestPayload(SensorData{36.6f}), 1);
  TestBus::Instance().Publish(TestPayload(MotorCmd{-500}), 2);
  TestBus::Instance().Publish(TestPayload(StatusMsg{0xAB}), 3);

  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 1);
  REQUIRE(handler.motor_count == 1);
  REQUIRE(handler.status_count == 1);
  REQUIRE(handler.last_temp == 36.6f);
  REQUIRE(handler.last_speed == -500);
  REQUIRE(handler.last_code == 0xAB);
  // last_sender_id should be from the last processed message
  REQUIRE(handler.last_sender_id == 3);
}

TEST_CASE("StaticNode direct dispatch handler state", "[static_node]") {
  StaticNodeBusFixture fix;

  osp::StaticNode<TestPayload, CountingHandler> node(
      "direct_state", 1, CountingHandler{});

  // No Start() - direct dispatch mode

  // Publish 5 SensorData messages
  TestBus::Instance().Publish(TestPayload(SensorData{10.0f}), 1);
  TestBus::Instance().Publish(TestPayload(SensorData{20.0f}), 1);
  TestBus::Instance().Publish(TestPayload(SensorData{30.0f}), 1);
  TestBus::Instance().Publish(TestPayload(SensorData{40.0f}), 1);
  TestBus::Instance().Publish(TestPayload(SensorData{50.0f}), 1);

  node.SpinOnce();

  auto& handler = node.GetHandler();
  REQUIRE(handler.sensor_count == 5);
  REQUIRE(handler.last_temp == 50.0f);
  REQUIRE(handler.motor_count == 0);
  REQUIRE(handler.status_count == 0);
}

TEST_CASE("StaticNode direct vs callback mode parity", "[static_node]") {
  // Use separate variant types so each mode has its own bus instance
  struct ParitySensor { float temp; uint32_t pad_ = 0; };
  struct ParityMotor { int32_t speed; uint32_t pad_ = 0; };
  struct ParityStatus { uint8_t code; uint8_t pad_[7] = {}; };

  using DirectPayload = std::variant<ParitySensor, ParityMotor, ParityStatus>;
  using DirectBus = osp::AsyncBus<DirectPayload>;

  struct ParityHandler {
    int32_t sensor_count = 0;
    int32_t motor_count = 0;
    int32_t status_count = 0;
    float last_temp = 0.0f;
    int32_t last_speed = 0;
    uint8_t last_code = 0;
    uint32_t last_sender_id = 0;

    void operator()(const ParitySensor& d,
                    const osp::MessageHeader& h) noexcept {
      ++sensor_count;
      last_temp = d.temp;
      last_sender_id = h.sender_id;
    }
    void operator()(const ParityMotor& c,
                    const osp::MessageHeader& h) noexcept {
      ++motor_count;
      last_speed = c.speed;
      last_sender_id = h.sender_id;
    }
    void operator()(const ParityStatus& s,
                    const osp::MessageHeader& h) noexcept {
      ++status_count;
      last_code = s.code;
      last_sender_id = h.sender_id;
    }
  };

  DirectBus::Instance().Reset();

  // --- Callback mode (with Start) ---
  osp::StaticNode<DirectPayload, ParityHandler> callback_node(
      "callback_mode", 5, ParityHandler{});
  callback_node.Start();

  DirectBus::Instance().Publish(DirectPayload(ParitySensor{25.5f}), 10);
  DirectBus::Instance().Publish(DirectPayload(ParityMotor{300}), 20);
  DirectBus::Instance().Publish(DirectPayload(ParityStatus{0x42}), 30);

  callback_node.SpinOnce();

  auto& cb_handler = callback_node.GetHandler();

  // Save callback mode results
  int32_t cb_sensor = cb_handler.sensor_count;
  int32_t cb_motor = cb_handler.motor_count;
  int32_t cb_status = cb_handler.status_count;
  float cb_temp = cb_handler.last_temp;
  int32_t cb_speed = cb_handler.last_speed;
  uint8_t cb_code = cb_handler.last_code;

  // Clean up callback node and reset bus
  callback_node.Stop();
  DirectBus::Instance().Reset();

  // --- Direct mode (without Start) ---
  osp::StaticNode<DirectPayload, ParityHandler> direct_node(
      "direct_mode", 5, ParityHandler{});
  // No Start() call - uses direct dispatch

  DirectBus::Instance().Publish(DirectPayload(ParitySensor{25.5f}), 10);
  DirectBus::Instance().Publish(DirectPayload(ParityMotor{300}), 20);
  DirectBus::Instance().Publish(DirectPayload(ParityStatus{0x42}), 30);

  direct_node.SpinOnce();

  auto& dir_handler = direct_node.GetHandler();

  // Verify both modes produce identical handler state
  REQUIRE(dir_handler.sensor_count == cb_sensor);
  REQUIRE(dir_handler.motor_count == cb_motor);
  REQUIRE(dir_handler.status_count == cb_status);
  REQUIRE(dir_handler.last_temp == cb_temp);
  REQUIRE(dir_handler.last_speed == cb_speed);
  REQUIRE(dir_handler.last_code == cb_code);

  DirectBus::Instance().Reset();
}
