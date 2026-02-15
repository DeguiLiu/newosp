/**
 * @file test_node.cpp
 * @brief Tests for node.hpp (lightweight communication node)
 */

#include "osp/node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <variant>

// --- Test message types ---
// NOTE: structs must be >= 8 bytes to avoid GCC 14 wide-read optimization
// triggering ASan stack-buffer-overflow (8-byte memcpy on smaller struct).

struct Heartbeat {
  uint32_t seq;
  uint32_t reserved = 0;
};

struct Command {
  int32_t action;
  uint32_t flags = 0;
};

struct Status {
  uint8_t code;
  uint8_t reserved[7] = {};
};

using NodePayload = std::variant<Heartbeat, Command, Status>;
using NodeBus = osp::AsyncBus<NodePayload>;

// Helper to reset bus between tests
struct NodeBusFixture {
  NodeBusFixture() { NodeBus::Instance().Reset(); }
  ~NodeBusFixture() { NodeBus::Instance().Reset(); }
};

TEST_CASE("Node creation and name", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("test_node", 42);
  REQUIRE(std::strcmp(node.Name(), "test_node") == 0);
  REQUIRE(node.Id() == 42);
  REQUIRE(!node.IsStarted());
  REQUIRE(node.SubscriptionCount() == 0);
}

TEST_CASE("Node name truncation", "[node]") {
  NodeBusFixture fix;

  // Name longer than kNodeNameMaxLen (31) should be truncated
  const char* long_name = "this_is_a_very_long_node_name_that_exceeds_max";
  osp::Node<NodePayload> node(long_name, 1);
  REQUIRE(std::strlen(node.Name()) == osp::kNodeNameMaxLen);
}

TEST_CASE("Node null name", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node(nullptr, 0);
  REQUIRE(std::strcmp(node.Name(), "") == 0);
}

TEST_CASE("Node start and stop", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("lifecycle", 1);

  auto r1 = node.Start();
  REQUIRE(r1.has_value());
  REQUIRE(node.IsStarted());

  // Double start should fail
  auto r2 = node.Start();
  REQUIRE(!r2.has_value());
  REQUIRE(r2.get_error() == osp::NodeError::kAlreadyStarted);

  node.Stop();
  REQUIRE(!node.IsStarted());
}

TEST_CASE("Node publish and subscribe", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> pub_node("publisher", 1);
  osp::Node<NodePayload> sub_node("subscriber", 2);

  int received_seq = -1;
  uint32_t received_sender = 0;

  auto r = sub_node.Subscribe<Heartbeat>(
      [&received_seq, &received_sender](
          const Heartbeat& hb, const osp::MessageHeader& hdr) {
        received_seq = static_cast<int>(hb.seq);
        received_sender = hdr.sender_id;
      });
  REQUIRE(r.has_value());
  REQUIRE(sub_node.SubscriptionCount() == 1);

  REQUIRE(pub_node.Publish(Heartbeat{100}));

  // Process messages (any node can drive the consumer)
  pub_node.SpinOnce();

  REQUIRE(received_seq == 100);
  REQUIRE(received_sender == 1);  // pub_node's ID
}

TEST_CASE("Node SubscribeSimple", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("simple_sub", 1);

  int32_t received_action = 0;
  auto r = node.SubscribeSimple<Command>(
      [&received_action](const Command& cmd) {
        received_action = cmd.action;
      });
  REQUIRE(r.has_value());

  node.Publish(Command{42});
  node.SpinOnce();

  REQUIRE(received_action == 42);
}

TEST_CASE("Node RAII unsubscribe on destroy", "[node]") {
  NodeBusFixture fix;

  std::atomic<int> count{0};

  {
    osp::Node<NodePayload> temp_node("temp", 1);
    temp_node.Subscribe<Heartbeat>(
        [&count](const Heartbeat&, const osp::MessageHeader&) {
          count.fetch_add(1);
        });

    // Publish and process while node is alive
    NodeBus::Instance().Publish(Heartbeat{1}, 0);
    NodeBus::Instance().ProcessBatch();
    REQUIRE(count.load() == 1);
  }
  // temp_node destroyed - subscription should be cleaned up

  NodeBus::Instance().Publish(Heartbeat{2}, 0);
  NodeBus::Instance().ProcessBatch();
  REQUIRE(count.load() == 1);  // No change - callback removed
}

TEST_CASE("Node Stop clears subscriptions", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("stoppable", 1);
  int count = 0;

  node.Subscribe<Status>(
      [&count](const Status&, const osp::MessageHeader&) { ++count; });
  REQUIRE(node.SubscriptionCount() == 1);

  node.Publish(Status{1});
  node.SpinOnce();
  REQUIRE(count == 1);

  node.Stop();
  REQUIRE(node.SubscriptionCount() == 0);

  // Re-publish - should not be received
  NodeBus::Instance().Publish(Status{2}, 0);
  NodeBus::Instance().ProcessBatch();
  REQUIRE(count == 1);
}

