/**
 * @file serial_demo.cpp
 * @brief Industrial serial communication demo with BT and HSM integration.
 *
 * Demonstrates:
 * - Node A (Sensor): HSM-driven lifecycle (Init → Calibrating → Running → Error)
 * - Node B (Controller): BT-driven decision logic (check → evaluate → act)
 * - Reliable bidirectional serial communication with CRC and ACK
 * - Timer-based periodic sensor data publishing
 * - PTY pairs for hardware-free testing
 */

#include "osp/serial_transport.hpp"
#include "osp/hsm.hpp"
#include "osp/bt.hpp"
#include "osp/timer.hpp"
#include "osp/log.hpp"

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

#if defined(OSP_PLATFORM_LINUX)
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// ============================================================================
// Message Types
// ============================================================================

struct SensorData {
  float temperature;
  float pressure;
  uint64_t timestamp_us;
};

struct ControlCommand {
  uint32_t mode;
  float target_value;
};

// ============================================================================
// Serial Message Type IDs
// ============================================================================

static constexpr uint16_t kMsgTypeSensorData = 1;
static constexpr uint16_t kMsgTypeControlCommand = 2;

// ============================================================================
// HSM Events for Sensor Node
// ============================================================================

static constexpr uint32_t kEventConfigure = 1;
static constexpr uint32_t kEventCalibrateDone = 2;
static constexpr uint32_t kEventError = 3;
static constexpr uint32_t kEventRecover = 4;

// ============================================================================
// Sensor Node Context (HSM)
// ============================================================================

struct SensorContext {
  osp::SerialTransport* transport;
  float temperature;
  float pressure;
  uint32_t error_count;
  bool calibrated;
  int32_t state_calibrating;
  int32_t state_running;
  int32_t state_error;
  osp::StateMachine<SensorContext, 8>* sm;
};

// ============================================================================
// Controller Node Context (BT)
// ============================================================================

struct ControllerContext {
  osp::SerialTransport* transport;
  SensorData last_sensor_data;
  bool sensor_data_valid;
  float threshold_high;
  float threshold_low;
  uint32_t action_mode;
};

// ============================================================================
// PTY Helper
// ============================================================================

struct PtyPair {
  int master_a;
  int master_b;
  char slave_a_name[64];
  char slave_b_name[64];
  bool valid;
};

static PtyPair CreatePtyPair() {
  PtyPair p{};
  p.valid = false;

#if defined(OSP_PLATFORM_LINUX)
  int slave_a, slave_b;
  if (::openpty(&p.master_a, &slave_a, p.slave_a_name, nullptr, nullptr) != 0) {
    return p;
  }
  if (::openpty(&p.master_b, &slave_b, p.slave_b_name, nullptr, nullptr) != 0) {
    ::close(p.master_a);
    ::close(slave_a);
    return p;
  }

  // Set masters to non-blocking
  int flags_a = ::fcntl(p.master_a, F_GETFL, 0);
  ::fcntl(p.master_a, F_SETFL, flags_a | O_NONBLOCK);
  int flags_b = ::fcntl(p.master_b, F_GETFL, 0);
  ::fcntl(p.master_b, F_SETFL, flags_b | O_NONBLOCK);

  // Close slave fds (SerialTransport will open them)
  ::close(slave_a);
  ::close(slave_b);

  p.valid = true;
#endif

  return p;
}

static void ClosePty(PtyPair& p) {
  if (p.master_a >= 0) ::close(p.master_a);
  if (p.master_b >= 0) ::close(p.master_b);
  p.master_a = p.master_b = -1;
  p.valid = false;
}

// ============================================================================
// Cross-connect PTY pairs (A's master → B's slave, B's master → A's slave)
// ============================================================================

static void CrossConnectPty(int master_a, int master_b) {
  uint8_t buf[256];

  // A → B
  ssize_t n = ::read(master_a, buf, sizeof(buf));
  if (n > 0) {
    ssize_t written = ::write(master_b, buf, static_cast<size_t>(n));
    (void)written;
  }

  // B → A
  n = ::read(master_b, buf, sizeof(buf));
  if (n > 0) {
    ssize_t written = ::write(master_a, buf, static_cast<size_t>(n));
    (void)written;
  }
}

