# newosp Examples

This directory contains demonstration programs showcasing the capabilities of newosp, a C++17 header-only embedded infrastructure library for ARM-Linux platforms.

## Building Examples

Enable example builds with CMake:

```bash
cmake -B build -DOSP_BUILD_EXAMPLES=ON
cmake --build build
```

Executables will be generated in `build/examples/`.

## Example Overview

| Example | Description | Key Modules |
|---------|-------------|-------------|
| `basic_demo` | Fundamental publish-subscribe pattern | Bus, Node |
| `benchmark` | Performance measurements for core components | Bus, ShmRingBuffer, Transport, MemPool, Timer, WorkerPool |
| `bt_patrol_demo` | Patrol robot with emergency handling and battery management | BehaviorTree |
| `client_demo` | Gateway simulation with parallel processing and heartbeat monitoring | Bus, Node, WorkerPool, Shutdown |
| `hsm_bt_combo_demo` | Industrial device controller combining state machines and behavior trees | HSM, BehaviorTree |
| `hsm_protocol_demo` | Communication protocol state machine with hierarchical states | HSM |
| `node_manager_hsm_demo` | Node connection management with heartbeat-driven state transitions | HSM, NodeManager |
| `priority_demo` | Priority-based admission control under mixed-priority load | Bus, Node |
| `protocol_demo` | Video streaming protocol handler (GB28181/RTSP-style) | Bus, Node, Timer |
| `realtime_executor_demo` | Lifecycle nodes with periodic sensor publishing | Bus, Executor, LifecycleNode, QoS |
| `serial_demo` | Industrial serial communication with CRC and ACK reliability | SerialTransport, HSM, BehaviorTree, Timer |

## Example Details

### basic_demo
Demonstrates the fundamental publish-subscribe pattern using AsyncBus and Node. Shows message type definitions with std::variant, typed subscriptions, and bus statistics querying.

### benchmark
Comprehensive performance benchmark measuring throughput, latency, and overhead for AsyncBus publish/subscribe, ShmRingBuffer shared memory SPSC, Transport frame encoding/decoding, MemPool allocation, Timer scheduling, and WorkerPool task dispatch.

### bt_patrol_demo
Patrol robot demonstration using BehaviorTree. The robot patrols waypoints while managing battery levels, handles emergency situations with immediate stop, and returns to base when battery is low.

### client_demo
Simulates a gateway with parallel data processing and heartbeat monitoring. Demonstrates WorkerPool usage for concurrent task execution and multi-node communication patterns.

### hsm_bt_combo_demo
Industrial device controller combining HSM for high-level mode management (Idle → Initializing → Running → Error → Shutdown) and BehaviorTree for Running mode behavior (check sensors → execute task → report status). Shows error handling and recovery logic.

### hsm_protocol_demo
Communication protocol state machine modeling a TCP-lite/Modbus-like connection protocol. Demonstrates hierarchical state inheritance where the Connected parent state handles DISCONNECT events for both Idle and Active child states.

### node_manager_hsm_demo
Node connection management with HSM-driven heartbeat state machine. Shows state transitions (Connected → Suspect → Disconnected), disconnect callbacks, reconnection handling, and multiple nodes with independent state machines.

### priority_demo
Demonstrates priority-based admission control and backpressure behavior under mixed-priority load. Shows how AsyncBus handles CriticalAlert (high), TelemetryData (medium), and DiagnosticLog (low) messages under congestion.

### protocol_demo
Video streaming protocol handler simulating GB28181/RTSP-style pipeline. Implements device registration, periodic heartbeat via TimerScheduler, and stream control with priority-based message handling.

### realtime_executor_demo
Lifecycle node demonstration with SingleThreadExecutor. Shows state transitions (Unconfigured → Inactive → Active → Finalized), periodic sensor data publishing with QoS profiles, and control node command processing.

### serial_demo
Industrial serial communication demonstration with PTY pairs for hardware-free testing. Node A (Sensor) uses HSM-driven lifecycle (Init → Calibrating → Running → Error), Node B (Controller) uses BT-driven decision logic (check → evaluate → act). Features reliable bidirectional communication with CRC-CCITT and ACK protocol.

## Running Examples

After building, run any example directly:

```bash
./build/examples/basic_demo
./build/examples/serial_demo
./build/examples/benchmark
```

Most examples are self-contained and require no additional configuration. The serial_demo automatically creates PTY pairs for testing on Linux platforms.
