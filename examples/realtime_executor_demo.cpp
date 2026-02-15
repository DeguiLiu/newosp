/**
 * @file realtime_executor_demo.cpp
 * @brief Demonstrates lifecycle nodes with SingleThreadExecutor.
 *
 * Shows:
 *   - LifecycleNode state transitions (Unconfigured -> Inactive -> Active -> Finalized)
 *   - Periodic sensor data publishing with QoS profiles
 *   - Control node subscribing and processing commands
 *   - SingleThreadExecutor managing multiple lifecycle nodes
 *   - TickTimer demo: ManualTickSource vs SteadyTickSource
 */

#include "osp/bus.hpp"
#include "osp/executor.hpp"
#include "osp/lifecycle_node.hpp"
#include "osp/node.hpp"
#include "osp/qos.hpp"
#include "osp/timer.hpp"

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

  // -------------------------------------------------------------------------
  // TickTimer Demo: ManualTickSource vs SteadyTickSource
  // -------------------------------------------------------------------------
  std::printf("\n=== TickTimer Demo: Pluggable Time Sources ===\n\n");

  // Demo 1: SteadyTickSource (default, wall-clock based)
  {
    std::printf("--- Demo 1: SteadyTickSource (wall-clock) ---\n");
    std::atomic<int> counter{0};
    osp::TimerScheduler<4> sched;  // Default: SteadyTickSource

    sched.Add(20, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      int count = c->fetch_add(1) + 1;
      std::printf("  [SteadyTimer] Fire #%d\n", count);
    }, &counter);

    std::printf("Starting scheduler with 20ms periodic timer...\n");
    sched.Start();

    // Query NsToNextTask
    uint64_t ns = sched.NsToNextTask();
    std::printf("  NsToNextTask: %lu ns (~%lu ms)\n",
                static_cast<unsigned long>(ns),
                static_cast<unsigned long>(ns / 1000000ULL));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.Stop();
    std::printf("  Total fires: %d\n\n", counter.load());
  }

  // Demo 2: ManualTickSource (deterministic, tick-driven)
  {
    std::printf("--- Demo 2: ManualTickSource (tick-driven) ---\n");
    osp::ManualTickSource::Reset();
    osp::ManualTickSource::SetTickPeriodNs(1000000ULL);  // 1ms per tick

    std::atomic<int> counter{0};
    osp::TimerScheduler<4, osp::ManualTickSource> sched;

    sched.Add(10, [](void* ctx) {
      auto* c = static_cast<std::atomic<int>*>(ctx);
      int count = c->fetch_add(1) + 1;
      std::printf("  [ManualTimer] Fire #%d at tick %lu\n",
                  count,
                  static_cast<unsigned long>(osp::ManualTickSource::GetTicks()));
    }, &counter);

    std::printf("Starting scheduler with 10ms periodic timer (tick-driven)...\n");
    sched.Start();

    // Manually advance time by 35 ticks (35ms) — should fire 3 times
    std::printf("  Advancing time by 35 ticks (35ms)...\n");
    for (int i = 0; i < 35; ++i) {
      osp::ManualTickSource::Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Give scheduler time
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Let scheduler catch up
    sched.Stop();
    std::printf("  Total fires: %d (expected 3)\n\n", counter.load());
  }

  // Demo 3: NsToNextTask for precise sleep
  {
    std::printf("--- Demo 3: NsToNextTask for Precise Sleep ---\n");
    osp::ManualTickSource::Reset();
    osp::ManualTickSource::SetTickPeriodNs(1000000ULL);  // 1ms per tick

    osp::TimerScheduler<4, osp::ManualTickSource> sched;

    sched.Add(50, [](void*) {});   // 50ms
    sched.Add(100, [](void*) {});  // 100ms

    std::printf("Added tasks: 50ms, 100ms\n");
    uint64_t ns = sched.NsToNextTask();
    std::printf("  NsToNextTask: %lu ns (~%lu ms)\n",
                static_cast<unsigned long>(ns),
                static_cast<unsigned long>(ns / 1000000ULL));

    // Advance time by 30ms
    for (int i = 0; i < 30; ++i) {
      osp::ManualTickSource::Tick();
    }

    ns = sched.NsToNextTask();
    std::printf("After 30ms:\n");
    std::printf("  NsToNextTask: %lu ns (~%lu ms, expected ~20ms)\n",
                static_cast<unsigned long>(ns),
                static_cast<unsigned long>(ns / 1000000ULL));

    // Advance past first task
    for (int i = 0; i < 25; ++i) {
      osp::ManualTickSource::Tick();
    }

    ns = sched.NsToNextTask();
    std::printf("After 55ms (past first task):\n");
    std::printf("  NsToNextTask: %lu ns (~%lu ms, expected ~45ms)\n\n",
                static_cast<unsigned long>(ns),
                static_cast<unsigned long>(ns / 1000000ULL));
  }

  std::printf("=== TickTimer Demo Complete ===\n");

  // -------------------------------------------------------------------------
  // PreciseSleepStrategy Demo: Reduce CPU usage with precise sleep
  // -------------------------------------------------------------------------
  std::printf("\n=== PreciseSleepStrategy Demo: Precise Sleep for Embedded Systems ===\n\n");

  // Reset bus for clean state
  Bus::Instance().Reset();

  // Create sensor node that publishes at 100Hz (10ms period)
  osp::Node<Payload> imu_sensor("imu_sensor", 10);
  osp::Node<Payload> processor("processor", 11);

  std::atomic<uint32_t> imu_recv_count{0};
  auto imu_sub = processor.Subscribe<SensorData>([&](const SensorData& data, const auto&) {
    imu_recv_count.fetch_add(1, std::memory_order_relaxed);
    std::printf("  [Processor] Received IMU data: temp=%.1f, pressure=%.1f\n",
                static_cast<double>(data.temperature),
                static_cast<double>(data.pressure));
  });

  if (!imu_sub.has_value()) {
    std::printf("ERROR: Failed to subscribe to IMU data\n");
    return 1;
  }

  // Demo 1: Default YieldSleepStrategy (backward compatible)
  {
    std::printf("--- Demo 1: YieldSleepStrategy (default, CPU spinning) ---\n");
    osp::StaticExecutor<Payload> exec;  // Default: YieldSleepStrategy
    exec.AddNode(imu_sensor);
    exec.AddNode(processor);

    exec.Start();

    // Simulate 100Hz sensor publishing
    for (int i = 0; i < 5; ++i) {
      SensorData data{25.0f + static_cast<float>(i), 1013.0f, GetTimestampUs()};
      imu_sensor.Publish(data);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    exec.Stop();

    std::printf("  Received: %u messages\n",
                imu_recv_count.load(std::memory_order_relaxed));
    std::printf("  Note: Executor uses yield(), may cause CPU spinning\n\n");
  }

  // Reset counter
  imu_recv_count.store(0, std::memory_order_relaxed);
  Bus::Instance().Reset();

  // Demo 2: PreciseSleepStrategy with 1ms default sleep
  {
    std::printf("--- Demo 2: PreciseSleepStrategy (1ms sleep, reduced CPU) ---\n");
    osp::PreciseSleepStrategy sleep(1000000ULL);  // 1ms default sleep
    osp::StaticExecutor<Payload, osp::PreciseSleepStrategy> exec(sleep);
    exec.AddNode(imu_sensor);
    exec.AddNode(processor);

    exec.Start();

    // Simulate 100Hz sensor publishing
    for (int i = 0; i < 5; ++i) {
      SensorData data{25.0f + static_cast<float>(i), 1013.0f, GetTimestampUs()};
      imu_sensor.Publish(data);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    exec.Stop();

    std::printf("  Received: %u messages\n",
                imu_recv_count.load(std::memory_order_relaxed));
    std::printf("  Note: Executor sleeps 1ms when idle, reducing CPU usage\n\n");
  }

  // Reset counter
  imu_recv_count.store(0, std::memory_order_relaxed);
  Bus::Instance().Reset();

  // Demo 3: RealtimeExecutor with PreciseSleepStrategy
  {
    std::printf("--- Demo 3: RealtimeExecutor + PreciseSleepStrategy ---\n");
    osp::RealtimeConfig cfg;
    cfg.cpu_affinity = 0;  // Pin to core 0
    osp::PreciseSleepStrategy sleep(500000ULL);  // 500us default sleep
    osp::RealtimeExecutor<Payload, osp::PreciseSleepStrategy> exec(cfg, sleep);
    exec.AddNode(imu_sensor);
    exec.AddNode(processor);

    exec.Start();

    // Simulate 100Hz sensor publishing
    for (int i = 0; i < 5; ++i) {
      SensorData data{25.0f + static_cast<float>(i), 1013.0f, GetTimestampUs()};
      imu_sensor.Publish(data);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    exec.Stop();

    std::printf("  Received: %u messages\n",
                imu_recv_count.load(std::memory_order_relaxed));
    std::printf("  Note: Realtime executor with 500us sleep, CPU pinned to core 0\n\n");
  }

  std::printf("=== PreciseSleepStrategy Demo Complete ===\n");
  std::printf("\nKey Takeaways:\n");
  std::printf("  - YieldSleepStrategy: backward compatible, may cause CPU spinning\n");
  std::printf("  - PreciseSleepStrategy: reduces CPU usage on embedded systems\n");
  std::printf("  - Configurable sleep duration (default 1ms, min 100us, max 10ms)\n");
  std::printf("  - Linux: uses clock_nanosleep for precise wakeup\n");
  std::printf("  - Other platforms: falls back to std::this_thread::sleep_for\n\n");

  return 0;
}
