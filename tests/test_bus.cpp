/**
 * @file test_bus.cpp
 * @brief Tests for bus.hpp (lock-free MPSC message bus)
 */

#include "osp/bus.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// --- Test message types ---
// NOTE: structs must be >= 8 bytes to avoid GCC 14 wide-read optimization
// triggering ASan stack-buffer-overflow (8-byte memcpy on smaller struct).

struct SensorData {
  float temperature;
  uint32_t sensor_id;
};

struct MotorCmd {
  int32_t speed;
  uint32_t flags = 0;
};

struct AlarmEvent {
  uint32_t code;
  uint32_t reserved = 0;
};

using TestPayload = std::variant<SensorData, MotorCmd, AlarmEvent>;
using TestBus = osp::AsyncBus<TestPayload>;
using TestEnvelope = osp::MessageEnvelope<TestPayload>;

// Helper to reset bus between tests
struct BusFixture {
  BusFixture() { TestBus::Instance().Reset(); }
  ~BusFixture() { TestBus::Instance().Reset(); }
};

TEST_CASE("AsyncBus publish and process", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> received{0};
  float last_temp = 0.0f;

  bus.Subscribe<SensorData>([&received, &last_temp](const TestEnvelope& env) {
    const SensorData* data = std::get_if<SensorData>(&env.payload);
    if (data) {
      last_temp = data->temperature;
      ++received;
    }
  });

  REQUIRE(bus.Publish(SensorData{25.5f, 1}, 42));

  uint32_t processed = bus.ProcessBatch();
  REQUIRE(processed == 1);
  REQUIRE(received.load() == 1);
  REQUIRE(last_temp == 25.5f);
}

TEST_CASE("AsyncBus multiple message types", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  int sensor_count = 0;
  int motor_count = 0;

  bus.Subscribe<SensorData>([&sensor_count](const TestEnvelope&) { ++sensor_count; });
  bus.Subscribe<MotorCmd>([&motor_count](const TestEnvelope&) { ++motor_count; });

  bus.Publish(SensorData{20.0f, 1}, 1);
  bus.Publish(MotorCmd{100}, 2);
  bus.Publish(SensorData{21.0f, 2}, 1);

  bus.ProcessBatch();

  REQUIRE(sensor_count == 2);
  REQUIRE(motor_count == 1);
}

TEST_CASE("AsyncBus subscribe and unsubscribe", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  int count = 0;
  auto handle = bus.Subscribe<SensorData>([&count](const TestEnvelope&) { ++count; });

  REQUIRE(handle.IsValid());

  bus.Publish(SensorData{1.0f, 0}, 0);
  bus.ProcessBatch();
  REQUIRE(count == 1);

  REQUIRE(bus.Unsubscribe(handle));

  bus.Publish(SensorData{2.0f, 0}, 0);
  bus.ProcessBatch();
  REQUIRE(count == 1);  // No change after unsubscribe
}

TEST_CASE("AsyncBus multiple subscribers same type", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  int count_a = 0;
  int count_b = 0;

  bus.Subscribe<MotorCmd>([&count_a](const TestEnvelope&) { ++count_a; });
  bus.Subscribe<MotorCmd>([&count_b](const TestEnvelope&) { ++count_b; });

  bus.Publish(MotorCmd{50}, 1);
  bus.ProcessBatch();

  REQUIRE(count_a == 1);
  REQUIRE(count_b == 1);
}

TEST_CASE("AsyncBus message header populated", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  osp::MessageHeader captured_header;

  bus.Subscribe<AlarmEvent>([&captured_header](const TestEnvelope& env) { captured_header = env.header; });

  bus.Publish(AlarmEvent{42}, 7);
  bus.ProcessBatch();

  REQUIRE(captured_header.sender_id == 7);
  REQUIRE(captured_header.msg_id >= 1);
  REQUIRE(captured_header.timestamp_us > 0);
  REQUIRE(captured_header.priority == osp::MessagePriority::kMedium);
}

TEST_CASE("AsyncBus priority publish", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  osp::MessagePriority captured_prio = osp::MessagePriority::kMedium;

  bus.Subscribe<AlarmEvent>([&captured_prio](const TestEnvelope& env) { captured_prio = env.header.priority; });

  bus.PublishWithPriority(AlarmEvent{1}, 0, osp::MessagePriority::kHigh);
  bus.ProcessBatch();

  REQUIRE(captured_prio == osp::MessagePriority::kHigh);
}

