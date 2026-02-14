# newosp

Modern C++17 header-only embedded infrastructure library for ARM-Linux industrial systems. Designed from scratch for LiDAR, robotics, and edge computing platforms. 38 headers, 788 tests, ASan/TSan/UBSan clean.

newosp provides a complete foundation for building high-performance embedded applications with zero global state, stack-first allocation, and lock-free messaging. All modules are header-only with RAII resource management and type-safe error handling.

## Core Features

- **Zero global state**: All state encapsulated in objects (RAII)
- **Stack-first allocation**: Fixed-capacity containers, zero heap in hot paths
- **`-fno-exceptions -fno-rtti` compatible**: Designed for embedded ARM-Linux
- **Type-safe error handling**: `expected<V,E>` and `optional<T>` vocabulary types
- **Header-only**: Single CMake INTERFACE library, C++17 standard
- **Lock-free messaging**: MPSC ring buffer bus with priority-based admission control
- **Template-based design patterns**: Tag dispatch, variadic templates, CRTP, compile-time composition

## Module Overview

### Foundation Layer (8 modules)

| Module | Description |
|--------|-------------|
| `platform.hpp` | Platform/architecture detection, compiler hints, `OSP_ASSERT`, `SteadyNowUs` |
| `vocabulary.hpp` | `expected`, `optional`, `FixedVector`, `FixedString`, `FixedFunction`, `function_ref`, `not_null`, `NewType`, `ScopeGuard` |
| `config.hpp` | Multi-format config parser (INI/JSON/YAML), template-based backend dispatch |
| `log.hpp` | Logging macros, compile-time level filtering (stderr backend) |
| `timer.hpp` | Timer task scheduler based on `std::chrono::steady_clock` |
| `shell.hpp` | Remote debug shell (telnet), TAB completion, command history |
| `mem_pool.hpp` | Fixed-block memory pool (`FixedPool<BlockSize, MaxBlocks>`), embedded free list |
| `shutdown.hpp` | Async-signal-safe graceful shutdown, LIFO callbacks, `pipe(2)` wakeup |

### Core Communication Layer (7 modules)

| Module | Description |
|--------|-------------|
| `bus.hpp` | Lock-free MPSC message bus (`AsyncBus<PayloadVariant>`), CAS publish, topic routing |
| `node.hpp` | Lightweight pub/sub node (`Node<PayloadVariant>`), Bus injection, FNV-1a topic hash |
| `worker_pool.hpp` | Multi-worker thread pool, AsyncBus + SPSC per-worker queues, AdaptiveBackoff |
| `spsc_ringbuffer.hpp` | Lock-free wait-free SPSC ring buffer (trivially_copyable, batch ops, FakeTSO) |
| `executor.hpp` | Scheduler (Single/Static/Pinned + RealtimeExecutor SCHED_FIFO) |
| `semaphore.hpp` | Lightweight semaphore (futex-based LightSemaphore/PosixSemaphore) |
| `data_fusion.hpp` | Multi-source data fusion (time alignment, interpolation) |

### State Machine & Behavior Tree (2 modules)

| Module | Description |
|--------|-------------|
| `hsm.hpp` | Hierarchical state machine (LCA transitions, guard conditions, zero heap) |
| `bt.hpp` | Behavior tree (Sequence/Fallback/Parallel, flat array storage, cache-friendly) |

### Network & Transport Layer (8 modules)

| Module | Description |
|--------|-------------|
| `socket.hpp` | TCP/UDP RAII wrapper (sockpp) |
| `io_poller.hpp` | epoll event loop (edge-triggered + timeout) |
| `connection.hpp` | Connection pool management (auto-reconnect, heartbeat) |
| `transport.hpp` | Network transport (v0/v1 frame protocol, SequenceTracker) |
| `shm_transport.hpp` | Shared memory IPC (lock-free SPSC, ARM memory ordering, CreateOrReplace crash recovery) |
| `serial_transport.hpp` | Industrial serial transport (CRC-CCITT, PTY testing, IEC 61508) |
| `net.hpp` | Network layer wrapper (address resolution, socket options) |
| `transport_factory.hpp` | Automatic transport selection (inproc/shm/tcp) |

### Service & Discovery Layer (6 modules)

| Module | Description |
|--------|-------------|
| `service.hpp` | RPC service (request-response, ServiceRegistry, AsyncClient) |
| `discovery.hpp` | Node discovery (UDP multicast + static config, TopicAwareDiscovery) |
| `node_manager.hpp` | Node management + heartbeat monitoring |
| `node_manager_hsm.hpp` | HSM-driven node heartbeat state machine (Connected/Suspect/Disconnected) |
| `service_hsm.hpp` | HSM-driven service lifecycle (Idle/Listening/Active/Error/ShuttingDown) |
| `discovery_hsm.hpp` | HSM-driven discovery flow (Idle/Announcing/Discovering/Stable/Degraded) |

### Application Layer (4 modules)

