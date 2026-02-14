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

### Single-File Demos

| Example | Description | Key Modules |
|---------|-------------|-------------|
| `basic_demo` | Fundamental publish-subscribe pattern | Bus, Node |
| `benchmark` | Performance measurements for core components | Bus, ShmRingBuffer, Transport, MemPool, Timer, WorkerPool |
| `bt_patrol_demo` | Patrol robot with emergency handling and battery management | BehaviorTree |
| `client_demo` | Gateway simulation with parallel processing and heartbeat monitoring | Bus, Node, WorkerPool, Shutdown |
| `codegen_demo` | YAML code generation demonstration | Config, codegen |
| `hsm_bt_combo_demo` | Industrial device controller combining state machines and behavior trees | HSM, BehaviorTree |
| `hsm_protocol_demo` | Communication protocol state machine with hierarchical states | HSM |
| `node_manager_hsm_demo` | Node connection management with heartbeat-driven state transitions | HSM, NodeManager |
| `priority_demo` | Priority-based admission control under mixed-priority load | Bus, Node |
| `protocol_demo` | Video streaming protocol handler (GB28181/RTSP-style) | Bus, Node, Timer |
| `realtime_executor_demo` | Lifecycle nodes with periodic sensor publishing | Bus, Executor, LifecycleNode, QoS |
| `serial_demo` | Industrial serial communication with CRC and ACK reliability | SerialTransport, HSM, BehaviorTree, Timer |
| `worker_pool_demo` | WorkerPool task dispatch and parallel processing | WorkerPool, Bus |

### Multi-File Application Demos

Complete application examples with dedicated subdirectories and design documentation.

| Example | Scenario | Modules | Complexity |
|---------|----------|---------|------------|
| [`client_gateway/`](client_gateway/) | IoT/video edge gateway: multi-client connect, parallel processing, heartbeat | Bus, Node, WorkerPool | Low |
| [`streaming_protocol/`](streaming_protocol/) | GB28181/RTSP video surveillance: registration, heartbeat, stream control | Bus, Node, Timer | Low |
| [`serial_ota/`](serial_ota/) | Industrial serial OTA firmware upgrade: HSM+BT, CRC, noise simulation, ACK retransmission | HSM, BT, Timer, Bus, WorkerPool, SpscRingbuffer, Shell + 6 more | High |
| [`shm_ipc/`](shm_ipc/) | Cross-process shared memory video streaming: producer-consumer, HSM back-pressure | ShmChannel, HSM, MemPool, Timer, Watchdog, FaultCollector, Shell + 5 more | High |
| [`net_stress/`](net_stress/) | TCP network stress test: 3-process (server/client/monitor), RPC echo + file transfer | Service, HSM, Bus, Node, Timer, Shell + 6 more | High |

## Single-File Demo Details

### basic_demo
Demonstrates the fundamental publish-subscribe pattern using AsyncBus and Node. Shows message type definitions with std::variant, typed subscriptions, and bus statistics querying.

### benchmark
Comprehensive performance benchmark measuring throughput, latency, and overhead for AsyncBus publish/subscribe, ShmRingBuffer shared memory SPSC, Transport frame encoding/decoding, MemPool allocation, Timer scheduling, and WorkerPool task dispatch.

### bt_patrol_demo
Patrol robot demonstration using BehaviorTree. The robot patrols waypoints while managing battery levels, handles emergency situations with immediate stop, and returns to base when battery is low.

### client_demo
Simulates a gateway with parallel data processing and heartbeat monitoring. Demonstrates WorkerPool usage for concurrent task execution and multi-node communication patterns.

### hsm_bt_combo_demo
Industrial device controller combining HSM for high-level mode management (Idle -> Initializing -> Running -> Error -> Shutdown) and BehaviorTree for Running mode behavior (check sensors -> execute task -> report status). Shows error handling and recovery logic.

### hsm_protocol_demo
Communication protocol state machine modeling a TCP-lite/Modbus-like connection protocol. Demonstrates hierarchical state inheritance where the Connected parent state handles DISCONNECT events for both Idle and Active child states.