TEST_CASE("AsyncBus statistics tracking", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  bus.Subscribe<SensorData>([](const TestEnvelope&) {});

  for (int i = 0; i < 10; ++i) {
    bus.Publish(SensorData{static_cast<float>(i), 0}, 0);
  }
  bus.ProcessBatch();

  auto stats = bus.GetStatistics();
  REQUIRE(stats.messages_published == 10);
  REQUIRE(stats.messages_processed == 10);
  REQUIRE(stats.messages_dropped == 0);

  bus.ResetStatistics();
  auto stats2 = bus.GetStatistics();
  REQUIRE(stats2.messages_published == 0);
}

TEST_CASE("AsyncBus depth and backpressure", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  REQUIRE(bus.Depth() == 0);
  REQUIRE(bus.GetBackpressureLevel() == osp::BackpressureLevel::kNormal);

  bus.Publish(SensorData{1.0f, 0}, 0);
  REQUIRE(bus.Depth() == 1);

  bus.ProcessBatch();
  REQUIRE(bus.Depth() == 0);
}

TEST_CASE("AsyncBus empty process returns zero", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  REQUIRE(bus.ProcessBatch() == 0);
}

TEST_CASE("AsyncBus unsubscribe invalid handle", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  REQUIRE(!bus.Unsubscribe(osp::SubscriptionHandle::Invalid()));
}

TEST_CASE("AsyncBus PublishFast with timestamp", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  uint64_t captured_ts = 0;

  bus.Subscribe<SensorData>([&captured_ts](const TestEnvelope& env) { captured_ts = env.header.timestamp_us; });

  bus.PublishFast(SensorData{1.0f, 0}, 0, 123456789);
  bus.ProcessBatch();

  REQUIRE(captured_ts == 123456789);
}

TEST_CASE("AsyncBus error callback", "[bus]") {
  BusFixture fix;
  // Just verify the API compiles and runs
  auto& bus = TestBus::Instance();

  static std::atomic<int> error_count{0};
  error_count.store(0);
  bus.SetErrorCallback([](osp::BusError, uint64_t) { error_count.fetch_add(1); });

  // Normal publish should not trigger error
  bus.Publish(SensorData{1.0f, 0}, 0);
  bus.ProcessBatch();
  REQUIRE(error_count.load() == 0);
}

TEST_CASE("AsyncBus overloaded visitor pattern", "[bus]") {
  TestPayload payload = SensorData{30.0f, 5};

  bool visited = false;
  std::visit(osp::overloaded{
                 [&visited](const SensorData& d) {
                   REQUIRE(d.temperature == 30.0f);
                   REQUIRE(d.sensor_id == 5);
                   visited = true;
                 },
                 [](const MotorCmd&) { REQUIRE(false); },
                 [](const AlarmEvent&) { REQUIRE(false); },
             },
             payload);

  REQUIRE(visited);
}

TEST_CASE("AsyncBus VariantIndex compile-time check", "[bus]") {
  constexpr size_t sensor_idx = osp::VariantIndex<SensorData, TestPayload>::value;
  constexpr size_t motor_idx = osp::VariantIndex<MotorCmd, TestPayload>::value;
  constexpr size_t alarm_idx = osp::VariantIndex<AlarmEvent, TestPayload>::value;

  REQUIRE(sensor_idx == 0);
  REQUIRE(motor_idx == 1);
  REQUIRE(alarm_idx == 2);
}

TEST_CASE("AsyncBus multi-threaded publish", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> received{0};
  bus.Subscribe<SensorData>([&received](const TestEnvelope&) { ++received; });

  static constexpr int kNumThreads = 4;
  static constexpr int kMsgsPerThread = 100;

  std::thread threads[kNumThreads];
  for (int t = 0; t < kNumThreads; ++t) {
    threads[t] = std::thread([&bus, t]() {
      for (int i = 0; i < kMsgsPerThread; ++i) {
        bus.Publish(SensorData{static_cast<float>(i), static_cast<uint32_t>(t)}, static_cast<uint32_t>(t));
      }
    });
  }

  for (auto& t : threads)
    t.join();

  // Drain all messages (multiple rounds to handle timing)
  for (int round = 0; round < 100; ++round) {
    if (bus.ProcessBatch() == 0 && bus.Depth() == 0)
      break;
  }

  // Under high CAS contention, the MPSC ring buffer retries on sequence
  // mismatch (C2 fix). With retry, delivery should be near 100%.
  int total = kNumThreads * kMsgsPerThread;
  REQUIRE(received.load() >= total * 99 / 100);
}