TEST_CASE("Node Publisher factory", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("pub_factory", 5);
  auto pub = node.CreatePublisher<Command>();

  REQUIRE(pub.IsValid());

  int received = 0;
  uint32_t received_sender = 0;

  node.Subscribe<Command>(
      [&received, &received_sender](
          const Command& cmd, const osp::MessageHeader& hdr) {
        received = cmd.action;
        received_sender = hdr.sender_id;
      });

  REQUIRE(pub.Publish(Command{99}));
  node.SpinOnce();

  REQUIRE(received == 99);
  REQUIRE(received_sender == 5);  // Node ID
}

TEST_CASE("Node Publisher with priority", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("prio_pub", 1);
  auto pub = node.CreatePublisher<Heartbeat>();

  osp::MessagePriority captured_prio = osp::MessagePriority::kMedium;
  node.Subscribe<Heartbeat>(
      [&captured_prio](const Heartbeat&, const osp::MessageHeader& hdr) {
        captured_prio = hdr.priority;
      });

  REQUIRE(pub.PublishWithPriority(Heartbeat{1}, osp::MessagePriority::kHigh));
  node.SpinOnce();

  REQUIRE(captured_prio == osp::MessagePriority::kHigh);
}

TEST_CASE("Node multiple subscriptions", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("multi_sub", 1);

  int hb_count = 0;
  int cmd_count = 0;
  int status_count = 0;

  node.Subscribe<Heartbeat>(
      [&hb_count](const Heartbeat&, const osp::MessageHeader&) {
        ++hb_count;
      });
  node.Subscribe<Command>(
      [&cmd_count](const Command&, const osp::MessageHeader&) {
        ++cmd_count;
      });
  node.Subscribe<Status>(
      [&status_count](const Status&, const osp::MessageHeader&) {
        ++status_count;
      });

  REQUIRE(node.SubscriptionCount() == 3);

  node.Publish(Heartbeat{1});
  node.Publish(Command{2});
  node.Publish(Status{3});
  node.SpinOnce();

  REQUIRE(hb_count == 1);
  REQUIRE(cmd_count == 1);
  REQUIRE(status_count == 1);
}

TEST_CASE("Node cross-node communication", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> sensor("sensor", 1);
  osp::Node<NodePayload> controller("controller", 2);
  osp::Node<NodePayload> actuator("actuator", 3);

  // sensor publishes Heartbeat
  // controller subscribes to Heartbeat, publishes Command
  // actuator subscribes to Command

  int32_t final_action = 0;

  controller.Subscribe<Heartbeat>(
      [&controller](const Heartbeat& hb, const osp::MessageHeader&) {
        controller.Publish(Command{static_cast<int32_t>(hb.seq * 2)});
      });

  actuator.Subscribe<Command>(
      [&final_action](const Command& cmd, const osp::MessageHeader&) {
        final_action = cmd.action;
      });

  // Sensor publishes heartbeat
  sensor.Publish(Heartbeat{50});

  // Process: heartbeat dispatched, controller publishes command
  sensor.SpinOnce();

  // Process: command dispatched to actuator
  sensor.SpinOnce();

  REQUIRE(final_action == 100);  // 50 * 2
}

TEST_CASE("Node PublishWithPriority", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> node("prio", 1);

  osp::MessagePriority cap = osp::MessagePriority::kMedium;
  node.Subscribe<Heartbeat>(
      [&cap](const Heartbeat&, const osp::MessageHeader& hdr) {
        cap = hdr.priority;
      });

  node.PublishWithPriority(Heartbeat{1}, osp::MessagePriority::kLow);
  node.SpinOnce();
  REQUIRE(cap == osp::MessagePriority::kLow);
}

TEST_CASE("Node default Publisher", "[node]") {
  // Default-constructed publisher should not be valid
  osp::Publisher<Command, NodePayload> pub;
  REQUIRE(!pub.IsValid());
}

TEST_CASE("node - Topic publish and subscribe", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> pub_node("publisher", 1);
  osp::Node<NodePayload> sub_node("subscriber", 2);

  int received_seq = -1;
  uint32_t received_sender = 0;

  auto r = sub_node.Subscribe<Heartbeat>(
      "sensor/imu",
      [&received_seq, &received_sender](
          const Heartbeat& hb, const osp::MessageHeader& hdr) {
        received_seq = static_cast<int>(hb.seq);
        received_sender = hdr.sender_id;
      });
  REQUIRE(r.has_value());

  // Publish with matching topic
  REQUIRE(pub_node.Publish(Heartbeat{100}, "sensor/imu"));
  pub_node.SpinOnce();

  REQUIRE(received_seq == 100);
  REQUIRE(received_sender == 1);
}