### node_manager_hsm_demo
Node connection management with HSM-driven heartbeat state machine. Shows state transitions (Connected -> Suspect -> Disconnected), disconnect callbacks, reconnection handling, and multiple nodes with independent state machines.

### priority_demo
Demonstrates priority-based admission control and backpressure behavior under mixed-priority load. Shows how AsyncBus handles CriticalAlert (high), TelemetryData (medium), and DiagnosticLog (low) messages under congestion.

### protocol_demo
Video streaming protocol handler simulating GB28181/RTSP-style pipeline. Implements device registration, periodic heartbeat via TimerScheduler, and stream control with priority-based message handling.

### realtime_executor_demo
Lifecycle node demonstration with SingleThreadExecutor. Shows state transitions (Unconfigured -> Inactive -> Active -> Finalized), periodic sensor data publishing with QoS profiles, and control node command processing.

### serial_demo
Industrial serial communication demonstration with PTY pairs for hardware-free testing. Node A (Sensor) uses HSM-driven lifecycle (Init -> Calibrating -> Running -> Error), Node B (Controller) uses BT-driven decision logic (check -> evaluate -> act). Features reliable bidirectional communication with CRC-CCITT and ACK protocol.

## Multi-File Application Demo Details

### client_gateway/
IoT/video edge gateway simulation. 4 clients connect, 32 data messages processed in parallel by WorkerPool (2 workers), heartbeat monitoring, orderly disconnect. Demonstrates Bus + Node + WorkerPool separation of concerns. See [client_gateway/README.md](client_gateway/README.md).

### streaming_protocol/
GB28181/RTSP video surveillance protocol pipeline. 4 nodes (Registrar, HeartbeatMonitor, StreamController, Client) share a single AsyncBus with priority-aware publishing. Timer-driven heartbeat generation with late-detection. See [streaming_protocol/README.md](streaming_protocol/README.md).

### serial_ota/
Industrial serial OTA firmware upgrade with 12 newosp components. Device uses HSM (6 states) for OTA processing, host uses BehaviorTree (4-node Sequence) for upgrade flow. SpscRingbuffer simulates bidirectional UART with ~5% channel noise and ACK-based retransmission. See [serial_ota/README.md](serial_ota/README.md).

### shm_ipc/
Cross-process shared memory IPC for video frame streaming. 3 processes (producer/consumer/monitor) communicate via POSIX shared memory with lock-free SPSC ring buffer. HSM-driven back-pressure (Streaming -> Paused -> Throttled). Integrates Watchdog + FaultCollector for monitoring. See [shm_ipc/README.md](shm_ipc/README.md).

### net_stress/
Full TCP network stress testing framework with 3 independent executables. Server runs 3 RPC services (handshake + echo + file transfer). Client supports up to 64 instances with HSM-driven connection management and dual-channel parallel testing (echo + file). Monitor provides independent latency probing with CAS-based statistics. See [net_stress/README.md](net_stress/README.md).

## Running Examples

After building, run any single-file example directly:

```bash
./build/examples/basic_demo
./build/examples/serial_demo
./build/examples/benchmark
```

For multi-file examples, see each subdirectory's README for specific instructions:

```bash
# Single-process demos
./build/examples/osp_client_gateway
./build/examples/osp_streaming_protocol
./build/examples/osp_serial_ota_demo

# Multi-process demos (run in separate terminals)
./build/examples/net_stress/osp_net_stress_server
./build/examples/net_stress/osp_net_stress_client 127.0.0.1 4
./build/examples/net_stress/osp_net_stress_monitor 127.0.0.1

./build/examples/shm_ipc/osp_shm_producer frame_ch 1000
./build/examples/shm_ipc/osp_shm_consumer frame_ch
./build/examples/shm_ipc/osp_shm_monitor frame_ch 9527
```

Most examples are self-contained and require no additional configuration. The serial_demo and serial_ota automatically create PTY pairs for testing on Linux platforms.
