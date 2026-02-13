/**
 * @file test_node.cpp
 * @brief Tests for node.hpp (lightweight communication node)
 */

#include "osp/node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <variant>

// --- Test message types (same as bus tests) ---

struct Heartbeat {
  uint32_t seq;
};

struct Command {
  int32_t action;
};

struct Status {
  uint8_t code;
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
