# Architecture

newosp is a modern C++17 header-only embedded infrastructure library designed for ARM-Linux industrial systems. This document describes its layered architecture and key design decisions.

## Overview

newosp provides 38 header-only modules organized into 7 layers, from low-level platform abstractions to high-level application frameworks.

## Layered Architecture

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
└──────────┬──────────────────┬───────────────────────────────┘
           │                  │
           v                  v
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

## Module Dependencies

```
Application Layer ──┬──> Service & Discovery ──┬──> Transport Layer ──> Network Layer
                    │                          │                                │
                    │                          └──> State Machine & BT          │
                    │                                      │                    │
                    └──> Core Communication ──┬──> State Machine & BT          │
                                              │                                 │
                                              ├──> Reliability Layer            │
                                              │           │                     │
                                              └───────────┴─────────────────────┴──> Foundation Layer
```

## Key Design Decisions

### 1. Control Plane / Data Plane Separation

newosp separates control messages from bulk data transfer:

- **Control Plane**: Small messages (< 256B) routed through AsyncBus using `std::variant`
  - Throughput: 5.9M msgs/s (x86), 0.5-1.0M msgs/s (ARM)
  - Use case: Commands, status updates, configuration changes

- **Data Plane**: Large payloads (KB-MB) transferred via ShmChannel zero-copy
  - Throughput: 40M ops/s (256B), 2.9M ops/s (4KB) on x86
  - Use case: Point cloud data, video frames, sensor arrays

This architectural choice prevents large payloads from bloating the variant size and degrading bus performance.

### 2. Lock-Free MPSC Message Bus

AsyncBus implements a lock-free multi-producer single-consumer ring buffer:

- **CAS-based publish**: Producers use compare-and-swap to claim sequence numbers
- **Template parameterization**: `AsyncBus<PayloadVariant, QueueDepth, BatchSize>` allows compile-time tuning
- **Priority admission control**: LOW (60%), MEDIUM (80%), HIGH (99%), CRITICAL (100%) thresholds
- **Cache line separation**: Producer/consumer counters on separate cache lines to avoid false sharing

```cpp
using LightBus = AsyncBus<Payload, 256, 64>;    // ~20KB (low memory)
using HighBus = AsyncBus<Payload, 4096, 256>;   // ~320KB (high throughput)
```

### 3. ARM Memory Ordering

ShmRingBuffer uses explicit acquire/release memory fences for ARM weak memory model:

| Operation | Memory Order | Purpose |
|-----------|-------------|---------|
| Producer writes slot data | Ordinary write | Fill data |
| Producer advances `prod_pos_` | `release` | Make data visible to consumer |
| Consumer reads `prod_pos_` | `acquire` | Synchronize with producer |
| Consumer reads slot data | Ordinary read | Access visible data |

This explicit ordering provides 3-5x performance improvement over default `seq_cst` on ARM platforms.

### 4. Zero-Heap Hot Path

All hot-path operations avoid heap allocation:

- Fixed-capacity containers: `FixedVector<T, Cap>`, `FixedString<Cap>`
- Stack-allocated ring buffers: `SpscRingbuffer<T, BufferSize>`
- Small buffer optimization: `FixedFunction<Sig, BufSize>` (default 2 pointers)
- Object pools: `ObjectPool<T, MaxObjects>` for predictable allocation

### 5. Template-Based Compile-Time Dispatch

newosp uses modern C++ patterns instead of virtual functions:

- **Tag dispatch**: Config backend selection (INI/JSON/YAML)
- **Variadic templates + `if constexpr`**: `Config<Backends...>` composition
- **CRTP**: Shell command extension without vtables
- **Type-based routing**: `std::variant` + `VariantIndex<T>` for message dispatch

This approach eliminates vtable overhead and enables aggressive compiler optimization.

## Data Flow Example: LiDAR System

Typical data flow in an industrial LiDAR system using newosp:

