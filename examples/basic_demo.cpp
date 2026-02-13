/**
 * @file basic_demo.cpp
 * @brief Basic demo showing OSP Bus/Node publish-subscribe usage.
 *
 * Demonstrates:
 *   - Defining message types and using std::variant as Payload
 *   - Creating nodes with typed subscriptions
 *   - Publishing and processing messages via SpinOnce()
 *   - Querying bus statistics after processing
 */

#include "osp/bus.hpp"
#include "osp/node.hpp"
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
  osp::log::Shutdown();
  return 0;
}