// ============================================================================
// Sensor Node HSM States
// ============================================================================

static osp::TransitionResult SensorUnconfiguredHandler(SensorContext& ctx, const osp::Event& event) {
  if (event.id == kEventConfigure) {
    OSP_LOG_INFO("Sensor", "Configuring sensor...");
    return ctx.sm->RequestTransition(ctx.state_calibrating);
  }
  return osp::TransitionResult::kUnhandled;
}

static osp::TransitionResult SensorCalibratingHandler(SensorContext& ctx, const osp::Event& event) {
  if (event.id == kEventCalibrateDone) {
    ctx.calibrated = true;
    OSP_LOG_INFO("Sensor", "Calibration complete");
    return ctx.sm->RequestTransition(ctx.state_running);
  }
  return osp::TransitionResult::kUnhandled;
}

static void SensorRunningEntry(SensorContext& ctx) {
  OSP_LOG_INFO("Sensor", "Sensor running");
}

static osp::TransitionResult SensorRunningHandler(SensorContext& ctx, const osp::Event& event) {
  if (event.id == kEventError) {
    ctx.error_count++;
    OSP_LOG_WARN("Sensor", "Error detected, count=%u", ctx.error_count);
    return ctx.sm->RequestTransition(ctx.state_error);
  }
  return osp::TransitionResult::kHandled;
}

static void SensorErrorEntry(SensorContext& ctx) {
  OSP_LOG_ERROR("Sensor", "Entered error state");
}

static osp::TransitionResult SensorErrorHandler(SensorContext& ctx, const osp::Event& event) {
  if (event.id == kEventRecover) {
    OSP_LOG_INFO("Sensor", "Recovering from error");
    return ctx.sm->RequestTransition(ctx.state_running);
  }
  return osp::TransitionResult::kHandled;
}

// ============================================================================
// Controller Node BT Actions
// ============================================================================

static osp::NodeStatus BtCheckSensorValid(ControllerContext& ctx) {
  if (ctx.sensor_data_valid) {
    return osp::NodeStatus::kSuccess;
  }
  return osp::NodeStatus::kFailure;
}

