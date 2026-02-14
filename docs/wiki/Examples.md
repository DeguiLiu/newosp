# Examples

This page provides an overview of demonstration programs showcasing newosp capabilities for ARM-Linux embedded platforms.

## Building Examples

Enable example builds with CMake:

```bash
cmake -B build -DOSP_BUILD_EXAMPLES=ON
cmake --build build
```

Executables will be generated in `build/examples/`.

## Single-File Demos

These standalone examples demonstrate individual features or simple integration patterns.

| Example | Description | Build Command |
|---------|-------------|---------------|
| `basic_demo` | Fundamental publish-subscribe pattern with Bus and Node | `./build/examples/basic_demo` |
| `benchmark` | Performance measurements for core components (Bus, ShmRingBuffer, Transport, MemPool, Timer, WorkerPool) | `./build/examples/benchmark` |
| `bt_patrol_demo` | Patrol robot with BehaviorTree-driven emergency handling and battery management | `./build/examples/bt_patrol_demo` |
| `client_demo` | Gateway simulation with parallel processing and heartbeat monitoring | `./build/examples/client_demo` |
| `codegen_demo` | YAML code generation demonstration | `./build/examples/codegen_demo` |
| `hsm_bt_combo_demo` | Industrial device controller combining HSM state management and BehaviorTree task execution | `./build/examples/hsm_bt_combo_demo` |
| `hsm_protocol_demo` | Communication protocol state machine with hierarchical state inheritance | `./build/examples/hsm_protocol_demo` |
| `node_manager_hsm_demo` | Node connection management with HSM-driven heartbeat state transitions | `./build/examples/node_manager_hsm_demo` |
| `priority_demo` | Priority-based admission control under mixed-priority load | `./build/examples/priority_demo` |
| `protocol_demo` | Video streaming protocol handler (GB28181/RTSP-style) with registration and heartbeat | `./build/examples/protocol_demo` |
| `realtime_executor_demo` | Lifecycle nodes with periodic sensor publishing and QoS profiles | `./build/examples/realtime_executor_demo` |
| `serial_demo` | Industrial serial communication with CRC-CCITT and ACK reliability, HSM+BT integration | `./build/examples/serial_demo` |
| `worker_pool_demo` | WorkerPool task dispatch and parallel processing | `./build/examples/worker_pool_demo` |

### Key Module Dependencies

- **Bus + Node**: `basic_demo`, `client_demo`, `priority_demo`, `protocol_demo`, `worker_pool_demo`
- **HSM**: `hsm_protocol_demo`, `node_manager_hsm_demo`, `hsm_bt_combo_demo`, `serial_demo`
- **BehaviorTree**: `bt_patrol_demo`, `hsm_bt_combo_demo`, `serial_demo`
- **Executor + LifecycleNode**: `realtime_executor_demo`
- **SerialTransport**: `serial_demo`
- **Timer**: `protocol_demo`, `serial_demo`
- **WorkerPool**: `client_demo`, `worker_pool_demo`

## Multi-File Application Demos

Complete application examples with dedicated subdirectories, design documentation, and realistic integration scenarios.

### client_gateway

**Scenario**: IoT/video edge gateway with multi-client connection management

**Features**:
- 4 clients connect to gateway
- 32 data messages processed in parallel by WorkerPool (2 workers)
- Heartbeat monitoring for client liveness
- Orderly disconnect handling

**Key Modules**: Bus, Node, WorkerPool, Shutdown

**Complexity**: Low

**Build and Run**:
```bash
./build/examples/osp_client_gateway
```

See `examples/client_gateway/README.md` for details.

### streaming_protocol

**Scenario**: GB28181/RTSP video surveillance protocol pipeline

**Features**:
- 4 nodes (Registrar, HeartbeatMonitor, StreamController, Client) share single AsyncBus
- Priority-aware message publishing
- Timer-driven heartbeat generation with late-detection
- Stream control command handling

**Key Modules**: Bus, Node, Timer

**Complexity**: Low

**Build and Run**:
```bash
./build/examples/osp_streaming_protocol
```

See `examples/streaming_protocol/README.md` for details.

