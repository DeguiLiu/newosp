/**
 * @file realtime_executor_demo.cpp
 * @brief Demonstrates lifecycle nodes with SingleThreadExecutor.
 *
 * Shows:
 *   - LifecycleNode state transitions (Unconfigured -> Inactive -> Active -> Finalized)
 *   - Periodic sensor data publishing with QoS profiles
 *   - Control node subscribing and processing commands
 *   - SingleThreadExecutor managing multiple lifecycle nodes
 */

#include "osp/bus.hpp"
#include "osp/executor.hpp"
#include "osp/lifecycle_node.hpp"
#include "osp/node.hpp"
#include "osp/qos.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <variant>

// -- Message types ----------------------------------------------------------

struct SensorData {
  float temperature;
  float pressure;
  uint64_t timestamp_us;
};

struct MotorCommand {
  int32_t speed;
  int32_t direction;
};

// -- Payload variant --------------------------------------------------------

using Payload = std::variant<SensorData, MotorCommand>;
using Bus = osp::AsyncBus<Payload>;

// -- Global state for lifecycle callbacks -----------------------------------

static uint32_t sensor_publish_count = 0;
static uint32_t control_recv_count = 0;
static osp::LifecycleNode<Payload>* g_sensor_node = nullptr;
static osp::LifecycleNode<Payload>* g_control_node = nullptr;

// -- SensorNode lifecycle callbacks -----------------------------------------

bool SensorOnConfigure() {
  std::printf("[SensorNode] OnConfigure: initializing sensor hardware\n");
  sensor_publish_count = 0;
  return true;
}

bool SensorOnActivate() {
  std::printf("[SensorNode] OnActivate: starting sensor data acquisition\n");
  return true;
}

bool SensorOnDeactivate() {
  std::printf("[SensorNode] OnDeactivate: stopping sensor data acquisition\n");
  return true;
}

bool SensorOnCleanup() {
  std::printf("[SensorNode] OnCleanup: releasing sensor hardware\n");
  return true;
}

void SensorOnShutdown() {
  std::printf("[SensorNode] OnShutdown: final cleanup\n");
}

// -- ControlNode lifecycle callbacks ----------------------------------------

bool ControlOnConfigure() {
  std::printf("[ControlNode] OnConfigure: initializing control system\n");
  control_recv_count = 0;
  return true;
}

bool ControlOnActivate() {
  std::printf("[ControlNode] OnActivate: starting control loop\n");
  return true;
}

bool ControlOnDeactivate() {
  std::printf("[ControlNode] OnDeactivate: stopping control loop\n");
  return true;
}

bool ControlOnCleanup() {
  std::printf("[ControlNode] OnCleanup: releasing control resources\n");
  return true;
}

void ControlOnShutdown() {
  std::printf("[ControlNode] OnShutdown: final cleanup\n");
}

// -- Subscription callback --------------------------------------------------

void OnSensorData(const SensorData& data, const osp::MessageHeader& header) {
  ++control_recv_count;
  std::printf("[ControlNode] Received sensor data: temp=%.1f, pressure=%.1f, "
              "timestamp=%lu (msg #%lu)\n",
              static_cast<double>(data.temperature),
              static_cast<double>(data.pressure),
              static_cast<unsigned long>(data.timestamp_us),
              static_cast<unsigned long>(header.msg_id));
}

// -- Helper: get timestamp in microseconds ----------------------------------

uint64_t GetTimestampUs() {
  auto now = std::chrono::steady_clock::now();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
      now.time_since_epoch());
  return static_cast<uint64_t>(us.count());
}

// ---------------------------------------------------------------------------