static osp::NodeStatus BtEvaluateThreshold(ControllerContext& ctx) {
  if (ctx.last_sensor_data.temperature > ctx.threshold_high) {
    ctx.action_mode = 1; // Cool down
    return osp::NodeStatus::kSuccess;
  }
  if (ctx.last_sensor_data.temperature < ctx.threshold_low) {
    ctx.action_mode = 2; // Heat up
    return osp::NodeStatus::kSuccess;
  }
  ctx.action_mode = 0; // Normal
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus BtSelectAction(ControllerContext& ctx) {
  // Action already selected in evaluate
  return osp::NodeStatus::kSuccess;
}

static osp::NodeStatus BtSendCommand(ControllerContext& ctx) {
  ControlCommand cmd;
  cmd.mode = ctx.action_mode;
  cmd.target_value = (ctx.action_mode == 1) ? 20.0f : 25.0f;

  auto r = ctx.transport->Send(kMsgTypeControlCommand, &cmd, sizeof(cmd));
  if (r) {
    OSP_LOG_INFO("Controller", "Sent command: mode=%u, target=%.1f",
                 cmd.mode, cmd.target_value);
    return osp::NodeStatus::kSuccess;
  }
  return osp::NodeStatus::kFailure;
}

// ============================================================================
// Serial RX Callbacks
// ============================================================================

static void SensorRxCallback(const void* payload, uint32_t size,
                             uint16_t type_index, uint16_t seq, void* ctx) {
  (void)seq;
  auto* sensor_ctx = static_cast<SensorContext*>(ctx);

  if (type_index == kMsgTypeControlCommand && size == sizeof(ControlCommand)) {
    ControlCommand cmd;
    std::memcpy(&cmd, payload, sizeof(cmd));
    OSP_LOG_INFO("Sensor", "Received command: mode=%u, target=%.1f",
                 cmd.mode, cmd.target_value);
  }
}

static void ControllerRxCallback(const void* payload, uint32_t size,
                                 uint16_t type_index, uint16_t seq, void* ctx) {
  (void)seq;
  auto* ctrl_ctx = static_cast<ControllerContext*>(ctx);

  if (type_index == kMsgTypeSensorData && size == sizeof(SensorData)) {
    std::memcpy(&ctrl_ctx->last_sensor_data, payload, sizeof(SensorData));
    ctrl_ctx->sensor_data_valid = true;
    OSP_LOG_INFO("Controller", "Received sensor data: temp=%.1f, pressure=%.1f",
                 ctrl_ctx->last_sensor_data.temperature,
                 ctrl_ctx->last_sensor_data.pressure);
  }
}

// ============================================================================
// Timer Callback - Periodic Sensor Data Publishing
// ============================================================================

static void SensorPublishCallback(void* ctx) {
  auto* sensor_ctx = static_cast<SensorContext*>(ctx);

  if (!sensor_ctx->calibrated) {
    return;
  }

  // Simulate sensor readings
  sensor_ctx->temperature = 22.0f + (rand() % 100) / 10.0f;
  sensor_ctx->pressure = 1013.0f + (rand() % 50) / 10.0f;

  SensorData data;
  data.temperature = sensor_ctx->temperature;
  data.pressure = sensor_ctx->pressure;
  data.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  auto r = sensor_ctx->transport->Send(kMsgTypeSensorData, &data, sizeof(data));
  if (r) {
    OSP_LOG_DEBUG("Sensor", "Published: temp=%.1f, pressure=%.1f",
                  data.temperature, data.pressure);
  }
}

// ============================================================================
// Main Demo
// ============================================================================

int main() {
  osp::log::Init();
  osp::log::SetLevel(osp::log::Level::kInfo);

  OSP_LOG_INFO("Demo", "=== Serial Communication Demo ===");

  // Create PTY pairs
  auto pty = CreatePtyPair();
  if (!pty.valid) {
    OSP_LOG_ERROR("Demo", "Failed to create PTY pairs");
    return 1;
  }

  OSP_LOG_INFO("Demo", "PTY A: %s", pty.slave_a_name);
  OSP_LOG_INFO("Demo", "PTY B: %s", pty.slave_b_name);

  // Configure serial transports
  osp::SerialConfig cfg_a;
  cfg_a.port_name.assign(osp::TruncateToCapacity, pty.slave_a_name);
  cfg_a.baud_rate = 115200;
  cfg_a.reliability.enable_ack = true;
  cfg_a.reliability.max_retries = 3;

  osp::SerialConfig cfg_b;
  cfg_b.port_name.assign(osp::TruncateToCapacity, pty.slave_b_name);
  cfg_b.baud_rate = 115200;
  cfg_b.reliability.enable_ack = true;
  cfg_b.reliability.max_retries = 3;

  osp::SerialTransport transport_a(cfg_a);
  osp::SerialTransport transport_b(cfg_b);

  if (!transport_a.Open()) {
    OSP_LOG_ERROR("Demo", "Failed to open transport A");
    ClosePty(pty);
    return 1;
  }

  if (!transport_b.Open()) {
    OSP_LOG_ERROR("Demo", "Failed to open transport B");
    ClosePty(pty);
    return 1;
  }

  OSP_LOG_INFO("Demo", "Serial transports opened");

  // Setup Sensor Node (HSM)
  SensorContext sensor_ctx{};
  sensor_ctx.transport = &transport_a;
  sensor_ctx.temperature = 25.0f;
  sensor_ctx.pressure = 1013.25f;
  sensor_ctx.error_count = 0;
  sensor_ctx.calibrated = false;

  osp::StateMachine<SensorContext, 8> sensor_sm(sensor_ctx);
  sensor_ctx.sm = &sensor_sm;

  int32_t s_unconfigured = sensor_sm.AddState({
    "Unconfigured", -1, SensorUnconfiguredHandler, nullptr, nullptr, nullptr
  });

  int32_t s_calibrating = sensor_sm.AddState({
    "Calibrating", -1, SensorCalibratingHandler, nullptr, nullptr, nullptr
  });

  int32_t s_running = sensor_sm.AddState({
    "Running", -1, SensorRunningHandler, SensorRunningEntry, nullptr, nullptr
  });

  int32_t s_error = sensor_sm.AddState({
    "Error", -1, SensorErrorHandler, SensorErrorEntry, nullptr, nullptr
  });

  sensor_ctx.state_calibrating = s_calibrating;
  sensor_ctx.state_running = s_running;
  sensor_ctx.state_error = s_error;

  sensor_sm.SetInitialState(s_unconfigured);
  sensor_sm.Start();

  transport_a.SetRxCallback(SensorRxCallback, &sensor_ctx);

  // Setup Controller Node (BT)
  ControllerContext ctrl_ctx{};
  ctrl_ctx.transport = &transport_b;
  ctrl_ctx.sensor_data_valid = false;
  ctrl_ctx.threshold_high = 28.0f;
  ctrl_ctx.threshold_low = 20.0f;
  ctrl_ctx.action_mode = 0;

  osp::BehaviorTree<ControllerContext, 16> ctrl_bt(ctrl_ctx, "controller_bt");

  int32_t bt_root = ctrl_bt.AddSequence("root");
  ctrl_bt.AddCondition("check_valid", BtCheckSensorValid, bt_root);
  ctrl_bt.AddAction("evaluate", BtEvaluateThreshold, bt_root);
  ctrl_bt.AddAction("select", BtSelectAction, bt_root);
  ctrl_bt.AddAction("send_cmd", BtSendCommand, bt_root);
  ctrl_bt.SetRoot(bt_root);

  transport_b.SetRxCallback(ControllerRxCallback, &ctrl_ctx);

  // Setup Timer for periodic sensor publishing
  osp::TimerScheduler<4> timer;
  timer.Start();
  timer.Add(500, SensorPublishCallback, &sensor_ctx); // 500ms period

  OSP_LOG_INFO("Demo", "Starting demo loop (5 seconds)...");

  // Trigger sensor configuration
  sensor_sm.Dispatch({kEventConfigure, nullptr});

  // Simulate calibration after 500ms
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  sensor_sm.Dispatch({kEventCalibrateDone, nullptr});

  // Main loop
  auto start_time = std::chrono::steady_clock::now();
  uint32_t loop_count = 0;

  while (true) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    if (elapsed >= 5) {
      break;
    }

    // Cross-connect PTY data
    CrossConnectPty(pty.master_a, pty.master_b);

    // Poll serial transports
    transport_a.Poll();
    transport_b.Poll();

    // Tick controller BT every 1 second
    if (loop_count % 100 == 0 && ctrl_ctx.sensor_data_valid) {
      ctrl_bt.Tick();
    }

    loop_count++;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  OSP_LOG_INFO("Demo", "Demo complete");

  // Print statistics
  auto stats_a = transport_a.GetStatistics();
  auto stats_b = transport_b.GetStatistics();

  std::printf("\n=== Statistics ===\n");
  std::printf("Node A (Sensor):\n");
  std::printf("  Frames sent:     %lu\n", stats_a.frames_sent);
  std::printf("  Frames received: %lu\n", stats_a.frames_received);
  std::printf("  Bytes sent:      %lu\n", stats_a.bytes_sent);
  std::printf("  Bytes received:  %lu\n", stats_a.bytes_received);
  std::printf("  CRC errors:      %lu\n", stats_a.crc_errors);
  std::printf("  Retransmits:     %lu\n", stats_a.retransmits);
  std::printf("\n");
  std::printf("Node B (Controller):\n");
  std::printf("  Frames sent:     %lu\n", stats_b.frames_sent);
  std::printf("  Frames received: %lu\n", stats_b.frames_received);
  std::printf("  Bytes sent:      %lu\n", stats_b.bytes_sent);
  std::printf("  Bytes received:  %lu\n", stats_b.bytes_received);
  std::printf("  CRC errors:      %lu\n", stats_b.crc_errors);
  std::printf("  Retransmits:     %lu\n", stats_b.retransmits);

  // Cleanup
  timer.Stop();
  transport_a.Close();
  transport_b.Close();
  ClosePty(pty);

  osp::log::Shutdown();

  return 0;
}