### serial_ota

**Scenario**: Industrial serial OTA firmware upgrade with noise simulation

**Features**:
- Device uses HSM (6 states) for OTA processing
- Host uses BehaviorTree (4-node Sequence) for upgrade flow
- SpscRingbuffer simulates bidirectional UART with ~5% channel noise
- ACK-based retransmission for reliability
- Integrates 12 newosp components

**Key Modules**: HSM, BehaviorTree, Timer, Bus, WorkerPool, SpscRingbuffer, Shell, SerialTransport, MemPool, Shutdown, Log, Config

**Complexity**: High

**Build and Run**:
```bash
./build/examples/osp_serial_ota_demo
```

See `examples/serial_ota/README.md` for details.

### shm_ipc

**Scenario**: Cross-process shared memory video frame streaming

**Features**:
- 3 processes (producer/consumer/monitor) communicate via POSIX shared memory
- Lock-free SPSC ring buffer for zero-copy transfer
- HSM-driven back-pressure (Streaming → Paused → Throttled)
- Watchdog + FaultCollector integration for monitoring
- Telnet shell for runtime diagnostics

**Key Modules**: ShmChannel, HSM, MemPool, Timer, Watchdog, FaultCollector, Shell, Bus, Node, Shutdown, Log, Config

**Complexity**: High

**Build and Run** (separate terminals):
```bash
# Terminal 1: Producer
./build/examples/shm_ipc/osp_shm_producer frame_ch 1000

# Terminal 2: Consumer
./build/examples/shm_ipc/osp_shm_consumer frame_ch

# Terminal 3: Monitor (optional)
./build/examples/shm_ipc/osp_shm_monitor frame_ch 9527
# Then: telnet localhost 9527
```

See `examples/shm_ipc/README.md` for details.

### net_stress

**Scenario**: Full TCP network stress testing framework

**Features**:
- 3 independent executables (server/client/monitor)
- Server runs 3 RPC services (handshake + echo + file transfer)
- Client supports up to 64 instances with HSM-driven connection management
- Dual-channel parallel testing (echo + file)
- Monitor provides independent latency probing with CAS-based statistics
- Telnet shell for runtime control

**Key Modules**: Service, HSM, Bus, Node, Timer, Shell, Transport, Discovery, Shutdown, Log, Config, WorkerPool

**Complexity**: High

**Build and Run** (separate terminals):
```bash
# Terminal 1: Server
./build/examples/net_stress/osp_net_stress_server

# Terminal 2: Client (4 instances)
./build/examples/net_stress/osp_net_stress_client 127.0.0.1 4

# Terminal 3: Monitor (optional)
./build/examples/net_stress/osp_net_stress_monitor 127.0.0.1
```

See `examples/net_stress/README.md` for details.

## Performance Benchmarks

Dedicated benchmark programs for throughput and latency measurements.

### benchmarks/serial_benchmark

Serial transport throughput testing with PTY pairs, multiple payload sizes, ACK/no-ACK modes.

**Build and Run**:
```bash
./build/examples/benchmarks/serial_benchmark
```

### benchmarks/transport_benchmark

TCP loopback and ShmRingBuffer SPSC throughput testing across payload sizes.

**Build and Run**:
```bash
./build/examples/benchmarks/transport_benchmark
```

### benchmarks/bus_payload_benchmark

AsyncBus throughput with large variant types (64B-8KB payloads) to measure envelope overhead impact.

**Build and Run**:
```bash
./build/examples/benchmarks/bus_payload_benchmark
```

See [[Performance]] for detailed benchmark results.

## Example Details

### basic_demo

Demonstrates the fundamental publish-subscribe pattern using AsyncBus and Node. Shows message type definitions with `std::variant`, typed subscriptions, and bus statistics querying.

**Key Concepts**: Message variant definition, Node creation, typed subscription callbacks, Bus statistics

### benchmark

Comprehensive performance benchmark measuring throughput, latency, and overhead for AsyncBus publish/subscribe, ShmRingBuffer shared memory SPSC, Transport frame encoding/decoding, MemPool allocation, Timer scheduling, and WorkerPool task dispatch.