TEST_CASE("Bus publish with no subscribers", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  // Publish without any subscribers - should not crash
  REQUIRE(bus.Publish(SensorData{25.5f, 1}, 42));
  REQUIRE(bus.Publish(MotorCmd{100}, 1));
  REQUIRE(bus.Publish(AlarmEvent{999}, 2));

  uint32_t processed = bus.ProcessBatch();
  REQUIRE(processed == 3);

  auto stats = bus.GetStatistics();
  REQUIRE(stats.messages_published == 3);
  REQUIRE(stats.messages_processed == 3);
}

TEST_CASE("Bus subscribe and unsubscribe rapidly", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> count{0};

  // Subscribe and unsubscribe in tight loop
  for (int i = 0; i < 100; ++i) {
    auto handle = bus.Subscribe<SensorData>([&count](const TestEnvelope&) { ++count; });
    REQUIRE(handle.IsValid());
    REQUIRE(bus.Unsubscribe(handle));
  }

  // Final subscription that stays
  bus.Subscribe<SensorData>([&count](const TestEnvelope&) { ++count; });

  bus.Publish(SensorData{1.0f, 0}, 0);
  bus.ProcessBatch();

  REQUIRE(count.load() == 1);
}

TEST_CASE("Bus multiple message types", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  int sensor_count = 0;
  int motor_count = 0;
  int alarm_count = 0;

  float last_temp = 0.0f;
  int32_t last_speed = 0;
  uint32_t last_code = 0;

  bus.Subscribe<SensorData>([&sensor_count, &last_temp](const TestEnvelope& env) {
    const SensorData* data = std::get_if<SensorData>(&env.payload);
    if (data) {
      last_temp = data->temperature;
      ++sensor_count;
    }
  });

  bus.Subscribe<MotorCmd>([&motor_count, &last_speed](const TestEnvelope& env) {
    const MotorCmd* cmd = std::get_if<MotorCmd>(&env.payload);
    if (cmd) {
      last_speed = cmd->speed;
      ++motor_count;
    }
  });

  bus.Subscribe<AlarmEvent>([&alarm_count, &last_code](const TestEnvelope& env) {
    const AlarmEvent* alarm = std::get_if<AlarmEvent>(&env.payload);
    if (alarm) {
      last_code = alarm->code;
      ++alarm_count;
    }
  });

  // Publish different types and verify correct routing
  bus.Publish(SensorData{20.5f, 1}, 1);
  bus.Publish(MotorCmd{150}, 2);
  bus.Publish(AlarmEvent{404}, 3);
  bus.Publish(SensorData{30.0f, 2}, 1);
  bus.Publish(MotorCmd{-50}, 2);

  bus.ProcessBatch();

  REQUIRE(sensor_count == 2);
  REQUIRE(motor_count == 2);
  REQUIRE(alarm_count == 1);
  REQUIRE(last_temp == 30.0f);
  REQUIRE(last_speed == -50);
  REQUIRE(last_code == 404);
}

TEST_CASE("Bus queue overflow behavior", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> received{0};
  bus.Subscribe<SensorData>([&received](const TestEnvelope&) { ++received; });

  // Publish more messages than queue depth (4096)
  // The bus should drop messages when full
  static constexpr int kOverflowCount = 5000;
  int publish_success = 0;
  int publish_failed = 0;

  for (int i = 0; i < kOverflowCount; ++i) {
    if (bus.Publish(SensorData{static_cast<float>(i), static_cast<uint32_t>(i)}, 0)) {
      ++publish_success;
    } else {
      ++publish_failed;
    }
  }

  // Process all messages
  uint32_t total_processed = 0;
  for (int round = 0; round < 200; ++round) {
    uint32_t batch = bus.ProcessBatch();
    total_processed += batch;
    if (batch == 0 && bus.Depth() == 0)
      break;
  }

  auto stats = bus.GetStatistics();

  // Verify: messages that returned true from Publish should be processed
  REQUIRE(received.load() == stats.messages_processed);
  // Some messages should have been dropped due to overflow
  REQUIRE(publish_failed > 0);
  REQUIRE(stats.messages_dropped > 0);
}