int main() {
  std::printf("=== Realtime Executor Demo ===\n\n");

  // Reset singleton bus (clean state for demo)
  Bus::Instance().Reset();

  // Create lifecycle nodes
  osp::LifecycleNode<Payload> sensor_node("sensor", /*id=*/1);
  osp::LifecycleNode<Payload> control_node("control", /*id=*/2);

  g_sensor_node = &sensor_node;
  g_control_node = &control_node;

  // Register lifecycle callbacks for SensorNode
  sensor_node.SetOnConfigure(SensorOnConfigure);
  sensor_node.SetOnActivate(SensorOnActivate);
  sensor_node.SetOnDeactivate(SensorOnDeactivate);
  sensor_node.SetOnCleanup(SensorOnCleanup);
  sensor_node.SetOnShutdown(SensorOnShutdown);

  // Register lifecycle callbacks for ControlNode
  control_node.SetOnConfigure(ControlOnConfigure);
  control_node.SetOnActivate(ControlOnActivate);
  control_node.SetOnDeactivate(ControlOnDeactivate);
  control_node.SetOnCleanup(ControlOnCleanup);
  control_node.SetOnShutdown(ControlOnShutdown);

  // Subscribe ControlNode to SensorData
  auto sub_result = control_node.Subscribe<SensorData>(OnSensorData);
  if (!sub_result.has_value()) {
    std::printf("ERROR: Failed to subscribe ControlNode to SensorData\n");
    return 1;
  }

  // Create SingleThreadExecutor
  osp::SingleThreadExecutor<Payload> executor;
  executor.AddNode(sensor_node);
  executor.AddNode(control_node);

  std::printf("--- Lifecycle Transitions ---\n\n");

  // Transition: Unconfigured -> Inactive (Configure)
  std::printf("Triggering Configure transition...\n");
  auto cfg_result = sensor_node.Configure();
  if (!cfg_result.has_value()) {
    std::printf("ERROR: SensorNode Configure failed\n");
    return 1;
  }
  cfg_result = control_node.Configure();
  if (!cfg_result.has_value()) {
    std::printf("ERROR: ControlNode Configure failed\n");
    return 1;
  }
  std::printf("\n");

  // Transition: Inactive -> Active (Activate)
  std::printf("Triggering Activate transition...\n");
  auto act_result = sensor_node.Activate();
  if (!act_result.has_value()) {
    std::printf("ERROR: SensorNode Activate failed\n");
    return 1;
  }
  act_result = control_node.Activate();
  if (!act_result.has_value()) {
    std::printf("ERROR: ControlNode Activate failed\n");
    return 1;
  }
  std::printf("\n");

  // Publish sensor data with QoS profile (kQosSensorData: best-effort, keep last 1)
  std::printf("--- Publishing Sensor Data ---\n\n");
  for (uint32_t i = 0; i < 5; ++i) {
    SensorData data{
        20.0f + static_cast<float>(i) * 0.5f,  // temperature
        1013.25f + static_cast<float>(i) * 0.1f,  // pressure
        GetTimestampUs()
    };

    // Publish with HIGH priority (simulating critical sensor data)
    bool published = sensor_node.PublishWithPriority(
        data, osp::MessagePriority::kHigh);

    if (published) {
      ++sensor_publish_count;
      std::printf("[SensorNode] Published sensor data #%u: temp=%.1f, "
                  "pressure=%.1f\n",
                  sensor_publish_count,
                  static_cast<double>(data.temperature),
                  static_cast<double>(data.pressure));
    } else {
      std::printf("[SensorNode] WARNING: Failed to publish sensor data #%u\n",
                  sensor_publish_count + 1);
    }

    // Process messages after each publish
    uint32_t processed = executor.SpinOnce();
    if (processed > 0) {
      std::printf("  [Executor] Processed %u messages\n", processed);
    }
  }

  std::printf("\n");

  // Transition: Active -> Inactive (Deactivate)
  std::printf("--- Deactivating Nodes ---\n\n");
  std::printf("Triggering Deactivate transition...\n");
  auto deact_result = sensor_node.Deactivate();
  if (!deact_result.has_value()) {
    std::printf("ERROR: SensorNode Deactivate failed\n");
    return 1;
  }
  deact_result = control_node.Deactivate();
  if (!deact_result.has_value()) {
    std::printf("ERROR: ControlNode Deactivate failed\n");
    return 1;
  }
  std::printf("\n");

  // Transition: Inactive -> Unconfigured (Cleanup)
  std::printf("--- Cleaning Up Nodes ---\n\n");
  std::printf("Triggering Cleanup transition...\n");
  auto cleanup_result = sensor_node.Cleanup();
  if (!cleanup_result.has_value()) {
    std::printf("ERROR: SensorNode Cleanup failed\n");
    return 1;
  }
  cleanup_result = control_node.Cleanup();
  if (!cleanup_result.has_value()) {
    std::printf("ERROR: ControlNode Cleanup failed\n");
    return 1;
  }
  std::printf("\n");

  // Show bus statistics
  auto stats = Bus::Instance().GetStatistics();
  std::printf("--- Bus Statistics ---\n");
  std::printf("  Published : %lu\n",
              static_cast<unsigned long>(stats.messages_published));
  std::printf("  Processed : %lu\n",
              static_cast<unsigned long>(stats.messages_processed));
  std::printf("  Dropped   : %lu\n",
              static_cast<unsigned long>(stats.messages_dropped));
  std::printf("\n");

  std::printf("--- Summary ---\n");
  std::printf("  Sensor published: %u messages\n", sensor_publish_count);
  std::printf("  Control received: %u messages\n", control_recv_count);
  std::printf("\n");

  // Shutdown (Unconfigured -> Finalized)
  std::printf("--- Shutting Down ---\n\n");
  sensor_node.Shutdown();
  control_node.Shutdown();

  std::printf("=== Demo Complete ===\n");
  return 0;
}