**Key Concepts**: Performance profiling, throughput measurement, latency percentiles

### bt_patrol_demo

Patrol robot demonstration using BehaviorTree. The robot patrols waypoints while managing battery levels, handles emergency situations with immediate stop, and returns to base when battery is low.

**Key Concepts**: BehaviorTree composition (Sequence/Selector/Decorator), Condition nodes, Action nodes, context-driven behavior

### client_demo

Simulates a gateway with parallel data processing and heartbeat monitoring. Demonstrates WorkerPool usage for concurrent task execution and multi-node communication patterns.

**Key Concepts**: WorkerPool task submission, parallel processing, heartbeat monitoring, orderly shutdown

### codegen_demo

YAML code generation demonstration showing how to define message types, nodes, and connections in YAML and generate C++ code.

**Key Concepts**: YAML-driven code generation, message schema definition, node configuration

### hsm_bt_combo_demo

Industrial device controller combining HSM for high-level mode management (Idle → Initializing → Running → Error → Shutdown) and BehaviorTree for Running mode behavior (check sensors → execute task → report status). Shows error handling and recovery logic.

**Key Concepts**: HSM+BT integration, hierarchical state management, task-level behavior control, error recovery

### hsm_protocol_demo

Communication protocol state machine modeling a TCP-lite/Modbus-like connection protocol. Demonstrates hierarchical state inheritance where the Connected parent state handles DISCONNECT events for both Idle and Active child states.

**Key Concepts**: Hierarchical state machines, LCA-based transitions, parent state event handling, guard conditions

### node_manager_hsm_demo

Node connection management with HSM-driven heartbeat state machine. Shows state transitions (Connected → Suspect → Disconnected), disconnect callbacks, reconnection handling, and multiple nodes with independent state machines.

**Key Concepts**: Heartbeat-driven state transitions, timeout handling, disconnect callbacks, per-node state machines

### priority_demo

Demonstrates priority-based admission control and backpressure behavior under mixed-priority load. Shows how AsyncBus handles CriticalAlert (high), TelemetryData (medium), and DiagnosticLog (low) messages under congestion.

**Key Concepts**: Message priority, admission control, backpressure, queue watermark thresholds

### protocol_demo

Video streaming protocol handler simulating GB28181/RTSP-style pipeline. Implements device registration, periodic heartbeat via TimerScheduler, and stream control with priority-based message handling.

**Key Concepts**: Protocol state machine, timer-driven heartbeat, stream control commands, priority messaging

### realtime_executor_demo

Lifecycle node demonstration with SingleThreadExecutor. Shows state transitions (Unconfigured → Inactive → Active → Finalized), periodic sensor data publishing with QoS profiles, and control node command processing.

**Key Concepts**: LifecycleNode state machine, QoS profiles (Reliability/History/Deadline/Lifespan), Executor-driven scheduling

### serial_demo

Industrial serial communication demonstration with PTY pairs for hardware-free testing. Node A (Sensor) uses HSM-driven lifecycle (Init → Calibrating → Running → Error), Node B (Controller) uses BT-driven decision logic (check → evaluate → act). Features reliable bidirectional communication with CRC-CCITT and ACK protocol.

**Key Concepts**: SerialTransport, CRC-CCITT checksum, ACK reliability, HSM+BT integration, PTY testing

### worker_pool_demo

WorkerPool task dispatch and parallel processing demonstration. Shows task submission, worker thread execution, and result collection.

**Key Concepts**: WorkerPool task submission, parallel execution, SPSC queue-based dispatch

## Running Examples

After building, run any single-file example directly:

```bash
./build/examples/basic_demo
./build/examples/serial_demo
./build/examples/benchmark
```

For multi-file examples, see each subdirectory's README for specific instructions. Most examples are self-contained and require no additional configuration. The `serial_demo` and `serial_ota` automatically create PTY pairs for testing on Linux platforms.

## Next Steps

- See [[Quick-Start]] for getting started with newosp
- See [[Architecture]] for design principles and module overview
- See [[Performance]] for detailed benchmark results and tuning recommendations