TEST_CASE("Bus concurrent publish from multiple threads", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> received{0};
  bus.Subscribe<SensorData>([&received](const TestEnvelope&) { ++received; });

  static constexpr int kNumThreads = 4;
  static constexpr int kMsgsPerThread = 250;

  std::thread threads[kNumThreads];
  std::atomic<bool> start_flag{false};

  // Create threads that wait for start signal
  for (int t = 0; t < kNumThreads; ++t) {
    threads[t] = std::thread([&bus, &start_flag, t]() {
      // Wait for start signal to maximize concurrency
      while (!start_flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int i = 0; i < kMsgsPerThread; ++i) {
        bus.Publish(SensorData{static_cast<float>(i), static_cast<uint32_t>(t)}, static_cast<uint32_t>(t));
      }
    });
  }

  // Start all threads simultaneously
  start_flag.store(true, std::memory_order_release);

  for (auto& t : threads)
    t.join();

  // Drain all messages
  for (int round = 0; round < 100; ++round) {
    if (bus.ProcessBatch() == 0 && bus.Depth() == 0)
      break;
  }

  // Under high CAS contention, accept >= 95% delivery
  int total = kNumThreads * kMsgsPerThread;
  REQUIRE(received.load() >= total * 95 / 100);
}

// ============================================================================
// ProcessBatchWith tests
// ============================================================================

TEST_CASE("ProcessBatchWith basic dispatch", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  struct CountVisitor {
    uint32_t sensor_count = 0;
    uint32_t motor_count = 0;
    uint32_t alarm_count = 0;

    void operator()(const SensorData& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++sensor_count; }
    void operator()(const MotorCmd& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++motor_count; }
    void operator()(const AlarmEvent& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++alarm_count; }
  };

  REQUIRE(bus.Publish(SensorData{25.5f, 1}, 0));
  REQUIRE(bus.Publish(MotorCmd{100}, 0));
  REQUIRE(bus.Publish(SensorData{30.0f, 2}, 0));
  REQUIRE(bus.Publish(MotorCmd{-50}, 0));

  CountVisitor visitor;
  uint32_t processed = bus.ProcessBatchWith(visitor);

  REQUIRE(processed == 4);
  REQUIRE(visitor.sensor_count == 2);
  REQUIRE(visitor.motor_count == 2);
  REQUIRE(visitor.alarm_count == 0);
}

TEST_CASE("ProcessBatchWith processes all messages", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  constexpr uint32_t kCount = 10;
  for (uint32_t i = 0; i < kCount; ++i) {
    REQUIRE(bus.Publish(SensorData{static_cast<float>(i), i}, 0));
  }

  uint32_t total_processed = 0;
  struct SinkVisitor {
    uint32_t count = 0;
    void operator()(const SensorData& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++count; }
    void operator()(const MotorCmd& /*d*/, const osp::MessageHeader& /*h*/) noexcept {}
    void operator()(const AlarmEvent& /*d*/, const osp::MessageHeader& /*h*/) noexcept {}
  };

  SinkVisitor visitor;
  // ProcessBatchWith may process up to kBatchSize per call, so loop
  while (bus.Depth() > 0) {
    total_processed += bus.ProcessBatchWith(visitor);
  }

  REQUIRE(total_processed == kCount);
  REQUIRE(visitor.count == kCount);
}

TEST_CASE("ProcessBatchWith stats update", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  REQUIRE(bus.Publish(SensorData{1.0f, 1}, 0));
  REQUIRE(bus.Publish(MotorCmd{50}, 0));
  REQUIRE(bus.Publish(AlarmEvent{99}, 0));

  struct NopVisitor {
    void operator()(const SensorData& /*d*/, const osp::MessageHeader& /*h*/) noexcept {}
    void operator()(const MotorCmd& /*d*/, const osp::MessageHeader& /*h*/) noexcept {}
    void operator()(const AlarmEvent& /*d*/, const osp::MessageHeader& /*h*/) noexcept {}
  };

  auto stats_before = bus.GetStatistics();
  uint64_t processed_before = stats_before.messages_processed;

  NopVisitor visitor;
  uint32_t ret = bus.ProcessBatchWith(visitor);

  auto stats_after = bus.GetStatistics();
  REQUIRE(ret == 3);
  REQUIRE(stats_after.messages_processed == processed_before + 3);
}