```
┌─────────────────────────────────────────────────────────────────┐
│                        LiDAR System                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────┐   ShmChannel    ┌──────────┐   ShmChannel       │
│  │ Acquire  │  (raw points)   │ Process  │  (calibrated)      │
│  │ Module   ├────────────────>│ Module   ├──────────┐         │
│  │ (SPI)    │                 │ (denoise)│          │         │
│  └────┬─────┘                 └────┬─────┘          │         │
│       │                            │                │         │
│       │ Bus (control msgs)         │                v         │
│       │                            │         ┌──────────┐     │
│       v                            v         │  Fusion  │     │
│  ┌──────────────────────────────────┐       │  Module  │     │
│  │     Control Node (HSM)           │       │ (merge)  │     │
│  │  States: Init -> Calibrating     │       └────┬─────┘     │
│  │          -> Running -> Standby   │            │           │
│  └──────────────────────────────────┘            │           │
│       ^                            ^              │           │
│       │ Bus (status)               │              │           │
│       │                            │              v           │
│  ┌────┴─────┐                 ┌────┴─────┐   ShmChannel      │
│  │ Diag     │  SerialTransport│ Timer    │  (full frame)     │
│  │ Module   │<───────────────>│ Scheduler│                   │
│  │ (UART)   │  (config/OTA)   │ (trigger)│       │           │
│  └──────────┘                 └──────────┘       │           │
│       ^                                           v           │
│       │                                    ┌──────────┐       │
│       └────────────────────────────────────│  Output  │       │
│                  Bus (diagnostics)         │  Module  │       │
│                                            │ (UDP/PTP)│       │
│                                            └──────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

Key points:

- Point cloud data (large) flows through ShmChannel zero-copy paths
- Control messages (small) flow through Bus pub-sub
- HSM manages device lifecycle states
- SerialTransport provides diagnostic channel independent of data path
- TimerScheduler drives periodic scanning triggers

## Resource Budget

Typical memory footprint for a complete system:

| Component | Static Memory | Heap | Threads |
|-----------|--------------|------|---------|
| Foundation Layer | ~50 KB | ~5 KB | 4 |
| Core Communication | ~400 KB | ~20 KB | 6 |
| Network & Transport | ~10 KB | ~5 KB | 2 |
| Application Layer | ~20 KB | 0 | 2 |
| Total Framework | ~500 KB | ~30 KB | 14 |

For embedded systems with 256 MB - 2 GB RAM, newosp consumes less than 1% of available memory.

## Performance Characteristics

Measured on x86 (Intel i7-9750H), ARM estimates based on 1 GHz Cortex-A:

| Metric | x86 | ARM (est) | Typical Requirement | Margin |
|--------|-----|-----------|---------------------|--------|
| Bus throughput | 5.9M msgs/s | 0.5-1.0M msgs/s | < 1K msgs/s | > 500x |
| Bus P99 latency | 157 ns | 500-1500 ns | < 100 us | > 60x |
| SHM SPSC (256B) | 40M ops/s | 5-10M ops/s | < 30K ops/s | > 150x |
| MemPool alloc | 98M ops/s | 10-20M ops/s | < 100K ops/s | > 100x |
| Timer jitter (avg) | < 10 us | < 50 us | < 1 ms | > 20x |

All metrics provide 100x+ margin over typical industrial embedded requirements.

## Thread Safety Model

| Module | Thread Safety |
|--------|--------------|
| AsyncBus | Lock-free MPSC publish, SharedMutex for subscriptions |
| Node | Thread-safe publish, single-consumer SpinOnce |
| SpscRingbuffer | Single-producer single-consumer (SPSC contract) |
| ShmRingBuffer | Lock-free CAS, single-writer multiple-readers |
| WorkerPool | Atomic flags + CV + SPSC per-worker queues |
| HSM | Single-threaded Dispatch, non-reentrant handlers |
| BehaviorTree | Single-threaded Tick, non-reentrant callbacks |

## Design Patterns

newosp leverages modern C++ patterns:

- **Lock-free MPSC**: Inspired by LMAX Disruptor
- **SPSC ring buffer**: Wait-free single-producer single-consumer
- **SBO callbacks**: Small buffer optimization for `FixedFunction`
- **LCA state transitions**: Hierarchical state machine entry/exit paths
- **Two-tier scheduling**: MPSC bus + SPSC per-worker queues
- **Zero-copy IPC**: Shared memory ring buffer with eventfd notification
- **Compile-time dispatch**: Tag dispatch + template specialization

## Next Steps

- See [[Quick-Start]] to build your first application
- Explore [[Examples]] for real-world use cases
- Read [[API-Reference]] for detailed module documentation
- Check [[Performance-Tuning]] for optimization guidelines
