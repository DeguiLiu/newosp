# newosp

[![CI](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/newosp/actions/workflows/ci.yml)

Modern C++14/17 header-only embedded infrastructure library for ARM-Linux, extracted and modernized from the OSP (Open Streaming Platform) codebase (~140k LOC).

**[中文文档](README_zh.md)**

## Features

- **Zero global state**: All state encapsulated in objects (RAII)
- **Stack-first allocation**: Fixed-capacity containers, zero heap in hot paths
- **`-fno-exceptions -fno-rtti` compatible**: Designed for embedded ARM-Linux
- **Type-safe error handling**: `expected<V,E>` and `optional<T>` vocabulary types
- **Header-only**: Single CMake INTERFACE library, C++17 standard
- **Lock-free messaging**: MPSC ring buffer bus with priority-based admission control
- **Template-based design patterns**: Tag dispatch, variadic templates, CRTP, compile-time composition

## Modules

### Foundation Layer

| Module | Description |
|--------|-------------|
| `platform.hpp` | Platform/architecture detection, compiler hints, `OSP_ASSERT` macro |
| `vocabulary.hpp` | `expected`, `optional`, `FixedVector`, `FixedString`, `FixedFunction`, `function_ref`, `not_null`, `NewType`, `ScopeGuard` |
| `config.hpp` | Multi-format config parser (INI/JSON/YAML) with template-based backend dispatch |
| `log.hpp` | Logging macros with compile-time level filtering (stderr backend) |

### Core Layer

| Module | Description |
|--------|-------------|
| `timer.hpp` | Timer task scheduler based on `std::chrono::steady_clock` |
| `shell.hpp` | Remote debug shell (telnet) with TAB completion, command history, `OSP_SHELL_CMD` registration |
| `mem_pool.hpp` | Fixed-block memory pool (`FixedPool<BlockSize, MaxBlocks>`) with embedded free list |
| `shutdown.hpp` | Async-signal-safe graceful shutdown with LIFO callbacks and `pipe(2)` wakeup |
| `bus.hpp` | Lock-free MPSC message bus (`AsyncBus<PayloadVariant>`) with type-based routing |
| `node.hpp` | Lightweight pub/sub node abstraction (`Node<PayloadVariant>`) inspired by ROS2/CyberRT |
| `worker_pool.hpp` | Multi-worker thread pool built on AsyncBus with SPSC per-worker queues |
| `executor.hpp` | Scheduler (Single/Static/Pinned + Realtime SCHED_FIFO/DEADLINE) |
| `hsm.hpp` | Hierarchical state machine (LCA-based transitions, nested states) |
| `bt.hpp` | Behavior tree (Sequence/Fallback/Parallel composite nodes) |

### Network Layer

| Module | Description |
|--------|-------------|
| `socket.hpp` | TCP/UDP RAII wrapper (based on sockpp) |
| `connection.hpp` | Connection pool management (auto-reconnect, heartbeat) |
| `io_poller.hpp` | epoll event loop (edge-triggered + timeout) |
| `transport.hpp` | Transparent network transport (TCP/UDP frame protocol) |
| `semaphore.hpp` | Lightweight semaphore (futex-based) |
| `shm_transport.hpp` | Shared memory IPC (lock-free SPSC ring buffer) |
| `data_fusion.hpp` | Multi-source data fusion (time alignment, interpolation) |
| `discovery.hpp` | Node discovery (UDP multicast + static config) |
| `service.hpp` | RPC service (request-response pattern) |
| `node_manager.hpp` | Node management + heartbeat monitoring |
| `node_manager_hsm.hpp` | HSM-driven node connection management |

### Advanced Features

| Module | Description |
|--------|-------------|
| `lifecycle_node.hpp` | Lifecycle node (Unconfigured → Inactive → Active → Finalized) |
| `qos.hpp` | QoS configuration (Reliability/History/Deadline/Lifespan) |
| `serial_transport.hpp` | Industrial serial transport (CRC-CCITT, PTY testing, IEC 61508) |
| `app.hpp` | Application/Instance two-tier model (compatible with original OSP) |
| `post.hpp` | Unified posting (local/remote/broadcast + sync messages) |
| `transport_factory.hpp` | Automatic transport selection (inproc/shm/tcp) |
| `net.hpp` | Network layer wrapper (address resolution, socket options) |

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Application: App/Instance, LifecycleNode, QoS                  │
├─────────────────────────────────────────────────────────────────┤
│  Service: Service (RPC), Discovery, NodeManager                 │
├─────────────────────────────────────────────────────────────────┤
│  Transport: Transport (TCP/UDP), ShmTransport, SerialTransport  │
│             TransportFactory (inproc/shm/tcp auto-selection)    │
├─────────────────────────────────────────────────────────────────┤
│  Network: Socket, Connection, IoPoller (epoll), Net             │
├─────────────────────────────────────────────────────────────────┤
│  Messaging: AsyncBus (MPSC), Node (Pub/Sub), Post (unified)     │
├─────────────────────────────────────────────────────────────────┤
│  Scheduling: Executor (Realtime), WorkerPool, HSM, BT           │
├─────────────────────────────────────────────────────────────────┤
│  Foundation: Config, Log, Timer, Shell, MemPool, Shutdown       │
│              Platform, Vocabulary (expected/optional/Fixed*)     │
└─────────────────────────────────────────────────────────────────┘
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Build with all config backends

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DOSP_CONFIG_INI=ON \
    -DOSP_CONFIG_JSON=ON \
    -DOSP_CONFIG_YAML=ON
cmake --build build -j$(nproc)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `OSP_BUILD_TESTS` | ON | Build test suite (Catch2 v3.5.2) |
| `OSP_BUILD_EXAMPLES` | OFF | Build example programs |
| `OSP_CONFIG_INI` | ON | Enable INI config backend (inih) |
| `OSP_CONFIG_JSON` | OFF | Enable JSON config backend (nlohmann/json) |
| `OSP_CONFIG_YAML` | OFF | Enable YAML config backend (fkYAML) |
| `OSP_NO_EXCEPTIONS` | OFF | Disable exceptions (`-fno-exceptions`) |
| `OSP_WITH_SOCKPP` | ON | Enable sockpp network library (socket/transport) |

## Quick Start

```cpp
#include "osp/config.hpp"
#include "osp/bus.hpp"
#include "osp/node.hpp"
#include "osp/log.hpp"

// Multi-format config
osp::MultiConfig cfg;
cfg.LoadFile("app.yaml");
int32_t port = cfg.GetInt("network", "port", 8080);

// Type-based pub/sub messaging
struct SensorData { float temp; };
struct MotorCmd { int speed; };
using Payload = std::variant<SensorData, MotorCmd>;

osp::Node<Payload> sensor("sensor", 1);
sensor.Subscribe<SensorData>([](const SensorData& d, const auto& h) {
    OSP_LOG_INFO("sensor", "temp=%.1f from sender %u", d.temp, h.sender_id);
});
sensor.Publish(SensorData{25.0f});
sensor.SpinOnce();
```

## CI Pipeline

| Job | Description |
|-----|-------------|
| **build-and-test** | Ubuntu, Debug + Release |
| **build-with-options** | `-fno-exceptions -fno-rtti` compatibility |
| **sanitizers** | AddressSanitizer, ThreadSanitizer, UBSan |
| **code-quality** | clang-format, cpplint |

## Requirements

- CMake >= 3.14
- C++17 compiler (GCC >= 7, Clang >= 5)
- Linux (ARM-Linux embedded platform)

## Third-party Dependencies

All dependencies are fetched automatically via CMake FetchContent:

| Library | Version | Usage | Condition |
|---------|---------|-------|-----------|
| [inih](https://github.com/benhoyt/inih) | r58 | INI config parsing | `OSP_CONFIG_INI=ON` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | JSON config parsing | `OSP_CONFIG_JSON=ON` |
| [fkYAML](https://github.com/fktn-k/fkYAML) | v0.4.0 | YAML config parsing | `OSP_CONFIG_YAML=ON` |
| [sockpp](https://github.com/fpagliughi/sockpp) | v1.0.0 | TCP/UDP socket wrapper | `OSP_WITH_SOCKPP=ON` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | Unit testing | `OSP_BUILD_TESTS=ON` |

## Examples and Tests

- Example programs: `examples/` directory, see [examples/README.md](examples/README.md)
- Unit tests: `tests/` directory, 328+ test cases, see [tests/README.md](tests/README.md)

## Design Patterns

This library uses template-based modern C++ patterns instead of traditional virtual-function OOP:

- **Tag dispatch + Template specialization**: Config backend selection (INI/JSON/YAML)
- **Variadic templates + `if constexpr`**: `Config<Backends...>` compile-time composition
- **CRTP**: Extensible shell commands without virtual functions
- **SBO Callback**: `FixedFunction<Sig, Cap>` with zero heap allocation
- **Lock-free MPSC**: `AsyncBus` sequence-based ring buffer with CAS publish
- **Type-based routing**: `std::variant` + `VariantIndex<T>` compile-time dispatch

## License

Apache-2.0