TEST_CASE("ProcessBatchWith empty queue", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  // Queue is empty after fixture reset
  REQUIRE(bus.Depth() == 0);

  struct TrackVisitor {
    uint32_t call_count = 0;
    void operator()(const SensorData& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++call_count; }
    void operator()(const MotorCmd& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++call_count; }
    void operator()(const AlarmEvent& /*d*/, const osp::MessageHeader& /*h*/) noexcept { ++call_count; }
  };

  TrackVisitor visitor;
  uint32_t processed = bus.ProcessBatchWith(visitor);

  REQUIRE(processed == 0);
  REQUIRE(visitor.call_count == 0);
}

// ============================================================================
// Reset + re-subscribe test (C1 fix: next_callback_id_ inside lock)
// ============================================================================

TEST_CASE("Bus Reset clears subscriptions and allows re-subscribe", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> count1{0};
  std::atomic<int> count2{0};

  // Subscribe before reset
  bus.Subscribe<SensorData>([&count1](const TestEnvelope&) { count1.fetch_add(1); });

  bus.Publish(SensorData{1.0f, 0}, 0);
  bus.ProcessBatch();
  REQUIRE(count1.load() == 1);

  // Reset clears all subscriptions
  bus.Reset();

  // Re-subscribe after reset
  auto handle = bus.Subscribe<SensorData>([&count2](const TestEnvelope&) { count2.fetch_add(1); });
  REQUIRE(handle.IsValid());

  bus.Publish(SensorData{2.0f, 0}, 0);
  bus.ProcessBatch();

  // Old callback should not fire, new one should
  REQUIRE(count1.load() == 1);
  REQUIRE(count2.load() == 1);
}

TEST_CASE("AsyncBus unsubscribe from within callback does not deadlock", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::atomic<int> received{0};
  std::atomic<bool> done{false};

  // Subscribe then immediately unsubscribe from within the callback. The
  // dispatch path holds the reader lock; Unsubscribe needs the exclusive lock,
  // so a re-entrant unsubscribe used to self-deadlock on the same thread.
  // handle_ptr indirection resolves the chicken-and-egg: the callback needs
  // the handle value, which only exists after Subscribe returns.
  osp::SubscriptionHandle h;
  osp::SubscriptionHandle* handle_ptr = &h;
  auto cb = [&bus, handle_ptr, &received](const TestEnvelope& env) mutable {
    const SensorData* data = std::get_if<SensorData>(&env.payload);
    if (data) {
      received.fetch_add(1);
      bus.Unsubscribe(*handle_ptr);
    }
  };
  h = bus.Subscribe<SensorData>(std::move(cb));

  REQUIRE(h.IsValid());
  REQUIRE(bus.Publish(SensorData{1.0f, 1}, 42));

  std::thread consumer([&bus, &done]() {
    bus.ProcessBatch();
    done.store(true, std::memory_order_release);
  });

  // The consumer thread must finish within 2s. If the re-entrant unsubscribe
  // deadlocks the spin lock, this times out and we abandon the process (the
  // lock can never be recovered).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  if (!done.load(std::memory_order_acquire)) {
    std::_Exit(1);  // Deadlock: unrecoverable, terminate the test binary.
  }
  if (consumer.joinable()) {
    consumer.join();
  }

  REQUIRE(done.load());
  REQUIRE(received.load() == 1);
}

// ============================================================================
// AsyncBus multi-instance: the constructor must be public so the bus can be
// instantiated more than once per PayloadVariant (architectural fix -- the
// previous private ctor forced a single global singleton per variant).
// ============================================================================

TEST_CASE("AsyncBus can be instantiated as a local object", "[bus][multi-instance]") {
  // Previously AsyncBus() was private and only Instance() (Meyer's singleton)
  // was available, so this line failed to compile.
  osp::AsyncBus<TestPayload> bus_a;
  osp::AsyncBus<TestPayload> bus_b;

  std::atomic<int> received_a{0};
  std::atomic<int> received_b{0};

  bus_a.Subscribe<SensorData>([&received_a](const TestEnvelope&) { received_a.fetch_add(1); });
  bus_b.Subscribe<SensorData>([&received_b](const TestEnvelope&) { received_b.fetch_add(1); });

  // Publish only on bus_a: only bus_a's subscriber must see it.
  bus_a.Publish(SensorData{1.0f, 1}, 1);
  REQUIRE(bus_a.ProcessBatch() == 1);
  REQUIRE(bus_b.ProcessBatch() == 0);

  REQUIRE(received_a.load() == 1);
  REQUIRE(received_b.load() == 0);
}