| Module | Description |
|--------|-------------|
| `app.hpp` | Application/Instance two-tier model (MakeIID, HSM-driven) |
| `post.hpp` | Unified posting (AppRegistry + OspPost + OspSendAndWait) |
| `qos.hpp` | QoS configuration (Reliability/History/Deadline/Lifespan) |
| `lifecycle_node.hpp` | Lifecycle node (Unconfigured/Inactive/Active/Finalized, HSM-driven) |

### Reliability Layer (3 modules)

| Module | Description |
|--------|-------------|
| `watchdog.hpp` | Software watchdog (deadline monitoring, timeout callbacks) |
| `fault_collector.hpp` | Fault collection and reporting (FaultReporter POD injection, ring buffer) |
| `shell_commands.hpp` | Built-in diagnostic shell command bridge (zero-intrusion, 15 Register functions) |

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Application Layer                           │
│  ┌──────────────────────────┐  ┌──────────────────────────────┐    │
│  │ app.hpp                  │  │ lifecycle_node.hpp           │    │
│  │ post.hpp                 │  │ qos.hpp                      │    │
│  └──────────────────────────┘  └──────────────────────────────┘    │
└────────────────────────┬────────────────────┬───────────────────────┘
                         │                    │
                         v                    v
┌─────────────────────────────────────────────────────────────────────┐
│                    Service & Discovery Layer                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │ service.hpp  │  │ discovery    │  │ node_manager.hpp         │  │
│  │ service_hsm  │  │ discovery_hsm│  │ node_manager_hsm.hpp     │  │
│  └──────────────┘  └──────────────┘  └──────────────────────────┘  │
└──────────┬──────────────────┬──────────────────────────────────────┘
           │                  │
           v                  v
┌──────────────────────────────────────────────────────────────┐
│                      Transport Layer                         │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ transport.hpp / shm_transport.hpp / serial_transport   │  │
│  │ transport_factory.hpp / data_fusion.hpp                │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────┬──────────────────────────────────────────────────┘
           │
           v
┌──────────────────────────────────────────────────────────────┐
│                       Network Layer                          │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ socket.hpp / connection.hpp                            │  │
│  │ io_poller.hpp / net.hpp                                │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────┬──────────────────────────────────────────────────┘
           │
           v
┌──────────────────────────────────────────────────────────────┐
│                  Core Communication Layer                    │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ bus.hpp / node.hpp                                     │  │
│  │ spsc_ringbuffer.hpp / worker_pool.hpp                  │  │
│  │ executor.hpp / semaphore.hpp                           │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────┬──────────────┬───────────────────────────────────┘
           │              │
           v              v
┌──────────────────────┐  ┌──────────────────────────────────┐
│ State Machine & BT   │  │    Reliability Layer             │
│  ┌────────────────┐  │  │  ┌────────────────────────────┐  │
│  │ hsm.hpp        │  │  │  │ watchdog.hpp               │  │
│  │ bt.hpp         │  │  │  │ fault_collector.hpp        │  │
│  └────────────────┘  │  │  │ shell_commands.hpp         │  │
└──────────┬───────────┘  │  └────────────────────────────┘  │
           │              └──────────┬───────────────────────┘
           │                         │
           v                         v
┌─────────────────────────────────────────────────────────────┐
│                      Foundation Layer                       │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ platform.hpp / vocabulary.hpp                         │  │
│  │ config.hpp / log.hpp                                  │  │
│  │ timer.hpp / shell.hpp / mem_pool.hpp / shutdown.hpp   │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Quick Navigation

- [[Quick-Start]] - Build instructions and hello world example
- [[Architecture]] - Detailed architecture design and module dependencies
- [[API-Foundation-Core]] - Foundation and Core Communication API reference
- [[API-Network-Transport]] - Network and Transport layer API reference
- [[API-State-Scheduler]] - State Machine, Behavior Tree, and Scheduler API reference
- [[API-Service-App]] - Service, Discovery, and Application layer API reference
- [[Performance]] - Benchmark results and performance analysis
- [[Examples]] - Example programs and usage patterns

## Performance Highlights

- **Lock-free MPSC Bus**: 10M+ msg/s single-threaded, 5M+ msg/s multi-threaded (4 publishers)
- **SPSC Ring Buffer**: 50M+ ops/s (batch mode), wait-free on ARM Cortex-A
- **Shared Memory IPC**: 2.5M+ msg/s (4KB payload), zero-copy between processes
- **Serial Transport**: 115200 baud with CRC-CCITT, <1ms latency for 256B frames
- **TCP Transport**: 1M+ msg/s (small payloads), automatic reconnection and heartbeat
- **Memory footprint**: <100KB for core modules (bus + node + executor + transport)
- **Zero heap allocation**: Hot paths use stack-only fixed-capacity containers

## Requirements

- CMake >= 3.14
- C++17 compiler (GCC >= 7, Clang >= 5)
- Linux (ARM-Linux embedded platform)

## License

MIT - see [LICENSE](https://github.com/DeguiLiu/newosp/blob/main/LICENSE)