TEST_CASE("node - Topic filtering", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> pub_node("publisher", 1);
  osp::Node<NodePayload> sub_node("subscriber", 2);

  int received_count = 0;

  // Subscribe to "topicA"
  auto r = sub_node.Subscribe<Heartbeat>(
      "topicA",
      [&received_count](const Heartbeat&, const osp::MessageHeader&) {
        ++received_count;
      });
  REQUIRE(r.has_value());

  // Publish to "topicB" - should NOT be received
  REQUIRE(pub_node.Publish(Heartbeat{1}, "topicB"));
  pub_node.SpinOnce();
  REQUIRE(received_count == 0);

  // Publish to "topicA" - should be received
  REQUIRE(pub_node.Publish(Heartbeat{2}, "topicA"));
  pub_node.SpinOnce();
  REQUIRE(received_count == 1);
}

TEST_CASE("node - Topic and non-topic coexistence", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> pub_node("publisher", 1);
  osp::Node<NodePayload> sub_node("subscriber", 2);

  int non_topic_count = 0;
  int topic_count = 0;

  // Non-topic subscriber (receives all)
  sub_node.Subscribe<Heartbeat>(
      [&non_topic_count](const Heartbeat&, const osp::MessageHeader&) {
        ++non_topic_count;
      });

  // Topic subscriber (receives only matching topic)
  sub_node.Subscribe<Heartbeat>(
      "specific_topic",
      [&topic_count](const Heartbeat&, const osp::MessageHeader&) {
        ++topic_count;
      });

  // Publish without topic
  REQUIRE(pub_node.Publish(Heartbeat{1}));
  pub_node.SpinOnce();
  REQUIRE(non_topic_count == 1);
  REQUIRE(topic_count == 0);  // Topic subscriber should not receive

  // Publish with matching topic
  REQUIRE(pub_node.Publish(Heartbeat{2}, "specific_topic"));
  pub_node.SpinOnce();
  REQUIRE(non_topic_count == 2);  // Non-topic subscriber receives all
  REQUIRE(topic_count == 1);      // Topic subscriber receives matching

  // Publish with different topic
  REQUIRE(pub_node.Publish(Heartbeat{3}, "other_topic"));
  pub_node.SpinOnce();
  REQUIRE(non_topic_count == 3);  // Non-topic subscriber receives all
  REQUIRE(topic_count == 1);      // Topic subscriber does not receive
}

TEST_CASE("node - Multiple topics same type", "[node]") {
  NodeBusFixture fix;

  osp::Node<NodePayload> pub_node("publisher", 1);
  osp::Node<NodePayload> sub_node("subscriber", 2);

  int imu_count = 0;
  int gps_count = 0;

  // Subscribe to different topics of same type
  sub_node.Subscribe<Heartbeat>(
      "sensor/imu",
      [&imu_count](const Heartbeat&, const osp::MessageHeader&) {
        ++imu_count;
      });

  sub_node.Subscribe<Heartbeat>(
      "sensor/gps",
      [&gps_count](const Heartbeat&, const osp::MessageHeader&) {
        ++gps_count;
      });

  // Publish to IMU topic
  REQUIRE(pub_node.Publish(Heartbeat{1}, "sensor/imu"));
  pub_node.SpinOnce();
  REQUIRE(imu_count == 1);
  REQUIRE(gps_count == 0);

  // Publish to GPS topic
  REQUIRE(pub_node.Publish(Heartbeat{2}, "sensor/gps"));
  pub_node.SpinOnce();
  REQUIRE(imu_count == 1);
  REQUIRE(gps_count == 1);

  // Publish to both
  REQUIRE(pub_node.Publish(Heartbeat{3}, "sensor/imu"));
  REQUIRE(pub_node.Publish(Heartbeat{4}, "sensor/gps"));
  pub_node.SpinOnce();
  REQUIRE(imu_count == 2);
  REQUIRE(gps_count == 2);
}

TEST_CASE("node - Fnv1a32 hash consistency", "[node]") {
  // Same string produces same hash
  uint32_t hash1 = osp::Fnv1a32("test_topic");
  uint32_t hash2 = osp::Fnv1a32("test_topic");
  REQUIRE(hash1 == hash2);
  REQUIRE(hash1 != 0);

  // Different strings produce different hashes
  uint32_t hash_a = osp::Fnv1a32("topicA");
  uint32_t hash_b = osp::Fnv1a32("topicB");
  REQUIRE(hash_a != hash_b);

  // nullptr produces 0
  uint32_t hash_null = osp::Fnv1a32(nullptr);
  REQUIRE(hash_null == 0);

  // Empty string produces non-zero hash
  uint32_t hash_empty = osp::Fnv1a32("");
  REQUIRE(hash_empty != 0);
  REQUIRE(hash_empty == 2166136261u);  // FNV-1a offset basis
}