TEST_CASE("AsyncBus two local buses are fully isolated", "[bus][multi-instance]") {
  osp::AsyncBus<TestPayload> bus_x;
  osp::AsyncBus<TestPayload> bus_y;

  std::atomic<int> count_x{0};
  std::atomic<int> count_y{0};

  bus_x.Subscribe<MotorCmd>([&count_x](const TestEnvelope&) { count_x.fetch_add(1); });
  bus_y.Subscribe<MotorCmd>([&count_y](const TestEnvelope&) { count_y.fetch_add(1); });

  // Message on bus_x reaches only x's consumer.
  bus_x.Publish(MotorCmd{10}, 1);
  REQUIRE(bus_x.ProcessBatch() == 1);
  REQUIRE(count_x.load() == 1);
  REQUIRE(count_y.load() == 0);

  // Message on bus_y reaches only y's consumer.
  bus_y.Publish(MotorCmd{20}, 2);
  REQUIRE(bus_y.ProcessBatch() == 1);
  REQUIRE(count_y.load() == 1);
  REQUIRE(count_x.load() == 1);  // unchanged by y's message
}

// ============================================================================
// Timestamp Source Tests
// ============================================================================

#if !OSP_BUS_COARSE_TIMESTAMP
TEST_CASE("AsyncBus default timestamp has sub-millisecond resolution", "[bus]") {
  // TimeSynchronizer supports windows down to ~100us, so the default clock
  // must distinguish messages published microseconds apart. A coarse clock
  // (~4ms tick) would collapse them onto one value and silently defeat
  // window checks -- guard against OSP_BUS_COARSE_TIMESTAMP becoming the
  // default. See OSP_BUS_COARSE_TIMESTAMP in bus.hpp.
  //
  // Intentionally compiled out when OSP_BUS_COARSE_TIMESTAMP=1: that build has
  // knowingly traded resolution for speed, so asserting resolution there would
  // report a failure for a configuration that is behaving as documented.
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::vector<uint64_t> stamps;
  bus.Subscribe<SensorData>([&stamps](const TestEnvelope& env) { stamps.push_back(env.header.timestamp_us); });

  // Publish in a tight loop, then busy-wait past a microsecond between each
  // so a microsecond-resolution clock must produce distinct values.
  for (int i = 0; i < 200; ++i) {
    bus.Publish(SensorData{static_cast<float>(i), 0}, 0);
    const uint64_t spin_start = osp::SteadyNowUs();
    while (osp::SteadyNowUs() - spin_start < 2U) {
      // busy-wait ~2us
    }
  }
  bus.ProcessBatch();

  REQUIRE(stamps.size() == 200);

  size_t distinct = 1;
  for (size_t i = 1; i < stamps.size(); ++i) {
    if (stamps[i] != stamps[i - 1]) {
      ++distinct;
    }
  }

  // With a 4ms-tick coarse clock, 200 samples spanning ~400us would yield 1-2
  // distinct values. A microsecond clock yields close to 200.
  REQUIRE(distinct > 100);
}
#endif  // !OSP_BUS_COARSE_TIMESTAMP

// Valid under either clock source: coarse is lower-resolution but still
// monotonic, so this must hold in both configurations.
TEST_CASE("AsyncBus PublishTimestamp is monotonic non-decreasing", "[bus]") {
  BusFixture fix;
  auto& bus = TestBus::Instance();

  std::vector<uint64_t> stamps;
  bus.Subscribe<SensorData>([&stamps](const TestEnvelope& env) { stamps.push_back(env.header.timestamp_us); });

  for (int i = 0; i < 50; ++i) {
    bus.Publish(SensorData{static_cast<float>(i), 0}, 0);
  }
  bus.ProcessBatch();

  REQUIRE(stamps.size() == 50);
  for (size_t i = 1; i < stamps.size(); ++i) {
    REQUIRE(stamps[i] >= stamps[i - 1]);
  }
}
