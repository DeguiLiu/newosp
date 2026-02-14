/**
 * @file basic_demo.cpp
 * @brief Basic demo showing OSP Bus/Node publish-subscribe usage.
 *
 * Demonstrates:
 *   - Defining message types and using std::variant as Payload
 *   - Creating nodes with typed subscriptions (runtime callback)
 *   - StaticNode with compile-time handler binding (zero-overhead dispatch)
 *   - Publishing and processing messages via SpinOnce()
 *   - Querying bus statistics after processing
 */

#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/static_node.hpp"
#include "osp/log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <variant>

// -- Message types ----------------------------------------------------------

struct SensorData {
  float temp;
  float humidity;
};

struct SystemLog {
  char message[64];
  uint8_t level;
};

// -- Payload variant --------------------------------------------------------

using Payload = std::variant<SensorData, SystemLog>;
using Bus = osp::AsyncBus<Payload>;

// ---------------------------------------------------------------------------

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kDebug);

  // Reset singleton bus (clean state for demo)
  Bus::Instance().Reset();

  // Create nodes
  osp::Node<Payload> sensor("sensor", /*id=*/1);
  osp::Node<Payload> logger("logger", /*id=*/2);

  // Subscribe sensor node to SensorData
  sensor.Subscribe<SensorData>(
      [](const SensorData& d, const osp::MessageHeader& h) {
        OSP_LOG_INFO("sensor", "recv temp=%.1f humidity=%.1f (msg #%lu)",
                     static_cast<double>(d.temp),
                     static_cast<double>(d.humidity),
                     static_cast<unsigned long>(h.msg_id));
      });

  // Subscribe logger node to SystemLog
  logger.Subscribe<SystemLog>(
      [](const SystemLog& l, const osp::MessageHeader& h) {
        OSP_LOG_INFO("logger", "recv [L%u] %s (msg #%lu)",
                     static_cast<unsigned>(l.level), l.message,
                     static_cast<unsigned long>(h.msg_id));
      });

  // Publish messages
  sensor.Publish(SensorData{23.5f, 61.2f});
  sensor.Publish(SensorData{24.1f, 59.8f});

  SystemLog log_msg{};
  std::strncpy(log_msg.message, "system boot complete", sizeof(log_msg.message) - 1);
  log_msg.level = 1;
  logger.Publish(log_msg);

  // Process all pending messages
  uint32_t processed = sensor.SpinOnce();
  OSP_LOG_INFO("main", "processed %u messages", processed);

  // Show bus statistics
  auto stats = Bus::Instance().GetStatistics();
  std::printf("\n--- Bus Statistics ---\n"
              "  published : %lu\n"
              "  processed : %lu\n"
              "  dropped   : %lu\n",
              static_cast<unsigned long>(stats.messages_published),
              static_cast<unsigned long>(stats.messages_processed),
              static_cast<unsigned long>(stats.messages_dropped));

  // Clean shutdown
  sensor.Stop();
  logger.Stop();

  // =====================================================================
  // Part 2: StaticNode -- compile-time handler binding (zero overhead)
  // =====================================================================

  Bus::Instance().Reset();
  std::printf("\n--- StaticNode Demo (compile-time dispatch) ---\n");

  // Define a handler struct with operator() for each message type.
  // Because the type is known at compile time, the compiler can inline.
  struct DemoHandler {
    uint32_t sensor_count{0};
    uint32_t log_count{0};

    void operator()(const SensorData& d, const osp::MessageHeader& /*h*/) {
      ++sensor_count;
      OSP_LOG_INFO("static", "sensor temp=%.1f humidity=%.1f",
                   static_cast<double>(d.temp),
                   static_cast<double>(d.humidity));
    }
    void operator()(const SystemLog& l, const osp::MessageHeader& /*h*/) {
      ++log_count;
      OSP_LOG_INFO("static", "log [L%u] %s",
                   static_cast<unsigned>(l.level), l.message);
    }
  };

  osp::StaticNode<Payload, DemoHandler> static_node("static", 10, DemoHandler{});
  static_node.Start();

  static_node.Publish(SensorData{30.0f, 55.0f});
  static_node.Publish(log_msg);
  static_node.SpinOnce();

  const auto& h = static_node.GetHandler();
  std::printf("  handler stats: sensor=%u  log=%u\n", h.sensor_count, h.log_count);

  static_node.Stop();

  osp::log::Shutdown();
  return 0;
}
